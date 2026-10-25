#!/usr/bin/env python3
"""Analyze a devourer LA-mode capture (DVLA file) taken against ambient / injected 802.11 OFDM.

What it checks, without needing a CW reference:
  * sample statistics per path (DC, rms, clipping at the 12-bit rails, stuck bits);
  * burst detection on the power envelope (frames vs. noise floor);
  * PSD of burst-only vs. noise-only samples (where the signal sits in frequency);
  * L-LTF orientation test: normalized cross-correlation of the samples against the known
    802.11a/g/n L-LTF symbol, for x and for conj(x). The L-LTF tone sequence is not symmetric
    in k, so only the correct I/Q orientation correlates; a spectrally inverted capture
    correlates as conj(x). Also reports CFO from the two repeated LTF symbols.

Layouts follow devourer docs/la-capture.md: path A = I[11:0], Q[23:12] of the low dword;
JGR3 path B = Q[43:32], I[55:44] of the high dword.

    la_analyze.py cap.bin [--path a|b|both] [--offset-mhz F] [--plot out.png] [--json out.json]
"""
import argparse
import json
import struct
import sys

import numpy as np

RATE_MHZ = {0: 80.0, 1: 40.0, 2: 20.0, 3: 10.0, 4: 5.0, 5: 2.5, 6: 1.25, 7: 160.0}

# 802.11a L-LTF frequency-domain sequence, subcarriers -26..+26 (DC = 0).
LTF = np.array([1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1,
                0, 1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1,
                1, 1, 1], dtype=float)


def read_dvla(path):
    with open(path, "rb") as f:
        hdr = f.read(32)
        if hdr[:4] != b"DVLA":
            sys.exit(f"{path}: not a DVLA file")
        n = struct.unpack_from("<I", hdr, 12)[0]
        meta = {"version": hdr[4], "trig_mode": hdr[5], "smp_rate": hdr[7], "dma_type": hdr[8],
                "wrap": hdr[9], "n": n, "finish_addr": struct.unpack_from("<I", hdr, 16)[0]}
        meta["fs_mhz"] = RATE_MHZ.get(meta["smp_rate"], 20.0)
        words = np.fromfile(f, dtype="<u8", count=n)
    return meta, words


def sext(v, bits):
    v = v.astype(np.int64)
    s = 1 << (bits - 1)
    return (v ^ s) - s


def field(w, lsb, bits):
    return (w >> np.uint64(lsb)) & np.uint64((1 << bits) - 1)


def unpack(words, path):
    if path == "a":
        i, q = field(words, 0, 12), field(words, 12, 12)
    else:
        i, q = field(words, 44, 12), field(words, 32, 12)
    return sext(i, 12).astype(np.float64) + 1j * sext(q, 12).astype(np.float64), (i, q)


def ltf_symbol(fs_mhz):
    """One 3.2 us L-LTF symbol sampled at fs (fs a multiple of 20 MHz or 10/5 MHz narrowband)."""
    nfft = int(round(64 * fs_mhz / 20.0))
    spec = np.zeros(nfft, complex)
    for k, v in zip(range(-26, 27), LTF):
        spec[k % nfft] = v
    return np.fft.ifft(spec)


def ncorr(x, ref):
    """Normalized sliding |<x, ref>| / (|x_win| |ref|)."""
    n = len(ref)
    if len(x) < n:
        return np.zeros(0)
    c = np.abs(np.correlate(x, ref, mode="valid"))
    e = np.convolve(np.abs(x) ** 2, np.ones(n), mode="valid")
    return c / (np.sqrt(e) * np.linalg.norm(ref) + 1e-12)


def ltf_hits(x, fs_mhz, thresh=0.6):
    """Find L-LTF pairs: correlation peaks >= thresh separated by exactly one symbol."""
    ref = ltf_symbol(fs_mhz)
    n = len(ref)
    c = ncorr(x, ref)
    hits = []
    i = 0
    while i < len(c) - n:
        if c[i] >= thresh and c[i] == c[max(0, i - n // 2):i + n // 2].max():
            j = i + n
            lo, hi = max(0, j - 2), min(len(c), j + 3)
            if hi > lo and c[lo:hi].max() >= thresh:
                j = lo + int(np.argmax(c[lo:hi]))
                # CFO from the phase advance between the two identical LTF symbols.
                a, b = x[i:i + n], x[j:j + n]
                ph = np.angle(np.vdot(a, b))
                cfo_khz = ph / (2 * np.pi * (n / (fs_mhz * 1e6))) / 1e3
                hits.append({"idx": int(i), "corr": float(c[i]), "corr2": float(c[j]),
                             "cfo_khz": float(cfo_khz)})
                i = j + n
                continue
        i += 1
    return hits, (float(c.max()) if len(c) else 0.0)


def bursts(x, fs_mhz, thr_db=10.0):
    win = max(4, int(0.8 * fs_mhz))  # 0.8 us
    p = np.convolve(np.abs(x) ** 2, np.ones(win) / win, mode="same")
    floor = np.percentile(p, 20)
    on = p > floor * 10 ** (thr_db / 10)
    edges = np.flatnonzero(np.diff(on.astype(np.int8)))
    segs = []
    start = 0 if on[0] else None
    for e in edges:
        if start is None:
            start = e + 1
        else:
            segs.append((start, e + 1))
            start = None
    if start is not None:
        segs.append((start, len(x)))
    segs = [(a, b) for a, b in segs if (b - a) >= 4 * win]
    return on, segs, float(10 * np.log10(floor + 1e-12)), p


def welch(x, nfft=512):
    if len(x) < nfft:
        nfft = max(16, 1 << int(np.floor(np.log2(max(16, len(x))))))
    w = np.hanning(nfft)
    hop = nfft // 2
    acc = np.zeros(nfft)
    k = 0
    for s in range(0, len(x) - nfft + 1, hop):
        acc += np.abs(np.fft.fft((x[s:s + nfft] - 0) * w)) ** 2
        k += 1
    acc /= max(k, 1)
    return np.fft.fftshift(np.fft.fftfreq(nfft)), np.fft.fftshift(acc)


def analyze_path(x, raw, fs, offset_mhz):
    i, q = raw
    res = {}
    res["dc"] = [float(x.real.mean()), float(x.imag.mean())]
    res["rms"] = float(np.sqrt(np.mean(np.abs(x - x.mean()) ** 2)))
    res["clip_frac"] = float(np.mean((np.abs(x.real) >= 2047) | (np.abs(x.imag) >= 2047)))
    allbits = np.bitwise_or.reduce(np.concatenate([i, q]).astype(np.uint64)) if len(i) else 0
    res["bits_used_mask"] = hex(int(allbits))
    res["iq_amp_ratio"] = float(np.std(x.real) / (np.std(x.imag) + 1e-12))
    xc = x - x.mean()
    on, segs, floor_db, _ = bursts(xc, fs)
    res["noise_floor_db"] = floor_db
    res["n_bursts"] = len(segs)
    res["burst_us"] = [round((b - a) / fs, 1) for a, b in segs[:20]]
    res["duty"] = float(on.mean())
    h_ok, cmax_ok = ltf_hits(xc, fs)
    h_inv, cmax_inv = ltf_hits(np.conj(xc), fs)
    res["ltf_hits"] = len(h_ok)
    res["ltf_hits_conj"] = len(h_inv)
    res["ltf_corr_max"] = cmax_ok
    res["ltf_corr_max_conj"] = cmax_inv
    res["ltf_detail"] = h_ok[:10]
    res["orientation"] = ("correct" if cmax_ok > cmax_inv + 0.1 else
                          "inverted" if cmax_inv > cmax_ok + 0.1 else "undetermined")
    if segs:
        sig = np.concatenate([xc[a:b] for a, b in segs])
        f, ps = welch(sig)
        f = f * fs
        # power-weighted centroid within +/- fs/2 and occupied band (-10 dB from peak, smoothed)
        sm = np.convolve(ps, np.ones(9) / 9, mode="same")
        occ = f[sm > sm.max() * 0.1]
        res["burst_psd_centroid_mhz"] = float(np.sum(f * ps) / np.sum(ps))
        res["burst_occupied_mhz"] = [float(occ.min()), float(occ.max())]
        if offset_mhz is not None:
            res["expected_center_mhz"] = offset_mhz
    return res, segs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--path", choices=["a", "b", "both"], default="a")
    ap.add_argument("--offset-mhz", type=float, default=None,
                    help="expected signal centre offset from the tuned centre (MHz)")
    ap.add_argument("--plot")
    ap.add_argument("--json")
    a = ap.parse_args()
    meta, words = read_dvla(a.dump)
    fs = meta["fs_mhz"]
    out = {"file": a.dump, "meta": meta, "paths": {}}
    paths = ["a", "b"] if a.path == "both" else [a.path]
    data = {}
    for p in paths:
        x, raw = unpack(words, p)
        if not np.any(x):
            out["paths"][p] = {"empty": True}
            continue
        res, segs = analyze_path(x, raw, fs, a.offset_mhz)
        out["paths"][p] = res
        data[p] = (x - x.mean(), segs)
    if a.path == "both" and len(data) == 2:
        xa, xb = data["a"][0], data["b"][0]
        c = np.vdot(xa, xb) / (np.linalg.norm(xa) * np.linalg.norm(xb) + 1e-12)
        out["ab_coherence"] = {"mag": float(abs(c)), "phase_deg": float(np.degrees(np.angle(c)))}
    print(json.dumps(out, indent=1))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    if a.plot and data:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        rows = len(data)
        fig, axs = plt.subplots(rows, 3, figsize=(16, 4 * rows), squeeze=False)
        for r, (p, (x, segs)) in enumerate(data.items()):
            t = np.arange(len(x)) / fs
            axs[r, 0].plot(t, 20 * np.log10(np.abs(x) + 1), lw=0.4)
            axs[r, 0].set(title=f"path {p.upper()} |x| (dBFS-ish)", xlabel="us", ylabel="dB")
            f, ps = welch(x)
            axs[r, 1].plot(f * fs, 10 * np.log10(ps + 1e-9), label="all", lw=0.8)
            if segs:
                sig = np.concatenate([x[s:e] for s, e in segs])
                f2, ps2 = welch(sig)
                axs[r, 1].plot(f2 * fs, 10 * np.log10(ps2 + 1e-9), label="bursts", lw=0.8)
                on = np.ones(len(x), bool)
                for s, e in segs:
                    on[max(0, s - 50):e + 50] = False
                if on.sum() > 1024:
                    f3, ps3 = welch(x[on])
                    axs[r, 1].plot(f3 * fs, 10 * np.log10(ps3 + 1e-9), label="noise", lw=0.8)
            if a.offset_mhz is not None:
                axs[r, 1].axvline(a.offset_mhz, color="k", ls="--", lw=0.6)
            axs[r, 1].set(title=f"PSD path {p.upper()} (fs={fs} MHz)", xlabel="MHz offset", ylabel="dB")
            axs[r, 1].legend()
            nf = 128 if fs >= 20 else 64
            axs[r, 2].specgram(x, NFFT=nf, Fs=fs, noverlap=nf // 2, cmap="viridis")
            axs[r, 2].set(title=f"spectrogram path {p.upper()}", xlabel="us", ylabel="MHz")
        fig.suptitle(a.dump.split("/")[-1])
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
