#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Two-path check for an 8822C dual-path stream (wsdr_cu stream --path ab): path A in PREFIX.cs16,
# path B in PREFIX-b.cs16. Finds frame bursts by power, then per burst estimates the A/B delay
# (cross-correlation peak, parabolic interpolation) and the coherence |<a b*>| / sqrt(<|a|^2><|b|^2>)
# after aligning, against the same measure in noise-only stretches.
import argparse, json
import numpy as np

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--max-bursts", type=int, default=200)
    a = ap.parse_args()
    meta = json.load(open(a.prefix + ".json"))
    fs = meta["sample_rate"]
    def load(p):
        r = np.fromfile(p, dtype="<i2").astype(np.float32).reshape(-1, 2)
        return r[:, 0] + 1j * r[:, 1]
    xa, xb = load(a.prefix + ".cs16"), load(a.prefix + "-b.cs16")
    n = min(len(xa), len(xb))
    xa, xb = xa[:n] - xa[:n].mean(), xb[:n] - xb[:n].mean()
    win = max(16, int(20e-6 * fs))
    k = n // win
    pw = (np.abs(xa[:k * win]) ** 2).reshape(k, win).mean(1)
    floor = np.median(pw)
    hot = pw > 10 * floor
    quiet = pw < 1.5 * floor
    def coh(sa, sb):
        return float(np.abs(np.vdot(sb, sa)) / np.sqrt(np.vdot(sa, sa).real * np.vdot(sb, sb).real))
    lags, cohs, phases = [], [], []
    i = 0
    while i < k and len(lags) < a.max_bursts:
        if not hot[i]:
            i += 1
            continue
        j = i
        while j < k and hot[j]:
            j += 1
        if j - i >= 3:
            sa, sb = xa[i * win:j * win], xb[i * win:j * win]
            L = 16
            c = np.array([np.abs(np.vdot(np.roll(sb, s)[L:-L], sa[L:-L])) for s in range(-L, L + 1)])
            m = int(np.argmax(c))
            frac = 0.0
            if 0 < m < 2 * L:
                y0, y1, y2 = c[m - 1], c[m], c[m + 1]
                frac = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
            s = m - L
            lags.append(s + frac)
            sbr = np.roll(sb, s)[L:-L]
            cohs.append(coh(sa[L:-L], sbr))
            phases.append(float(np.angle(np.vdot(sbr, sa[L:-L]))))
        i = j
    qa = np.concatenate([xa[q * win:(q + 1) * win] for q in np.flatnonzero(quiet)[:2000]])
    qb = np.concatenate([xb[q * win:(q + 1) * win] for q in np.flatnonzero(quiet)[:2000]])
    out = {"file": a.prefix, "fs": fs, "bursts": len(lags),
           "lag_samples_median": float(np.median(lags)) if lags else None,
           "lag_ns_median": float(np.median(lags) / fs * 1e9) if lags else None,
           "lag_samples_iqr": [float(np.percentile(lags, 25)), float(np.percentile(lags, 75))] if lags else None,
           "coherence_burst_median": float(np.median(cohs)) if cohs else None,
           "coherence_burst_p10": float(np.percentile(cohs, 10)) if cohs else None,
           "coherence_noise": coh(qa, qb),
           "rms_a": float(np.sqrt(np.mean(np.abs(xa) ** 2)) / 16), "rms_b": float(np.sqrt(np.mean(np.abs(xb) ** 2)) / 16)}
    print(json.dumps(out, indent=1))

if __name__ == "__main__":
    main()
