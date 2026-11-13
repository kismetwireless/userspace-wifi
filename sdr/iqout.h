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

#ifndef __USBWIFISDR_IQOUT_H__
#define __USBWIFISDR_IQOUT_H__

/* I/Q samples out to other tools: the raw sample formats SDR tools read, and SigMF
 * metadata for a recording */

#include <stdio.h>

#include "iqradio.h"

#ifdef __cplusplus
extern "C" {
#endif

enum iqout_format {
    /* Float I and Q, as the radio gives them */
    IQOUT_CF32,
    /* Signed 16 and 8 bit, full scale at 1.0 */
    IQOUT_CS16,
    IQOUT_CS8,
    /* Unsigned 8 bit around 127.5, as rtl_sdr writes */
    IQOUT_CU8,
};

/* "cf32", "cs16", "cs8" or "cu8"; -1 for anything else */
int iqout_parse_format(const char *name, enum iqout_format *fmt);
const char *iqout_format_name(enum iqout_format fmt);
/* Bytes per complex sample */
size_t iqout_sample_bytes(enum iqout_format fmt);

/* n samples to fmt in out, which holds n * iqout_sample_bytes(); the number of samples that
 * clipped */
size_t iqout_convert(enum iqout_format fmt, const iqradio_sample_t *in, size_t n, void *out);

struct iqout_meta {
    enum iqout_format fmt;
    double rate, center_hz;
    /* Of the first sample, seconds since the epoch */
    double start_time;
    /* Free text from the adapter (its USB product string), escaped on output; may be NULL */
    const char *hw;
    const char *description;
};

/* SigMF metadata for a recording of one tuning; 0, or -1 on a write error */
int iqout_write_sigmf(FILE *f, const struct iqout_meta *meta);

#ifdef __cplusplus
}
#endif

#endif
