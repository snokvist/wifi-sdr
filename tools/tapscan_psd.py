#!/usr/bin/env python3
"""Noise-floor width / liveness of LA ring dumps (wsdr_probe tapscan -> PREFIX-label.u64).

usage: tapscan_psd.py [--json tapscan.jsonl] [--fs 40e6] [--layout iq12|b12|hi12|a8x2|b8x2|a8|b8] [--plot out.png] FILE.u64...

Per file: decode (layout iq12: I=[11:0], Q=[23:12], 12-bit signed), median Welch PSD (Hann, 512),
reference level = median over |f| < 4 MHz (DC bin excluded), and the -3 / -10 dB edges found by walking
outward from 0 on a 5-bin smoothed PSD. Also reports adjacent-duplicate fraction and the PSD level at
+-15 MHz relative to the in-band reference (a ~20 MHz tap sits ~>20 dB down there).
"""
import argparse, json, os, sys
import numpy as np


def s12(x):
    x = (x & 0xFFF).astype(np.int64)
    return np.where(x >= 0x800, x - 0x1000, x).astype(np.float64)


def s8(v):
    v = (v & 0xFF).astype(np.int64)
    return np.where(v >= 0x80, v - 0x100, v).astype(np.float64)


def b8(w, k):
    return s8(w >> np.uint64(8 * k))


# Samples per 64-bit LA word for each layout.
SPW = {"iq12": 1, "b12": 1, "hi12": 1, "a8x2": 2, "b8x2": 2, "a8": 1, "b8": 1}


def decode(w, layout):
    """iq12: dma 0/1 (and dma 4 path A), I=[11:0] Q=[23:12]. b12: dma 4 path B, I=[35:24] Q=[47:36].
    a8x2 / b8x2: dma 11, int8 I,Q; path A = bytes 0-3, path B = bytes 4-7, two samples per word, time order (bytes 2,3) then (bytes 0,1) -> 2x the word rate.
    a8 / b8: dma 10, int8 I,Q, path A = bytes 0,1, path B = bytes 2,3, one sample per word."""
    if layout in ("a8x2", "b8x2"):
        o = 0 if layout == "a8x2" else 4
        first = b8(w, o + 2) + 1j * b8(w, o + 3)
        second = b8(w, o) + 1j * b8(w, o + 1)
        return np.stack([first, second], axis=1).reshape(-1)
    if layout in ("a8", "b8"):
        o = 0 if layout == "a8" else 2
        return b8(w, o) + 1j * b8(w, o + 1)
    if layout == "iq12":
        return s12(w) + 1j * s12(w >> 12)
    if layout == "b12":  # 8822B dma 4: path B, the same 12+12 layout 24 bits up
        return s12(w >> 24) + 1j * s12(w >> 36)
    if layout == "hi12":  # 8822C-style path B position
        return s12(w >> 44) + 1j * s12(w >> 32)
    raise SystemExit("unknown layout " + layout)


SNAP = 16384


def seg_psd(x, nseg=512, snap=SNAP):
    """Per-segment periodograms; segments never straddle a snapshot boundary (16384 words)."""
    win = np.hanning(nseg)
    segs = []
    for s0 in range(0, len(x), snap):
        xs = x[s0:s0 + snap]
        xs = xs - xs.mean()
        segs += [xs[i:i + nseg] * win for i in range(0, len(xs) - nseg + 1, nseg // 2)]
    return np.abs(np.fft.fftshift(np.fft.fft(np.array(segs), axis=1), axes=1)) ** 2


def psd_median(x, fs, nseg=512, snap=SNAP):
    f = np.fft.fftshift(np.fft.fftfreq(nseg, 1 / fs))
    return f, np.median(seg_psd(x, nseg, snap), axis=0)


def burst_psd(x, fs, nseg=512, thr_db=6.0, snap=SNAP):
    """Mean PSD of segments whose power is > thr_db above the median segment (frames on air)."""
    P = seg_psd(x, nseg, snap)
    pw = P.sum(axis=1)
    sel = pw > np.median(pw) * 10 ** (thr_db / 10)
    f = np.fft.fftshift(np.fft.fftfreq(nseg, 1 / fs))
    if sel.sum() == 0:
        return f, None, 0
    return f, P[sel].mean(axis=0) / np.median(P, axis=0), int(sel.sum())


def edges(f, p_db, ref, drop):
    n = len(f)
    c = int(np.argmin(np.abs(f)))
    lo = hi = None
    for i in range(c + 2, n):
        if p_db[i] < ref - drop:
            hi = f[i]
            break
    for i in range(c - 2, -1, -1):
        if p_db[i] < ref - drop:
            lo = f[i]
            break
    return lo, hi


def analyze(path, fs, layout):
    w = np.fromfile(path, dtype="<u8")
    x = decode(w, layout)
    spw = SPW[layout]
    fs = fs * spw
    f, p = psd_median(x, fs, 512, SNAP * spw)
    k = max(5, int(round(1.5e6 / (fs / 512))))  # ~1.5 MHz smoothing
    ps = np.convolve(p, np.ones(k) / k, mode="same")
    p_db = 10 * np.log10(ps + 1e-12)
    inb = (np.abs(f) < 4e6) & (np.abs(f) > fs / 512 * 2)
    ref = np.median(p_db[inb])
    l3, h3 = edges(f, p_db, ref, 3)
    l6, h6 = edges(f, p_db, ref, 6)
    l10, h10 = edges(f, p_db, ref, 10)
    def at(fr):
        i = int(np.argmin(np.abs(f - fr)))
        return float(p_db[i] - ref)
    lw = w & 0xFFFFFF
    out = dict(file=os.path.basename(path), n=len(w), fs=fs, rms=float(np.sqrt(np.mean(np.abs(x - x.mean()) ** 2))),
               adj24=float(np.mean(lw[1:] == lw[:-1])), distinct24=int(len(np.unique(lw))),
               hi40_distinct=int(len(np.unique(w >> 24))),
               m3=[None if v is None else round(float(v) / 1e6, 2) for v in (l3, h3)],
               m6=[None if v is None else round(float(v) / 1e6, 2) for v in (l6, h6)],
               m10=[None if v is None else round(float(v) / 1e6, 2) for v in (l10, h10)])
    for fr in (10e6, 15e6, 18e6, 30e6, 35e6, 50e6, 70e6):
        if fr < fs / 2:
            out[f"rel_{int(fr/1e6)}M"] = [round(at(-fr), 1), round(at(fr), 1)]
    fb, rb, nb = burst_psd(x, fs, 512, 6.0, SNAP * spw)
    out["burst_segs"] = nb
    if rb is not None:
        rb_db = 10 * np.log10(np.convolve(rb, np.ones(k) / k, mode="same"))
        above = f[rb_db > rb_db.max() - 6]
        out["burst_peak_mhz"] = round(float(f[np.argmax(rb_db)]) / 1e6, 2)
        out["burst_peak_db"] = round(float(rb_db.max()), 1)
        out["burst_6db_span_mhz"] = [round(float(above.min()) / 1e6, 2), round(float(above.max()) / 1e6, 2)]
        out["_burst_db"] = rb_db
    return out, f, p_db - ref


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--fs", type=float, default=None, help="LA word rate (80e6 >> rate_sel); x2 for *8x2")
    ap.add_argument("--json", help="tapscan JSONL (gives rate per label)")
    ap.add_argument("--layout", default="iq12", help="iq12 (dma0/1) | a8x2 / b8x2 (dma11) | a8 / b8 (dma10)")
    ap.add_argument("--plot")
    ap.add_argument("--bursts", action="store_true", help="also plot the burst/median PSD ratio")
    a = ap.parse_args()
    rates = {}
    if a.json:
        for line in open(a.json):
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if d.get("ev") == "tap":
                rates[d["label"]] = d["rate"]
    curves = []
    for fn in a.files:
        label = os.path.basename(fn).rsplit("-", 1)[-1][:-4] if "-" in os.path.basename(fn) else fn
        fs = a.fs
        for k, r in rates.items():
            if os.path.basename(fn).endswith("-" + k + ".u64"):
                fs = 80e6 / (1 << r)
        if fs is None:
            fs = 40e6
        res, f, pr = analyze(fn, fs, a.layout)
        bdb = res.pop("_burst_db", None)
        print(json.dumps(res))
        curves.append((res["file"], f, pr))
        if bdb is not None and a.bursts:
            curves.append((res["file"] + " bursts", f, bdb))
    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(10, 5))
        for name, f, pr in curves:
            ax.plot(f / 1e6, pr, lw=1, label=name)
        ax.set_xlabel("MHz from centre")
        ax.set_ylabel("dB re in-band median")
        ax.set_ylim(-40, 30 if a.bursts else 15)
        ax.grid(alpha=.3)
        ax.legend(fontsize=7)
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
