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

#ifndef __USBWIFISDR_IQDEV_H__
#define __USBWIFISDR_IQDEV_H__

/* The I/Q adapters, whichever chip: names, opening, and the record stream.  An adapter is
 * named after its driver interface, "mt7921sdr-N" for the usbwifi source's "mt7921-N". */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IQDEV_NAME_LEN 48
#define IQDEV_HW_LEN 96

enum iqdev_chip {
    IQDEV_MT7921,
    IQDEV_MT7925,
    IQDEV_NONE,
};

struct iqdev;

struct iqdev_info {
    char name[IQDEV_NAME_LEN];
    char hardware[IQDEV_HW_LEN];
    enum iqdev_chip chip;
};

/* Driver messages at or below max_level (kernel log levels) go to log */
typedef void (*iqdev_log_t)(void *ctx, int level, const char *msg);

/* The firmware directories, searched in order; only the first call does anything */
int iqdev_init(const char *const *fw_dirs, size_t n, iqdev_log_t log, void *ctx, int max_level);

/* The chip an adapter name is for, IQDEV_NONE if it isn't one of ours */
enum iqdev_chip iqdev_chip_of(const char *name);
/* "MT7921AU", for messages */
const char *iqdev_chip_name(enum iqdev_chip chip);

/* The attached adapters */
int iqdev_list(struct iqdev_info *list, int max);

/* Loads the firmware and brings up monitor mode; NULL with err set on failure */
struct iqdev *iqdev_open(const char *name, char *err, size_t errlen);
void iqdev_close(struct iqdev *d);

enum iqdev_chip iqdev_chip(const struct iqdev *d);
const uint8_t *iqdev_mac(struct iqdev *d);
/* The adapter is on a USB 3 port, which the stream needs */
bool iqdev_superspeed(struct iqdev *d);

/* Tunes to freq_khz at 20, 40 or 80 MHz wide and (re)starts the stream; *got_khz is where it
 * went, and *gen the first stream generation there.  0 or a negative errno */
int iqdev_tune(struct iqdev *d, unsigned int freq_khz, unsigned int bw_mhz,
        unsigned int *got_khz, unsigned int *gen);
/* The chip's AGC, or the analog gain held at an LNA and LPF code (the MT7921AU's live, the
 * MT7925's from its next tune) */
int iqdev_gain(struct iqdev *d, bool hold, unsigned int lna, unsigned int lpf);
/* Whole IQB2 records, without waiting: 0 when none are ready, -1 once the stream has ended */
ssize_t iqdev_read(struct iqdev *d, void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
