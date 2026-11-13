# USB Wi-Fi SDR

Two MediaTek USB Wi-Fi chips can stream raw I/Q from their receiver, which turns a cheap
Wi-Fi adapter into a 2.4 / 5 / 6 GHz SDR receiver.  The SDR build adds the drivers for that
stream, on the same kernel emulation as `uwifi`, and three ways to get the samples out:

- `usbsdr`, a command line tool that records I/Q to a file or a SigMF recording, or writes
  it to stdout for another program;
- a SoapySDR module (`driver=uwifisdr`), for GNU Radio, gqrx, SDR++, soapy_power, and anything
  else on SoapySDR, with a GNU Radio Companion block beside gr-soapy's own;
- `libuwifisdr.a` and `sdr/iqradio.h`, for programs of their own (Kismet's `usbwifisdr`
  datasource).

| Chip | Adapters | Tunes | Widths | Rate | Stream |
|---|---|---|---|---|---|
| MT7921AU | `mt7921sdr-N` | 2412-2472, 5005-5995 MHz | 20, 40, 80 | 1x the width | continuous |
| MT7925 | `mt7925sdr-N` | 2412-2472, 5005-7115 MHz | 20, 40, 80, 160 | 2x the width | 8192-sample snapshots, gaps between |

Centers are on the chip's 5 MHz raster; a narrower list applies to wider channels (`usbsdr
info`).  The rate is `width x 1.00004 MS/s` (x2 on the MT7925): the chips' sample clocks run
40 ppm fast of the nominal rate.  Both need a USB 3 port; on USB 2 most samples are lost.

## Credits

The MT7921AU I/Q capture is the work of **Mtk79ICAP** by the MtkWifiRev contributors,
<https://github.com/MtkWifiRev/Mtk79ICAP>.  This tree carries, from its commit 38221102d671:

- `drivers/mt76_icap/`: their mt76 driver with ICAP I/Q streaming (BSD-3-Clause-Clear), copied
  unmodified apart from one marked addition (see its `UPSTREAM`);
- `sdr/third_party/libmt7921iq/`: their IQB2 stream decoder (MIT);
- `sdr/mt7961_fw.c`: their iq_dma_v65 firmware patch, from `firmware/mt7961-iq-fw-v65.py`
  (MIT), applied to the stock linux-firmware image as it loads.

Their licenses are kept with the code.  The MT7925 I/Q capture (`sdr/mt7925_host.c`,
`sdr/mt7925_pump/`), the host side, `usbsdr`, the SoapySDR module and the GNU Radio block are
this project's.

## Building

    make SDR=1                  # uwifi, usbsdr, and the SoapySDR module if SoapySDR is found
    make SDR=1 SOAPY=0          # without the SoapySDR module
    sudo make SDR=1 install     # usbsdr to $(PREFIX)/bin, the module to SoapySDR's module directory

The SoapySDR module needs SoapySDR's development files (`libsoapysdr-dev`, `soapysdr-devel`,
or Homebrew's `soapysdr`) and installs to `<SoapySDR libdir>/SoapySDR/modules<ABI>`; set
`SOAPY_MODDIR` to put it elsewhere, and `SOAPY_SDR_PLUGIN_PATH` to point SoapySDR at it.
With the module, everything is built position-independent (`-fPIC`); changing `SDR` or
`SOAPY` rebuilds what that changes.

## Linux USB memory

Linux limits the memory all usbfs programs together may have in flight
(`/sys/module/usbcore/parameters/usbfs_memory_mb`, 16 MB by default).  The MT7921AU stream
wants 512 receive buffers of 128 KiB, 64 MB; under the default the driver takes 96, logs
that it did, and loses more samples at 40 and 80 MHz (dozens of breaks a second at 80 MHz
on a 16 MB limit).  To give it all of them, as root:

    echo 100 > /sys/module/usbcore/parameters/usbfs_memory_mb

or `usbcore.usbfs_memory_mb=100` on the kernel command line; 0 removes the limit.

## Firmware

The adapters load the firmware in `firmware/mediatek`, found as `uwifi` finds it (`--fw-dir`,
`./firmware`, the install firmware directory, Kismet's, then on Linux the system's).  Both I/Q
drivers depend on the exact firmware build:

- MT7921AU: the stock linux-firmware `WIFI_RAM_CODE_MT7961_1.bin`, which is patched for the
  I/Q stream as it loads (`sdr/mt7961_fw.c`, from Mtk79ICAP's iq_dma_v65 patch).  A different
  build is reported as having no I/Q stream.
- MT7925: `WIFI_RAM_CODE_MT7925_1_1.bin` build 20260813113118.  A small RISC-V pump
  (`sdr/mt7925_pump/`) is copied into the MCU and entered through an unused command; it is
  tied to this build's addresses, and other builds are refused.

The copies in `firmware/` are these builds; keep them when refreshing linux-firmware for
`uwifi`, or the SDR build stops working.

## usbsdr

    ./usbsdr list
    ./usbsdr info -d mt7921sdr-0
    ./usbsdr capture -c 6 -T 10 -w ch6.cf32                     # 10 s of channel 6, float
    ./usbsdr capture -f 5180M -b 40 -m -w ch36                    # SigMF: ch36.sigmf-data, .sigmf-meta
    ./usbsdr capture -c 36 -b 80 -F cs16 -g agc -w - | other-tool # cs16 on stdout, chip AGC
    ./usbsdr capture -c 1 -F cu8 -w - | other-tool                # rtl_sdr-style cu8

Formats are `cf32` (as the radio gives them, full scale 1.0), `cs16`, `cs8` and `cu8`
(rtl_sdr's unsigned 8 bit).  Progress, sample losses and clipped samples go to stderr; `-q`
turns progress off.  Writing stops at `-n` samples, `-T` seconds of samples, a signal, or when
the reader of stdout goes away.

Gain is held at the band's default code unless `-g` says otherwise: `-g DB` holds it DB above
the band's lowest usable code (`usbsdr info` gives the range), `-g agc` hands it to the chip's
AGC, which dips by a few dB on every nearby frame.  The MT7925's gain applies at a tune.

## SoapySDR

    SoapySDRUtil --find=driver=uwifisdr
    SoapySDRUtil --probe=driver=uwifisdr,device=mt7925sdr-0
    rx_sdr -d driver=uwifisdr -f 2437e6 -s 20000800 -F CF32 out.cf32

Device arguments: `device` (or `serial`) names the adapter, default the first; `firmware`
adds a firmware directory (the first device made in a process decides the search path; the
`UWIFI_FIRMWARE` environment variable does the same).  Unlike `usbsdr`, the module doesn't
look in `./firmware`; without an install, point one of these at the tree's `firmware`.  `UWIFI_VERBOSE=1` logs the drivers'
debug messages through SoapySDR's logger.

- One RX channel, antenna `RX`; formats CF32 (native, full scale 1.0), CS16, CS8 and CU8.
- Frequency has two parts: `RF`, the raster point nearest the request, and `BB`, a digital
  shift of up to half the width that moves the requested frequency to 0 Hz.  Plain
  `setFrequency()` tunes both, so the stream is centered where it was asked to be.
- Sample rate and bandwidth both choose the width: the narrowest width giving at least the
  rate asked for.  A new width keeps the frequency.
- A new frequency or width (and a new gain on the MT7925) returns once the stream after it is
  flowing, about 100-250 ms on the MT7921AU, and nothing read after it is from before.
- One gain, `RF`, in dB above the band's lowest code; gain mode on is the chip's AGC.  The
  MT7925 retunes to apply a new gain, which restarts its stream.
- `readStream()` returns `SOAPY_SDR_OVERFLOW` when samples were lost, in the driver or
  because the reader fell behind the module's buffer (about 200 ms at 20 MS/s).  There are no
  hardware timestamps.
- The MT7925's snapshots come one after another with gaps between them; tools that assume a
  continuous stream (FM demodulators, long FFT averages) see the joins.

Tools: `soapy_power -d driver=uwifisdr -r 20.0008e6 -g 14 -f 5170M:5330M --crop 0.25` sweeps
a band (simplesoapy 1.5.1 checks only the first frequency range, so 5 GHz needs it to check
them all).  rx_tools' `rx_power` doesn't work: it assumes at most 2.8 MS/s.

The emulated kernel inside the module is linked with hidden symbols, so it doesn't collide
with anything the host program loads, and only one process can hold an adapter at a time.

## GNU Radio

GNU Radio 3.10's gr-soapy has a source block per radio (Soapy RTLSDR Source, Soapy HackRF
Source, ...); `gnuradio/soapy_uwifisdr_source.block.yml` adds **Soapy USB Wi-Fi SDR Source**
to the same list, installed with the module when GNU Radio is found (`GRC_BLOCKSDIR` to put
it elsewhere, or `GRC_BLOCKS_PATH` to use it from the tree).  Its Sample Rate snaps to the
width that covers it, so `20e6` runs at 20.0008e6.  In Python, without GRC:

    from gnuradio import soapy
    src = soapy.source("driver=uwifisdr", "fc32", 1, "", "", [""], [""])
    src.set_sample_rate(0, 20.0008e6)

gr-osmosdr, and so gqrx, reaches it through its soapy backend: `soapy=0,driver=uwifisdr`.

`gnuradio/wifi_rx_test.py` is a receive test: it compiles a flowgraph of the block feeding an
802.11 receiver, runs it, and counts the frames that pass their FCS by type, without printing
addresses or names.  `-r` picks the receiver:

- `ieee802_11` (`wifi_rx_ieee802_11.grc`): [gr-ieee802-11](https://github.com/bastibl/gr-ieee802-11)
  by Bastian Bloessl, maint-3.10 (install gr-foo first), 802.11a/g;
- `ieee80211` (`wifi_rx_ieee80211.grc`): [gr-ieee80211](https://github.com/JiaoXianjun/gr-ieee80211)
  by Xianjun Jiao, 802.11a/g/n/ac SISO at 20 MHz; it builds only against UHD (`libuhd-dev`).
  `--upstream-log FILE` keeps its per-packet lines, which count frames that failed.

      GRC_BLOCKS_PATH=gnuradio ./gnuradio/wifi_rx_test.py -r ieee80211 -f 5180e6 -t 20 --min-frames 10

Both decode the legacy OFDM frames around them: beacons and control frames on 5 GHz, and on
2.4 GHz only from networks that send them at OFDM rates (most send 1 Mbps DSSS beacons).
gr-ieee80211 takes HT and VHT only at 20 MHz, one stream, BCC, no STBC; most traffic is wider
or two-stream, and its frames show up in the log as legacy frames failing their CRC.

## Layout

| Path | |
|---|---|
| `drivers/mt76_icap/` | Mtk79ICAP's mt76 with the MT7921AU I/Q stream, unmodified (see its UPSTREAM) |
| `drivers/mt76_icap_glue.c` | mt76 functions from files not copied (DMA, PCI, debugfs) |
| `sdr/mt7921_host.c`, `sdr/mt7961_fw.c` | the MT7921AU stream through the driver's character device, and its firmware patch |
| `sdr/mt7925_host.c`, `sdr/mt7925_pump/` | the MT7925 stream on the stock mt7925u driver, and the MCU pump |
| `sdr/iqdev.c` | both chips behind one adapter interface |
| `sdr/third_party/libmt7921iq/` | Mtk79ICAP's IQB2 stream decoder |
| `sdr/iqradio.c` | complex samples from the records: tuning, gain, stall and loss detection |
| `sdr/iqout.c` | sample formats and SigMF metadata |
| `tools/usbsdr.c`, `soapy/uwifisdr_soapy.cpp` | the tool and the SoapySDR module |
| `gnuradio/` | the GNU Radio Companion block, and the 802.11 receive tests |

The MT7921AU's driver is a fork of mt76, so the SDR build links it instead of the kernel's
mt76; the MT7925 runs on the kernel's mt76 and mt7925u, prelinked into one object
(`sdr/mt7925_sdr.o`) that hides everything but the `mt7925_iq_` functions, so both mt76
copies live in one binary.  That's why the SDR build has its own library and doesn't include
the other drivers.

## Why a build option and not its own repository

The I/Q drivers need nearly all of this tree: all of kcompat, the mac80211 layer, the common
device and firmware code, and the mt76 core with mt7925u.  They also needed additions to kcompat
(a firmware filter, module parameters, misc devices, poll, and more of skb, sync and usb).
Kept apart, they would have to carry their own copy of the emulation or reach into a
submodule's objects for the MT7925 prelink, and the two would drift.  As a build option they
share one emulation, and the code only they use (the Mtk79ICAP mt76, `sdr/`, `soapy/`) stays
out of the default build.

## Kismet

Kismet vendors this tree (`capture_userspace_usb_wifi/userspace-wifi`).  Vendoring the SDR
build in replaces Kismet's own copies of the I/Q drivers on the `usbwifi-sdr` branch:

- the vendored tree is built with `SDR=1 SOAPY=0` for `libuwifisdr.a`;
- `kismet_cap_usbwifisdr` (the datasource) and `sdr_decoders/` stay in Kismet, and link
  `libuwifisdr.a` with the whole archive, as the usbwifi datasource does `libuwifi.a`;
- the datasource already uses `iqdev.h` and `iqradio.h`, whose interface is unchanged
  (`iqradio_read()` takes `iqradio_sample_t`, which is `float complex` in C).
