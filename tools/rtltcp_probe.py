#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Minimal rtl_tcp client for testing wsdr_tcp (or any rtl_tcp server).

Connects, checks the 12-byte "RTL0" header, sends sample rate / gain / frequency like SDR++ does,
records SECONDS of u8 IQ and reports the delivered sample rate. Optional mid-capture commands
(--then T:freq=HZ / T:gain=DB / T:auto) exercise retune and gain changes. --out PREFIX writes
PREFIX.cs16 (u8 - 127.5, x16) + PREFIX.json so tools/stream_check.py can check the beacon timing of
exactly what a client received.
Usage: rtltcp_probe.py [--host 127.0.0.1] [--port 1234] [--freq 5180e6] [--rate 2.4e6] [--gain 30]
                       [--seconds 10] [--then 5:freq=5200e6] [--out PREFIX]
"""
import argparse
import json
import socket
import struct
import time

import numpy as np


def cmd(s, c, v):
    s.sendall(struct.pack(">BI", c, int(v) & 0xFFFFFFFF))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--freq", type=float, default=5180e6)
    ap.add_argument("--rate", type=float, default=2.4e6)
    ap.add_argument("--gain", type=float, default=30)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--then", action="append", default=[])
    ap.add_argument("--out")
    a = ap.parse_args()

    s = socket.create_connection((a.host, a.port), timeout=5)
    hdr = s.recv(12, socket.MSG_WAITALL)
    magic, tuner, ngain = hdr[:4], *struct.unpack(">II", hdr[4:12])
    assert magic == b"RTL0", hdr
    cmd(s, 0x02, a.rate)
    cmd(s, 0x03, 1)
    cmd(s, 0x04, round(a.gain * 10))
    cmd(s, 0x01, a.freq)
    then = []
    for t in a.then:
        when, what = t.split(":", 1)
        then.append((float(when), what))
    then.sort()

    want = int(a.seconds * a.rate) * 2
    buf = bytearray()
    t0 = time.monotonic()
    first = None
    marks = []
    s.settimeout(2)
    while len(buf) < want:
        el = time.monotonic() - t0
        while then and el >= then[0][0]:
            _, what = then.pop(0)
            if what.startswith("freq="):
                cmd(s, 0x01, float(what[5:]))
            elif what.startswith("gain="):
                cmd(s, 0x03, 1)
                cmd(s, 0x04, round(float(what[5:]) * 10))
            elif what == "auto":
                cmd(s, 0x03, 0)
            marks.append({"t": round(el, 3), "cmd": what, "byte": len(buf)})
        chunk = s.recv(1 << 16)
        if not chunk:
            break
        if first is None:
            first = time.monotonic()
        buf += chunk
    t1 = time.monotonic()
    s.close()
    n = len(buf) // 2
    iq = np.frombuffer(bytes(buf[: 2 * n]), dtype=np.uint8).astype(np.float32) - 127.5
    rms = float(np.sqrt(np.mean(iq ** 2) * 2))
    clip = float(np.mean((iq <= -127.5) | (iq >= 127.5)))
    span = t1 - (first or t0)
    res = {
        "tuner": tuner, "gains": ngain, "samples": n, "seconds_wall": round(span, 3),
        "rate_req": a.rate, "rate_delivered": round(n / span) if span > 0 else 0,
        "first_byte_ms": round(((first or t1) - t0) * 1e3, 1), "rms_u8": round(rms, 2),
        "clip_frac": clip, "marks": marks,
    }
    print(json.dumps(res))
    if a.out:
        (iq * 16).round().astype("<i2").tofile(a.out + ".cs16")
        json.dump({"sample_rate": a.rate, "source": "rtl_tcp", "probe": res}, open(a.out + ".json", "w"))


if __name__ == "__main__":
    main()
