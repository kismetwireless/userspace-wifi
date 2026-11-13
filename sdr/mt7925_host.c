/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <kcompat/host.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "../common/uwifi.h"
#include "mt76/mt76.h"
#include "mt76/mt76_connac_mcu.h"
#include "mt76_icap/mt7921/iqstream.h"
#include "mt76_icap/mt7921/iqstream_ioctl.h"
#include "mt7925_host.h"
#include "mt7925_pump.h"
#include "mt7925_pump/iqpump.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the pump control block is written as host memory"
#endif

#define OPEN_TIMEOUT_MS 20000
#define XFER_MAX 64

/* Everything below is specific to this WM firmware build (from the image's trailer) */
#define FW_BUILD "20260813113118"
/* The pump replaces the handler of UNI command 0x7d, which the mt7925 driver never sends:
 * entry 39 of the runtime dispatch table of {u16 id, u16 pad, u32 handler} */
#define PUMP_UNI_ID 0x7d
#define UNI_ENTRY 0x0221c074
#define UNI_STOCK 0xe004180c
/* Free ULM between the ROM patch's end and the WM image */
#define PUMP_CODE 0x00909c00
#define PUMP_FREE_END 0x0090d000
/* GPT4, the firmware's microsecond counter */
#define MCU_TIMER_US 0x81060068
/* The firmware's event buffers: alloc(size), then send(pkt, port, 0) to the host */
#define FW_EVT_ALLOC 0xe0028948
#define FW_EVT_SEND 0xe00850e6
#define FW_EVT_PORT 6

_Static_assert(PUMP_CODE + sizeof(mt7925_pump_code) <= MT7925_PUMP_CTL_ADDR, "pump code overlaps its control block");
_Static_assert(sizeof(mt7925_pump_code) % 4 == 0 && sizeof(struct mt7925_pump_ctl) % 4 == 0, "pump not whole words");
_Static_assert(MT7925_PUMP_CTL_ADDR + sizeof(struct mt7925_pump_ctl) <= PUMP_FREE_END, "pump control block too large");

/* The capture block, set up in the order of the w1700k-sdr MT7996 capture setup
 * (include/hw/mt7996) and checked on hardware.  Armed, ICAP0 fills the 16 KiB SYSRAM window
 * (1024 beats, two samples of each RX path) while the route grants it the window's banks;
 * the pump's captures go to the packet pool instead (below).  The packers and the dense DFE
 * mode that give one path four samples a beat on the MT7921 and MT7996 don't on the MT7925
 * (paths still alternate, or nothing strobes). */
#define ICAP_CR0 0x82023090
#define ICAP_IDLE 0x400
#define ICAP_ARM 0x401
#define ICAP_TRIGGER 0x403
#define ICAP_BUSY BIT(1)
#define ICAP_CR10 0x820230b8
#define ICAP_PACK_MASK 0xf0000
/* Path 0's word of each half beat */
#define HALF_WORDS 1
#define GRANT_REG 0x88009004
#define GRANT_BITS 0xff000000
/* The firmware's capture route: route(0, node, 0, 0, 0) programs the DFE for the node */
#define ROUTE_FN 0xe00b40e4
#define ROUTE_NODE 0x00204000
/* DFE capture mux: chain 0 tap 0x14 and the rate field, 3 for 40 MS/s */
#define DFE_MUX 0x830ad448
#define DFE_MUX_MASK (0xff | 3 << 16 | BIT(21))
#define DFE_MUX_VAL(rate) (0x14 | (rate) << 16 | BIT(21))
#define CAPTRIG 0x830a1000
#define CAPTRIG_CFG 0x830a1004

/* Captures four times the window: the route's pool path (bit 0) sends ICAP0 into the
 * packet pool, ADDR0 then in 4-byte units of it, the beats linear.  Only the pool's first
 * 64 KiB is left alone: the firmware's event buffers follow it, and RX frames and the page
 * manager use the rest (captures there come back clobbered, then the firmware hangs).
 * 64 KiB is 8192 samples a path, 205 us at 40 MS/s.  The WM's view of the pool is cached
 * (write back, ~32 KiB): the pump reads 64 KiB of RX pool, harmless to read, to push out the
 * last capture's lines.  A record with a word no sample could be is dropped all the same. */
#define POOL_CPU 0xe0400000
#define POOL_OFF 0x0
#define POOL_BYTES 0x10000
#define POOL_BEATS (POOL_BYTES / 16)
#define ROUTE_PATH 0x88009028
#define ROUTE_POOL BIT(0)
#define ICAP_ADDR0_START 0x82023098
#define ICAP_ADDR0_END 0x8202309c
#define WIN_ADDR0_END 0xffc
#define POOL_EVICT 0x10000
#define POOL_EVICT_AT 0x40000
/* Samples leave bits 31:30 and 15:14 clear */
#define SAMPLE_PAD 0xc000c000

#define OP_RMW(a, clr, set) { MT7925_PUMP_OP_RMW, (a), (clr), (set), 0 }
#define OP_WR(a, v) OP_RMW(a, 0xffffffff, v)

/* Arming up to the route call; the DFE mux and arm_post follow it */
static const u32 arm_pre[][5] = {
    /* Clear any earlier capture */
    OP_RMW(CAPTRIG_CFG, 0xff, 0),
    OP_RMW(0x830a3008, 0, BIT(21)),
    OP_RMW(0x830a3008, 0, BIT(22)),
    OP_WR(DFE_MUX, 0),
    OP_WR(0x830ad44c, 0),
    /* ICAP0 into the window: band 0, 1024 beats, stop after 0x400 strobes */
    OP_WR(ICAP_CR0, 0),
    OP_WR(ICAP_CR0, ICAP_IDLE),
    OP_RMW(ICAP_CR10, ICAP_PACK_MASK, 0),
    OP_RMW(GRANT_REG, 0, GRANT_BITS),
    OP_RMW(0x8800900c, 0x38, 0),
    OP_RMW(0x88009024, 0, 0x02000000),
    OP_RMW(0x88009028, 0x401, 0x400),
    OP_WR(ICAP_CR0, ICAP_ARM),
    OP_WR(0x820230b0, 0),
    OP_WR(0x82023098, 0),
    OP_WR(0x8202309c, 0xffc),
    OP_WR(0x820230a4, 0x400),
    OP_WR(ICAP_CR0, ICAP_TRIGGER),
    /* Baseband capture clocks */
    OP_RMW(0x83080004, 0, BIT(25)),
    OP_RMW(0x83080008, 0, BIT(25)),
    { MT7925_PUMP_OP_CALL, ROUTE_FN, 0, ROUTE_NODE, 0 },
};

/* The PHY's RX gain force per chain (CSD +0xc4, as the MT7996): sets A and B at an LNA and
 * LPF code; 0 gives the gain back to the AGC */
#define RXGAIN_REG(c) (0x830000c4 + ((c) << 16))
#define RXGAIN_LNA_MAX 4
#define RXGAIN_LPF_MAX 9
#define RXGAIN_FORCE(lna, lpf) (BIT(0) | BIT(1) | (lna) << 8 | (lpf) << 12 | (lna) << 16 | (lpf) << 20)
#define RXGAIN_OPS 2

static const u32 arm_post[][5] = {
    OP_RMW(CAPTRIG, BIT(17), 0),
    OP_RMW(CAPTRIG, 0, BIT(19) | BIT(16) | BIT(26)),
    OP_RMW(CAPTRIG, 0xff00, 0x100),
    OP_WR(0x830a1008, 0),
    /* The pump takes the grant for each capture */
    OP_RMW(GRANT_REG, GRANT_BITS, 0),
    OP_WR(ICAP_CR0, ICAP_IDLE),
};

static const u32 disarm[][5] = {
    OP_WR(CAPTRIG, 0),
    OP_RMW(CAPTRIG_CFG, 0xff, 0),
    OP_RMW(GRANT_REG, GRANT_BITS, 0),
    OP_WR(ICAP_CR0, ICAP_IDLE),
};

/* Around each pool capture.  ADDR0 outside the window without the pool path reaches MCU
 * memory, so it moves only while the path is set. */
static const u32 pool_pre[][5] = {
    OP_WR(ICAP_CR0, ICAP_IDLE),
    OP_RMW(ROUTE_PATH, 0, ROUTE_POOL),
    OP_WR(ICAP_ADDR0_START, POOL_OFF / 4),
    OP_WR(ICAP_ADDR0_END, (POOL_OFF + POOL_BYTES) / 4 - 4),
};

static const u32 pool_post[][5] = {
    OP_WR(ICAP_ADDR0_START, 0),
    OP_WR(ICAP_ADDR0_END, WIN_ADDR0_END),
    OP_RMW(ROUTE_PATH, ROUTE_POOL, 0),
};

_Static_assert(ARRAY_SIZE(arm_pre) + 1 + ARRAY_SIZE(arm_post) + ARRAY_SIZE(disarm) + ARRAY_SIZE(pool_pre) +
        ARRAY_SIZE(pool_post) + 2 * RXGAIN_OPS <= MT7925_PUMP_OPS_MAX, "too many pump ops");

/* IQB2 sample format and DFE rate field at 20, 40, 80 and 160 MHz: twice the width, the rate
 * field 3 = 40 MS/s down to 0 = 320 MS/s.  The words are DENSE at every rate. */
static const struct {
    u32 fmt, rate;
} bw_cfg[4] = {
    { MT7921_IQ_FMT_DENSE40, 3 },
    { MT7921_IQ_FMT_DENSE80, 2 },
    { MT7921_IQ_FMT_DENSE80, 1 },
    { MT7921_IQ_FMT_DENSE80, 0 },
};

/* Records: the MCU event RXD, the connac3 UNI event header, then the IQB2 header and samples */
#define RXD_LEN MT7925_PUMP_RXD_LEN
#define UNI_EID (RXD_LEN + 4)
#define UNI_OPTION (RXD_LEN + 6)
#define UNI_EXT_EID (RXD_LEN + 8)
#define UNI_DATA (RXD_LEN + MT7925_PUMP_IQB2_OFS)
#define IQ_EID 0xed
#define IQ_EXT_EID 0x50
/* The firmware's answer to a command, the command id at UNI_DATA */
#define RSP_EID 0x01
#define IQB2_LEN ((u32) sizeof(struct mt7921_iqb2_hdr))
/* A record per firmware event buffer, which reach the host 4 KiB at most: 9 to a snapshot */
#define CHUNK 4016
#define SNAP_BYTES (POOL_BEATS * 8 * HALF_WORDS)
#define RING_BYTES (8u << 20)

_Static_assert(MT7925_PUMP_HDR_WORDS * 4 == MT7925_PUMP_IQB2_OFS + IQB2_LEN, "pump header isn't UNI and IQB2");
_Static_assert(offsetof(struct mt7921_iqb2_hdr, payload_len) == MT7925_PUMP_IQB2_PLEN &&
        offsetof(struct mt7921_iqb2_hdr, seq) == MT7925_PUMP_IQB2_SEQ &&
        offsetof(struct mt7921_iqb2_hdr, cursor) == MT7925_PUMP_IQB2_CURSOR &&
        offsetof(struct mt7921_iqb2_hdr, generation) == MT7925_PUMP_IQB2_GEN &&
        offsetof(struct mt7921_iqb2_hdr, tick) == MT7925_PUMP_IQB2_TICK, "IQB2 fields moved");
_Static_assert(UNI_DATA + IQB2_LEN + CHUNK <= 4096, "record larger than reaches the host");
_Static_assert(MT7921_IQ_RING_CODE(SNAP_BYTES) != 0, "snapshot not an IQB2 ring size");
_Static_assert(CHUNK % (8 * HALF_WORDS) == 0, "record not whole beats");

/* Snapshots per command, ~8 ms each.  The firmware queues the next command behind the
 * running one; the driver's own commands wait behind at most MAX_OUT bursts. */
#define BURST 8
#define MAX_OUT 2
#define POLL_MS 20
#define SNAP_TIMEOUT_US 5000
#define ALLOC_US 20000
#define BURST_TIMEOUT_MS 1000
#define STOP_TIMEOUT_MS 1000
#define RESTART_DELAY_MS 20
#define MAX_START_FAILS 5

/* The driver's RX and MCU send paths, wrapped for the adapters with a stream open.  Entries
 * outlive the streams, since the driver may still be inside a wrapper when one closes. */
struct hook {
    struct mt76_dev *mdev;
    const struct mt76_driver_ops *orig_drv;
    const struct mt76_mcu_ops *orig_mcu;
    struct mt76_driver_ops drv;
    struct mt76_mcu_ops mcu;
    struct mt7925_iq *q;
};

#define MAX_HOOKS 8
static struct hook hooks[MAX_HOOKS];
static pthread_mutex_t hooks_lock = PTHREAD_MUTEX_INITIALIZER;

struct mt7925_iq {
    struct uwifi_dev *dev;
    struct mt76_dev *mdev;
    struct hook *hook;
    /* Wrappers running with this stream */
    atomic_t users;

    /* Pump control; taken after mdev->mcu.mutex when both are held */
    pthread_mutex_t ctl;
    pthread_cond_t answered;
    bool live, stopping, want, ended, check;
    unsigned int fails, outstanding;
    uint64_t restart_at, sent_at;
    unsigned int bw_idx;
    u32 timeouts, nobuf;
    /* The RX gain force word, 0 for the AGC */
    u32 rxgain;
    struct mt7925_pump_ctl c;
    pthread_t worker;
    bool worker_started;
    volatile bool worker_run;

    /* Whole IQB2 records from the RX path; head and tail are free-running.  Generations
     * before gen_base are from before the last tune. */
    pthread_mutex_t ring_lock;
    u8 *ring;
    uint64_t head, tail;
    u32 gen_base, next_gen;
    /* Counters: the RX ones under ring_lock, the pump ones under ctl */
    struct mt7925_iq_stats st;
};

static uint64_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

/* MCU memory over READ_EXT/WRITE_EXT, XFER_MAX at a time (longer ones come back wrong);
 * 0 or a negative errno */
static int xfer(struct mt7925_iq *q, bool in, u32 addr, void *buf, size_t len) {
    struct mt76_dev *mdev = q->mdev;
    size_t max = min_t(size_t, mdev->usb.data_len, XFER_MAX) & ~3u, off = 0;
    int r = 0;

    if ((addr & 3) || (len & 3) || max == 0)
        return -EINVAL;
    mutex_lock(&mdev->usb.usb_ctrl_mtx);
    while (off < len && r >= 0) {
        size_t n = len - off < max ? len - off : max;
        u32 a = addr + (u32) off;

        if (!in)
            memcpy(mdev->usb.data, (u8 *) buf + off, n);
        r = __mt76u_vendor_request(mdev, in ? MT_VEND_READ_EXT : MT_VEND_WRITE_EXT,
                (in ? USB_DIR_IN : USB_DIR_OUT) | USB_TYPE_VENDOR, (u16) (a >> 16), (u16) a,
                mdev->usb.data, n);
        if (r >= 0 && in)
            memcpy((u8 *) buf + off, mdev->usb.data, n);
        off += n;
    }
    mutex_unlock(&mdev->usb.usb_ctrl_mtx);
    return r < 0 ? r : 0;
}

static int rd32(struct mt7925_iq *q, u32 addr, u32 *val) {
    __le32 v;
    int r = xfer(q, true, addr, &v, 4);

    *val = r < 0 ? 0 : le32_to_cpu(v);
    return r;
}

static int wr32(struct mt7925_iq *q, u32 addr, u32 val) {
    __le32 v = cpu_to_le32(val);

    return xfer(q, false, addr, &v, 4);
}

/* Writes and reads back */
static int load(struct mt7925_iq *q, u32 addr, const void *data, size_t len) {
    u8 *chk = malloc(len);
    int r;

    if (chk == NULL)
        return -ENOMEM;
    if ((r = xfer(q, false, addr, (void *) data, len)) == 0 &&
            (r = xfer(q, true, addr, chk, len)) == 0 && memcmp(chk, data, len) != 0)
        r = -EIO;
    free(chk);
    return r;
}

#define CTL_FIELD(f) (MT7925_PUMP_CTL_ADDR + (u32) offsetof(struct mt7925_pump_ctl, f))

/* Points UNI_ENTRY's command at the pump, or back at its stock handler; anything but the two
 * there means another firmware build */
static int uni_swap(struct mt7925_iq *q, bool pump) {
    u32 id, handler;
    int r;

    if ((r = rd32(q, UNI_ENTRY, &id)) < 0 || (r = rd32(q, UNI_ENTRY + 4, &handler)) < 0)
        return r;
    if ((id & 0xffff) != PUMP_UNI_ID || (handler != UNI_STOCK && handler != PUMP_CODE))
        return -EOPNOTSUPP;
    if (handler == (pump ? PUMP_CODE : UNI_STOCK))
        return 0;
    return wr32(q, UNI_ENTRY + 4, pump ? PUMP_CODE : UNI_STOCK);
}

static void ops_add(struct mt7925_pump_ctl *c, u32 *n, const u32 (*ops)[5], size_t count) {
    for (size_t i = 0; i < count; i++, (*n)++)
        memcpy(c->ops[*n], ops[i], sizeof(c->ops[0]));
}

static void ctl_fill(struct mt7925_iq *q, u32 gen) {
    struct mt7925_pump_ctl *c = &q->c;
    u8 *t = (u8 *) c->hdr;
    struct mt7921_iqb2_hdr *h = (struct mt7921_iqb2_hdr *) (t + MT7925_PUMP_IQB2_OFS);
    const u32 mux[1][5] = { OP_RMW(DFE_MUX, DFE_MUX_MASK, DFE_MUX_VAL(bw_cfg[q->bw_idx].rate)) };
    u32 n = 0;

    memset(c, 0, sizeof(*c));
    c->magic = MT7925_PUMP_MAGIC;
    c->version = MT7925_PUMP_VERSION;
    c->gen = gen;
    c->alloc_fn = FW_EVT_ALLOC;
    c->send_fn = FW_EVT_SEND;
    c->send_port = FW_EVT_PORT;
    c->timer_reg = MCU_TIMER_US;
    c->cr0 = ICAP_CR0;
    c->ctl_idle = ICAP_IDLE;
    c->ctl_arm = ICAP_ARM;
    c->ctl_trigger = ICAP_TRIGGER;
    c->busy_mask = ICAP_BUSY;
    c->snap_timeout_us = SNAP_TIMEOUT_US;
    c->grant_reg = GRANT_REG;
    c->win_addr = POOL_CPU + POOL_OFF;
    c->win_beats = POOL_BEATS;
    c->linear = 1;
    c->evict_addr = POOL_CPU + POOL_EVICT_AT;
    c->evict_len = POOL_EVICT;
    c->half_words = HALF_WORDS;
    c->chunk = CHUNK;
    c->burst = BURST;
    c->alloc_us = ALLOC_US;

    /* An event as the firmware's own; the pump fills the length and the per-record fields */
    t[UNI_EID - RXD_LEN] = IQ_EID;
    t[UNI_OPTION - RXD_LEN] = 6;
    t[UNI_EXT_EID - RXD_LEN] = IQ_EXT_EID;
    h->magic = cpu_to_le32(MT7921_IQB2_MAGIC);
    h->version = cpu_to_le16(MT7921_IQB2_VERSION);
    h->hdr_len = cpu_to_le16(IQB2_LEN);
    /* The ring holds a snapshot, the cursor the byte offset into it */
    h->flags = cpu_to_le32(bw_cfg[q->bw_idx].fmt << 4 | MT7921_IQ_RING_CODE(SNAP_BYTES) << MT7921_IQ_RING_SHIFT);

    ops_add(c, &n, arm_pre, ARRAY_SIZE(arm_pre));
    ops_add(c, &n, mux, 1);
    ops_add(c, &n, arm_post, ARRAY_SIZE(arm_post));
    if (q->rxgain) {
        const u32 force[RXGAIN_OPS][5] = { OP_WR(RXGAIN_REG(0), q->rxgain), OP_WR(RXGAIN_REG(1), q->rxgain) };

        ops_add(c, &n, force, RXGAIN_OPS);
    }
    c->setup_n = n;
    ops_add(c, &n, disarm, ARRAY_SIZE(disarm));
    if (q->rxgain) {
        const u32 release[RXGAIN_OPS][5] = { OP_WR(RXGAIN_REG(0), 0), OP_WR(RXGAIN_REG(1), 0) };

        ops_add(c, &n, release, RXGAIN_OPS);
    }
    c->teardown_n = n - c->setup_n;
    ops_add(c, &n, pool_pre, ARRAY_SIZE(pool_pre));
    c->pre_n = ARRAY_SIZE(pool_pre);
    ops_add(c, &n, pool_post, ARRAY_SIZE(pool_post));
    c->post_n = ARRAY_SIZE(pool_post);
}

/* Queues a burst.  Caller holds mdev->mcu.mutex, then q->ctl: the command goes straight to
 * the driver's send, past the wrapper. */
static int burst_send(struct mt7925_iq *q) {
    struct mt76_dev *mdev = q->mdev;
    __le32 arg = 0;
    struct sk_buff *skb;
    int r, seq;

    if ((skb = mt76_mcu_msg_alloc(mdev, &arg, sizeof(arg))) == NULL)
        return -ENOMEM;
    r = q->hook->orig_mcu->mcu_skb_send_msg(mdev, skb,
            __MCU_CMD_FIELD_UNI | FIELD_PREP(__MCU_CMD_FIELD_ID, PUMP_UNI_ID), &seq);
    if (r < 0)
        return r;
    if (q->outstanding++ == 0)
        q->sent_at = now_ms();
    q->st.bursts++;
    return 0;
}

/* The pump's counters since the last look; a capture that timed out gets the next burst to
 * arm again.  Caller holds q->ctl. */
static void pump_check(struct mt7925_iq *q) {
    u32 v[offsetof(struct mt7925_pump_ctl, armed) / 4 + 1];

    if (xfer(q, true, MT7925_PUMP_CTL_ADDR, v, sizeof(v)) < 0)
        return;
    q->st.pump_state = v[offsetof(struct mt7925_pump_ctl, state) / 4];
    q->st.pump_snaps = v[offsetof(struct mt7925_pump_ctl, snaps) / 4];
    q->st.pump_timeouts = v[offsetof(struct mt7925_pump_ctl, timeouts) / 4];
    q->st.pump_nobuf = v[offsetof(struct mt7925_pump_ctl, nobuf) / 4];
    if (q->st.pump_state == MT7925_PUMP_BADCTL) {
        dev_warn(q->mdev->dev, "iq: the pump refused its control block\n");
        q->ended = true;
    }
    if (q->st.pump_timeouts != q->timeouts) {
        dev_warn(q->mdev->dev, "iq: %u captures timed out, arming again\n",
                q->st.pump_timeouts - q->timeouts);
        q->timeouts = q->st.pump_timeouts;
        wr32(q, CTL_FIELD(armed), 0);
    }
    if (q->st.pump_nobuf != q->nobuf) {
        dev_warn(q->mdev->dev, "iq: %u snapshots cut short, no firmware buffers\n",
                q->st.pump_nobuf - q->nobuf);
        q->nobuf = q->st.pump_nobuf;
    }
}

static void deadline(struct timespec *ts, unsigned int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long) (ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

/* Ends the stream, disarms the capture and gives the command back to the firmware.  Caller
 * holds mdev->mcu.mutex, then q->ctl. */
static void pump_stop(struct mt7925_iq *q) {
    struct timespec ts;
    u32 state, gen;

    if (!q->live)
        return;
    q->stopping = true;
    deadline(&ts, STOP_TIMEOUT_MS);
    /* Queued bursts end at once; one more command runs the teardown if none is queued */
    if (wr32(q, CTL_FIELD(stop), MT7925_PUMP_STOP_ALL) == 0 && burst_send(q) == 0)
        while (q->outstanding > 0 && pthread_cond_timedwait(&q->answered, &q->ctl, &ts) == 0)
            ;
    if (q->outstanding > 0) {
        dev_warn(q->mdev->dev, "iq: pump did not stop\n");
    } else {
        if (rd32(q, CTL_FIELD(state), &state) == 0 && state != MT7925_PUMP_STOPPED)
            dev_warn(q->mdev->dev, "iq: pump stopped in state %u\n", state);
        /* A command the firmware still had would reach the stock handler with the pump's
         * argument, so the swap waits for every answer */
        uni_swap(q, false);
    }
    pump_check(q);
    pthread_mutex_lock(&q->ring_lock);
    if (rd32(q, CTL_FIELD(gen), &gen) == 0 && (s32) (gen - q->next_gen) > 0)
        q->next_gen = gen;
    else
        q->next_gen += 1u << 20;
    pthread_mutex_unlock(&q->ring_lock);
    q->live = false;
    q->stopping = false;
    q->outstanding = 0;
}

/* A new stream at q->bw_idx.  Caller holds mdev->mcu.mutex, then q->ctl. */
static int pump_start(struct mt7925_iq *q) {
    u32 gen;
    int r;

    pthread_mutex_lock(&q->ring_lock);
    gen = q->gen_base = q->next_gen;
    pthread_mutex_unlock(&q->ring_lock);
    ctl_fill(q, gen);
    q->timeouts = q->nobuf = 0;
    q->outstanding = 0;
    /* A chip reset reloads the firmware, so the pump goes in every time */
    if ((r = load(q, PUMP_CODE, mt7925_pump_code, sizeof(mt7925_pump_code))) < 0) {
        dev_warn(q->mdev->dev, "iq: unable to load the pump (%d)\n", r);
        return r;
    }
    if ((r = xfer(q, false, MT7925_PUMP_CTL_ADDR, &q->c, sizeof(q->c))) < 0 ||
            (r = uni_swap(q, true)) < 0)
        return r;
    if ((r = burst_send(q)) < 0) {
        uni_swap(q, false);
        return r;
    }
    q->live = true;
    return 0;
}

static struct mt7925_iq *hook_get(struct mt76_dev *mdev, struct hook **hp) {
    struct mt7925_iq *q = NULL;

    *hp = NULL;
    pthread_mutex_lock(&hooks_lock);
    for (int i = 0; i < MAX_HOOKS; i++) {
        if (hooks[i].mdev != mdev)
            continue;
        *hp = &hooks[i];
        if ((q = hooks[i].q) != NULL)
            atomic_inc(&q->users);
        break;
    }
    pthread_mutex_unlock(&hooks_lock);
    return q;
}

/* len bytes at ring position pos from the skb at off.  Caller holds ring_lock. */
static int ring_put(struct mt7925_iq *q, uint64_t pos, const struct sk_buff *skb, int off, u32 len) {
    while (len > 0) {
        u32 at = (u32) (pos & (RING_BYTES - 1)), n = len < RING_BYTES - at ? len : RING_BYTES - at;

        if (skb_copy_bits(skb, off, q->ring + at, (int) n) < 0)
            return -EFAULT;
        pos += n;
        off += (int) n;
        len -= n;
    }
    return 0;
}

/* The firmware's answer to a burst; false for everything else */
static bool rsp_rx(struct mt7925_iq *q, struct sk_buff *skb) {
    u8 hdr[UNI_DATA + 2];

    if (skb->len < sizeof(hdr) || skb_copy_bits(skb, 0, hdr, sizeof(hdr)) < 0 ||
            get_unaligned_le32(hdr) >> 27 != PKT_TYPE_RX_EVENT || hdr[UNI_EID] != RSP_EID ||
            get_unaligned_le16(hdr + UNI_DATA) != PUMP_UNI_ID)
        return false;
    pthread_mutex_lock(&q->ctl);
    if (q->outstanding > 0 && --q->outstanding > 0)
        q->sent_at = now_ms();
    q->check = true;
    pthread_cond_broadcast(&q->answered);
    pthread_mutex_unlock(&q->ctl);
    return true;
}

/* Whether an RX frame wrote over any of a record's plen sample bytes at off */
static bool clobbered(const struct sk_buff *skb, int off, u32 plen) {
    __le32 w[CHUNK / 4];

    if (plen > sizeof(w) || skb_copy_bits(skb, off, w, (int) plen) < 0)
        return true;
    for (u32 i = 0; i < plen / 4; i++)
        if (le32_to_cpu(w[i]) & SAMPLE_PAD)
            return true;
    return false;
}

/* An IQB2 record into the ring, or a burst's answer; false for everything else */
static bool iq_rx(struct mt7925_iq *q, struct sk_buff *skb) {
    u8 hdr[UNI_DATA + IQB2_LEN];
    const struct mt7921_iqb2_hdr *h = (const struct mt7921_iqb2_hdr *) (hdr + UNI_DATA);
    u32 plen, gen, rec;

    if (skb->len < sizeof(hdr) || skb_copy_bits(skb, 0, hdr, sizeof(hdr)) < 0 ||
            get_unaligned_le32(hdr) >> 27 != PKT_TYPE_RX_EVENT || hdr[UNI_EID] != IQ_EID ||
            hdr[UNI_EXT_EID] != IQ_EXT_EID || le32_to_cpu(h->magic) != MT7921_IQB2_MAGIC)
        return rsp_rx(q, skb);

    plen = le32_to_cpu(h->payload_len);
    gen = le32_to_cpu(h->generation);
    rec = IQB2_LEN + plen;
    pthread_mutex_lock(&q->ring_lock);
    if (le16_to_cpu(h->version) != MT7921_IQB2_VERSION || le16_to_cpu(h->hdr_len) != IQB2_LEN ||
            plen > CHUNK || (plen & 7) || UNI_DATA + rec > skb->len) {
        q->st.bad++;
    } else if (plen == 0 || (s32) (gen - q->gen_base) < 0) {
        /* A burst's end, or from before the last tune */
    } else if (clobbered(skb, UNI_DATA + IQB2_LEN, plen)) {
        /* The decoder sees it as a sequence gap */
        q->st.clobbered++;
    } else if (RING_BYTES - (q->head - q->tail) < rec) {
        q->st.drops++;
    } else if (ring_put(q, q->head, skb, UNI_DATA, rec) < 0) {
        q->st.bad++;
    } else {
        q->head += rec;
        q->st.records++;
        q->st.bytes += plen;
        if ((s32) (gen + 1 - q->next_gen) > 0)
            q->next_gen = gen + 1;
    }
    pthread_mutex_unlock(&q->ring_lock);
    return true;
}

static void hook_rx_skb(struct mt76_dev *mdev, enum mt76_rxq_id qid, struct sk_buff *skb, u32 *info) {
    struct hook *h;
    struct mt7925_iq *q = hook_get(mdev, &h);
    bool taken = false;

    if (q != NULL) {
        taken = iq_rx(q, skb);
        atomic_dec(&q->users);
    }
    if (taken)
        dev_kfree_skb(skb);
    else
        h->orig_drv->rx_skb(mdev, qid, skb, info);
}

/* Passes through; the wrapper being gone tells the worker the chip was reset */
static int hook_mcu_send(struct mt76_dev *mdev, struct sk_buff *skb, int cmd, int *seq) {
    struct hook *h;
    struct mt7925_iq *q = hook_get(mdev, &h);

    if (q != NULL)
        atomic_dec(&q->users);
    return h->orig_mcu->mcu_skb_send_msg(mdev, skb, cmd, seq);
}

static struct hook *hook_install(struct mt7925_iq *q) {
    struct mt76_dev *mdev = q->mdev;
    struct hook *h = NULL;

    pthread_mutex_lock(&hooks_lock);
    for (int i = 0; i < MAX_HOOKS && h == NULL; i++)
        if (hooks[i].mdev == mdev)
            h = &hooks[i];
    for (int i = 0; i < MAX_HOOKS && h == NULL; i++)
        if (hooks[i].mdev == NULL)
            h = &hooks[i];
    if (h != NULL) {
        h->mdev = mdev;
        if (mdev->drv != &h->drv) {
            h->orig_drv = mdev->drv;
            h->drv = *mdev->drv;
            h->drv.rx_skb = hook_rx_skb;
        }
        if (mdev->mcu_ops != &h->mcu) {
            h->orig_mcu = mdev->mcu_ops;
            h->mcu = *mdev->mcu_ops;
            h->mcu.mcu_skb_send_msg = hook_mcu_send;
        }
        h->q = q;
        mdev->drv = &h->drv;
        mdev->mcu_ops = &h->mcu;
    }
    pthread_mutex_unlock(&hooks_lock);
    return h;
}

static void hook_remove(struct mt7925_iq *q) {
    struct hook *h = q->hook;

    pthread_mutex_lock(&hooks_lock);
    h->q = NULL;
    if (q->mdev->drv == &h->drv)
        q->mdev->drv = h->orig_drv;
    if (q->mdev->mcu_ops == &h->mcu)
        q->mdev->mcu_ops = h->orig_mcu;
    pthread_mutex_unlock(&hooks_lock);
    while (atomic_read(&q->users) > 0)
        usleep(1000);
}

/* Keeps bursts queued, notices when the pump stops answering and restarts it */
static void *worker(void *arg) {
    struct mt7925_iq *q = arg;

    while (q->worker_run) {
        struct timespec ts;
        bool act;

        pthread_mutex_lock(&q->ctl);
        if (!q->live || q->outstanding >= MAX_OUT) {
            deadline(&ts, POLL_MS);
            pthread_cond_timedwait(&q->answered, &q->ctl, &ts);
        }
        if (!uwifi_alive(q->dev))
            q->ended = true;
        if (!q->stopping) {
            /* A chip reset brings the driver's own MCU ops back, and loses the pump */
            if (q->mdev->mcu_ops != &q->hook->mcu) {
                q->live = false;
                hook_install(q);
                q->restart_at = now_ms() + RESTART_DELAY_MS;
            }
            if (q->live && q->outstanding > 0 && now_ms() - q->sent_at > BURST_TIMEOUT_MS) {
                dev_warn(q->mdev->dev, "iq: pump stopped answering\n");
                q->live = false;
                q->restart_at = now_ms() + RESTART_DELAY_MS;
            }
        }
        act = !q->ended && !q->stopping && ((q->live && q->outstanding < MAX_OUT) ||
                (q->want && !q->live && now_ms() >= q->restart_at));
        pthread_mutex_unlock(&q->ctl);
        if (!act)
            continue;

        mutex_lock(&q->mdev->mcu.mutex);
        pthread_mutex_lock(&q->ctl);
        if (q->ended || q->stopping) {
            /* Changed while the lock was dropped */
        } else if (q->live && q->outstanding < MAX_OUT) {
            if (q->check) {
                q->check = false;
                pump_check(q);
            }
            if (!q->ended && burst_send(q) < 0) {
                q->live = false;
                q->restart_at = now_ms() + RESTART_DELAY_MS;
            }
        } else if (q->want && !q->live && now_ms() >= q->restart_at) {
            if (pump_start(q) == 0) {
                q->fails = 0;
                q->st.restarts++;
            } else if (++q->fails >= MAX_START_FAILS) {
                q->ended = true;
            }
            q->restart_at = now_ms() + RESTART_DELAY_MS;
        }
        pthread_mutex_unlock(&q->ctl);
        mutex_unlock(&q->mdev->mcu.mutex);
    }
    return NULL;
}

int mt7925_iq_list(struct mt7925_iq_info *out, int max) {
    struct uwifi_dev_info *list;
    size_t n, i;
    int count = 0;

    if (uwifi_list(&list, &n) < 0)
        return 0;
    for (i = 0; i < n && count < max; i++) {
        if (strcmp(list[i].driver, "mt7925u") != 0)
            continue;
        snprintf(out[count].name, sizeof(out[count].name), "%s", list[i].ifname);
        snprintf(out[count].hardware, sizeof(out[count].hardware), "%s (%04x:%04x) %s",
                list[i].driver, list[i].vid, list[i].pid, list[i].speed);
        count++;
    }
    free(list);
    return count;
}

/* Frames still arrive in monitor mode alongside the stream; not used */
static void host_rx(void *ctx, struct ieee80211_hw *hw, const struct ieee80211_rx_status *status,
        const u8 *data, size_t len) {
    (void) ctx; (void) hw; (void) status; (void) data; (void) len;
}

struct mt7925_iq *mt7925_iq_open(const char *name, char *err, size_t errlen) {
    struct mt7925_iq *q = calloc(1, sizeof(*q));
    const char *fw;
    int r;

    if (q == NULL || (q->ring = malloc(RING_BYTES)) == NULL) {
        snprintf(err, errlen, "out of memory");
        if (q != NULL)
            free(q);
        return NULL;
    }
    pthread_mutex_init(&q->ctl, NULL);
    pthread_cond_init(&q->answered, NULL);
    pthread_mutex_init(&q->ring_lock, NULL);
    atomic_set(&q->users, 0);
    q->next_gen = 1;

    if ((q->dev = uwifi_open_ifname(name, OPEN_TIMEOUT_MS)) == NULL) {
        snprintf(err, errlen, "unable to open, or the firmware didn't load");
        goto fail;
    }
    q->mdev = uwifi_hw(q->dev)->priv;
    fw = uwifi_hw(q->dev)->wiphy->fw_version;
    if (strstr(fw, FW_BUILD) == NULL) {
        snprintf(err, errlen, "firmware %s isn't the build the I/Q stream is mapped on (%s)", fw, FW_BUILD);
        goto fail;
    }
    if ((r = uwifi_start_monitor(q->dev, host_rx, q)) < 0) {
        snprintf(err, errlen, "monitor mode failed: %s", strerror(-r));
        goto fail;
    }
    if ((r = uni_swap(q, false)) < 0) {
        snprintf(err, errlen, "the firmware's command table isn't as mapped: %s", strerror(-r));
        goto fail;
    }
    if ((q->hook = hook_install(q)) == NULL) {
        snprintf(err, errlen, "too many open streams");
        goto fail;
    }
    q->worker_run = true;
    if (pthread_create(&q->worker, NULL, worker, q) != 0) {
        snprintf(err, errlen, "unable to start the stream thread");
        goto fail;
    }
    q->worker_started = true;
    return q;

fail:
    mt7925_iq_close(q);
    return NULL;
}

void mt7925_iq_close(struct mt7925_iq *q) {
    if (q == NULL)
        return;
    if (q->worker_started) {
        q->worker_run = false;
        pthread_join(q->worker, NULL);
    }
    if (q->hook != NULL) {
        mutex_lock(&q->mdev->mcu.mutex);
        pthread_mutex_lock(&q->ctl);
        q->want = false;
        pump_stop(q);
        pthread_mutex_unlock(&q->ctl);
        mutex_unlock(&q->mdev->mcu.mutex);
        hook_remove(q);
    }
    if (q->dev != NULL)
        uwifi_close(q->dev);
    pthread_mutex_destroy(&q->ctl);
    pthread_cond_destroy(&q->answered);
    pthread_mutex_destroy(&q->ring_lock);
    free(q->ring);
    free(q);
}

const uint8_t *mt7925_iq_mac(struct mt7925_iq *q) {
    return uwifi_mac(q->dev);
}

bool mt7925_iq_superspeed(struct mt7925_iq *q) {
    const char *speed = uwifi_info(q->dev)->speed;

    return strcmp(speed, "5Gbps") == 0 || strcmp(speed, "10Gbps") == 0;
}

bool mt7925_iq_alive(struct mt7925_iq *q) {
    return uwifi_alive(q->dev) && !q->ended;
}

/* The stock firmware tunes, so only real channels: the 20 MHz channels and the 40, 80 and
 * 160 MHz blocks, by their center */
static int find_chan(struct mt7925_iq *q, unsigned int freq_khz, unsigned int bw_mhz, struct uwifi_chan *out) {
    enum nl80211_chan_width w = bw_mhz == 20 ? NL80211_CHAN_WIDTH_20 :
        bw_mhz == 40 ? NL80211_CHAN_WIDTH_40 : bw_mhz == 80 ? NL80211_CHAN_WIDTH_80 :
        NL80211_CHAN_WIDTH_160;
    struct uwifi_chan *list;
    size_t n;
    int r = -ERANGE;

    if (uwifi_hop_list(q->dev, ~0u, 1u << w, &list, &n) < 0)
        return -ENOMEM;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned int) list[i].center_freq1 * 1000 == freq_khz) {
            *out = list[i];
            r = 0;
            break;
        }
    }
    free(list);
    return r;
}

int mt7925_iq_gain(struct mt7925_iq *q, bool hold, unsigned int lna, unsigned int lpf) {
    if (hold && (lna > RXGAIN_LNA_MAX || lpf > RXGAIN_LPF_MAX))
        return -EINVAL;
    pthread_mutex_lock(&q->ctl);
    q->rxgain = hold ? RXGAIN_FORCE(lna, lpf) : 0;
    pthread_mutex_unlock(&q->ctl);
    return 0;
}

int mt7925_iq_tune(struct mt7925_iq *q, unsigned int freq_khz, unsigned int bw_mhz,
        unsigned int *got_khz, unsigned int *gen) {
    unsigned int bw_idx = bw_mhz == 20 ? 0 : bw_mhz == 40 ? 1 : bw_mhz == 80 ? 2 : 3;
    struct uwifi_chan chan;
    int r;

    if (bw_mhz != 20 && bw_mhz != 40 && bw_mhz != 80 && bw_mhz != 160)
        return -EINVAL;
    if (q->ended)
        return -EIO;
    if ((r = find_chan(q, freq_khz, bw_mhz, &chan)) < 0)
        return r;

    mutex_lock(&q->mdev->mcu.mutex);
    pthread_mutex_lock(&q->ctl);
    q->want = false;
    pump_stop(q);
    /* What the old channel left in flight; its later records are dropped by generation */
    pthread_mutex_lock(&q->ring_lock);
    q->tail = q->head;
    q->gen_base = q->next_gen;
    pthread_mutex_unlock(&q->ring_lock);
    pthread_mutex_unlock(&q->ctl);
    mutex_unlock(&q->mdev->mcu.mutex);

    if ((r = uwifi_set_chan(q->dev, &chan)) < 0) {
        dev_warn(q->mdev->dev, "iq: unable to set the channel (%d)\n", r);
        return r;
    }

    mutex_lock(&q->mdev->mcu.mutex);
    pthread_mutex_lock(&q->ctl);
    q->bw_idx = bw_idx;
    q->want = true;
    q->fails = 0;
    if ((r = pump_start(q)) < 0)
        dev_warn(q->mdev->dev, "iq: unable to start the pump (%d)\n", r);
    q->restart_at = now_ms() + RESTART_DELAY_MS;
    *gen = q->gen_base;
    pthread_mutex_unlock(&q->ctl);
    mutex_unlock(&q->mdev->mcu.mutex);

    *got_khz = chan.center_freq1 * 1000;
    return r;
}

static void ring_get(struct mt7925_iq *q, uint64_t pos, void *out, u32 len) {
    u32 at = (u32) (pos & (RING_BYTES - 1)), first = len < RING_BYTES - at ? len : RING_BYTES - at;

    memcpy(out, q->ring + at, first);
    memcpy((u8 *) out + first, q->ring, len - first);
}

ssize_t mt7925_iq_read(struct mt7925_iq *q, void *buf, size_t len) {
    size_t n = 0;

    pthread_mutex_lock(&q->ring_lock);
    while (q->head - q->tail >= IQB2_LEN) {
        struct mt7921_iqb2_hdr h;
        u32 rec;

        ring_get(q, q->tail, &h, IQB2_LEN);
        rec = le16_to_cpu(h.hdr_len) + le32_to_cpu(h.payload_len);
        if (n + rec > len)
            break;
        ring_get(q, q->tail, (u8 *) buf + n, rec);
        q->tail += rec;
        n += rec;
    }
    pthread_mutex_unlock(&q->ring_lock);
    if (n == 0 && !mt7925_iq_alive(q))
        return -1;
    return (ssize_t) n;
}

void mt7925_iq_stats(struct mt7925_iq *q, struct mt7925_iq_stats *st) {
    pthread_mutex_lock(&q->ctl);
    pthread_mutex_lock(&q->ring_lock);
    *st = q->st;
    pthread_mutex_unlock(&q->ring_lock);
    pthread_mutex_unlock(&q->ctl);
}
