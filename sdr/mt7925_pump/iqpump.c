/*
 * MT7925 I/Q pump: runs on the WM in place of a UNI command handler.  Each
 * command takes a burst of snapshots: trigger the ICAP into its SYSRAM window,
 * wait for it to fill, then send one path's samples to the host as MCU events
 * through the firmware's own event buffers.  The MT7925U has no MCU_DMA TX0
 * to stream from, and the WM can't copy fast enough to keep up with a ring.
 *
 * Everything chip-specific comes from the control block the host writes:
 * register addresses, the arm and disarm sequences as ops, the event header.
 * Each record is the RXD (the send fills it), the host's UNI and IQB2 header
 * template, then chunk bytes of samples.  A burst ends with a record of no
 * payload, so the host knows to send the next command.
 * Built freestanding and relocation-free; the entry must be the first code.
 */

#include "iqpump.h"

typedef uint32_t u32;
typedef uint16_t u16;

#define RD(a)		(*(volatile u32 *)(a))
#define WR(a, v)	(*(volatile u32 *)(a) = (v))

/* Sample words: the window's 512-beat page pairs, low halves then high halves */
#define PAGE_BEATS	512
#define PAGE_BYTES	0x2000
#define HI_OFS		0x1000

typedef u32 (*alloc_t)(u32);
typedef u32 (*send_t)(u32, u32, u32);
typedef u32 (*call_t)(u32, u32, u32, u32, u32);

static inline __attribute__((always_inline)) u32 now(struct mt7925_pump_ctl *c)
{
	return RD(c->timer_reg);
}

/* The low half of beat b, at the path's word */
static inline __attribute__((always_inline)) u32 beat_addr(struct mt7925_pump_ctl *c, u32 b)
{
	if (c->linear)
		return c->win_addr + b * 16 + 4 * c->path;
	return c->win_addr + (b / PAGE_BEATS) * PAGE_BYTES + (b % PAGE_BEATS) * 8 + 4 * c->path;
}

/* From a beat's low half to its high half */
static inline __attribute__((always_inline)) u32 hi_ofs(struct mt7925_pump_ctl *c)
{
	return c->linear ? 8 : HI_OFS;
}

static inline __attribute__((always_inline)) void run_ops(struct mt7925_pump_ctl *c, u32 from, u32 n)
{
	for (u32 i = from; i < from + n; i++) {
		volatile u32 *o = c->ops[i];

		if (o[0] == MT7925_PUMP_OP_RMW)
			WR(o[1], (RD(o[1]) & ~o[2]) | o[3]);
		else if (o[0] == MT7925_PUMP_OP_CALL)
			((call_t)o[1])(o[2], o[3], o[4], 0, 0);
	}
}

/* One capture, between the pre and post ops; its start tick, or -1 */
static inline __attribute__((always_inline)) int snap(struct mt7925_pump_ctl *c, u32 *tick)
{
	u32 t0;
	int r = 0;

	if (c->fill)
		for (u32 b = 0; b < c->win_beats; b++)
			for (u32 w = 0; w < c->half_words; w++) {
				WR(beat_addr(c, b) + 4 * w, c->fill);
				WR(beat_addr(c, b) + hi_ofs(c) + 4 * w, c->fill);
			}
	run_ops(c, c->setup_n + c->teardown_n, c->pre_n);
	WR(c->grant_reg, RD(c->grant_reg) | c->grant_bits);
	WR(c->cr0, c->ctl_idle);
	WR(c->cr0, c->ctl_arm);
	t0 = now(c);
	WR(c->cr0, c->ctl_trigger);
	while (RD(c->cr0) & c->busy_mask)
		if (now(c) - t0 > c->snap_timeout_us) {
			r = -1;
			break;
		}
	c->dbg[0] = now(c) - t0;
	WR(c->grant_reg, RD(c->grant_reg) & ~c->grant_bits);
	WR(c->cr0, c->ctl_idle);
	run_ops(c, c->setup_n + c->teardown_n + c->pre_n, c->post_n);
	for (u32 a = c->evict_addr; a < c->evict_addr + c->evict_len; a += 4)
		(void)RD(a);
	*tick = t0;
	return r;
}

/* A record of plen bytes of samples from beat b; -1 with no buffer */
static inline __attribute__((always_inline)) int record(struct mt7925_pump_ctl *c, u32 b, u32 plen, u32 tick)
{
	u32 size = MT7925_PUMP_HDR_WORDS * 4 + plen, t0 = now(c), pkt, d, h, i;

	while (!(pkt = ((alloc_t)c->alloc_fn)(size)))
		if (now(c) - t0 > c->alloc_us)
			return -1;
	d = RD(pkt + 0xc);
	for (i = 0; i < MT7925_PUMP_RXD_LEN; i += 4)
		WR(d + i, 0);
	d += MT7925_PUMP_RXD_LEN;
	for (i = 0; i < MT7925_PUMP_HDR_WORDS; i++)
		WR(d + 4 * i, c->hdr[i]);
	*(volatile u16 *)d = (u16)size;
	h = d + MT7925_PUMP_IQB2_OFS;
	WR(h + MT7925_PUMP_IQB2_PLEN, plen);
	WR(h + MT7925_PUMP_IQB2_SEQ, c->seq);
	WR(h + MT7925_PUMP_IQB2_CURSOR, 8 * c->half_words * b);
	WR(h + MT7925_PUMP_IQB2_GEN, c->gen);
	WR(h + MT7925_PUMP_IQB2_TICK, tick);
	d += MT7925_PUMP_HDR_WORDS * 4;
	for (i = 0; i < plen; b++) {
		u32 lo = beat_addr(c, b);

		WR(d + i, RD(lo));
		if (c->half_words == 2) {
			WR(d + i + 4, RD(lo + 4));
			WR(d + i + 8, RD(lo + hi_ofs(c)));
			WR(d + i + 12, RD(lo + hi_ofs(c) + 4));
			i += 16;
		} else {
			WR(d + i + 4, RD(lo + hi_ofs(c)));
			i += 8;
		}
	}
	((send_t)c->send_fn)(pkt, c->send_port, 0);
	c->seq++;
	return 0;
}

__attribute__((section(".text.entry"), used))
int entry(void *req)
{
	struct mt7925_pump_ctl *c = (struct mt7925_pump_ctl *)CTL_ADDR;
	u32 bytes = c->win_beats * 8 * c->half_words, last = 0, beat, beat_at, tick, k;
	int first = 1;

	(void)req;
	if (c->magic != MT7925_PUMP_MAGIC || c->version != MT7925_PUMP_VERSION ||
	    !c->alloc_fn || !c->send_fn || !c->half_words || c->half_words > 2 ||
	    !c->chunk || c->chunk % (8 * c->half_words) ||
	    !c->win_beats || c->win_beats % PAGE_BEATS || c->path > 2 - c->half_words ||
	    c->setup_n + c->teardown_n + c->pre_n + c->post_n > MT7925_PUMP_OPS_MAX) {
		c->state = MT7925_PUMP_BADCTL;
		return 0;
	}
	c->state = MT7925_PUMP_RUNNING;
	if (!c->armed && !c->stop) {
		run_ops(c, 0, c->setup_n);
		/* The first capture after arming leaves old window contents; drop it */
		if (snap(c, &tick) < 0)
			c->timeouts++;
		c->armed = 1;
	}
	beat = c->beat;
	beat_at = now(c);

	for (k = 0; !c->burst || k < c->burst; k++) {
		if (c->stop)
			break;
		if (c->beat != beat) {
			beat = c->beat;
			beat_at = now(c);
		} else if (c->beat_us && now(c) - beat_at > c->beat_us) {
			c->state = MT7925_PUMP_HOST_LOST;
			break;
		}
		while (!first && now(c) - last < c->period_us)
			;
		first = 0;
		last = now(c);
		if (snap(c, &tick) < 0) {
			c->timeouts++;
			c->state = MT7925_PUMP_SNAP_TIMEOUT;
			break;
		}
		for (u32 off = 0; off < bytes; off += c->chunk) {
			u32 plen = bytes - off < c->chunk ? bytes - off : c->chunk;

			if (record(c, off / (8 * c->half_words), plen, tick) < 0) {
				/* The host sees the rest of the snapshot as a seq gap */
				c->seq += (bytes - off + c->chunk - 1) / c->chunk;
				c->nobuf++;
				c->state = MT7925_PUMP_NO_BUF;
				break;
			}
		}
		c->gen++;
		c->snaps++;
		if (c->state != MT7925_PUMP_RUNNING)
			break;
	}
	if (c->stop == MT7925_PUMP_STOP_ALL) {
		run_ops(c, c->setup_n, c->teardown_n);
		c->armed = 0;
	}
	if (c->state == MT7925_PUMP_RUNNING)
		c->state = c->stop == MT7925_PUMP_STOP_ALL ? MT7925_PUMP_STOPPED : MT7925_PUMP_DONE;
	/* The burst's end marker; without a buffer the host finds the state by polling */
	record(c, 0, 0, now(c));
	return 0;
}
