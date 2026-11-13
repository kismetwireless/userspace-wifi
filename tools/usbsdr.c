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

/* USB Wi-Fi SDR tool: list the I/Q adapters, show what they tune, and record I/Q to a file,
 * a SigMF recording, or stdout for another tool */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "iqdev.h"
#include "iqout.h"
#include "iqradio.h"

#ifndef FIRMWAREDIR
#define FIRMWAREDIR "/usr/local/share/uwifi/firmware"
#endif

#define MAX_ADAPTERS 16
/* Samples a read; about 3 ms at 20 MHz */
#define CHUNK 65536
#define PATH_MAX_LEN 4096

static volatile sig_atomic_t stop;

static void handle_signal(int sig) {
    (void) sig;
    stop = 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s list\n"
            "       %s info [-d NAME]\n"
            "       %s capture [-d NAME] (-f FREQ | -c CHAN) [-b BW] [-g DB|agc] [-F FORMAT]\n"
            "                  [-n SAMPLES | -T SECONDS] [-m] -w FILE|-\n"
            "\n"
            "  -d, --device NAME     adapter, such as mt7921sdr-0 (default the first)\n"
            "  -f, --freq FREQ       center: 2437M, 5.18G, 2437 (MHz) or Hz\n"
            "  -c, --channel CHAN    center on a Wi-Fi channel: 6, 36, 6g:37\n"
            "  -b, --bandwidth MHZ   20, 40 or 80; 160 on the MT7925 (default 20)\n"
            "  -g, --gain DB|agc     analog gain held DB above the band's lowest, or the chip's AGC\n"
            "                        (default: held at the band's default)\n"
            "  -F, --format FORMAT   cf32, cs16, cs8 or cu8 (default cf32)\n"
            "  -n, --samples N       stop after N samples\n"
            "  -T, --duration S      stop after S seconds of samples\n"
            "  -w, --write FILE      write samples to FILE; '-' for stdout\n"
            "  -m, --sigmf           FILE names a SigMF recording: FILE.sigmf-data and FILE.sigmf-meta\n"
            "  -q, --quiet           no progress on stderr\n"
            "      --fw-dir DIR      search DIR for firmware first\n"
            "  -v, --verbose         driver debug logging\n",
            argv0, argv0, argv0);
}

static void sdr_log(void *ctx, int level, const char *msg) {
    (void) ctx; (void) level;
    fprintf(stderr, "driver: %s\n", msg);
}

/* 2437M, 5.18G, 2437000000; a bare number under 100000 is MHz */
static int parse_freq(const char *s, double *hz) {
    char *end;
    double v;

    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || !isfinite(v) || v <= 0)
        return -1;
    if (*end == 0)
        v *= v < 1e5 ? 1e6 : 1;
    else if (strcasecmp(end, "k") == 0)
        v *= 1e3;
    else if (strcasecmp(end, "m") == 0)
        v *= 1e6;
    else if (strcasecmp(end, "g") == 0)
        v *= 1e9;
    else
        return -1;
    *hz = round(v);
    return 0;
}

/* Wi-Fi channel numbers: 1-14 on 2.4 GHz, 32-177 on 5 GHz, 6g:1-233 on 6 GHz */
static int parse_chan(const char *s, double *hz) {
    bool six = strncasecmp(s, "6g:", 3) == 0;
    char *end;
    long ch;

    if (six)
        s += 3;
    errno = 0;
    ch = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != 0)
        return -1;
    if (six && ch >= 1 && ch <= 233)
        *hz = (5950.0 + 5.0 * ch) * 1e6;
    else if (!six && ch >= 1 && ch <= 13)
        *hz = (2407.0 + 5.0 * ch) * 1e6;
    else if (!six && ch == 14)
        *hz = 2484e6;
    else if (!six && ch >= 32 && ch <= 177)
        *hz = (5000.0 + 5.0 * ch) * 1e6;
    else
        return -1;
    return 0;
}

static int parse_count(const char *s, double *v) {
    char *end;

    errno = 0;
    *v = strtod(s, &end);
    return errno != 0 || end == s || *end != 0 || !isfinite(*v) || *v <= 0 ? -1 : 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

static int find_adapter(const char *want, struct iqdev_info *out) {
    struct iqdev_info list[MAX_ADAPTERS];
    int n = iqdev_list(list, MAX_ADAPTERS);

    for (int i = 0; i < n; i++) {
        if (want == NULL || strcmp(list[i].name, want) == 0) {
            *out = list[i];
            return 0;
        }
    }
    if (want == NULL)
        fprintf(stderr, "no I/Q adapters found (MT7921AU or MT7925 on USB)\n");
    else
        fprintf(stderr, "%s: no such adapter; see '%s'\n", want, "usbsdr list");
    return -1;
}

static int cmd_list(void) {
    struct iqdev_info list[MAX_ADAPTERS];
    int n = iqdev_list(list, MAX_ADAPTERS);

    for (int i = 0; i < n; i++)
        printf("%-14s %-9s %s\n", list[i].name, iqdev_chip_name(list[i].chip), list[i].hardware);
    if (n == 0)
        fprintf(stderr, "no I/Q adapters found (MT7921AU or MT7925 on USB)\n");
    return n > 0 ? 0 : 1;
}

/* The lowest and highest tunable center of a band at a width, 0 if none */
static void band_range(enum iqdev_chip chip, unsigned int bw, double lo, double hi, double *first,
        double *last) {
    *first = *last = 0;
    for (double hz = lo; hz <= hi; hz += 5e6) {
        if (!iqradio_center_ok(chip, hz, bw))
            continue;
        if (*first == 0)
            *first = hz;
        *last = hz;
    }
}

static int cmd_info(const char *name) {
    static const unsigned int widths[] = { 20, 40, 80, 160 };
    static const struct {
        const char *name;
        double lo, hi;
    } bands[] = {
        /* Scanned in 5 MHz steps from a raster point: 2407 + 5n, 5000 + 5n */
        { "2.4 GHz", 2402e6, 2500e6 },
        { "5-7 GHz", 5000e6, 7200e6 },
    };
    struct iqdev_info info;
    struct iqradio *r;
    struct iqdev *d;
    char err[256];
    const uint8_t *mac;

    if (find_adapter(name, &info) < 0)
        return 1;
    if ((d = iqdev_open(info.name, err, sizeof(err))) == NULL) {
        fprintf(stderr, "%s: %s\n", info.name, err);
        return 1;
    }
    mac = iqdev_mac(d);
    printf("Adapter:   %s, %s\n", info.name, info.hardware);
    printf("Chip:      %s\n", iqdev_chip_name(info.chip));
    printf("MAC:       %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("USB 3:     %s\n", iqdev_superspeed(d) ? "yes" : "no (the stream needs it; most samples will be lost)");
    if ((r = iqradio_new(d)) == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    printf("Gain:      0-%.0f dB held on 2.4 GHz, 0-%.0f dB on 5 GHz, or AGC\n",
            iqradio_gain_max(r, 2437e6), iqradio_gain_max(r, 5500e6));
    printf("Tuning:    centers on a 5 MHz raster\n");
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        double rate = widths[w] * IQRADIO_RATE_UNIT * (info.chip == IQDEV_MT7925 ? 2 : 1);
        bool any = false;

        for (size_t b = 0; b < sizeof(bands) / sizeof(bands[0]); b++) {
            double first, last;

            band_range(info.chip, widths[w], bands[b].lo, bands[b].hi, &first, &last);
            if (first == 0)
                continue;
            if (!any)
                printf("  %3u MHz, %.4f MS/s:", widths[w], rate / 1e6);
            printf("%s %.0f-%.0f MHz", any ? "," : "", first / 1e6, last / 1e6);
            any = true;
        }
        if (any)
            printf("\n");
    }
    if (info.chip == IQDEV_MT7925)
        printf("Stream:    snapshots of 8192 samples, contiguous within each, gaps between\n");
    else
        printf("Stream:    continuous\n");
    iqradio_free(r);
    return 0;
}

struct capture_opts {
    const char *name, *path;
    double hz, gain, limit_samples, limit_secs;
    unsigned int bw;
    enum iqout_format fmt;
    bool sigmf, quiet;
};

static int open_output(const char *path) {
    int fd;

    if (strcmp(path, "-") == 0) {
        if (isatty(STDOUT_FILENO)) {
            fprintf(stderr, "not writing samples to a terminal; pipe stdout to another program\n");
            return -1;
        }
        return STDOUT_FILENO;
    }
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0)
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return fd;
}

static int write_meta(const struct capture_opts *o, const char *base, const struct iqdev_info *info,
        double rate, double start) {
    char path[PATH_MAX_LEN], desc[128];
    struct iqout_meta m = { o->fmt, rate, o->hz, start, info->hardware, desc };
    FILE *f;
    int r;

    snprintf(desc, sizeof(desc), "%s I/Q, %u MHz channel", iqdev_chip_name(info->chip), o->bw);
    if (snprintf(path, sizeof(path), "%s.sigmf-meta", base) >= (int) sizeof(path))
        return -1;
    if ((f = fopen(path, "w")) == NULL) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    r = iqout_write_sigmf(f, &m);
    if (fclose(f) != 0 || r < 0) {
        fprintf(stderr, "%s: write failed\n", path);
        return -1;
    }
    return 0;
}

static double now_s(int clock) {
    struct timespec ts;

    clock_gettime(clock, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static int cmd_capture(const struct capture_opts *o) {
    char base[PATH_MAX_LEN], data_path[PATH_MAX_LEN], err[256];
    iqradio_sample_t *buf = NULL;
    void *out = NULL;
    struct iqdev_info info;
    struct iqradio *r = NULL;
    struct iqdev *d = NULL;
    double rate, start, last, limit;
    unsigned long long total = 0, clips = 0;
    int fd = -1, ret = 1, e;

    if (find_adapter(o->name, &info) < 0)
        return 1;
    if (!iqradio_center_ok(info.chip, o->hz, o->bw)) {
        fprintf(stderr, "%s can't tune %.3f MHz at %u MHz wide; see 'usbsdr info'\n",
                iqdev_chip_name(info.chip), o->hz / 1e6, o->bw);
        return 1;
    }

    if (snprintf(data_path, sizeof(data_path), "%s", o->path) >= (int) sizeof(data_path)) {
        fprintf(stderr, "file name too long\n");
        return 1;
    }
    if (o->sigmf) {
        size_t len = strlen(o->path);

        if (strcmp(o->path, "-") == 0) {
            fprintf(stderr, "a SigMF recording needs a file name, not stdout\n");
            return 1;
        }
        snprintf(base, sizeof(base), "%s", o->path);
        /* Either file's name stands for the recording */
        if (len > 11 && (strcmp(o->path + len - 11, ".sigmf-data") == 0 ||
                    strcmp(o->path + len - 11, ".sigmf-meta") == 0))
            base[len - 11] = 0;
        if (snprintf(data_path, sizeof(data_path), "%s.sigmf-data", base) >= (int) sizeof(data_path)) {
            fprintf(stderr, "file name too long\n");
            return 1;
        }
    }
    if ((fd = open_output(data_path)) < 0)
        return 1;

    if ((d = iqdev_open(info.name, err, sizeof(err))) == NULL) {
        fprintf(stderr, "%s: %s\n", info.name, err);
        goto done;
    }
    if (!iqdev_superspeed(d))
        fprintf(stderr, "%s isn't on a USB 3 port; most samples will be lost\n", info.name);
    /* The radio owns the adapter from here, and closes it if it can't be made */
    r = iqradio_new(d);
    d = NULL;
    if (r == NULL) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    /* Before the tune, which applies the MT7925's */
    if ((e = iqradio_set_gain(r, o->gain)) < 0) {
        fprintf(stderr, "%s: setting the gain failed: %s\n", info.name, strerror(-e));
        goto done;
    }
    if ((e = iqradio_tune(r, o->hz, o->bw)) < 0) {
        fprintf(stderr, "%s: tuning to %.3f MHz failed: %s\n", info.name, o->hz / 1e6, strerror(-e));
        goto done;
    }
    rate = iqradio_rate(r);
    limit = o->limit_samples > 0 ? o->limit_samples : o->limit_secs > 0 ? ceil(o->limit_secs * rate) : 0;
    if ((buf = malloc(CHUNK * sizeof(*buf))) == NULL ||
            (out = malloc(CHUNK * iqout_sample_bytes(o->fmt))) == NULL) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    start = now_s(CLOCK_REALTIME);
    if (o->sigmf && write_meta(o, base, &info, rate, start) < 0)
        goto done;
    if (!o->quiet)
        fprintf(stderr, "%s: %.3f MHz, %u MHz wide, %.4f MS/s %s to %s\n", info.name, o->hz / 1e6, o->bw,
                rate / 1e6, iqout_format_name(o->fmt), strcmp(data_path, "-") == 0 ? "stdout" : data_path);

    last = now_s(CLOCK_MONOTONIC);
    ret = 0;
    while (!stop && (limit == 0 || total < limit)) {
        size_t n = CHUNK;
        double t;

        if (limit > 0 && limit - total < n)
            n = (size_t) (limit - total);
        if (iqradio_read(r, buf, n) < 0) {
            if (!stop) {
                fprintf(stderr, "%s: the stream stalled or the adapter went away\n", info.name);
                ret = 1;
            }
            break;
        }
        clips += iqout_convert(o->fmt, buf, n, out);
        if (write_all(fd, out, n * iqout_sample_bytes(o->fmt)) < 0) {
            /* The reader went away: a normal end for a pipe */
            if (errno != EPIPE) {
                fprintf(stderr, "write failed: %s\n", strerror(errno));
                ret = 1;
            }
            break;
        }
        total += n;
        if (!o->quiet && (t = now_s(CLOCK_MONOTONIC)) - last >= 1.0) {
            fprintf(stderr, "\r%llu samples, %lu losses, %llu clipped   ", total, iqradio_losses(r), clips);
            last = t;
        }
    }
    if (!o->quiet)
        fprintf(stderr, "\r%llu samples, %lu losses, %llu clipped   \n", total, iqradio_losses(r), clips);

done:
    iqradio_free(r);
    iqdev_close(d);
    free(buf);
    free(out);
    if (fd > STDOUT_FILENO && close(fd) < 0) {
        fprintf(stderr, "%s: %s\n", data_path, strerror(errno));
        ret = 1;
    }
    return ret;
}

int main(int argc, char *argv[]) {
    enum { OPT_FW_DIR = 256 };
    static const struct option longopts[] = {
        { "device", required_argument, NULL, 'd' },
        { "freq", required_argument, NULL, 'f' },
        { "channel", required_argument, NULL, 'c' },
        { "bandwidth", required_argument, NULL, 'b' },
        { "gain", required_argument, NULL, 'g' },
        { "format", required_argument, NULL, 'F' },
        { "samples", required_argument, NULL, 'n' },
        { "duration", required_argument, NULL, 'T' },
        { "write", required_argument, NULL, 'w' },
        { "sigmf", no_argument, NULL, 'm' },
        { "quiet", no_argument, NULL, 'q' },
        { "fw-dir", required_argument, NULL, OPT_FW_DIR },
        { "verbose", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct capture_opts o = { .bw = 20, .gain = IQRADIO_GAIN_DEFAULT, .fmt = IQOUT_CF32 };
    const char *fw_dir = NULL, *cmd;
    bool have_freq = false;
    int verbose = 0, opt;
    char *end;

    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return argc < 2;
    }
    cmd = argv[1];
    optind = 2;
    while ((opt = getopt_long(argc, argv, "d:f:c:b:g:F:n:T:w:mqvh", longopts, NULL)) != -1) {
        switch (opt) {
            case 'd': o.name = optarg; break;
            case 'f':
            case 'c':
                if ((opt == 'f' ? parse_freq : parse_chan)(optarg, &o.hz) < 0) {
                    fprintf(stderr, "bad %s: %s\n", opt == 'f' ? "frequency" : "channel", optarg);
                    return 1;
                }
                have_freq = true;
                break;
            case 'b':
                errno = 0;
                o.bw = (unsigned int) strtoul(optarg, &end, 10);
                if (errno != 0 || end == optarg || *end != 0 ||
                        (o.bw != 20 && o.bw != 40 && o.bw != 80 && o.bw != 160)) {
                    fprintf(stderr, "bad bandwidth: %s (20, 40, 80 or 160)\n", optarg);
                    return 1;
                }
                break;
            case 'g':
                if (strcasecmp(optarg, "agc") == 0) {
                    o.gain = 0;
                    break;
                }
                errno = 0;
                o.gain = strtod(optarg, &end);
                if (errno != 0 || end == optarg || *end != 0 || !isfinite(o.gain) || o.gain < 0 || o.gain > 100) {
                    fprintf(stderr, "bad gain: %s\n", optarg);
                    return 1;
                }
                /* 0 dB is the lowest held code; the radio takes 0 as the AGC */
                if (o.gain < 0.01)
                    o.gain = 0.01;
                break;
            case 'F':
                if (iqout_parse_format(optarg, &o.fmt) < 0) {
                    fprintf(stderr, "bad format: %s (cf32, cs16, cs8 or cu8)\n", optarg);
                    return 1;
                }
                break;
            case 'n':
            case 'T':
                if (parse_count(optarg, opt == 'n' ? &o.limit_samples : &o.limit_secs) < 0) {
                    fprintf(stderr, "bad %s: %s\n", opt == 'n' ? "sample count" : "duration", optarg);
                    return 1;
                }
                break;
            case 'w': o.path = optarg; break;
            case 'm': o.sigmf = true; break;
            case 'q': o.quiet = true; break;
            case OPT_FW_DIR: fw_dir = optarg; break;
            case 'v': verbose++; break;
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }
    if (optind != argc) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    {
        const char *fw_paths[] = { fw_dir, "firmware", FIRMWAREDIR, "/usr/local/share/kismet/firmware",
            "/usr/share/kismet/firmware" };
        const char *const *paths = fw_dir != NULL ? fw_paths : fw_paths + 1;
        size_t n_paths = sizeof(fw_paths) / sizeof(fw_paths[0]) - (fw_dir != NULL ? 0 : 1);

        if (iqdev_init(paths, n_paths, sdr_log, NULL, verbose ? 7 : 4) < 0) {
            fprintf(stderr, "unable to start the USB layer\n");
            return 1;
        }
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "info") == 0)
        return cmd_info(o.name);
    if (strcmp(cmd, "capture") == 0) {
        if (!have_freq || o.path == NULL) {
            fprintf(stderr, "capture needs a frequency (-f or -c) and an output (-w FILE or -w -)\n");
            return 1;
        }
        return cmd_capture(&o);
    }
    usage(argv[0]);
    return 1;
}
