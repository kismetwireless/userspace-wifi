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

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "iqradio.h"
#include "mt7921iq.h"

#define RAW_BYTES (1u << 20)
/* A stream that delivers nothing for this long has stalled */
#define STALL_S 2.0
#define RASTER_HZ 5e6
/* Channel 0 of each band; 2.4 GHz channels are 2407 + 5n MHz */
#define RASTER24_HZ 2407e6
#define RASTER5_HZ 5000e6
/* The MT7921AU's held gain takes effect after the stream has run under the AGC this long */
#define HOLD_SETTLE_S 0.03
/* Stream generations remembered as being from before a retune */
#define OLD_GENS 8

struct iqradio {
    struct iqdev *d;
    struct mt7921iq_dec *dec;
    uint8_t *raw;
    size_t raw_len;
    /* After a retune, records are dropped until one from a generation not seen before it;
     * the generation the retune reports may never arrive, if the driver restarts it for a
     * bad DMA alignment */
    uint32_t old_gens[OLD_GENS];
    int nold;
    bool waiting;
    unsigned int bw_mhz;
    double hz, rate;
    bool held;
    /* The held gain: IQRADIO_GAIN_DEFAULT, or dB above the band's lowest usable code */
    double gain_db;
    /* Samples still to drop from the start of a new stream */
    size_t skip;
    unsigned long losses;
};

/* The analog gain codes of a band under the hold (mt7921iq.h): the usable LNA codes, lna_db
 * apart, and the LPF codes on top of each, lpf_db apart, from the levels of the same stations
 * at each code.  The default hold puts the noise floor of a 20 MHz channel near -55 dBFS on
 * 5 GHz, as the AGC does when idle, and a step lower on 2.4 GHz, where Bluetooth and
 * neighbouring channels pile up and a hold can't duck them: lower still, the chain's own
 * noise (-64 dBFS) buries the band's and the stream fills with zero runs (10-18% at the
 * lowest codes, 1-2% here); higher, nearby stations clip.  2.4 GHz: LNA 6 and 7 repeat 5,
 * LNA 0 is nearly off. */
struct gain_map {
    unsigned int lna_min, lna_max, lpf_max;
    double lna_db, lpf_db;
    unsigned int def_lna, def_lpf;
};

static const struct gain_map gain_24 = { 1, 5, 8, 5.2, 1.6, 3, 2 };
static const struct gain_map gain_5 = { 2, 5, 8, 6.0, 2.0, 5, 6 };
/* The MT7925's PHY force (both bands, measured on 5 GHz): its snapshots' floor is -37 dBFS
 * at any code, so the default is the highest that leaves nearby stations under the rail,
 * about 10 dB over the AGC's own level */
static const struct gain_map gain_7925 = { 0, 4, 9, 6.0, 2.0, 4, 0 };

static const struct gain_map *gain_map_of(const struct iqradio *r, double hz) {
    if (iqdev_chip(r->d) == IQDEV_MT7925)
        return &gain_7925;
    return hz != 0 && hz < 3e9 ? &gain_24 : &gain_5;
}

/* Tunable centers for each width: 2.4 GHz, then 5 GHz, and up through 6 GHz on the MT7925
 * (hi6).  160 MHz is the MT7925's only, and not on 2.4 GHz. */
static const struct {
    unsigned int bw;
    double lo24, hi24, lo5, hi5, hi6;
} centers[] = {
    { 20, 2412e6, 2472e6, 5005e6, 5995e6, 7115e6 },
    { 40, 2422e6, 2462e6, 5015e6, 5985e6, 7085e6 },
    { 80, 2422e6, 2462e6, 5035e6, 5965e6, 7025e6 },
    { 160, 0, 0, 5075e6, 5925e6, 6985e6 },
};

static double now_s(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

bool iqradio_center_ok(enum iqdev_chip chip, double hz, unsigned int bw_mhz) {
    size_t i = bw_mhz == 20 ? 0 : bw_mhz == 40 ? 1 : bw_mhz == 80 ? 2 : bw_mhz == 160 ? 3 : 4;
    double base = hz < 3.5e9 ? RASTER24_HZ : RASTER5_HZ, steps;

    if (i >= sizeof(centers) / sizeof(centers[0]) || chip == IQDEV_NONE || (chip == IQDEV_MT7921 && bw_mhz > 80))
        return false;
    steps = (hz - base) / RASTER_HZ;
    if (fabs(steps - round(steps)) > 1e-6)
        return false;
    if (hz < 3.5e9)
        return hz >= centers[i].lo24 && hz <= centers[i].hi24;
    return hz >= centers[i].lo5 && hz <= (chip == IQDEV_MT7925 ? centers[i].hi6 : centers[i].hi5);
}

static uint32_t le32(const uint8_t *b) {
    return (uint32_t) b[0] | (uint32_t) b[1] << 8 | (uint32_t) b[2] << 16 | (uint32_t) b[3] << 24;
}

static bool is_old(const struct iqradio *r, uint32_t gen) {
    for (int i = 0; i < r->nold; i++)
        if (r->old_gens[i] == gen)
            return true;
    return false;
}

static void note_gen(struct iqradio *r, uint32_t gen) {
    if (is_old(r, gen))
        return;
    if (r->nold == OLD_GENS) {
        memmove(r->old_gens, r->old_gens + 1, sizeof(r->old_gens[0]) * (OLD_GENS - 1));
        r->nold--;
    }
    r->old_gens[r->nold++] = gen;
}

/* Moves whole records along to the decoder, noting their generations; records from before a
 * retune, or all of them when discarding, are dropped */
static int feed(struct iqradio *r, bool discard) {
    size_t off = 0, start = 0;

    while (r->raw_len - off >= MT7921IQ_HDR_LEN) {
        const uint8_t *h = r->raw + off;
        uint32_t hdr_len = (uint32_t) h[6] | (uint32_t) h[7] << 8, gen = le32(h + 24);
        uint64_t rec = ((uint64_t) hdr_len + le32(h + 8) + 3) & ~(uint64_t) 3;

        if (le32(h) != MT7921IQ_IQB2_MAGIC || hdr_len < MT7921IQ_HDR_LEN || rec > RAW_BYTES)
            return -1;
        if (r->raw_len - off < rec)
            break;
        if (r->waiting && !discard && !is_old(r, gen)) {
            r->waiting = false;
            r->nold = 0;
        }
        if (discard || r->waiting)
            start = off + (size_t) rec;
        note_gen(r, gen);
        off += (size_t) rec;
    }
    if (off > start && mt7921iq_dec_push(r->dec, r->raw + start, off - start) < 0)
        return -1;
    memmove(r->raw, r->raw + off, r->raw_len - off);
    r->raw_len -= off;
    return 0;
}

/* Everything the driver holds from before now, dropped; -1 if the stream ended */
static int drain(struct iqradio *r) {
    ssize_t m;

    while ((m = iqdev_read(r->d, r->raw + r->raw_len, RAW_BYTES - r->raw_len)) > 0) {
        r->raw_len += (size_t) m;
        if (feed(r, true) < 0)
            r->raw_len = 0;
    }
    r->raw_len = 0;
    return m < 0 ? -1 : 0;
}

static int read_samples(struct iqradio *r, iqradio_sample_t *buf, size_t n) {
    double deadline = now_s() + STALL_S;
    struct mt7921iq_chunk info;
    size_t got = 0;

    while (got < n) {
        size_t k = mt7921iq_dec_read(r->dec, MT7921IQ_CF32, buf + got, n - got, &info);
        ssize_t m;

        if (k > 0) {
            if (info.flags & (MT7921IQ_F_SEQ_GAP | MT7921IQ_F_CURSOR | MT7921IQ_F_OVERRUN))
                r->losses++;
            got += k;
            continue;
        }
        if ((m = iqdev_read(r->d, r->raw + r->raw_len, RAW_BYTES - r->raw_len)) < 0)
            return -1;
        if (m == 0) {
            if (now_s() > deadline)
                return -1;
            usleep(200);
            continue;
        }
        r->raw_len += (size_t) m;
        if (feed(r, false) < 0) {
            mt7921iq_dec_reset(r->dec);
            r->raw_len = 0;
        }
    }
    return 0;
}

struct iqradio *iqradio_new(struct iqdev *d) {
    struct iqradio *r = calloc(1, sizeof(*r));

    if (r == NULL || (r->raw = malloc(RAW_BYTES)) == NULL || (r->dec = mt7921iq_dec_new()) == NULL) {
        if (r != NULL)
            free(r->raw);
        free(r);
        iqdev_close(d);
        return NULL;
    }
    r->d = d;
    return r;
}

void iqradio_free(struct iqradio *r) {
    if (r == NULL)
        return;
    iqdev_close(r->d);
    mt7921iq_dec_free(r->dec);
    free(r->raw);
    free(r);
}

/* The band's codes for the held gain: the LNA steps first, then the LPF on top */
static void gain_codes(const struct gain_map *m, double db, unsigned int *lna, unsigned int *lpf) {
    unsigned int steps;

    if (db < 0) {
        *lna = m->def_lna;
        *lpf = m->def_lpf;
        return;
    }
    steps = (unsigned int) (db / m->lna_db);
    *lna = m->lna_min + (steps > m->lna_max - m->lna_min ? m->lna_max - m->lna_min : steps);
    *lpf = (unsigned int) fmin(m->lpf_max, fmax(0, (db - (*lna - m->lna_min) * m->lna_db) / m->lpf_db));
}

/* Samples to drop while a hold settles; the MT7925's is set up with the stream */
static size_t settle(const struct iqradio *r) {
    if (!r->held || iqdev_chip(r->d) != IQDEV_MT7921)
        return 0;
    return (size_t) (HOLD_SETTLE_S * r->rate);
}

/* The hold at the band of hz; a hold outlives retunes, but each band has its own codes */
static int apply_gain(struct iqradio *r, double hz) {
    unsigned int lna, lpf;

    gain_codes(gain_map_of(r, hz), r->gain_db, &lna, &lpf);
    return iqdev_gain(r->d, true, lna, lpf);
}

int iqradio_tune(struct iqradio *r, double hz, unsigned int bw_mhz) {
    unsigned int gen, got_khz;
    int ret;

    if (!iqradio_center_ok(iqdev_chip(r->d), hz, bw_mhz))
        return -ERANGE;
    if (drain(r) < 0)
        return -EIO;
    /* Before the tune: the MT7925 sets its gain up with the stream */
    if (r->held && (ret = apply_gain(r, hz)) < 0)
        return ret;
    if ((ret = iqdev_tune(r->d, (unsigned int) (hz / 1e3 + 0.5), bw_mhz, &got_khz, &gen)) < 0)
        return ret;
    /* The driver rounds to its own raster */
    if (fabs(got_khz * 1e3 - hz) > 1e3)
        return -ERANGE;
    r->hz = hz;
    r->bw_mhz = bw_mhz;
    /* The MT7925 samples at twice the channel width */
    r->rate = bw_mhz * IQRADIO_RATE_UNIT * (iqdev_chip(r->d) == IQDEV_MT7925 ? 2 : 1);
    r->waiting = true;
    r->skip = settle(r);
    mt7921iq_dec_reset(r->dec);
    return 0;
}

double iqradio_rate(const struct iqradio *r) {
    return r->rate;
}

double iqradio_center(const struct iqradio *r) {
    return r->hz;
}

int iqradio_read(struct iqradio *r, iqradio_sample_t *buf, size_t n) {
    while (r->skip > 0) {
        size_t k = r->skip < n ? r->skip : n;

        if (read_samples(r, buf, k) < 0)
            return -1;
        r->skip -= k;
    }
    return read_samples(r, buf, n);
}

int iqradio_flush(struct iqradio *r) {
    /* Nothing from the new generation is buffered yet */
    if (r->waiting)
        return 0;
    if (drain(r) < 0)
        return -1;
    mt7921iq_dec_reset(r->dec);
    return 0;
}

double iqradio_gain_max(const struct iqradio *r, double hz) {
    const struct gain_map *m = gain_map_of(r, hz);

    return (m->lna_max - m->lna_min) * m->lna_db + m->lpf_max * m->lpf_db;
}

double iqradio_gain(const struct iqradio *r) {
    const struct gain_map *m;

    if (!r->held)
        return 0;
    if (r->gain_db >= 0)
        return r->gain_db;
    m = gain_map_of(r, r->hz);
    return (m->def_lna - m->lna_min) * m->lna_db + m->def_lpf * m->lpf_db;
}

unsigned long iqradio_losses(const struct iqradio *r) {
    return r->losses;
}

int iqradio_set_gain(struct iqradio *r, double db) {
    int ret;

    if (db == 0) {
        if ((ret = iqdev_gain(r->d, false, 0, 0)) < 0)
            return ret;
        r->held = false;
        return 0;
    }
    r->gain_db = db;
    if ((ret = apply_gain(r, r->hz)) < 0)
        return ret;
    r->held = true;
    if (r->rate > 0)
        r->skip = settle(r);
    return 0;
}
