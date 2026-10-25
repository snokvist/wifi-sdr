#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Live two-channel view of the 8812CU through the SoapySDR "wsdr" module (channels 0/1 = RX paths A/B).

Panels: spectrum of both paths (mean and max hold), a waterfall per path (max over the samples since
the previous update), and the A/B magnitude-squared coherence and phase across the band, accumulated
(decaying) over frame bursts only (segments >= 6 dB above the median power). The ~400 ns
B-before-A skew shows up as a linear phase slope.
Usage: SOAPY_SDR_PLUGIN_PATH=build-soapy tools/cu_live.py [--freq 5180e6] [--rate 5e6] [--gain 28]
       [--seconds N --png FILE]   (close after N s and save a snapshot)
"""
import argparse
import threading
import time

import matplotlib
matplotlib.use("QtAgg")
import matplotlib.pyplot as plt
import numpy as np
import SoapySDR
from matplotlib.animation import FuncAnimation
from SoapySDR import SOAPY_SDR_CF32, SOAPY_SDR_OVERFLOW, SOAPY_SDR_RX

ap = argparse.ArgumentParser()
ap.add_argument("--args", default="driver=wsdr,pid=0xc812")
ap.add_argument("--freq", type=float, default=5180e6)
ap.add_argument("--rate", type=float, default=5e6)
ap.add_argument("--gain", type=float, default=28)
ap.add_argument("--seconds", type=float, default=0, help="close after this long (0 = run until closed)")
ap.add_argument("--png", help="save the figure here when closing")
a = ap.parse_args()

NFFT, KEEP, ROWS = 512, 1 << 17, 200
dev = SoapySDR.Device(a.args)
dev.setSampleRate(SOAPY_SDR_RX, 0, a.rate)
dev.setFrequency(SOAPY_SDR_RX, 0, a.freq)
dev.setGainMode(SOAPY_SDR_RX, 0, False)
dev.setGain(SOAPY_SDR_RX, 0, a.gain)
fs = dev.getSampleRate(SOAPY_SDR_RX, 0)
st = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0, 1])
dev.activateStream(st)

lock = threading.Lock()
ring = np.zeros((2, KEEP), np.complex64)
stats = {"n": 0, "ovf": 0, "t0": time.monotonic()}
run = True


def reader():
    bufs = [np.empty(16384, np.complex64) for _ in range(2)]
    while run:
        r = dev.readStream(st, bufs, 16384, timeoutUs=200000)
        if r.ret > 0:
            with lock:
                ring[:, :-r.ret] = ring[:, r.ret:]
                ring[0, -r.ret:] = bufs[0][: r.ret]
                ring[1, -r.ret:] = bufs[1][: r.ret]
                stats["n"] += r.ret
            if r.flags & SOAPY_SDR_OVERFLOW:
                stats["ovf"] += 1
        elif r.ret == SOAPY_SDR_OVERFLOW:
            stats["ovf"] += 1


th = threading.Thread(target=reader, daemon=True)
th.start()

f_mhz = (a.freq + np.fft.fftshift(np.fft.fftfreq(NFFT, 1 / fs))) / 1e6
win = np.hanning(NFFT).astype(np.float32)
wnorm = np.sum(win**2)
wf = np.full((2, ROWS, NFFT), -120.0)
hold = np.full((2, NFFT), -120.0)
acc = {"ab": np.zeros(NFFT, complex), "aa": np.zeros(NFFT), "bb": np.zeros(NFFT), "segs": 0.0, "n": 0}

fig = plt.figure(figsize=(14, 9.5))
gs = fig.add_gridspec(3, 2, height_ratios=[1.1, 1.6, 1.1])
ax_s = fig.add_subplot(gs[0, :])
ax_w = [fig.add_subplot(gs[1, k]) for k in range(2)]
ax_c = fig.add_subplot(gs[2, :])
ax_p = ax_c.twinx()
l_mean = [ax_s.plot(f_mhz, np.zeros(NFFT), c, lw=1.2, label=f"path {p} mean")[0] for c, p in (("C0", "A"), ("C3", "B"))]
l_hold = [ax_s.plot(f_mhz, np.zeros(NFFT), c, lw=0.7, alpha=0.45, label=f"path {p} max hold")[0] for c, p in (("C0", "A"), ("C3", "B"))]
ax_s.set_ylim(-110, -10)
ax_s.set_xlim(f_mhz[0], f_mhz[-1])
ax_s.set_ylabel("dBFS / bin")
ax_s.legend(loc="upper right", fontsize=8, ncol=4)
ax_s.grid(alpha=0.3)
ims = []
for k, p in enumerate("AB"):
    ims.append(ax_w[k].imshow(wf[k], aspect="auto", origin="upper", vmin=-85, vmax=-15, cmap="viridis",
                              extent=[f_mhz[0], f_mhz[-1], ROWS, 0]))
    ax_w[k].set_title(f"path {p} waterfall (max per update)", fontsize=10)
    ax_w[k].set_xlabel("MHz")
ax_w[0].set_ylabel("updates ago")
l_coh, = ax_c.plot(f_mhz, np.zeros(NFFT), "C2", lw=1.2, label="|coherence|² A/B (bursts)")
l_ph, = ax_p.plot(f_mhz, np.zeros(NFFT), "C1.", ms=2, label="phase B−A (bursts)")
ax_c.set_ylim(0, 1.02)
ax_c.set_xlim(f_mhz[0], f_mhz[-1])
ax_c.set_ylabel("coherence²")
ax_c.set_xlabel("MHz")
ax_p.set_ylim(-np.pi, np.pi)
ax_p.set_ylabel("phase (rad)")
ax_c.grid(alpha=0.3)
ax_c.legend(handles=[l_coh, l_ph], loc="lower right", fontsize=8)
title = fig.suptitle("")
fig.tight_layout(rect=[0, 0, 1, 0.96])


def update(_):
    with lock:
        x = ring.copy()
        n, ovf = stats["n"], stats["ovf"]
    segs = x[:, : (KEEP // NFFT) * NFFT].reshape(2, -1, NFFT) * win
    X = np.fft.fftshift(np.fft.fft(segs, axis=2), axes=2)
    P = np.abs(X) ** 2 / wnorm / NFFT
    pdb = 10 * np.log10(P + 1e-15)
    mean = 10 * np.log10(P.mean(1) + 1e-15)
    pw = P[0].sum(1)
    new = int(np.clip((n - acc["n"]) // NFFT, 1, P.shape[1]))  # segments that arrived since the last update
    acc["n"] = n
    hot = pw > 4 * np.median(pw)
    hot[:-new] = False
    for k in range(2):
        hold[k] = np.maximum(hold[k] - 0.3, pdb[k][-new:].max(0))
        l_mean[k].set_ydata(mean[k])
        l_hold[k].set_ydata(hold[k])
        wf[k] = np.roll(wf[k], 1, axis=0)
        wf[k][0] = pdb[k][-new:].max(0)
        ims[k].set_data(wf[k])
    for key in ("ab", "aa", "bb", "segs"):
        acc[key] *= 0.97
    if hot.any():
        A, B = X[0][hot], X[1][hot]
        acc["ab"] += (B * np.conj(A)).sum(0)
        acc["aa"] += (np.abs(A) ** 2).sum(0)
        acc["bb"] += (np.abs(B) ** 2).sum(0)
        acc["segs"] += hot.sum()
    coh_txt = "waiting for frames"
    if acc["segs"] >= 8:
        c2 = np.abs(acc["ab"]) ** 2 / (acc["aa"] * acc["bb"] + 1e-30)
        l_coh.set_ydata(c2)
        l_ph.set_ydata(np.angle(acc["ab"]))
        coh_txt = f"~{acc['segs']:.0f} burst segments, median coherence² {np.median(c2):.2f}"
    el = time.monotonic() - stats["t0"]
    title.set_text(f"RTL8812CU two-path live · {a.freq / 1e6:.1f} MHz · {fs / 1e6:g} MS/s · gain {a.gain:g} dB (fixed) · "
                   f"{n / max(el, 1e-3) / 1e6:.2f} MS/s in · overflows {ovf} · {coh_txt}")
    return [*l_mean, *l_hold, *ims, l_coh, l_ph, title]


ani = FuncAnimation(fig, update, interval=150, blit=False, cache_frame_data=False)
if a.seconds:
    t = fig.canvas.new_timer(interval=int(a.seconds * 1000))
    t.single_shot = True
    t.add_callback(plt.close, fig)
    t.start()
if a.png:
    fig.canvas.mpl_connect("close_event", lambda _: fig.savefig(a.png, dpi=90))
try:
    plt.show()
finally:
    run = False
    th.join(timeout=2)
    dev.deactivateStream(st)
    dev.closeStream(st)
