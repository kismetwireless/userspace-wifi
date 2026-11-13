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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iqdev.h"
#include "mt7921_host.h"
#include "mt7925_host.h"

#define MAX_LIST 16

struct iqdev {
    enum iqdev_chip chip;
    union {
        struct mt7921_iq *mt7921;
        struct mt7925_iq *mt7925;
    };
};

/* Our name prefix and the driver's, by chip */
static const struct {
    const char *sdr, *hw, *chip;
} chips[] = {
    [IQDEV_MT7921] = { "mt7921sdr-", "mt7921-", "MT7921AU" },
    [IQDEV_MT7925] = { "mt7925sdr-", "mt7925-", "MT7925" },
};

/* Both drivers share the one uwifi instance the MT7921 side initializes */
int iqdev_init(const char *const *fw_dirs, size_t n, iqdev_log_t log, void *ctx, int max_level) {
    return mt7921_iq_init(fw_dirs, n, log, ctx, max_level);
}

enum iqdev_chip iqdev_chip_of(const char *name) {
    for (size_t c = 0; c < sizeof(chips) / sizeof(chips[0]); c++) {
        size_t plen = strlen(chips[c].sdr);
        const char *idx = name + plen;

        if (strncmp(name, chips[c].sdr, plen) == 0 && *idx != 0 &&
                strspn(idx, "0123456789") == strlen(idx))
            return (enum iqdev_chip) c;
    }
    return IQDEV_NONE;
}

const char *iqdev_chip_name(enum iqdev_chip chip) {
    return chip < IQDEV_NONE ? chips[chip].chip : "unknown";
}

/* The driver's mt7921-N to our mt7921sdr-N */
static void sdr_name(enum iqdev_chip chip, const char *hw, char *out, size_t len) {
    size_t plen = strlen(chips[chip].hw);

    if (strncmp(hw, chips[chip].hw, plen) == 0)
        hw += plen;
    /* An interface index; the bound keeps the compiler from counting the whole info struct */
    snprintf(out, len, "%s%.24s", chips[chip].sdr, hw);
}

int iqdev_list(struct iqdev_info *list, int max) {
    struct mt7921_iq_info l21[MAX_LIST];
    struct mt7925_iq_info l25[MAX_LIST];
    int n21 = mt7921_iq_list(l21, MAX_LIST), n25 = mt7925_iq_list(l25, MAX_LIST), count = 0;

    for (int i = 0; i < n21 && count < max; i++, count++) {
        list[count].chip = IQDEV_MT7921;
        sdr_name(IQDEV_MT7921, l21[i].name, list[count].name, sizeof(list[count].name));
        snprintf(list[count].hardware, sizeof(list[count].hardware), "%s", l21[i].hardware);
    }
    for (int i = 0; i < n25 && count < max; i++, count++) {
        list[count].chip = IQDEV_MT7925;
        sdr_name(IQDEV_MT7925, l25[i].name, list[count].name, sizeof(list[count].name));
        snprintf(list[count].hardware, sizeof(list[count].hardware), "%s", l25[i].hardware);
    }
    return count;
}

struct iqdev *iqdev_open(const char *name, char *err, size_t errlen) {
    enum iqdev_chip chip = iqdev_chip_of(name);
    char hw[IQDEV_NAME_LEN];
    struct iqdev *d;

    if (chip == IQDEV_NONE) {
        snprintf(err, errlen, "not an I/Q adapter name");
        return NULL;
    }
    if ((d = calloc(1, sizeof(*d))) == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    snprintf(hw, sizeof(hw), "%s%s", chips[chip].hw, name + strlen(chips[chip].sdr));
    d->chip = chip;
    if ((chip == IQDEV_MT7921 && (d->mt7921 = mt7921_iq_open(hw, err, errlen)) == NULL) ||
            (chip == IQDEV_MT7925 && (d->mt7925 = mt7925_iq_open(hw, err, errlen)) == NULL)) {
        free(d);
        return NULL;
    }
    return d;
}

void iqdev_close(struct iqdev *d) {
    if (d == NULL)
        return;
    if (d->chip == IQDEV_MT7921)
        mt7921_iq_close(d->mt7921);
    else
        mt7925_iq_close(d->mt7925);
    free(d);
}

enum iqdev_chip iqdev_chip(const struct iqdev *d) {
    return d->chip;
}

const uint8_t *iqdev_mac(struct iqdev *d) {
    return d->chip == IQDEV_MT7921 ? mt7921_iq_mac(d->mt7921) : mt7925_iq_mac(d->mt7925);
}

bool iqdev_superspeed(struct iqdev *d) {
    return d->chip == IQDEV_MT7921 ? mt7921_iq_superspeed(d->mt7921) :
        mt7925_iq_superspeed(d->mt7925);
}

int iqdev_tune(struct iqdev *d, unsigned int freq_khz, unsigned int bw_mhz,
        unsigned int *got_khz, unsigned int *gen) {
    if (d->chip == IQDEV_MT7921)
        return mt7921_iq_tune(d->mt7921, freq_khz, bw_mhz, got_khz, gen);
    return mt7925_iq_tune(d->mt7925, freq_khz, bw_mhz, got_khz, gen);
}

int iqdev_gain(struct iqdev *d, bool hold, unsigned int lna, unsigned int lpf) {
    if (d->chip == IQDEV_MT7921)
        return mt7921_iq_gain(d->mt7921, hold, lna, lpf);
    return mt7925_iq_gain(d->mt7925, hold, lna, lpf);
}

ssize_t iqdev_read(struct iqdev *d, void *buf, size_t len) {
    if (d->chip == IQDEV_MT7921)
        return mt7921_iq_read(d->mt7921, buf, len);
    return mt7925_iq_read(d->mt7925, buf, len);
}
