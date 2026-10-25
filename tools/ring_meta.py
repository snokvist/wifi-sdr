#!/usr/bin/env python3
"""Correlate the upper 40 bits of free-running LA ring words with sample amplitude.

Input: raw ring dumps from `wsdr_probe ring --dump PREFIX` (PREFIX-readK.u64, u64 LE words in ring order).
For each distinct upper-bits value: count, mean |x|, first/last index and number of contiguous runs —
if the upper bits tag AGC state, each tag should cover contiguous runs with a distinct amplitude scale.

    ring_meta.py PREFIX [--reads N]
"""
import argparse

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("prefix")
ap.add_argument("--reads", type=int, default=4)
ap.add_argument("--top", type=int, default=6)
a = ap.parse_args()
for k in range(a.reads):
    try:
        w = np.fromfile(f"{a.prefix}-read{k}.u64", dtype="<u8")
    except FileNotFoundError:
        break
    i = ((w & 0xFFF).astype(np.int64) ^ 0x800) - 0x800
    q = (((w >> 12) & 0xFFF).astype(np.int64) ^ 0x800) - 0x800
    hi = w >> 24
    amp = np.sqrt(i * i + q * q)
    vals, cnt = np.unique(hi, return_counts=True)
    order = np.argsort(-cnt)
    print(f"read{k}: {len(vals)} distinct upper values, rms {np.sqrt(np.mean(amp**2)):.1f}")
    for v in vals[order][: a.top]:
        m = hi == v
        idx = np.flatnonzero(m)
        runs = 1 + np.count_nonzero(np.diff(idx) > 1)
        print(f"   hi={int(v):#012x} n={m.sum():6d} mean|x|={amp[m].mean():7.1f} "
              f"first={idx[0]:5d} last={idx[-1]:5d} runs={runs}")
