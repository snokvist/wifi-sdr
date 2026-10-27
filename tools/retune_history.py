#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Retune history through the SoapySDR "wsdr" module: visit frequencies (MHz) in order in one session
and print one JSON row per visit (floor, watched lines). Visiting a target from different predecessors
shows whether it inherits register state from the channel before.
Usage: SOAPY_SDR_PLUGIN_PATH=build-soapy retune_history.py --args driver=wsdr,bus=6 --rate 40e6 F1 F2 ...
The library's own JSON events share stdout: keep the rows with grep '"to"'."""
import argparse
import json
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_RX

ap = argparse.ArgumentParser()
ap.add_argument("--args", default="driver=wsdr")
ap.add_argument("--rate", type=float, default=40e6)
ap.add_argument("--gain", type=float, default=30)
ap.add_argument("--settle", type=float, default=0.5)
ap.add_argument("--nfft", type=int, default=8192)
ap.add_argument("--frames", type=int, default=300)
ap.add_argument("--watch", default="5346.26,5360,5400,5440,5480,5494.72")
ap.add_argument("freqs", nargs="+", type=float)
a = ap.parse_args()
watch = [float(x) for x in a.watch.split(",")]

dev = SoapySDR.Device(a.args)
dev.setSampleRate(SOAPY_SDR_RX, 0, a.rate)
dev.setGainMode(SOAPY_SDR_RX, 0, False)
dev.setGain(SOAPY_SDR_RX, 0, a.gain)
dev.setDCOffsetMode(SOAPY_SDR_RX, 0, True)
st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
dev.activateStream(st)
buf = np.empty(2 * a.nfft, dtype=np.int16)
w = np.hanning(a.nfft)
prev = None


def read_frame():
    got = errors = 0
    while got < a.nfft:
        sr = dev.readStream(st, [buf[2 * got :]], a.nfft - got, timeoutUs=500000)
        if sr.ret > 0:
            got += sr.ret
            errors = 0
        else:  # a timeout or an overflow: start the frame again, so no frame spans a gap
            got = 0
            errors += 1
            if errors >= 20:  # ~10 s without samples: the adapter is gone or wedged
                raise SystemExit(f"readStream failed {errors} times in a row (last {sr.ret})")
    x = buf.astype(np.float32).reshape(-1, 2)
    return x[:, 0] + 1j * x[:, 1]


for fq in a.freqs:
    # stop, retune, start: activating empties the module's ring, so no sample from the previous
    # frequency reaches this row; then drop a settle time (the restart, AGC/DC loops)
    dev.deactivateStream(st)
    dev.setFrequency(SOAPY_SDR_RX, 0, fq * 1e6)
    dev.activateStream(st)
    t0 = time.time()
    while time.time() - t0 < a.settle:
        read_frame()
    acc = np.zeros(a.nfft)
    for _ in range(a.frames):
        z = read_frame()
        z = z - z.mean()
        acc += np.abs(np.fft.fftshift(np.fft.fft(z * w))) ** 2
    db = 10 * np.log10(acc / a.frames + 1e-12)
    f = fq + np.fft.fftshift(np.fft.fftfreq(a.nfft, 1 / a.rate)) / 1e6
    inner = (np.abs(f - fq) < 0.4 * a.rate / 1e6) & (np.abs(f - fq) > 0.7)
    floor = float(np.median(db[inner]))
    seen = {}
    for wf in watch:
        sel = np.abs(f - wf) <= max(0.08, a.rate / a.nfft / 1e6)  # at least the nearest bin
        if abs(wf - fq) < 0.4 * a.rate / 1e6 and abs(wf - fq) > 0.7:
            seen[wf] = [round(float(db[sel].max() - floor), 1), round(float(db[sel].max()), 1)]  # [snr, abs]
    print(json.dumps({"from": prev, "to": fq, "floor_db": round(floor, 1), "watch": seen}), flush=True)
    prev = fq

dev.deactivateStream(st)
dev.closeStream(st)
