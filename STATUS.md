# STATUS

_Last updated: 2026-10-25 07:00 CET_

**Current phase:** Phase 0 done for the one adapter present → starting Phase 1 (LA baseline, 8812BU).
Phase 2 reading (ac54_sdr and firmware images) is running in parallel.

## Hardware

| Adapter | VID:PID | Chip (as identified) | USB link | bcdUSB | Endpoints | Port (sysfs) | devourer RX |
|---|---|---|---|---|---|---|---|
| "AC1200 Techkey" | 0bda:b812 | RTL8822B cut 3 (UMC), 2T2R, EFUSE rfe_type 3, chip-id 0x0a | **5000M (USB 3)** | 3.00 | bulk IN 0x84; bulk OUT 0x05/0x06/0x08; intr IN 0x87 | bus 2 port 1 (`…/0000:65:00.3/usb2/2-1`) | **HEALTHY** — [doctor ch1](evidence/phase0/doctor-8812bu-ch1.jsonl): 1899 ok / 636 crc in 8 s |
| RTL8812CU | — | not attached | — | — | — | — | — (NEEDS N1) |
| RTL8812EU | — | not attached (stretch) | — | — | — | — | — |

- Switchable (uhubctl) hub: **none** → recovery ladder ends at `usbreset`, then a replug (NEEDS N3).
- Host uplink `enp3s0`: protected, untouched. In-tree `rtw88_8822bu/cu/21cu` temp-blacklisted by
  `tools/rtw88_guard.sh on` (restore with `sudo tools/rtw88_guard.sh off`).
- Test signal: **ambient Wi-Fi** (ch1 ≈180 frames/s, ch36 ≈110 frames/s). No controllable TX yet (NEEDS N2).
- devourer: local snokvist clone fast-forwarded to OpenIPC master 0bf1957 (with your OK), built Release.

## What works (with evidence)
- devourer bring-up + RX on the 8812BU: `evidence/phase0/`.

## Blocked / waiting
- 8812CU work (Phase 1 second half, Phase 5) — N1.
- Controllable test transmitter — N2 (ambient suffices to proceed).

## Open needs (detail in NEEDS.md)
- N1 plug in an RTL8812CU · N2 a test transmitter (second adapter / SDR / OK to unblock MT7922)
- N3 uhubctl-capable USB 3 hub · N4 judgment: ac54_sdr is unlicensed (re-implementing, not copying)
- N5 (stretch) RTL8812EU

## Next step
LA-mode manual + CRC-triggered captures on the 8812BU at ch1, decode with `la_decode.py`, PSD plot,
orientation check via an off-centre tune against an ambient AP.
