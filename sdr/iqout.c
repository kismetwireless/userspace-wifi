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

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "iqout.h"

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SIGMF_ENDIAN "_be"
#else
#define SIGMF_ENDIAN "_le"
#endif

static const struct {
    const char *name, *sigmf;
    size_t bytes;
} formats[] = {
    [IQOUT_CF32] = { "cf32", "cf32" SIGMF_ENDIAN, 8 },
    [IQOUT_CS16] = { "cs16", "ci16" SIGMF_ENDIAN, 4 },
    [IQOUT_CS8] = { "cs8", "ci8", 2 },
    [IQOUT_CU8] = { "cu8", "cu8", 2 },
};

int iqout_parse_format(const char *name, enum iqout_format *fmt) {
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (strcmp(name, formats[i].name) == 0) {
            *fmt = (enum iqout_format) i;
            return 0;
        }
    }
    return -1;
}

const char *iqout_format_name(enum iqout_format fmt) {
    return formats[fmt].name;
}

size_t iqout_sample_bytes(enum iqout_format fmt) {
    return formats[fmt].bytes;
}

/* v scaled and rounded into [lo, hi], counting a clip */
static long scale(float v, float k, long lo, long hi, size_t *clips) {
    long s = lrintf(v * k);

    if (s < lo || s > hi) {
        (*clips)++;
        return s < lo ? lo : hi;
    }
    return s;
}

size_t iqout_convert(enum iqout_format fmt, const iqradio_sample_t *in, size_t n, void *out) {
    const float *f = (const float *) in;
    size_t clips = 0;

    switch (fmt) {
        case IQOUT_CF32:
            memcpy(out, in, n * 2 * sizeof(float));
            break;
        case IQOUT_CS16: {
            int16_t *o = out;

            for (size_t i = 0; i < 2 * n; i++)
                o[i] = (int16_t) scale(f[i], 32767.0f, -32768, 32767, &clips);
            break;
        }
        case IQOUT_CS8: {
            int8_t *o = out;

            for (size_t i = 0; i < 2 * n; i++)
                o[i] = (int8_t) scale(f[i], 127.0f, -128, 127, &clips);
            break;
        }
        case IQOUT_CU8: {
            uint8_t *o = out;

            for (size_t i = 0; i < 2 * n; i++)
                o[i] = (uint8_t) scale(f[i] + 1.0f, 127.5f, 0, 255, &clips);
            break;
        }
    }
    /* A sample clips if either of its parts does; close enough to count parts */
    return clips;
}

/* A JSON string: quoted, with quotes, backslashes and control characters escaped, and
 * bytes that aren't printable ASCII as \u escapes so the output stays valid UTF-8 */
static int json_str(FILE *f, const char *s) {
    if (fputc('"', f) == EOF)
        return -1;
    for (const unsigned char *p = (const unsigned char *) (s != NULL ? s : ""); *p; p++) {
        int r;

        if (*p == '"' || *p == '\\')
            r = fprintf(f, "\\%c", *p);
        else if (*p < 0x20 || *p >= 0x7f)
            r = fprintf(f, "\\u%04x", *p);
        else
            r = fputc(*p, f) == EOF ? -1 : 1;
        if (r < 0)
            return -1;
    }
    return fputc('"', f) == EOF ? -1 : 0;
}

int iqout_write_sigmf(FILE *f, const struct iqout_meta *m) {
    time_t secs = (time_t) m->start_time;
    long ms = lround((m->start_time - (double) secs) * 1e3);
    char when[40];
    struct tm tm;

    if (ms >= 1000) {
        secs++;
        ms -= 1000;
    }
    if (gmtime_r(&secs, &tm) == NULL || strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &tm) == 0)
        return -1;

    if (fprintf(f, "{\n  \"global\": {\n    \"core:datatype\": \"%s\",\n    \"core:sample_rate\": %.3f,\n"
                "    \"core:version\": \"1.0.0\",\n    \"core:recorder\": \"usbsdr\",\n    \"core:hw\": ",
                formats[m->fmt].sigmf, m->rate) < 0 ||
            json_str(f, m->hw) < 0 ||
            fprintf(f, ",\n    \"core:description\": ") < 0 ||
            json_str(f, m->description) < 0 ||
            fprintf(f, "\n  },\n  \"captures\": [\n    {\n      \"core:sample_start\": 0,\n"
                "      \"core:frequency\": %.0f,\n      \"core:datetime\": \"%s.%03ldZ\"\n    }\n  ],\n"
                "  \"annotations\": []\n}\n", m->center_hz, when, ms) < 0)
        return -1;
    return fflush(f) == EOF ? -1 : 0;
}
