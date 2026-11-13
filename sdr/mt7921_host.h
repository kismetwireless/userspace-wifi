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

#ifndef __USBWIFISDR_MT7921_HOST_H__
#define __USBWIFISDR_MT7921_HOST_H__

/* The MT7921AU's I/Q capture, run by the Mtk79ICAP mt76 driver in userspace.  This side is
 * built against the kernel emulation and keeps its types from the capture source. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MT7921_NAME_LEN 40
#define MT7921_HW_LEN 96

struct mt7921_iq;

struct mt7921_iq_info {
    char name[MT7921_NAME_LEN];
    char hardware[MT7921_HW_LEN];
};

/* Driver messages at or below max_level (kernel log levels) go to log */
typedef void (*mt7921_iq_log_t)(void *ctx, int level, const char *msg);

/* The firmware directories, searched in order; only the first call does anything */
int mt7921_iq_init(const char *const *fw_dirs, size_t n, mt7921_iq_log_t log, void *ctx, int max_level);

/* The attached MT7921AU adapters, by name such as mt7921-0 */
int mt7921_iq_list(struct mt7921_iq_info *list, int max);

/* Loads the firmware, brings up monitor mode and starts the stream; NULL with err set on
 * failure */
struct mt7921_iq *mt7921_iq_open(const char *name, char *err, size_t errlen);
void mt7921_iq_close(struct mt7921_iq *q);

const uint8_t *mt7921_iq_mac(struct mt7921_iq *q);
/* The adapter is on a USB 3 port, which the stream needs */
bool mt7921_iq_superspeed(struct mt7921_iq *q);
bool mt7921_iq_alive(struct mt7921_iq *q);

/* Tunes to the raster point nearest freq_khz at 20, 40 or 80 MHz wide; *got_khz is where it
 * went, and *gen the first stream generation there.  0 or a negative errno */
int mt7921_iq_tune(struct mt7921_iq *q, unsigned int freq_khz, unsigned int bw_mhz,
        unsigned int *got_khz, unsigned int *gen);

/* The chip's AGC, or the analog gain held at an LNA and LPF code */
int mt7921_iq_gain(struct mt7921_iq *q, bool hold, unsigned int lna, unsigned int lpf);

/* Whole stream records, without waiting: 0 when none are ready, -1 once the stream has
 * ended */
ssize_t mt7921_iq_read(struct mt7921_iq *q, void *buf, size_t len);

#endif
