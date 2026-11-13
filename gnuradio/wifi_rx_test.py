#!/usr/bin/env python3
"""
Receive tests for the GNU Radio block: compiles a flowgraph that feeds the Soapy USB Wi-Fi
SDR Source into an 802.11 receiver, runs it, and counts the frames that pass their FCS by
type.  Only counts are printed, no addresses or names.

Receivers:
  ieee802_11  gr-ieee802-11 (bastibl, maint-3.10): 802.11a/g, wifi_rx_ieee802_11.grc
  ieee80211   gr-ieee80211 (JiaoXianjun): 802.11a/g/n/ac SISO, wifi_rx_ieee80211.grc

Needs GNU Radio 3.10 with gr-soapy, the receiver, and the uwifisdr SoapySDR module.  GRC
must find the block: install it, or put this directory in GRC_BLOCKS_PATH.

    ./wifi_rx_test.py -r ieee80211 -f 5180e6 -t 20 --min-frames 10
"""

import argparse
import collections
import importlib.util
import os
import subprocess
import sys
import tempfile
import threading
import time

import pmt
from gnuradio import gr

NAMES = {
    (0, 0): "assoc req", (0, 1): "assoc resp", (0, 4): "probe req", (0, 5): "probe resp",
    (0, 8): "beacon", (0, 10): "disassoc", (0, 11): "auth", (0, 12): "deauth",
    (0, 13): "action", (0, 14): "action no-ack",
    (1, 5): "NDP announce", (1, 8): "block ack req", (1, 9): "block ack", (1, 11): "RTS",
    (1, 12): "CTS", (1, 13): "ACK",
    (2, 0): "data", (2, 4): "null", (2, 8): "QoS data", (2, 12): "QoS null",
}

# gr-ieee80211's packets: format byte, little-endian length, then the frame with its FCS
PHY_FORMATS = {0: "legacy", 1: "HT", 2: "VHT"}


def frame_ieee802_11(blob):
    return bytes(blob), None


def frame_ieee80211(blob):
    b = bytes(blob)
    if len(b) < 3 or b[0] not in PHY_FORMATS:
        return b"", None
    n = b[1] | b[2] << 8
    return b[3:3 + n][:-4], PHY_FORMATS[b[0]]


RECEIVERS = {
    "ieee802_11": ("wifi_rx_ieee802_11", "decode_mac", frame_ieee802_11),
    "ieee80211": ("wifi_rx_ieee80211", "decode", frame_ieee80211),
}


class Counter(gr.basic_block):
    def __init__(self, parse):
        gr.basic_block.__init__(self, name="frame_counter", in_sig=None, out_sig=None)
        self.parse = parse
        self.lock = threading.Lock()
        self.kinds = collections.Counter()
        self.formats = collections.Counter()
        self.snr = []
        self.message_port_register_in(pmt.intern("in"))
        self.set_msg_handler(pmt.intern("in"), self.handle)

    def handle(self, msg):
        meta, blob = pmt.car(msg), pmt.cdr(msg)
        if not pmt.is_u8vector(blob):
            return
        frame, fmt = self.parse(pmt.u8vector_elements(blob))
        if len(frame) < 2:
            return
        ftype, sub = (frame[0] >> 2) & 3, (frame[0] >> 4) & 0xF
        with self.lock:
            self.kinds[NAMES.get((ftype, sub), f"type {ftype}/{sub}")] += 1
            if fmt:
                self.formats[fmt] += 1
            snr = pmt.dict_ref(meta, pmt.intern("snr"), pmt.PMT_NIL)
            if pmt.is_real(snr):
                self.snr.append(pmt.to_double(snr))


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().split("\n\n")[0])
    ap.add_argument("-r", "--receiver", choices=sorted(RECEIVERS), default="ieee802_11")
    ap.add_argument("-f", "--freq", type=float, default=5180e6, help="center frequency, Hz")
    ap.add_argument("-g", "--gain", type=float, default=14, help="RF gain, dB")
    ap.add_argument("--agc", action="store_true", help="the chip's AGC instead of a held gain")
    ap.add_argument("-t", "--seconds", type=float, default=20)
    ap.add_argument("--min-frames", type=int, default=1, help="fail with fewer frames than this")
    ap.add_argument("--upstream-log", metavar="FILE", help="the receiver's own stdout, to FILE")
    a = ap.parse_args()

    name, decoder, parse = RECEIVERS[a.receiver]
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as out:
        r = subprocess.run(["grcc", "-o", out, os.path.join(here, name + ".grc")],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("grcc failed:\n" + r.stdout + r.stderr)
        spec = importlib.util.spec_from_file_location(name, os.path.join(out, name + ".py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

    tb = getattr(mod, name)(freq=a.freq, gain=a.gain, agc=int(a.agc))
    count = Counter(parse)
    tb.msg_connect((getattr(tb, decoder), "out"), (count, "in"))

    # gr-ieee80211 prints a line per packet on stdout
    sys.stdout.flush()
    saved = os.dup(1)
    devnull = os.open(a.upstream_log or os.devnull, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.dup2(devnull, 1)
    try:
        tb.start()
        time.sleep(a.seconds)
        tb.stop()
        tb.wait()
    finally:
        os.dup2(saved, 1)
        os.close(saved)
        os.close(devnull)

    total = sum(count.kinds.values())
    print(f"{a.receiver}, {a.freq / 1e6:.1f} MHz, {a.seconds:g} s: {total} frames with a good FCS "
          f"({total / a.seconds:.1f}/s)")
    for kind, n in count.kinds.most_common():
        print(f"  {kind:14} {n}")
    if count.formats:
        print("  PHY: " + ", ".join(f"{f} {n}" for f, n in count.formats.most_common()))
    if count.snr:
        s = sorted(count.snr)
        print(f"  SNR dB: min {s[0]:.1f}, median {s[len(s) // 2]:.1f}, max {s[-1]:.1f}")
    if total < a.min_frames:
        sys.exit(f"FAIL: fewer than {a.min_frames} frames")
    print("PASS")


if __name__ == "__main__":
    main()
