#!/usr/bin/env python3
"""L-LTF search at frequency offsets in multi-snapshot LA dumps (wsdr_probe tapscan count=N).

usage: tapscan_ltf.py FILE.u64 --fs 80e6 --offsets -30,-20,-10,0,10,20,30 [--thresh 0.6] [--layout iq12|a8x2|..]
(--fs is the LA word rate; layouts with two samples per word double it)

Per 16384-sample snapshot and per offset: mix the offset to DC, low-pass (+-9 MHz windowed sinc),
resample to 20 MS/s and look for L-LTF symbol pairs (tools/la_analyze.ltf_hits) in x and conj(x).
Offsets that are not 20 MHz channel centres (e.g. 0 and +-20 for an 80 MHz channel at ch42) and conj(x)
are the false-positive controls. A hit at an offset outside +-10 MHz proves the tap is wider than 20 MHz.
"""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from la_analyze import ltf_hits  # noqa: E402
from tapscan_psd import decode, SNAP, SPW  # noqa: E402


def lpf(fs, cutoff=9e6, ntap=129):
    n = np.arange(ntap) - (ntap - 1) / 2
    h = 2 * cutoff / fs * np.sinc(2 * cutoff / fs * n) * np.hamming(ntap)
    return h / h.sum()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--fs", type=float, required=True)
    ap.add_argument("--offsets", default="-30,-20,-10,0,10,20,30")
    ap.add_argument("--thresh", type=float, default=0.6)
    ap.add_argument("--layout", default="iq12")
    a = ap.parse_args()
    w = np.fromfile(a.file, dtype="<u8")
    x = decode(w, a.layout)
    spw = SPW[a.layout]
    snap = SNAP * spw
    a.fs *= spw
    dec = int(round(a.fs / 20e6))
    h = lpf(a.fs)
    offs = [float(o) for o in a.offsets.split(",")]
    res = {o: {"hits": 0, "hits_conj": 0, "snaps_with_hit": 0, "cfo_khz": []} for o in offs}
    nsnap = len(x) // snap
    t = np.arange(snap) / a.fs
    for s in range(nsnap):
        xs = x[s * snap:(s + 1) * snap]
        xs = xs - xs.mean()
        for o in offs:
            y = np.convolve(xs * np.exp(-2j * np.pi * o * 1e6 * t), h, mode="same")[::dec]
            hk, _ = ltf_hits(y, 20.0, a.thresh)
            hc, _ = ltf_hits(np.conj(y), 20.0, a.thresh)
            res[o]["hits"] += len(hk)
            res[o]["hits_conj"] += len(hc)
            res[o]["snaps_with_hit"] += 1 if hk else 0
            res[o]["cfo_khz"] += [round(v["cfo_khz"], 1) for v in hk]
    out = {"file": os.path.basename(a.file), "fs": a.fs, "snapshots": nsnap, "thresh": a.thresh, "offsets": {}}
    for o in offs:
        r = res[o]
        c = np.array(r["cfo_khz"]) if r["cfo_khz"] else None
        out["offsets"][f"{o:+g}"] = {"hits": r["hits"], "hits_conj": r["hits_conj"], "snaps_with_hit": r["snaps_with_hit"],
                                     "cfo_khz_median": None if c is None else float(np.median(c)),
                                     "cfo_khz_examples": r["cfo_khz"][:8]}
    print(json.dumps(out))


if __name__ == "__main__":
    main()
