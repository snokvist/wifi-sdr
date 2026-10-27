#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Two-path check for an 8822B dual-path stream (wsdr_probe stream --dma 4 --path ab --rate 0|1|2): path
# A in PREFIX.cs16, path B in PREFIX-b.cs16, at the rate in PREFIX.json (80, 40 or 20 MS/s; 80 only
# --gapped, where stretches that cross a bank join count against the coherence).
# 1. L-LTFs found on each path on its own, per 20 MHz channel in the span (and between two as a
#    control), with how many land on both within 2 us, and conj(x) as the
#    orientation control.
# 2. A/B coherence (cross-spectral, median over the band) on frames, against noise-only stretches of
#    the same lengths (short stretches are biased upward, so only like-for-like compares).
# 3. A minus B level per frame: two antennas fade independently, a copied path would not.
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from la_analyze import ltf_hits  # noqa: E402

FS = 40e6  # set from PREFIX.json


def load(p):
    r = np.fromfile(p, dtype="<i2").astype(np.float32).reshape(-1, 2)
    return r[:, 0] + 1j * r[:, 1]


def ltf_check(A, B, offsets=(-10, 10, 0), chunk=1 << 20):
    n = np.arange(129) - 64
    h = 2 * 9e6 / FS * np.sinc(2 * 9e6 / FS * n) * np.hamming(129)
    h /= h.sum()
    res = {}
    dec = int(FS // 20e6)  # to 20 MS/s
    for off in offsets:
        t_hits = {"A": [], "B": []}
        conj = {"A": 0, "B": 0}
        for c0 in range(0, len(A) - chunk + 1, chunk):
            mix = np.exp(-2j * np.pi * off * 1e6 * (np.arange(chunk) + c0) / FS)
            for nm, x in (("A", A), ("B", B)):
                s = x[c0:c0 + chunk]
                s = (s - s.mean()) * mix
                y = np.convolve(s, h, mode="same")[::dec] if dec > 1 else s
                t_hits[nm] += [c0 / FS + v["idx"] / 20e6 for v in ltf_hits(y, 20.0, 0.6)[0]]
                conj[nm] += len(ltf_hits(np.conj(y), 20.0, 0.6)[0])
        tb = np.array(sorted(t_hits["B"]))
        both = sum(1 for x in t_hits["A"] if len(tb) and np.min(np.abs(tb - x)) < 2e-6)
        res[f"{off:+d} MHz"] = {"A": len(t_hits["A"]), "B": len(t_hits["B"]), "both": both,
                                "conj_A": conj["A"], "conj_B": conj["B"]}
    return res


def coherence_check(A, B):
    nfft = round(6.4e-6 * FS)  # 6.4 us segments at every rate
    win = round(4e-6 * FS)  # 4 us power windows
    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / FS)) / 1e6
    band = (np.abs(f) < 0.45 * FS / 1e6) & (np.abs(f) > 0.5)

    def coh(a, b):
        m = (len(a) // nfft) * nfft
        Fa = np.fft.fft(a[:m].reshape(-1, nfft) * np.hanning(nfft), axis=1)
        Fb = np.fft.fft(b[:m].reshape(-1, nfft) * np.hanning(nfft), axis=1)
        X = np.fft.fftshift((Fa * np.conj(Fb)).mean(0))
        pa = np.fft.fftshift((np.abs(Fa) ** 2).mean(0))
        pb = np.fft.fftshift((np.abs(Fb) ** 2).mean(0))
        return float(np.median((np.abs(X) / np.sqrt(pa * pb))[band])), float(10 * np.log10(pa[band].sum() / pb[band].sum()))

    k = len(A) // win
    pa = (np.abs(A[:k * win]) ** 2).reshape(k, win).mean(1)
    pb = (np.abs(B[:k * win]) ** 2).reshape(k, win).mean(1)
    fa, fb = np.median(pa), np.median(pb)
    frames, lens = [], []
    i = 0
    while i < k:
        if pa[i] < 10 * fa:
            i += 1
            continue
        j = i
        while j < k and pa[j] > 10 * fa:
            j += 1
        if j - i >= 4:  # >= 16 us
            frames.append(coh(A[i * win:j * win], B[i * win:j * win]))
            lens.append(j - i)
        i = j
    quiet = (pa < 1.3 * fa) & (pb < 1.3 * fb)
    rng = np.random.default_rng(1)
    noise = []
    for L in lens:
        for _ in range(50):
            s = int(rng.integers(0, k - L))
            if quiet[s:s + L].all():
                noise.append(coh(A[s * win:(s + L) * win], B[s * win:(s + L) * win])[0])
                break
    c = np.array([x[0] for x in frames])
    d = np.array([x[1] for x in frames])
    pct = lambda v, q: round(float(np.percentile(v, q)), 2)  # noqa: E731
    return {"frames": len(c), "coh_frames_median": pct(c, 50), "coh_frames_p10": pct(c, 10),
            "noise_stretches": len(noise), "coh_noise_median": pct(noise, 50), "coh_noise_p90": pct(noise, 90),
            "a_minus_b_db_p10_p50_p90": [pct(d, 10), pct(d, 50), pct(d, 90)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    a = ap.parse_args()
    global FS
    with open(a.prefix + ".json") as j:
        FS = float(json.load(j)["sample_rate"])
    if FS not in (20e6, 40e6, 80e6):
        sys.exit("bu_twopath: 80, 40 or 20 MS/s only")
    A, B = load(a.prefix + ".cs16"), load(a.prefix + "-b.cs16")
    n = min(len(A), len(B))
    A, B = A[:n] - A[:n].mean(), B[:n] - B[:n].mean()
    offsets = {80e6: (-30, -10, 10, 30, 0), 40e6: (-10, 10, 0)}.get(FS, (0,))
    out = {"file": os.path.basename(a.prefix), "fs": FS, "seconds": round(n / FS, 3),
           "ltf": ltf_check(A, B, offsets), "coherence": coherence_check(A, B)}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
