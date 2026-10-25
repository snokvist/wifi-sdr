#!/usr/bin/env python3
"""Phase 1 summary figure for one chip: off-centre tune test.

Receiver tuned to ch37 (5185 MHz), 40 Msps LA capture, ambient AP on ch36 (5180 MHz) => the AP must
appear at -5 MHz. Panel 1: PSD of the burst samples vs noise with the predicted occupied band
(-5 +/- 8.3 MHz) shaded and its mirror (+5) outlined. Panel 2: peak L-LTF correlation vs the
frequency shift applied before correlating (a correct, non-inverted capture peaks at -5 only).

    phase1_figure.py OUT.png TITLE CAP1.bin [CAP2.bin ...] [--paths a,b]
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import la_analyze as L  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("title")
ap.add_argument("caps", nargs="+")
ap.add_argument("--paths", default="a")
ap.add_argument("--expect-mhz", type=float, default=-5.0)
a = ap.parse_args()

import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

shifts = np.arange(-12, 12.5, 0.5)
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 4.8))
for cap in a.caps:
    meta, w = L.read_dvla(cap)
    fs = meta["fs_mhz"]
    for p in a.paths.split(","):
        x, _ = L.unpack(w, p)
        x = x - x.mean()
        n = np.arange(len(x))
        tag = f"{os.path.basename(cap).replace('.bin', '')} {p.upper()}"
        on, segs, _, _ = L.bursts(x, fs)
        if segs:
            sig = np.concatenate([x[s:e] for s, e in segs])
            f, ps = L.welch(sig, 256)
            ax1.plot(f * fs, 10 * np.log10(ps + 1e-9), lw=0.8, label=tag)
        cm = []
        for sh in shifts:
            y = x * np.exp(-2j * np.pi * sh / fs * n)
            c = L.ncorr(y, L.ltf_symbol(fs))
            cm.append(c.max() if len(c) else 0)
        ax2.plot(shifts, cm, marker=".", lw=0.8, label=tag)
e = a.expect_mhz
ax1.axvspan(e - 8.3, e + 8.3, color="g", alpha=0.12, label=f"predicted AP band ({e:+g} MHz)")
ax1.axvspan(-e - 8.3, -e + 8.3, fill=False, ls="--", ec="r", label="mirror (if inverted)")
ax1.set(xlabel="MHz from tuned centre", ylabel="dB", title="PSD of detected bursts")
ax1.legend(fontsize=7)
ax2.axvline(e, color="g", ls="--", lw=0.8)
ax2.axvline(-e, color="r", ls=":", lw=0.8)
ax2.set(xlabel="shift applied before L-LTF correlation (MHz)", ylabel="peak normalized corr",
        title="L-LTF detection vs frequency shift", ylim=(0, 1))
ax2.legend(fontsize=7)
fig.suptitle(a.title)
fig.tight_layout()
fig.savefig(a.out, dpi=110)
print(a.out)
