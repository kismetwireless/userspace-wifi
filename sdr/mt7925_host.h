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

#ifndef __USBWIFISDR_MT7925_HOST_H__
#define __USBWIFISDR_MT7925_HOST_H__

/* The MT7925's I/Q capture.  The stock firmware runs the radio; a small pump (mt7925_pump/)
 * copied into MCU memory and entered through an unused UNI command takes bursts of capture
 * snapshots and sends them to the host as IQB2 records, the framing of the MT7921AU stream,
 * a generation per snapshot.  Built against the kernel emulation, like mt7921_host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MT7925_NAME_LEN 40
#define MT7925_HW_LEN 96

struct mt7925_iq;

struct mt7925_iq_info {
    char name[MT7925_NAME_LEN];
    char hardware[MT7925_HW_LEN];
};

struct mt7925_iq_stats {
    uint64_t records, bytes, drops, bad;
    /* Records an RX frame wrote over in the pool before the pump sent them */
    uint64_t clobbered;
    uint32_t restarts, bursts;
    /* From the pump, since the last tune */
    uint32_t pump_state, pump_snaps, pump_timeouts, pump_nobuf;
};

/* The attached MT7925 adapters, by name such as mt7925-0; uwifi must be initialized */
int mt7925_iq_list(struct mt7925_iq_info *list, int max);

/* Loads the firmware, brings up monitor mode and checks the firmware build; NULL with err
 * set on failure.  The stream starts at the first tune. */
struct mt7925_iq *mt7925_iq_open(const char *name, char *err, size_t errlen);
void mt7925_iq_close(struct mt7925_iq *q);

const uint8_t *mt7925_iq_mac(struct mt7925_iq *q);
bool mt7925_iq_superspeed(struct mt7925_iq *q);
bool mt7925_iq_alive(struct mt7925_iq *q);

/* Tunes to freq_khz at 20, 40, 80 or 160 MHz wide and (re)starts the stream; *gen is the first
 * snapshot's generation there.  0 or a negative errno */
int mt7925_iq_tune(struct mt7925_iq *q, unsigned int freq_khz, unsigned int bw_mhz,
        unsigned int *got_khz, unsigned int *gen);

/* Whole IQB2 records (header and payload, each padded to 4 bytes), without waiting: 0 when
 * none are ready, -1 once the stream has ended */
ssize_t mt7925_iq_read(struct mt7925_iq *q, void *buf, size_t len);

void mt7925_iq_stats(struct mt7925_iq *q, struct mt7925_iq_stats *st);

/* The receive gain held at the PHY's LNA code (0-4) and LPF code (0-9) on both chains, or
 * given to the chip's AGC with hold false; takes effect at the next tune */
int mt7925_iq_gain(struct mt7925_iq *q, bool hold, unsigned int lna, unsigned int lpf);

#endif
