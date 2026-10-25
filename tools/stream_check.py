#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline continuity check of a wsdr_probe stream capture (PREFIX.cs16 + PREFIX.json).

Independent of the streamer's own framing checks, this asks whether the samples form one
continuous time series:
  * duplicate banks      no 8192-sample bank is byte-identical to another (replayed data)
  * ring-lag identity    fraction of samples equal to the sample one LA ring (16384) earlier;
                         a frozen/stale ring reads ~1.0, live noise ~0.0x
  * boundary steps       |x[n] - x[n-1]| at bank (8192) and chunk boundaries vs everywhere;
                         a splice of non-adjacent data would raise the boundary mean
  * beacon timing        burst onsets of the strongest periodic emitter; intervals should be
                         integer multiples of 102.4 ms (one TU x 100) = 256000 samples at 2.5 MS/s.
                         A dropped or repeated bank shifts every later beacon by 8192 samples (a
                         persistent step, counted as a slip); a deferred beacon is a lone outlier.
Usage: stream_check.py PREFIX [--json OUT]
"""
import argparse
import json

import numpy as np

BANK = 8192
CHUNKS = [16, 16 + 2044, 16 + 2 * 2044, 16 + 3 * 2044]  # sample offsets of chunk starts (128 B prefix)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--json")
    a = ap.parse_args()
    meta = json.load(open(a.prefix + ".json"))
    fs = meta["sample_rate"]
    raw = np.fromfile(a.prefix + ".cs16", dtype="<i2").reshape(-1, 2)
    x = (raw[:, 0].astype(np.float32) + 1j * raw[:, 1].astype(np.float32)) / 16.0
    n = len(x) // BANK * BANK
    x, raw = x[:n], raw[:n]
    nb = n // BANK
    out = {"file": a.prefix, "fs": fs, "samples": n, "banks": nb, "seconds": n / fs}

    # duplicate banks
    packed = raw.view("<u4").reshape(nb, BANK)
    hashes = {}
    dups = 0
    for i in range(nb):
        h = hash(packed[i].tobytes())
        dups += h in hashes
        hashes[h] = i
    out["duplicate_banks"] = int(dups)

    # ring-lag identity
    lag = 2 * BANK
    out["ring_lag_identical"] = float(np.mean(packed.reshape(-1)[lag:] == packed.reshape(-1)[:-lag]))
    out["adjacent_identical"] = float(np.mean(packed.reshape(-1)[1:] == packed.reshape(-1)[:-1]))

    # boundary steps
    d = np.abs(np.diff(x))
    idx = np.arange(1, n)
    pos = idx % BANK
    out["step_mean_all"] = float(d.mean())
    out["step_mean_bank_boundary"] = float(d[pos == 0].mean())
    out["step_mean_chunk_boundary"] = float(d[np.isin(pos, CHUNKS[1:])].mean())
    out["step_mean_prefix_boundary"] = float(d[pos == CHUNKS[0]].mean())

    # beacon timing: 20 us power envelope, onsets above a robust threshold
    w = max(1, int(round(fs * 20e-6)))
    p = np.abs(x[: n // w * w]) ** 2
    env = p.reshape(-1, w).mean(axis=1)
    floor = np.median(env)
    thr = floor * 10 ** (12 / 10)  # 12 dB over the median floor
    hot = env > thr
    on = np.flatnonzero(hot[1:] & ~hot[:-1]) + 1
    on = on[np.diff(np.concatenate([[-10**9], on])) > int(2e-3 / 20e-6)]  # >= 2 ms apart
    onset_s = on * w
    out["bursts"] = int(len(onset_s))
    tu100 = 0.1024 * fs
    best = None
    if len(onset_s) > 4:
        # The beacon train is the set of onsets whose pairwise spacing is ~k * 102.4 ms. Pick the
        # phase (mod 102.4 ms) with the most onsets, then measure their exact spacings.
        ph = np.mod(onset_s, tu100)
        hist, edges = np.histogram(ph, bins=512, range=(0, tu100))
        k = int(np.argmax(np.convolve(hist, np.ones(3), mode="same")))
        centre = (edges[k] + edges[k + 1]) / 2
        dist = np.abs(((ph - centre + tu100 / 2) % tu100) - tu100 / 2)
        train = onset_s[dist < 0.5e-3 * fs]
        if len(train) > 2:
            iv = np.diff(train)
            m = np.round(iv / tu100)
            resid = iv - m * tu100
            # Residual of each beacon against the TBTT grid, with the AP/our clock drift removed. A
            # dropped or repeated bank shifts every later beacon by a bank (persistent step); a beacon
            # deferred by a busy medium is a single outlier.
            k = np.round((train - train[0]) / tu100)
            g = (train - train[0]) - k * tu100
            drift = np.polyfit(k, g, 1) if len(k) > 3 else [0.0, 0.0]
            g = g - np.polyval(drift, k)
            steps = [i for i in range(3, len(g) - 2)
                     if abs(np.median(g[i:i + 3]) - np.median(g[i - 3:i])) > BANK / 2]
            best = {
                "beacons": int(len(train)),
                "interval_tu100_multiples": [int(v) for v in np.unique(m)],
                "resid_samples_median": float(np.median(resid)),
                "resid_samples_max_abs": float(np.max(np.abs(resid))),
                "resid_us_max_abs": float(np.max(np.abs(resid)) / fs * 1e6),
                "clock_drift_ppm": float(drift[0] / tu100 * 1e6),
                "grid_outliers_over_half_bank": int(np.sum(np.abs(g) > BANK / 2)),
                "bank_slips": len(steps),
            }
    out["beacon_train"] = best
    print(json.dumps(out, indent=1))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
