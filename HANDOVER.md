# Handover — 2026-10-25 (end of the long Claude session)

Read this first, then STATUS.md, REPORT.md and FINDINGS.md §4. LOG.md has the chronology.

## Where things stand
- **8812BU (RTL8822B), done and working:**
  - USB2: continuous LA streaming at 2.5/5 MS/s.
  - USB3 on the good port: 20 and 40 MS/s for 60 s with zero faults, and 40 MS/s with the full 40 MHz
    band (BW40 + vendor RX DFIR, `--dfir 40`).
  - 80 MS/s / ±37 MHz (BW80 + `--dfir 80`) and the raw-ADC tap (dma 11, int8, both paths, 160 MS/s)
    are snapshot-only (above the ~391 MB/s C2H ceiling).
- **Front ends:**
  - SoapySDR module "wsdr": `build-soapy/`, opt-in `-DWSDR_SOAPY=ON`. 2.5/5 MS/s on USB2; up to
    40 MS/s on USB3, where 40 MS/s automatically uses BW40 + DFIR. Live in gqrx via
    `soapy=0,driver=wsdr`.
  - rtl_tcp server: `wsdr_tcp`, 2.5/5 MS/s; use `--freq-offset 5e9` with a gqrx LNB LO for 5 GHz.
  - Recorder: `wsdr_probe stream --out`.
- **8812CU (RTL8822C):**
  - Custom code runs on its CPU (loader `src/cpu8822c.*`, tool `build-cu/wsdr_cu`).
  - C2H works: host 1600/1600 16 KiB; CPU bench 44.7 MB/s on USB2.
  - The C2H→TX address map is known (`notes/cu-custom-code.md`).
  - **Next task: the 8822C streamer (not started).**
- **MT7612U:** an Opus research agent was launched (background) on raw-IQ feasibility.
  - Expected output: `notes/mt7612u-sdr.md`, `evidence/phase6-mt-*`, `src/mt76sdr/`, `tools/mt_*`,
    `build-mt/`.
  - Review, then commit and push it. If it left a `cmake/mt.cmake`, add its include line.

## Hardware and ports (this host has four AMD xHCI controllers)
| Adapter | Where | Notes |
|---|---|---|
| 8812BU 0bda:b812 | port on xHCI **0000:67:00.3** (bus 5/6) | USB3 works only here; 65:00.3 wedges C2H. A plain replug comes back at USB2; USB3 needs a guarded rtw88 probe (LOG 11:05 / 12:40). Currently USB3 (6-1). |
| 8812CU 0bda:c812 | bus 1 port 1 (xHCI 65:00.3), USB2 | Hangs on ≥4 KiB EP0 block transfers → `wsdr_cu` defaults to `--cap 4`. A hang needs a physical replug. |
| MT7612U 0e8d:7612 | usb 8-1, xHCI 67:00.4, USB3 | `mt76x2u` loaded on plug despite the blacklist; unload it before use. Never unload `mt76`/`mt7921e` (host Wi-Fi). |

- **rtw88 guard ON**: blacklists rtw88_8822bu/cu/21cu, 88x2cu, mt76x2u.
- **Locks:** `tools/coord.sh` (8812bu, 8812cu, mt7612u, rtw88-guard, git). The owner must be
  `claude`/`opencode`; put sub-agent names in the reason.

## Repos and branches
- **wifi-sdr:** github.com/snokvist/wifi-sdr (PRIVATE), `main`. Commit under the git lock, then push.
- **devourer** `~/dev/snokvist/devourer`: currently on branch **`pr/jaguar3-runtime-thread-optional`**.
  - wifi-sdr's 8822C path needs `tuning.j3_runtime_thread = false`. `Session` sets it via SFINAE, so
    it also builds on master, but then the 8822C C2H reader loses packets to devourer's coex thread.
  - Open upstream PRs:
    - **OpenIPC/devourer#497**: 8822B RX DFIR by width.
    - **OpenIPC/devourer#498**: optional Jaguar3 runtime thread.
  - Local master is 0bf1957 (upstream), 37 ahead of the fork's master, unpushed. `build-dfir/` is a
    scratch build dir (gitignored).

## Next task: the 8822C streamer (design notes)
1. **LA ring on the 8822C, Jaguar3 dialect** (port from devourer `src/LaCapture.cpp` setup_bb /
   setup_dbg_port / setup_mac, GPL-2.0):
   - Registers:
     - `0x1EB4[23]` = 1 (report update)
     - `0x1CE4`: `[7:6]` = 0 (MAC-PHY timing), `[5:0]` dma_type, `[10:8]` smp_rate, `[17:13]` trig_sel = 3, `[26]` edge 0, `[27]` = 0, `[31:28]` = 0
     - `0x1CF4[23]` = 1 (LA on)
     - `0x1CE8` adv-trigger bits cleared, `0x1CF0` = 0
     - `0x1C3C[19:8]` = dbg port
   - Arm `0x7C0 = 0xCB`, never trigger.
   - Ring: TXFF 0x20000–0x40000 (la_regs_jgr3, half mode). smp_rate index: 0 = 80M, 1 = 40M,
     2 = 20M, … (verify with a CPU-timed probe like `latime8822b.S`).
   - Read the ring with 4-byte EP0 reads only.
   - Fixed gain: the Jaguar3 IGI / CCA registers differ from the 8822B; check devourer Jaguar3.
2. **Chunk layout:** C2H packets must not cross TX offset 0x6000 inside a bank.
   - Map: C2H 0x06000–0x0ffff → TX 0x36000–0x3ffff; 0x10000–0x15fff → 0x30000–0x35fff;
     0x16000–0x1ffff → 0x26000–0x2ffff; 0x20000–0x25fff → 0x20000–0x25fff.
   - Example layout: segment A [128, 0x6000) in two chunks, segment B [0x6000+24, 0x10000) in three.
     The trailer carries the 128 B bank prefix plus the 24 B at offset 0x6000.
3. **Firmware:** `stream8822c.S` linked at 0x8003003c (after the 0x3c-byte vendor stub). DMEM block at
   0x80208000 (verified writable). Same MIPS16 subset as the 8822B; µs counter REG 0x568 at
   0xb8640568. Generalize the host-seeded chunk table to N entries.
4. **Host:** generalize `Stream8822b` (layout struct + CPU-loader + LA-config abstractions), or add
   `Stream8822c`.
   - Keep aggregation off (0x10c[2] = 0) and the vendor CPU halted.
   - Remember the 8822B hazard: never write into the bank the ADC is filling.
5. **Budget:** USB2 at 44.7 MB/s → ≤5 MS/s at 8 B/sample. Path B shares the word (Q[43:32]/I[55:44]),
   so two coherent paths come at no transport cost.

## Rules that held all session
- RX only; RAM-only on-chip code; no efuse/flash writes; snapshot and restore registers.
- Don't touch `enp3s0`, `mt76`, or `mt7921e`.
- Never `pkill -f <pattern>` that also matches your own shell's command line; kill by PID.
- Verify sub-agent claims on hardware before reporting them as fact.
