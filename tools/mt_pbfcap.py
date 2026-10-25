#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""MT7612U packet-buffer ADC capture ("ADC dump") through the mt76x2u kernel driver's debugfs.

The vendor driver's Set_ADCDump_Proc (Netgear A6210 = MT7612U, ate/common/rt_ate.c) implements this
capture for RTMP-MAC chips and returns FALSE on RLT-MAC parts such as the MT7612U before touching
anything. This script runs the same register sequence on the MT7612U, with the "ADC 8" source (no
BBP writes), while mt76x2u keeps the chip running:

  MAC_SYS_CTRL 0x1004 = 0; PBF_CFG 0x0404 = 0; PBF_SYS_CTRL 0x0400 |= BIT14 (CAP_MODE)
  PBF_CAP_CTRL 0x0440: clear [31] and [15] (ADC 8), clear [28:16] (trigger offset), set [30] (start)
  MAC_SYS_CTRL = 0x8 (RX on); poll 0x440[30] until clear; MAC_SYS_CTRL = 0; start = 0x440[12:0]
  read back SMM 0x4000 + start*2 (16-bit lanes) and PKT 0x8000 + start*4 (32-bit), 4096 x 2 entries

Every register written is snapshot first and restored afterwards. RX only.
Usage (root): tools/mt_pbfcap.py --phy phy15 [--dry] [--n 2] [--out captures/phase6/pbf]
"""
import argparse
import json
import os
import time

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--phy", default="phy15")
ap.add_argument("--dry", action="store_true", help="read registers and windows only; write nothing")
ap.add_argument("--n", type=int, default=2, help="captures to take")
ap.add_argument("--out", default="captures/phase6/pbf")
a = ap.parse_args()

D = f"/sys/kernel/debug/ieee80211/{a.phy}/mt76"


def _put(name, text):
    fd = os.open(f"{D}/{name}", os.O_WRONLY)
    try:
        os.write(fd, text.encode())
    finally:
        os.close(fd)


def rr(addr):  # debugfs attributes are not seekable: one open per access
    _put("regidx", f"{addr}\n")
    fd = os.open(f"{D}/regval", os.O_RDONLY)
    try:
        return int(os.read(fd, 64).strip(), 0)
    finally:
        os.close(fd)


def wr(addr, val):
    _put("regidx", f"{addr}\n")
    _put("regval", f"0x{val & 0xffffffff:08x}\n")


def window(base, words):
    return np.array([rr(base + 4 * i) for i in range(words)], dtype=np.uint32)


REGS = {"MAC_SYS_CTRL": 0x1004, "PBF_SYS_CTRL": 0x0400, "PBF_CFG": 0x0404, "PBF_CAP_CTRL": 0x0440}
os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
saved = {k: rr(v) for k, v in REGS.items()}
print(json.dumps({"ev": "regs.before", **{k: f"0x{v:08x}" for k, v in saved.items()}}))

t0 = time.monotonic()
base_smm, base_pkt = window(0x4000, 4096), window(0x8000, 8192)
print(json.dumps({"ev": "baseline", "read_s": round(time.monotonic() - t0, 2),
                  "smm_nonzero": int((base_smm != 0).sum()), "pkt_nonzero": int((base_pkt != 0).sum()),
                  "smm_head": [f"0x{x:08x}" for x in base_smm[:4]], "pkt_head": [f"0x{x:08x}" for x in base_pkt[:4]]}))
np.save(a.out + "-baseline-smm.npy", base_smm)
np.save(a.out + "-baseline-pkt.npy", base_pkt)
if a.dry:
    raise SystemExit(0)

try:
    for n in range(a.n):
        wr(REGS["MAC_SYS_CTRL"], 0)
        wr(REGS["PBF_CFG"], 0)
        wr(REGS["PBF_SYS_CTRL"], saved["PBF_SYS_CTRL"] | 0x4000)
        cap = rr(REGS["PBF_CAP_CTRL"]) & ~0x80008000 & ~0x1FFF0000
        wr(REGS["PBF_CAP_CTRL"], cap)
        wr(REGS["PBF_CAP_CTRL"], cap | 0x40000000)
        wr(REGS["MAC_SYS_CTRL"], 0x8)
        t = time.monotonic()
        polls = 0
        while rr(REGS["PBF_CAP_CTRL"]) & 0x40000000 and time.monotonic() - t < 3:
            polls += 1
        done = not (rr(REGS["PBF_CAP_CTRL"]) & 0x40000000)
        wr(REGS["MAC_SYS_CTRL"], 0)
        r440 = rr(REGS["PBF_CAP_CTRL"])
        start = r440 & 0x1FFF
        # Read the whole windows (unrolled from `start` below), as the vendor loop wraps them.
        smm, pkt = window(0x4000, 4096), window(0x8000, 8192)
        np.save(f"{a.out}-{n}-smm.npy", smm)
        np.save(f"{a.out}-{n}-pkt.npy", pkt)
        print(json.dumps({"ev": "capture", "n": n, "done": done, "polls": polls, "ms": round((time.monotonic() - t) * 1e3, 1),
                          "r440": f"0x{r440:08x}", "start": start,
                          "smm_changed": int((smm != base_smm).sum()), "pkt_changed": int((pkt != base_pkt).sum()),
                          "smm_head": [f"0x{x:08x}" for x in smm[:4]], "pkt_head": [f"0x{x:08x}" for x in pkt[:4]]}))
        # restore between captures so normal RX resumes
        wr(REGS["PBF_CAP_CTRL"], saved["PBF_CAP_CTRL"])
        wr(REGS["PBF_SYS_CTRL"], saved["PBF_SYS_CTRL"])
        wr(REGS["PBF_CFG"], saved["PBF_CFG"])
        wr(REGS["MAC_SYS_CTRL"], saved["MAC_SYS_CTRL"])
        time.sleep(0.2)
finally:
    for k in ("PBF_CAP_CTRL", "PBF_SYS_CTRL", "PBF_CFG", "MAC_SYS_CTRL"):
        wr(REGS[k], saved[k])
    print(json.dumps({"ev": "regs.after", **{k: f"0x{rr(v):08x}" for k, v in REGS.items()}}))
