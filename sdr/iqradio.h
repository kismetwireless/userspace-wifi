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

#ifndef __USBWIFISDR_IQRADIO_H__
#define __USBWIFISDR_IQRADIO_H__

/* An adapter's I/Q stream as complex samples: 2.4 and 5 GHz, 20, 40 or 80 MHz wide (and
 * 6 GHz and 160 MHz on the MT7925), tuned on the Wi-Fi channel raster */

#include <stddef.h>

#include "iqdev.h"

/* Interleaved float I and Q, full scale 1.0; the same layout in C and C++ */
#ifdef __cplusplus
#include <complex>
typedef std::complex<float> iqradio_sample_t;
extern "C" {
#else
#include <complex.h>
typedef float complex iqradio_sample_t;
#endif

struct iqradio;

/* Every lane runs this much fast of the nominal rate */
#define IQRADIO_RATE_UNIT 1.00004e6

/* Takes over an open adapter */
struct iqradio *iqradio_new(struct iqdev *d);
/* Also closes the adapter */
void iqradio_free(struct iqradio *r);

/* A center the chip can tune at this width: on the 5 MHz raster, inside the band */
bool iqradio_center_ok(enum iqdev_chip chip, double hz, unsigned int bw_mhz);

/* 0, or a negative errno; the stream starts over from the new tuning */
int iqradio_tune(struct iqradio *r, double hz, unsigned int bw_mhz);
double iqradio_rate(const struct iqradio *r);
double iqradio_center(const struct iqradio *r);

/* n samples of one continuous run; -1 if the stream ended or stalled */
int iqradio_read(struct iqradio *r, iqradio_sample_t *buf, size_t n);
/* Drops what the driver holds, so the next read is from now */
int iqradio_flush(struct iqradio *r);

/* The analog gain held near db dB above the band's lowest usable code, or at the band's
 * default; 0 gives it to the chip's AGC.  The MT7925's takes effect at the next tune */
#define IQRADIO_GAIN_DEFAULT (-1.0)
int iqradio_set_gain(struct iqradio *r, double db);
/* The highest held gain on the band of hz, in dB above its lowest usable code */
double iqradio_gain_max(const struct iqradio *r, double hz);
/* The held gain in dB (the band's default, as dB, when held there), or 0 under the AGC */
double iqradio_gain(const struct iqradio *r);

/* Breaks in the stream that lost samples (dropped blocks, ring overruns, framing breaks) since
 * the radio was made */
unsigned long iqradio_losses(const struct iqradio *r);

#ifdef __cplusplus
}
#endif

#endif
