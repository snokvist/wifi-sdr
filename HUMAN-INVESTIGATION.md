# Human investigation guide

A map of this workspace: what has been measured, where each fact lives, and where to look
further. This file is an index. The detail lives in the files it points to.

_Written 2026-10-25. Workspace state at commit `1c4183b` plus this file._

## Where things stand

| Phase | State | Where |
|---|---|---|
| 0 Recon / bench | Done: both adapters HEALTHY under devourer's doctor | `STATUS.md`, `evidence/phase0/` |
| 1 LA-mode baseline | Done on the 8812BU and the 8812CU | `FINDINGS.md` §2, `evidence/phase1-*` |
| 2 Document ac54_sdr's streaming method | Done | `FINDINGS.md` §3–4, `notes/ac54-analysis.md` |
| 3 Continuous streaming, 8812BU | Groundwork done; **paused** | `FINDINGS.md` §4 "Measured on our 8812BU", `STATUS.md` "Blocked" |
| 4 rtl_tcp / SoapySDR / recorder | Not started (paused) | — |
| 5 8812CU port | Not started | `FINDINGS.md` §2 (8822C LA dialect), §3 (8822C firmware layout) |
| 6 8812EU | Not started; no adapter | `NEEDS.md` N5 |

The two paused items, the on-chip transfer step and the snapshot-mode rtl_tcp server, were
stopped by a safety classifier on Claude's side, not by a hardware result. `LOG.md` has the
timeline.

## Read in this order

1. **`STATUS.md`**: one page on hardware, what works, what's blocked and open needs.
2. **`FINDINGS.md`**: the technical record. Every claim has a measurement or a citation.
   - §1 Bench units: chip cut, RFE type and USB link speed of each adapter.
   - §2 LA-mode sample path, measured: bit layout on both chips, I/Q orientation test, off-centre
     placement test, 8822C two-path coherence (≈7.4-sample A/B skew), and quirks (CCK freezes
     the bus at 2.4 GHz, the BW40 tap anomaly).
   - §3 Firmware images and the embedded CPU: image identity (the "vendor IMEM" ac54 uses is the
     linux-firmware `rtw8822b_fw.bin`), load addresses, and the CPU/ISA subset measured on
     silicon.
   - §4 Continuous-streaming data path (ac54's method, as documentation), then **"Measured on our
     8812BU"** (free-running ring live after devourer bring-up, the fixed-gain recipe and IGI gain
     map, snapshot capture, custom code running on the CPU, the µs timer check), then the open
     **Unknowns** list.
3. **`notes/ac54-analysis.md`**: a full read-out of djObsidian/ac54_sdr with file:line citations.
   §5 is the data path end to end, §6 the framing, sequencing and freshness scheme, §8 its
   measured results and caveats, §10 its own unknowns. ac54_sdr has **no license**: treat it as
   documentation only and don't copy its code (`NEEDS.md` N4).
4. **`notes/fw-disasm.md`**: static RE of the 8822B and 8822C NIC firmware (written by a
   subagent). Claims are tagged **[V]** (verified from bytes or a cited source) or **[I]**
   (inferred). §2 covers the MMIO census and how the vendor firmware moves C2H/bulk data. §3 is
   H2C dispatch, §4 8822C calibration read-back, §5 the hardware PSD/FFT engine, §6 the 8822C
   custom-firmware entry, and §7 "Most promising leads". **Not yet cross-checked by Claude**;
   check the [I] items before relying on them.
5. **`NEEDS.md`**: open hardware and judgment requests: N2 test transmitter, N3 uhubctl hub,
   N4 license call, N5 8812EU.
6. **`LOG.md`**: the append-only timeline, including every package installed.

## Key measured results (details and evidence in FINDINGS)

- **Sample layout:** path A is I[11:0], Q[23:12] in each u64. On the 8822C, path B is Q[43:32],
  I[55:44]. There is no spectral inversion (13/13 L-LTF detections).
- **Fixed gain, 8822B:** `0x838[1]=1` (OFDM RX CCA off) with CCK off, then pin IGI at
  `0xc50/0xe50[6:0]`. That gives ≈0.95 dB per IGI step, and **higher IGI means less gain**: noise
  RMS 198 @0x1c … 4 @0x40. Evidence: `evidence/phase3-igi-sweep-bu-ch36.txt` and
  `evidence/phase3-snap-{cca,nocca}-frame.png`.
- **Traps:** `0x808[29]=0` (OFDM off) freezes the LA writer. Turning MAC RX off also freezes it.
  CCK holds the bus at 2.4 GHz.
- **Readback:** a 128 KiB EP0 read takes 12.5 ms over USB 3 on the 8812BU. The 8812CU sits at
  USB 2 (480M) on this host.
- **Embedded CPU (8822B):** MIPS16 plus only part of MIPS16e2. LUI works; ORI and XORI behave
  like LI. Use LUI+ADDIU for 32-bit constants. REG 0x568 is a µs counter, matching host time
  within 0.01%. The probe is `src/fw/marker8822b.S`.

## Code in this workspace (GPL-2.0, builds on devourer)

| File | What it is |
|---|---|
| `src/rtlreg.{h,cpp}` | Register, SRAM-window and DDMA access over vendor control requests |
| `src/session.{h,cpp}` | Opens an adapter and brings it up through devourer |
| `src/la88xx.{h,cpp}` | LA ring configure/arm/stop, register snapshot+restore, `snap()` |
| `src/cpu88xx.{h,cpp}` | 8822B CPU halt/load/start behind the vendor prologue (RAM only), DMEM I/O |
| `src/fw/marker8822b.S` | On-chip marker and ISA probe (MIPS16) |
| `src/wsdr_probe.cpp` | `ring`, `snap`, `marker` subcommands (usage in its header comment) |
| `tools/la_analyze.py` | DVLA reader: burst detection, PSD, L-LTF correlation, CFO |
| `tools/phase1_figure.py`, `tools/ring_meta.py` | Phase 1 figure; upper-bit metadata vs amplitude |
| `tools/rtkfw88xx.py`, `tools/extract_vendor_fw.sh` | Regenerate `vendor/` from local sources (header lists the inputs) |
| `tools/fwre/run.sh`, `fwre.py` | Regenerate the firmware listings behind `notes/fw-disasm.md` |
| `tools/rtw88_guard.sh on\|off\|status` | Temporary blacklist of in-kernel and DKMS drivers for the test adapters |

Build and run:

```sh
cd ~/wifi-sdr
cmake -S . -B build && cmake --build build -j --target wsdr_probe
sudo tools/rtw88_guard.sh on        # then replug the adapters
sudo DEVOURER_LOG_LEVEL=warn build/wsdr_probe snap --pid 0xb812 --channel 36 --igi 0x24 --nocca --count 4 --dump captures/x
python3 -I tools/la_analyze.py captures/x-snap0.bin     # see its --help
sudo tools/rtw88_guard.sh off       # restore the normal drivers when done
```

`vendor/`, `ref/`, `captures/` and `build/` are gitignored. `vendor/` is regenerated locally and is
never committed or redistributed.

## External references

| What | Where | Local copy |
|---|---|---|
| devourer (userspace Realtek driver; LA capture lives in `src/LaCapture.cpp`, `docs/la-capture.md`) | https://github.com/OpenIPC/devourer | `ref/devourer` @ 0bf1957; working clone `~/dev/snokvist/devourer` |
| Vendor 8822C driver (phydm, halmac, halrf sources) | devourer submodule `reference/rtl88x2cu` | `~/dev/snokvist/devourer/reference/rtl88x2cu` |
| Vendor 8822B driver (the tree ac54 pins) | https://github.com/morrownr/88x2bu-20210702 | `ref/88x2bu-20210702` @ d300436 |
| ac54_sdr (8822BU SDR via custom RAM firmware; **no license**) | https://github.com/djObsidian/ac54_sdr | `ref/ac54_sdr` @ 2b41f98 |
| djObsidian/wifi-sdr (related prior work) | https://github.com/djObsidian/wifi-sdr | `ref/wifi-sdr` @ f729a82 |
| rtl8922-sdr (related work on a newer Realtek part) | https://github.com/plaingca/rtl8922-sdr | `ref/rtl8922-sdr` @ b41386b |
| linux-firmware images | `/lib/firmware/rtw88/rtw8822{b,c}_fw.bin.zst` | extracted into `vendor/` |
| In-kernel rtw88 driver (register names, LA/DMA usage) | Linux `drivers/net/wireless/realtek/rtw88/` | — |

Inside the vendor driver trees, the phydm debug/LA code (`phydm_adc_sampling.c`), the halmac
register map (`halmac_reg2.h`) and the phydm DIG code are what FINDINGS and fw-disasm cite most.

## Open questions (full list: FINDINGS §4 "Unknowns", fw-disasm §8)

- Continuous transfer off the chip on Linux and its latency tail at 20 and 40 Msps (paused).
- The fixed-gain recipe on the 8822C (JGR3 equivalent not yet tested).
- Whether the 8812CU can be switched to USB 3 on this host; at USB 2 it is bandwidth-limited.
- The BW40 tap anomaly, and the higher out-of-band floor on 8822C path B at 40 Msps.
- Whether devourer's background threads (thermal tracking) disturb captures.
- Meaning of the upper bits [31:24] that appear only during frames with CCA on.

## Bench rules in force (from the project brief)

- No permanent writes: never write efuse or flash, and never change VID/PID. On-chip code goes in
  RAM only, so a power cycle always recovers the adapter.
- Snapshot and restore any register you touch. If an adapter wedges, recover in this order:
  `libusb_reset_device`, then `usbreset`, then a VBUS cycle (needs a uhubctl hub, N3), then a
  manual replug.
- Receive is the goal. Transmit only to create test signals, with devourer's normal frame
  injection, on standard channels, at the lowest power that works (Sweden/ETSI).
- Don't touch the host's own network interface (`enp3s0`). Blacklist drivers only for the test
  adapters and restore them afterwards (`tools/rtw88_guard.sh off`). It is **on** as of this
  writing.
