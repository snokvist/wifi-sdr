#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# A-vs-B delay per frame for an 8822B dual-path stream (wsdr_probe stream --dma 4 --path ab --rate 1:
# PREFIX.cs16 / PREFIX-b.cs16 at 40 MS/s). For frames whose L-LTF is found on both paths, per 20 MHz
# half: the channel estimates H_A, H_B on the 52 subcarriers, then the delay tau that maximises
# |sum_k H_A conj(H_B) e^(-j 2 pi k tau / 64)| (no phase unwrapping); the peak height (0..1) says how
# well a pure delay explains the A/B difference. A fixed hardware skew shows as one tight cluster
# across many transmitters. Negative = path B leads path A.
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from la_analyze import ltf_hits
FS = 40e6
L = np.zeros(64)
seq = [1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,1,1,-1,-1,1,1,-1,1,-1,1,1,1,1,0,1,-1,-1,1,1,-1,1,-1,1,-1,-1,-1,-1,-1,1,1,-1,-1,1,-1,1,-1,1,1,1,1]
ks = np.arange(-26, 27)
for k, v in zip(ks, seq): L[k % 64] = v
used = ks[ks != 0]
def load(p):
    r = np.fromfile(p, dtype="<i2").astype(np.float32).reshape(-1, 2); return r[:, 0] + 1j * r[:, 1]
A, B = load(sys.argv[1] + ".cs16"), load(sys.argv[1] + "-b.cs16")
n = np.arange(129) - 64; h = 2 * 9e6 / FS * np.sinc(2 * 9e6 / FS * n) * np.hamming(129); h /= h.sum()
CH = 1 << 20
for off in (-10, 10):
    rows = []
    for c0 in range(0, len(A) - CH + 1, CH):
        mix = np.exp(-2j * np.pi * off * 1e6 * (np.arange(CH) + c0) / FS)
        ya = np.convolve((A[c0:c0+CH] - A[c0:c0+CH].mean()) * mix, h, mode="same")[::2]
        yb = np.convolve((B[c0:c0+CH] - B[c0:c0+CH].mean()) * mix, h, mode="same")[::2]
        hb = [v["idx"] for v in ltf_hits(yb, 20.0, 0.6)[0]]
        for v in ltf_hits(ya, 20.0, 0.6)[0]:
            i = v["idx"]
            if not hb or min(abs(np.array(hb) - i)) > 40 or i + 128 > len(ya): continue
            Ha = (np.fft.fft(ya[i:i+64]) + np.fft.fft(ya[i+64:i+128]))[used % 64] / L[used % 64]
            Hb = (np.fft.fft(yb[i:i+64]) + np.fft.fft(yb[i+64:i+128]))[used % 64] / L[used % 64]
            R = Ha * np.conj(Hb)
            taus = np.arange(-16, 16.001, 0.02)  # samples at 20 MS/s; + = B lags A
            E = np.exp(-2j * np.pi * np.outer(taus, used) / 64)
            m = np.abs(E @ R) / np.abs(R).sum()
            j = int(np.argmax(m)); tau = taus[j]
            ph = np.degrees(np.angle((E[j] * R).sum()))
            rows.append((tau / 20e6 * 1e9, ph, m[j], 20 * np.log10(np.abs(Ha).mean() / np.abs(Hb).mean())))
    r = np.array(rows)
    if not len(r): print(off, "no frames"); continue
    good = r[r[:, 2] > 0.8]
    print(f"{off:+d} MHz: {len(r)} frames on both; fit quality median {np.median(r[:,2]):.2f}; delay ns (all) median {np.median(r[:,0]):+.1f} IQR {np.percentile(r[:,0],25):+.1f}..{np.percentile(r[:,0],75):+.1f}")
    if len(good):
        print(f"   quality > 0.8: {len(good)} frames, delay ns median {np.median(good[:,0]):+.1f} IQR {np.percentile(good[:,0],25):+.1f}..{np.percentile(good[:,0],75):+.1f}; histogram (25 ns bins): "
              + str(dict(zip(*np.unique((np.round(good[:,0] / 25) * 25).astype(int), return_counts=True)))))
