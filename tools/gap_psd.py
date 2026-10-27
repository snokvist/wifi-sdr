#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""PSD of wsdr captures on an absolute frequency axis: floor, band shape, strong narrow lines.
Usage: gap_psd.py --png OUT.png PREFIX:CENTRE_MHZ [...]
Blocks of --nfft samples are FFT'd independently; with the default 8192 (one bank) that also works
for gapped captures."""
import argparse
import json

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("caps", nargs="+")
ap.add_argument("--png")
ap.add_argument("--nfft", type=int, default=8192)
ap.add_argument("--banks", type=int, default=600)
ap.add_argument("--watch", default="5346.26,5320,5360,5400,5440,5480,5500")
a = ap.parse_args()
watch = [float(x) for x in a.watch.split(",")]

rows = []
for spec in a.caps:
    prefix, fc = spec.rsplit(":", 1)
    fc = float(fc)
    meta = json.load(open(prefix + ".json"))
    fs = meta["sample_rate"]
    res = meta.get("result", {})  # gapped: far fewer samples than the wall time holds
    gapped = res.get("samples", 0) < 0.5 * fs * res.get("wall_s", 0)
    if gapped and a.nfft != 8192:
        raise SystemExit(f"{prefix}: gapped capture - --nfft must be one bank (8192)")
    raw = np.fromfile(prefix + ".cs16", dtype=np.int16)
    n = min(len(raw) // (2 * a.nfft), a.banks)
    if n == 0:
        raise SystemExit(f"{prefix}: shorter than one --nfft block")
    x = raw[: n * 2 * a.nfft].astype(np.float32).reshape(n, a.nfft, 2) / 16.0
    z = x[..., 0] + 1j * x[..., 1]
    z -= z.mean(axis=1, keepdims=True)
    w = np.hanning(a.nfft).astype(np.float32)
    p = np.mean(np.abs(np.fft.fftshift(np.fft.fft(z * w, axis=1), axes=1)) ** 2, axis=0)
    db = 10 * np.log10(p + 1e-12)
    f = fc + np.fft.fftshift(np.fft.fftfreq(a.nfft, 1 / fs)) / 1e6
    inner = np.abs(f - fc) < 0.4 * fs / 1e6
    centre = np.abs(f - fc) < 0.05  # DC residue
    floor = np.median(db[inner & ~centre])
    # band shape: median level in 2 MHz slices (null where the span does not reach)
    shape = [None if abs(o) + 1 > fs / 2e6 else round(float(np.median(db[(f >= fc + o - 1) & (f < fc + o + 1)]) - floor), 1)
             for o in (-18, -12, -6, 6, 12, 18)]
    # narrow lines: > floor + 10 dB vs a 200-bin running median
    k = 201
    pad = np.pad(db, k // 2, mode="edge")
    med = np.median(np.lib.stride_tricks.sliding_window_view(pad, k), axis=1)
    lines = []
    above = (db - med > 10) & inner & ~centre
    i = 0
    while i < len(db):
        if above[i]:
            j = i
            while j < len(db) and above[j]:
                j += 1
            m = i + int(np.argmax(db[i:j]))
            lines.append((round(float(f[m]), 3), round(float(db[m] - med[m]), 1)))
            i = j
        else:
            i += 1
    seen = {}
    for wf in watch:
        if abs(wf - fc) < 0.4 * fs / 1e6:
            sel = np.abs(f - wf) <= max(0.05, fs / a.nfft / 1e6)  # at least the nearest bin
            seen[wf] = round(float(np.max(db[sel] - med[sel])), 1)
    rows.append((prefix, fc, f, db, floor))
    print(json.dumps({"cap": prefix.split("/")[-1], "fc": fc, "floor_db": round(float(floor), 1),
                      "shape_db(-18,-12,-6,+6,+12,+18)": shape,
                      "lines": sorted(lines, key=lambda t: -t[1])[:8], "watch": seen}))

if a.png:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(16, 6))
    for prefix, fc, f, db, floor in rows:
        ax.plot(f, db, lw=0.6, label=f"{prefix.split('/')[-1]} ({fc:.0f})")
    for wf in watch:
        ax.axvline(wf, color="k", lw=0.4, ls=":")
    ax.set_xlabel("MHz")
    ax.set_ylabel("dB (arb)")
    ax.legend(fontsize=7, ncol=4)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(a.png, dpi=110)
