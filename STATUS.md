# STATUS

_Last updated: 2026-10-25 11:40 CET_

**Current phase:** Phase 3 (8812BU streaming), groundwork done. **Paused** — the next on-chip step was
interrupted (see "Blocked"). Phases 0–2 complete; Phase 1 done on both chips.

## Hardware

| Adapter | VID:PID | Chip | USB link | Port (sysfs) | devourer RX |
|---|---|---|---|---|---|
| "AC1200 Techkey" | 0bda:b812 | RTL8822B cut 3, 2T2R, rfe 3 | **5000M (USB 3)** | bus 2 port 1 | HEALTHY ([doctor ch1](evidence/phase0/doctor-8812bu-ch1.jsonl)) |
| RTL8812CU | 0bda:c812 | RTL8822C cut 5, 2T2R, rfe 3 | **480M (USB 2, bcdUSB 2.00)** | bus 5 port 1 | HEALTHY ([doctor ch36](evidence/phase0/doctor-8812cu-ch36.jsonl)) |

- No uhubctl-capable hub (NEEDS N3). Host uplink `enp3s0` untouched.
- **In-tree rtw88_8822bu/cu/21cu + DKMS 88x2cu are temp-blacklisted** (`tools/rtw88_guard.sh on`).
  Restore with `sudo ~/wifi-sdr/tools/rtw88_guard.sh off` (then replug or `modprobe`).
- Test signal: ambient Wi-Fi (ch36 AP). No controllable TX (NEEDS N2).
- devourer: local clone fast-forwarded to OpenIPC 0bf1957 (your OK), not pushed.

## What works (with evidence)
- **Phase 1 — LA baseline, both chips:** layout I[11:0]/Q[23:12] (+ 8822C path B Q[43:32]/I[55:44]),
  orientation correct (L-LTF test, 13/13 detections), off-centre placement correct
  ([BU](evidence/phase1-bu-offcentre.png), [CU](evidence/phase1-cu-offcentre.png)), 8822C two paths
  phase-coherent after a fixed ≈7.4-sample A/B skew. Details: FINDINGS §2.
- **Phase 2 — data path documented** (FINDINGS §3–4, `notes/ac54-analysis.md`). The "vendor IMEM" is the
  Linux firmware (byte-identical), not a Windows extraction.
- **Phase 3 groundwork (8812BU):** free-running LA ring live after devourer bring-up; **fixed-gain mode
  found** (0x838[1] + CCK off + IGI, ≈0.95 dB/IGI step) — an open item in ac54; contiguous snapshot
  capture (`wsdr_probe snap`, 819 µs per snapshot, ~12 ms readback); **custom code runs on the embedded
  CPU** with the REG 0x568 µs counter verified; ISA subset characterized. FINDINGS §4 "Measured on our 8812BU".

## Blocked
- **Continuous streaming (rest of Phase 3):** while writing the next on-chip program (a C2H transport
  benchmark), my response was stopped by a safety classifier. I deleted the truncated file and have not
  retried that content. Needs your direction (see chat).
- 8812CU streaming would also be USB-2-limited (≈35–40 MB/s) unless the die supports the runtime USB 3
  switch (open).

- **Snapshot-mode rtl_tcp server:** while writing it, my response was stopped by a safety classifier
  again. The partial `src/wsdr_tcp.cpp` was deleted and not retried. The snapshot logic it needed is now
  `La8822b::snap()` (used by `wsdr_probe snap`). Paused pending your direction.

## Background results
- The firmware-RE subagent finished and wrote `notes/fw-disasm.md` (not yet reviewed or merged into FINDINGS).

## Open needs (NEEDS.md)
N2 test transmitter · N3 uhubctl hub · N4 ac54 license judgment · N5 (stretch) RTL8812EU. N1 resolved
(8812CU plugged in).

## Next step (once unblocked)
C2H transport + latency measurement on Linux, then the bank-scheduled streamer at 10 → 20 → 40 Msps with
the integrity harness; snapshot-mode rtl_tcp server in parallel.
