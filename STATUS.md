# STATUS

_Last updated: 2026-10-25 12:45 CET_

**Current phase:** Phase 3 (8812BU continuous streaming), groundwork done, streaming in progress in a
parallel session (opencode; lane in `coord/from-opencode.md`). Phases 0–2 complete; Phase 1 done on both
chips. Interim write-up: [REPORT.md](REPORT.md).

## Hardware

| Adapter | VID:PID | Chip | USB link | Port (sysfs) | devourer RX |
|---|---|---|---|---|---|
| "AC1200 Techkey" | 0bda:b812 | RTL8822B cut 3, 2T2R, rfe 3 | **5000M (USB 3)** | bus 2 port 1 | HEALTHY ([doctor ch1](evidence/phase0/doctor-8812bu-ch1.jsonl)) |
| RTL8812CU | 0bda:c812 | RTL8822C cut 5, 2T2R, rfe 3 | **480M (USB 2, bcdUSB 2.00)** | bus 5 port 1 | HEALTHY ([doctor ch36](evidence/phase0/doctor-8812cu-ch36.jsonl)) |

- No uhubctl-capable hub (NEEDS N3). Host uplink `enp3s0` untouched.
- **In-tree rtw88_8822bu/cu/21cu and DKMS 88x2cu are temp-blacklisted** (`tools/rtw88_guard.sh on`).
  Restore with `sudo ~/wifi-sdr/tools/rtw88_guard.sh off`, then replug or `modprobe`.
- Test signal: ambient Wi-Fi (ch36 AP). No controllable TX (NEEDS N2).
- devourer: local clone fast-forwarded to OpenIPC 0bf1957 (your OK), not pushed.
- Two sessions share the workspace. Before using an adapter, the driver guard or git, take the lock
  (`coord/README.md`, `tools/coord.sh`).

## What works (with evidence)
- **Phase 1, LA baseline on both chips:**
  - Layout: I[11:0]/Q[23:12], plus 8822C path B at Q[43:32]/I[55:44].
  - Orientation correct (L-LTF test, 13/13 detections).
  - Off-centre placement correct ([BU](evidence/phase1-bu-offcentre.png),
    [CU](evidence/phase1-cu-offcentre.png)).
  - The 8822C's two paths are phase-coherent after a fixed ≈7.4-sample A/B skew.

  Details: FINDINGS §2.
- **Phase 2, data path documented** (FINDINGS §3–4, `notes/ac54-analysis.md`). The "vendor IMEM" is
  the Linux firmware, byte-identical, not a Windows extraction.
- **Phase 3 groundwork, 8812BU** (FINDINGS §4 "Measured on our 8812BU"):
  - The free-running LA ring is live after devourer bring-up.
  - **Fixed-gain mode** (0x838[1] + CCK off + IGI): −0.94 dB per IGI step over ≈45 dB. ac54 lists
    this as open.
  - Contiguous snapshot capture (`wsdr_probe snap`): 819 µs per snapshot, ~12.5 ms readback.
  - **Custom code runs on the embedded CPU.** The REG 0x568 µs counter is verified and the ISA subset
    characterized.
- **Firmware study** `notes/fw-disasm.md`: the 8822B PSD engine, the 8822C IQK read-out and the
  register names are cross-checked into FINDINGS §3, with one correction (no PSD registers on the
  8822C in this vendor tree).

## Not done yet
- Continuous streaming: 20 Msps for 60 s with zero faults, and the 40 Msps result. In progress in
  the parallel session.
- The tuning table across channels and bands. Only the ch36 noise-floor gain map exists.
- Phase 4 front ends (rtl_tcp, SoapySDR, recorder), Phase 5 (8812CU port), Phase 6 (8812EU).
- 8812CU streaming would be USB-2-limited (≈35–40 MB/s) unless the board supports the runtime USB 3
  switch (open).

## Open needs (NEEDS.md)
N2 test transmitter · N3 uhubctl hub · N4 ac54 license judgment · N5 (stretch) RTL8812EU. N1 is resolved
(8812CU plugged in).

## Next steps
See REPORT.md §7: Phase 3 streaming and its integrity harness at 10 → 20 → 40 Msps, then the tuning
table, the Phase 4 front ends and the 8812CU port.
