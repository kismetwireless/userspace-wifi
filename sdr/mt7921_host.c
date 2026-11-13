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

#include "../common/uwifi.h"
#include "mt7921/iqstream_ioctl.h"
#include "mt7921_host.h"
#include "mt7961_fw.h"

/* The driver's firmware load and mac80211 bring-up */
#define OPEN_TIMEOUT_MS 15000
/* Scatter-gather entries offered, for the driver's 128 KiB RX URBs */
#define USB_SG_ENTRIES 64
/* The data queue's URBs: MT_RX_SG_MAX_SIZE pages each */
#define RX_URB_BYTES (32 * 4096)
#define RX_URBS_WANT 512
#define RX_URBS_MIN 16

struct mt7921_iq {
    struct uwifi_dev *dev;
    struct miscdevice *misc;
    struct inode inode;
    struct file file;
};

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static int init_result;
static const char *const *init_dirs;
static size_t init_ndirs;
static int init_level;
static mt7921_iq_log_t log_fn;
static void *log_ctx;

static void host_log(void *ctx, int level, const char *msg) {
    (void) ctx;
    if (log_fn != NULL)
        log_fn(log_ctx, level, msg);
}

static int fw_filter(const char *name, uint8_t **data, size_t *len) {
    char msg[192];

    if (log_fn != NULL && mt7961_fw_state(*data, *len) == MT7961_FW_STOCK) {
        snprintf(msg, sizeof(msg), "patching firmware %s for the I/Q stream", name);
        log_fn(log_ctx, 6, msg);
    }
    return mt7961_fw_filter(name, data, len);
}

/* RX URBs on the data queue: as many as the stream wants, within Linux's usbfs memory limit
 * (usbcore.usbfs_memory_mb, 16 MB by default, 0 for none), which every submitted transfer
 * counts against; past it, submits fail with ENOMEM.  A quarter is left for the MCU and TX
 * queues. */
static int rx_urb_count(void) {
    long limit_mb = 0;
    FILE *f = fopen("/sys/module/usbcore/parameters/usbfs_memory_mb", "r");
    long fit;
    char msg[192];

    if (f != NULL) {
        if (fscanf(f, "%ld", &limit_mb) != 1)
            limit_mb = 0;
        fclose(f);
    }
    if (limit_mb <= 0)
        return RX_URBS_WANT;
    fit = limit_mb * 1024 * 1024 / 4 * 3 / RX_URB_BYTES;
    if (fit >= RX_URBS_WANT)
        return RX_URBS_WANT;
    if (fit < RX_URBS_MIN)
        fit = RX_URBS_MIN;
    if (log_fn != NULL) {
        snprintf(msg, sizeof(msg), "usbfs memory is limited to %ld MB, so %ld RX buffers instead of %d; "
                "raise /sys/module/usbcore/parameters/usbfs_memory_mb to %d for fewer losses",
                limit_mb, fit, RX_URBS_WANT, RX_URBS_WANT * RX_URB_BYTES / 1024 / 1024 * 4 / 3 + 1);
        log_fn(log_ctx, 4, msg);
    }
    return (int) fit;
}

static void host_init(void) {
    /* Continuous 40 and 80 MHz streams, and USB buffering against host stalls */
    kc_param_set("iq_dense", 1);
    kc_param_set("rx_urbs", rx_urb_count());
    kc_usb_set_sg(USB_SG_ENTRIES);
    kc_firmware_set_filter(fw_filter);
    uwifi_set_log_cb(host_log, NULL);
    init_result = uwifi_init(init_dirs, init_ndirs, init_level);
}

int mt7921_iq_init(const char *const *fw_dirs, size_t n, mt7921_iq_log_t log, void *ctx, int max_level) {
    init_dirs = fw_dirs;
    init_ndirs = n;
    init_level = max_level;
    log_fn = log;
    log_ctx = ctx;
    pthread_once(&init_once, host_init);
    return init_result;
}

int mt7921_iq_list(struct mt7921_iq_info *out, int max) {
    struct uwifi_dev_info *list;
    size_t n, i;
    int count = 0;

    if (uwifi_list(&list, &n) < 0)
        return 0;
    for (i = 0; i < n && count < max; i++) {
        if (strcmp(list[i].driver, "mt7921u") != 0)
            continue;
        snprintf(out[count].name, sizeof(out[count].name), "%s", list[i].ifname);
        snprintf(out[count].hardware, sizeof(out[count].hardware), "%s (%04x:%04x) %s",
                list[i].driver, list[i].vid, list[i].pid, list[i].speed);
        count++;
    }
    free(list);
    return count;
}

/* Frames still arrive in monitor mode alongside the stream; the decoders see the same
 * channel, so they're not used */
static void host_rx(void *ctx, struct ieee80211_hw *hw, const struct ieee80211_rx_status *status,
        const u8 *data, size_t len) {
    (void) ctx; (void) hw; (void) status; (void) data; (void) len;
}

struct mt7921_iq *mt7921_iq_open(const char *name, char *err, size_t errlen) {
    struct mt7921_iq *q = calloc(1, sizeof(*q));
    char dev_name[64];
    int r;

    if (q == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if ((q->dev = uwifi_open_ifname(name, OPEN_TIMEOUT_MS)) == NULL) {
        snprintf(err, errlen, "unable to open, or the firmware didn't load");
        goto fail;
    }
    if ((r = uwifi_start_monitor(q->dev, host_rx, q)) < 0) {
        snprintf(err, errlen, "monitor mode failed: %s", strerror(-r));
        goto fail;
    }
    snprintf(dev_name, sizeof(dev_name), "mt7921_iq_%s", wiphy_name(uwifi_hw(q->dev)->wiphy));
    if ((q->misc = kc_misc_find(dev_name)) == NULL) {
        snprintf(err, errlen, "the driver has no I/Q stream");
        goto fail;
    }
    q->file.private_data = q->misc;
    if ((r = q->misc->fops->open(&q->inode, &q->file)) < 0) {
        q->misc = NULL;
        if (r == -EOPNOTSUPP)
            snprintf(err, errlen, "firmware %s has no I/Q stream; it needs the stock linux-firmware "
                    "WIFI_RAM_CODE_MT7961_1.bin this source patches", uwifi_hw(q->dev)->wiphy->fw_version);
        else
            snprintf(err, errlen, "unable to start the I/Q stream: %s", strerror(-r));
        goto fail;
    }
    q->file.f_flags = O_NONBLOCK;
    return q;

fail:
    mt7921_iq_close(q);
    return NULL;
}

void mt7921_iq_close(struct mt7921_iq *q) {
    if (q == NULL)
        return;
    if (q->misc != NULL)
        q->misc->fops->release(&q->inode, &q->file);
    if (q->dev != NULL)
        uwifi_close(q->dev);
    free(q);
}

const uint8_t *mt7921_iq_mac(struct mt7921_iq *q) {
    return uwifi_mac(q->dev);
}

bool mt7921_iq_superspeed(struct mt7921_iq *q) {
    const char *speed = uwifi_info(q->dev)->speed;

    return strcmp(speed, "5Gbps") == 0 || strcmp(speed, "10Gbps") == 0;
}

bool mt7921_iq_alive(struct mt7921_iq *q) {
    return uwifi_alive(q->dev);
}

int mt7921_iq_tune(struct mt7921_iq *q, unsigned int freq_khz, unsigned int bw_mhz,
        unsigned int *got_khz, unsigned int *gen) {
    struct mt7921_iq_tune t = { freq_khz, 0, 0, bw_mhz };
    long r = q->misc->fops->unlocked_ioctl(&q->file, MT7921_IQ_IOC_TUNE, (unsigned long) &t);

    if (r < 0)
        return (int) r;
    *got_khz = t.freq_khz;
    *gen = t.generation;
    return 0;
}

int mt7921_iq_gain(struct mt7921_iq *q, bool hold, unsigned int lna, unsigned int lpf) {
    struct mt7921_iq_gain g = { hold ? MT7921_IQ_GAIN_HOLD : MT7921_IQ_GAIN_AGC, lna, lpf, 0 };

    return (int) q->misc->fops->unlocked_ioctl(&q->file, MT7921_IQ_IOC_SET_GAIN, (unsigned long) &g);
}

ssize_t mt7921_iq_read(struct mt7921_iq *q, void *buf, size_t len) {
    loff_t pos = 0;
    ssize_t n;

    if (!uwifi_alive(q->dev))
        return -1;
    n = q->misc->fops->read(&q->file, buf, len, &pos);
    if (n == -EAGAIN)
        return 0;
    return n > 0 ? n : -1;
}
