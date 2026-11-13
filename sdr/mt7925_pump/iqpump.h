/*
 * The MT7925 I/Q pump's control block, shared by the pump (iqpump.c, on the WM) and the
 * host (mt7925_host.c).  The host writes it to MCU memory at MT7925_PUMP_CTL_ADDR at each
 * tune; fields marked pump are written back while it runs.  All little endian.
 */
#ifndef MT7925_IQPUMP_H
#define MT7925_IQPUMP_H

#include <stdint.h>

#define MT7925_PUMP_MAGIC	0x504d5149u	/* "IQMP" */
#define MT7925_PUMP_VERSION	5

/* state */
#define MT7925_PUMP_IDLE	0
#define MT7925_PUMP_RUNNING	1
#define MT7925_PUMP_DONE	2	/* burst finished, capture still armed */
#define MT7925_PUMP_STOPPED	3	/* host stop, capture torn down */
#define MT7925_PUMP_BADCTL	4
#define MT7925_PUMP_SNAP_TIMEOUT 5	/* a capture never finished */
#define MT7925_PUMP_HOST_LOST	6	/* the host stopped beating */
#define MT7925_PUMP_NO_BUF	7	/* the firmware had no event buffer */

/* stop */
#define MT7925_PUMP_STOP_BURST	1	/* end the burst, stay armed */
#define MT7925_PUMP_STOP_ALL	2	/* end it and run the teardown */

/* ops: {op, addr, a, b, c} */
#define MT7925_PUMP_OP_RMW	1	/* *addr = (*addr & ~a) | b */
#define MT7925_PUMP_OP_CALL	2	/* addr(a, b, c, 0, 0) */
#define MT7925_PUMP_OPS_MAX	48

/* The record header after the MCU event RXD: the connac3 UNI event header, then IQB2 */
#define MT7925_PUMP_RXD_LEN	0x20
#define MT7925_PUMP_HDR_WORDS	11
#define MT7925_PUMP_IQB2_OFS	0x0c	/* from the end of the RXD */
#define MT7925_PUMP_IQB2_PLEN	8	/* fields the pump fills, from the IQB2 header */
#define MT7925_PUMP_IQB2_SEQ	12
#define MT7925_PUMP_IQB2_CURSOR	16
#define MT7925_PUMP_IQB2_GEN	24
#define MT7925_PUMP_IQB2_TICK	28

struct mt7925_pump_ctl {
	uint32_t magic;			/* host: MT7925_PUMP_MAGIC */
	uint32_t version;		/* host: MT7925_PUMP_VERSION */
	volatile uint32_t state;	/* pump */
	volatile uint32_t stop;		/* host: MT7925_PUMP_STOP_* */
	volatile uint32_t beat;		/* host: bumped at least every beat_us */
	volatile uint32_t gen;		/* the next snapshot's generation; pump counts it up */
	volatile uint32_t seq;		/* pump: records sent */
	volatile uint32_t snaps;	/* pump: snapshots sent */
	volatile uint32_t timeouts;	/* pump: captures that never finished */
	volatile uint32_t nobuf;	/* pump: records lost for want of a buffer */
	volatile uint32_t armed;	/* host 0: run the setup ops first; pump sets 1 */
	volatile uint32_t dbg[4];	/* pump: [0] the last capture's length in us */

	/* Firmware: event buffer alloc(size) -> pkt, data at *(pkt + 0xc); send(pkt, port, 0) */
	uint32_t alloc_fn;
	uint32_t send_fn;
	uint32_t send_port;
	uint32_t timer_reg;		/* free-running microsecond counter */

	/* Capture: CR0 control idle -> arm -> trigger, done when busy_mask clears.  The window's
	 * beats keep their low 8 bytes in one 4 KiB page and the high 8 in the next, 512 beats
	 * per page pair; with linear set they are 16 bytes each in order from win_addr (the
	 * packet pool route).  Each half holds one sample of each path: half_words 1 sends the
	 * path's word, 2 both, paths alternating. */
	uint32_t cr0;
	uint32_t ctl_idle;
	uint32_t ctl_arm;
	uint32_t ctl_trigger;
	uint32_t busy_mask;
	uint32_t snap_timeout_us;
	uint32_t grant_reg;		/* the route's window grant: held across a capture */
	uint32_t grant_bits;
	uint32_t win_addr;
	uint32_t win_beats;		/* multiple of 512 */
	uint32_t path;			/* 0 or 1, with half_words 1 */
	uint32_t half_words;		/* 1 or 2 */
	uint32_t fill;			/* nonzero: written over the path's words before each capture
					   (not with evict: cached writes land after the capture) */
	uint32_t linear;
	/* The WM caches the pool view (write back, ~32 KiB): evict_len bytes read from
	 * evict_addr before a capture's samples push out lines left from the last one.  The
	 * pump never writes the capture there, so no line is dirty. */
	uint32_t evict_addr;
	uint32_t evict_len;

	uint32_t chunk;			/* payload bytes per record, multiple of 8 * half_words */
	uint32_t burst;			/* snapshots per command; 0 = until stopped */
	uint32_t period_us;		/* least time from one snapshot to the next */
	uint32_t beat_us;		/* no host beat for this long ends a burst; 0 = none */
	uint32_t alloc_us;		/* how long to wait for an event buffer */

	uint32_t hdr[MT7925_PUMP_HDR_WORDS];	/* UNI header and IQB2 header template */

	uint32_t setup_n;		/* ops[0, setup_n) arm the capture */
	uint32_t teardown_n;		/* ops[setup_n, + teardown_n) disarm it */
	uint32_t pre_n;			/* the next pre_n ops run before each capture */
	uint32_t post_n;		/* and the next post_n after it */
	uint32_t ops[MT7925_PUMP_OPS_MAX][5];
};

#endif
