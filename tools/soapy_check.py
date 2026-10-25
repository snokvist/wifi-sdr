#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Headless check of the SoapySDR "wsdr" module on hardware.

Opens driver=wsdr, prints the advertised capabilities, streams CS16 at 5180 MHz while it exercises a
retune (5200 MHz and back) and a gain change, then closes and re-opens to test teardown. Writes the
uninterrupted first segment as PREFIX.cs16 + PREFIX.json for tools/stream_check.py.
Usage: SOAPY_SDR_PLUGIN_PATH=build-soapy tools/soapy_check.py [--seconds 20] [--rate 2.5e6] [--out PREFIX]
"""
import argparse
import json
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_CS16, SOAPY_SDR_OVERFLOW, SOAPY_SDR_RX, SOAPY_SDR_TIMEOUT

ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=float, default=20)
ap.add_argument("--rate", type=float, default=2.5e6)
ap.add_argument("--out")
a = ap.parse_args()


def stream(dev, seconds, events=(), keep=None):
    st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
    dev.activateStream(st)
    buf = np.empty(2 * 65536, np.int16)
    n = over = timeouts = errors = 0
    t0 = time.monotonic()
    first = None
    ev = sorted(events)
    while time.monotonic() - t0 < seconds:
        el = time.monotonic() - t0
        while ev and el >= ev[0][0]:
            _, fn, label = ev.pop(0)
            fn()
            print(f"  t={el:5.2f}s {label} (samples so far {n})")
        r = dev.readStream(st, [buf], 65536, timeoutUs=500000)
        if r.ret > 0:
            if first is None:
                first = time.monotonic() - t0
            if keep is not None:
                keep.append(buf[: 2 * r.ret].copy())
            n += r.ret
            if r.flags & SOAPY_SDR_OVERFLOW:
                over += 1
        elif r.ret == SOAPY_SDR_OVERFLOW:
            over += 1
        elif r.ret == SOAPY_SDR_TIMEOUT:
            timeouts += 1
        else:
            errors += 1
            if errors > 20:
                break
    el = time.monotonic() - t0
    dev.deactivateStream(st)
    dev.closeStream(st)
    return {"samples": n, "seconds": round(el, 3), "first_sample_s": round(first or -1, 3),
            "rate_avg": round(n / (el - (first or 0))) if el > (first or 0) else 0,
            "overflows": over, "timeouts": timeouts, "errors": errors}


res = {}
dev = SoapySDR.Device("driver=wsdr")  # a plain dict hits a python3-soapysdr 0.8.1 overload quirk
res["info"] = {
    "hw": dev.getHardwareKey(), "rates": list(dev.listSampleRates(SOAPY_SDR_RX, 0)),
    "freq_ranges": [str(r) for r in dev.getFrequencyRange(SOAPY_SDR_RX, 0)],
    "gains": list(dev.listGains(SOAPY_SDR_RX, 0)), "gain_range": str(dev.getGainRange(SOAPY_SDR_RX, 0)),
    "has_agc": dev.hasGainMode(SOAPY_SDR_RX, 0), "formats": list(dev.getStreamFormats(SOAPY_SDR_RX, 0)),
}
print(json.dumps(res["info"]))
dev.setSampleRate(SOAPY_SDR_RX, 0, a.rate)
dev.setFrequency(SOAPY_SDR_RX, 0, 5180e6)
dev.setGainMode(SOAPY_SDR_RX, 0, False)
dev.setGain(SOAPY_SDR_RX, 0, 30)
print("read back:", dev.getFrequency(SOAPY_SDR_RX, 0), dev.getSampleRate(SOAPY_SDR_RX, 0), dev.getGain(SOAPY_SDR_RX, 0))

keep = []
print("segment 1: uninterrupted capture")
res["clean"] = stream(dev, a.seconds, keep=keep)
print(json.dumps(res["clean"]))
print("segment 2: retune + gain change while streaming")
res["changes"] = stream(dev, 8, events=[
    (2, lambda: dev.setFrequency(SOAPY_SDR_RX, 0, 5200e6), "freq 5200 MHz"),
    (4, lambda: dev.setFrequency(SOAPY_SDR_RX, 0, 5180e6), "freq 5180 MHz"),
    (6, lambda: dev.setGain(SOAPY_SDR_RX, 0, 20), "gain 20 dB")])
print(json.dumps(res["changes"]))
del dev
print("segment 3: re-open after teardown")
dev = SoapySDR.Device("driver=wsdr")  # a plain dict hits a python3-soapysdr 0.8.1 overload quirk
dev.setSampleRate(SOAPY_SDR_RX, 0, a.rate)
dev.setFrequency(SOAPY_SDR_RX, 0, 5180e6)
res["reopen"] = stream(dev, 3)
print(json.dumps(res["reopen"]))
del dev

if a.out and keep:
    x = np.concatenate(keep)
    x.astype("<i2").tofile(a.out + ".cs16")
    json.dump({"sample_rate": a.rate, "source": "soapy wsdr", "result": res}, open(a.out + ".json", "w"))
print(json.dumps(res))
