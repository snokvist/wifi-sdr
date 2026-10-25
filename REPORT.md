# Realtek 88x2 USB Wi-Fi adapters as raw-IQ receivers: progress report

_2026-10-25 · workspace `~/wifi-sdr` · targets: RTL8812BU (8822B, Jaguar2), RTL8812CU (8822C, Jaguar3),
RTL8812EU (8822E, stretch)_

This is an interim report. Phases 0–2 are complete, Phase 3 has its groundwork measured on the
8812BU, and Phases 3 (streaming) to 6 remain. Every number below comes from these two bench units and
is backed by a file under `evidence/`, a capture under `captures/` (regenerable, not in git), or a
source citation in [FINDINGS.md](FINDINGS.md).

## 1. Summary

**What works, measured:**
- Both adapters come up cleanly under devourer (doctor: HEALTHY) and deliver the chip's LA-mode ADC
  samples (12-bit I/Q) to the host.
- **Sample format verified on both chips:** one complex sample per u64, path A I[11:0] / Q[23:12]. On
  the 8822C, path B is Q[43:32] / I[55:44]. There is no spectral inversion (13 of 13 L-LTF detections
  correct), and frequency placement is correct: tuned 5 MHz off an AP, it appears exactly at −5 MHz.
- **The 8812CU's two receive paths are phase-coherent** once a fixed ≈7.4-sample (≈370 ns) A/B skew
  is removed. The per-tone phase is stable to 0.4–4.3° rms.
- **Fixed-gain receive on the 8812BU.** With OFDM carrier sense and CCK off, the per-frame AGC stops,
  and the gain follows one register (IGI) at **−0.94 dB per step over ≈45 dB** (fit residual
  ≤0.56 dB). Prior work (ac54_sdr) lists gain freezing as unsolved.
- **Contiguous 819 µs snapshots** (16,384 samples at 20 Msps) from a free-running sample ring that
  stays live after devourer's bring-up. A full ring reads back in 12.5 ms over USB 3.
- **Custom code runs on the 8822B's embedded CPU** (RAM only, recoverable by power cycle). Its
  free-running counter is a true µs timer (200,256 µs vs 200,232 µs of host wall time), and the
  executable instruction subset is characterized.

**Not done yet:**
- Continuous streaming (20 Msps for 60 s with zero integrity faults; the 40 Msps result).
- The rtl_tcp/SoapySDR front ends and the recorder.
- The 8812CU port and the 8812EU stretch goal.

Section 7 has the plan.

## 2. Bench

| | RTL8812BU `0bda:b812` | RTL8812CU `0bda:c812` |
|---|---|---|
| Die / cut | 8822B cut 3, 2T2R, RFE 3 | 8822C cut 5, 2T2R, RFE 3 |
| USB link on this host | SuperSpeed 5000M | High-Speed 480M (bcdUSB 2.00) |
| Firmware boot (devourer) | OK (0x80 = 0xC078) | OK |
| devourer doctor | HEALTHY, ch1 | HEALTHY, ch36 |

- Host: Linux 7.0, libusb through devourer (OpenIPC/devourer @ 0bf1957).
- Test signals: ambient Wi-Fi only (APs on ch1, ch36, ch44, ch149). There is no controllable transmitter
  on the bench (NEEDS N2).
- The 8812BU was already in USB 3 mode (the in-kernel driver switches it at probe; the switch is lost
  at power-off). The 8812CU came up on USB 2, which caps it near 35–40 MB/s of bulk payload.

## 3. Results by phase

### Phase 0: recon and bench
Inventory, driver guard and devourer health checks for both units. Evidence: `evidence/phase0/`.

### Phase 1: LA-mode baseline, both chips
Captured with devourer `rxdemo` (`DEVOURER_LA_CAPTURE`) and analysed with `tools/la_analyze.py`.

| Check | 8812BU | 8812CU |
|---|---|---|
| Layout | A: I[11:0], Q[23:12]; high dword zero | A as BU; B: Q[43:32], I[55:44] |
| I/Q orientation (L-LTF x vs conj x) | 4/4 correct (0.69–0.78 vs ≤0.44) | 9/9 correct, both paths (0.78–0.97 vs ≤0.46) |
| Off-centre test (tuned ch37, AP on ch36) | AP found only at −5 MHz | same, both paths |
| CFO vs the ch36 AP | +77…+95 kHz | +28…+48 kHz |
| 40M rate at BW20 | genuine 40 Msps (≤1.5 % adjacent duplicates) | — |

Quirks and traps:
- **CCK freezes the 8822B sample bus** at 2.4 GHz: 16,104 of 16,384 words held one value. Clearing
  `0x808[28]` avoids it.
- With the default AGC, the gain switches per frame, so frames sit only a few dB over the noise. That
  motivated the fixed-gain work in Phase 3.
- **The BW40 tap is unexplained:** at BW40 the 40M tap still shows a ±11 MHz passband centred at 0.
  Streaming targets BW20.
- At 40 Msps, 8822C path B shows a ~20 dB higher out-of-band floor than path A. Open.

Figures: `evidence/phase1-bu-offcentre.png`, `evidence/phase1-cu-offcentre.png`, per-capture
`evidence/phase1-*.{png,json}`. Details: FINDINGS §2.

### Phase 2: ac54_sdr's streaming method, documented
- The method: a custom program on the chip's CPU moves sample-ring banks to the host over the bulk-IN
  endpoint, timed by the µs counter. The host checks every bank for integrity (header, length,
  overlap, sequence, freshness).
- ac54's own results, from one ASUS unit: 20 Msps for 60 s with zero faults. 40 Msps ran only in bursts
  (best 0.66 s), limited by per-transfer latency outliers of up to 369 µs against a 204.8 µs window.
- **The firmware it builds on is stock Linux firmware.** The "vendor IMEM" ac54 uses is byte-identical
  to linux-firmware `rtw8822b_fw.bin` v30.20 and to devourer's embedded image, so nothing comes from
  the Windows driver. The image header has no signature field, and the host download path has no
  secure boot.
- The 8822C differs: IMEM at 0x30000, most code in EMEM at 0x100000, and no reset/exception vectors in
  the image (a mask ROM is inferred). The 8822B approach does not map over 1:1.

Full read-out with file:line citations: [notes/ac54-analysis.md](notes/ac54-analysis.md). Condensed
data-path facts and preconditions: FINDINGS §3–4.

### Phase 3 groundwork: 8812BU
- **Free-running ring live after devourer bring-up** (≈1 % of words identical between two reads;
  "frozen" would be ≥95 %). Debug ports 0x0 and 0x880 are both live on this unit. devourer's docs call
  port 0 dead, which disagrees.
- **Fixed-gain recipe:** `0x838[1] = 1` (OFDM RX CCA off) + `0x808[28] = 0` (CCK off) + IGI pinned at
  `0xc50[6:0]` / `0xe50[6:0]`. There is no packet detection, so no AGC steps, and the writer stays live.
  - Contiguous snapshots show the difference: with CCA on, a frame shows the AGC spike and a gain
    step-down; with CCA off, it is flat at +22 dB over the noise with no clipping at IGI 0x20
    (`evidence/phase3-snap-{cca,nocca}-frame.png`).
  - **Trap:** `0x808[29] = 0` (OFDM block off) freezes the writer.
- **IGI gain map**, ch36, CCA off. The noise-floor RMS is the smallest of four ring reads per setting
  (`evidence/phase3-igi-sweep-bu-ch36.txt`). Higher IGI means less gain:

  | IGI | 0x10 | 0x18 | 0x1c | 0x20 | 0x24 | 0x28 | 0x2c | 0x30 | 0x34 | 0x38 | 0x40 | 0x48 | 0x50 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | noise RMS (of 2048) | 668 | 303 | 198 | 134 | 84 | 54 | 34 | 22 | 14 | 9.3 | 4.0 | 1.9 | 1.4 |
  | gain rel. 0x40 (dB) | +44.5 | +37.6 | +33.9 | +30.5 | +26.5 | +22.5 | +18.6 | +14.7 | +10.9 | +7.3 | 0 | −6.5 | −9.1 |

  The linear fit over 0x10–0x40 gives −0.94 dB per step (max residual 0.56 dB). Above 0x40 the curve
  flattens into the quantization floor. devourer's `InitWrite` leaves IGI at 0x40, which looks "deaf"
  (≈4 LSB RMS).
- **Receive-state bits:** with CCA on, word bits [31:24] are non-zero only during frames (values 0x40,
  0x80, 0x83–0x86, 0xc5, 0xc7, 0xc8, 0xd2). They look like BB state flags; not yet decoded.
- **Embedded CPU:**
  - Custom MIPS16 code appended after the vendor prologue runs from RAM at ≈4.2 M simple loop
    iterations/s.
  - REG 0x568 is a µs counter.
  - Of the MIPS16e2 extended-immediate group only LUI exists (ORI/XORI act as LI), so 32-bit
    constants are built with LUI + ADDIU.
  - Probe: `src/fw/marker8822b.S`.

### Firmware study (static, read-only)
[notes/fw-disasm.md](notes/fw-disasm.md) maps the 8822B/8822C NIC images: load map, MMIO census,
command dispatch, calibration and PSD hooks. Its claims are tagged verified/inferred. Cross-checked
against the vendor sources (FINDINGS §3):
- The 8822B has a hardware PSD engine (0x910/0xf44). It reads one tone per trigger, magnitude only, so
  it is an occupancy sensor, not an IQ source.
- The 8822C IQK result register (0x1b38) yields IQ-mismatch estimates that could correct captures
  offline.
- One correction: the study's "PSD at 0x910 on the 8822C" is wrong; that vendor code path is disabled
  for the 8822C.

## 4. Done-criteria status (from the project brief)

| Phase | Criterion | Status |
|---|---|---|
| 0 | Bench recon, adapters healthy | **Met** |
| 1 | LA baseline: layout, orientation, placement, both chips | **Met** |
| 2 | ac54 streaming method documented | **Met** |
| 3 | 20 Msps for 60 s with zero integrity faults | Not yet |
| 3 | Tuning table | Partial: IGI gain map on one channel, from the noise floor |
| 3 | 40 Msps result recorded | Not yet |
| 4 | rtl_tcp (+ SoapySDR if feasible) and CLI recorder (.cs16 + JSON sidecar) | Not started |
| 5 | 8812CU port | Not started (Phase 1 done on it) |
| 6 | 8812EU + proposed devourer PR structure | Adapter not available; PR structure proposed in §8 |

## 5. Caveats

- One unit per chip, one host, one room. None of the numbers is shown to generalize.
- Ambient Wi-Fi only. Orientation and placement are verified against real frames, but there is no CW
  tone or known-level source yet, so the dB-per-step figure is relative (noise floor), not calibrated.
- A live ring and contiguous snapshots are not continuous streaming. Snapshots capture about 6 % of
  wall time.
- Fixed gain is measured on the 8822B only. The 8822C equivalent is untested.
- ac54's results are reported as theirs and have not been reproduced here.

## 6. Reproducing

**Prerequisites:** cmake, a C++20 compiler, libusb-1.0 headers, python3 with numpy and matplotlib,
`binutils-mipsel-linux-gnu` (for the on-chip probe), and a devourer checkout (default
`~/dev/snokvist/devourer`; override with `-DDEVOURER_DIR=`).

```sh
cd ~/wifi-sdr
sudo tools/rtw88_guard.sh on          # temp-blacklist rtw88_8822bu/cu/21cu + 88x2cu, then replug
cmake -S . -B build && cmake --build build -j --target wsdr_probe

# Phase 0: health (from the devourer tree)
sudo ~/dev/snokvist/devourer/build/doctor --pid 0xb812 --channel 1

# Phase 1: one-shot LA capture with devourer, then analysis
sudo DEVOURER_PID=0xb812 DEVOURER_CHANNEL=36 \
     DEVOURER_LA_CAPTURE=cca/20M/dma0/port:0x880/t100 DEVOURER_LA_OUT=captures/x.bin \
     ~/dev/snokvist/devourer/build/rxdemo
python3 tools/la_analyze.py captures/x.bin --plot captures/x.png --json captures/x.json

# Phase 3 groundwork (8812BU)
sudo DEVOURER_LOG_LEVEL=warn build/wsdr_probe ring --pid 0xb812 --channel 36 --igi 0x28 --nocca
sudo DEVOURER_LOG_LEVEL=warn build/wsdr_probe snap --pid 0xb812 --channel 36 --igi 0x20 --nocca \
     --count 12 --dump captures/snap
sudo DEVOURER_LOG_LEVEL=warn build/wsdr_probe marker --pid 0xb812 --channel 36

sudo tools/rtw88_guard.sh off         # restore the normal drivers
```

- **Vendor material:** `tools/extract_vendor_fw.sh` regenerates `vendor/` from local linux-firmware
  and the vendor driver trees (inputs listed in its header). `tools/fwre/run.sh` regenerates the
  listings behind the firmware study. `vendor/` is never committed or redistributed.
- **Map of the workspace:** [HUMAN-INVESTIGATION.md](HUMAN-INVESTIGATION.md).

## 7. Planned next steps

The facts each step builds on are in FINDINGS §2–4 and the two notes. Open questions are listed in
FINDINGS §4 "Unknowns".

**Phase 3, 8812BU continuous streaming** (in progress in a parallel session; its lane is posted in
`coord/from-opencode.md`):
1. Bring up the on-chip transfer path on Linux and measure its latency distribution, since tail latency
   decides whether 40 Msps can hold.
2. Build the bank-scheduled streamer with a host-side integrity harness: per-bank header, length,
   overlap, sequence and freshness checks, plus a fault counter.
3. Run 10 → 20 → 40 Msps. Done when 20 Msps runs 60 s with zero faults; record the 40 Msps result
   whatever it is.
4. Tuning table: extend the IGI gain map across channels in both bands. Use a CW or known-level source
   once one is on the bench (N2).

**Phase 4, front ends:**
- An rtl_tcp server: snapshot mode first, continuous once Phase 3 holds.
- A SoapySDR module if feasible.
- A CLI recorder writing `.cs16` plus a JSON sidecar (rate, frequency, IGI, chip, integrity counters).
- Viewers: SDR++ (not in Ubuntu's apt; upstream builds), plus gqrx and inspectrum (in apt).

**Phase 5, 8812CU:**
- An LA ring configuration for the 8822C register dialect (devourer's `LaCapture.cpp` covers it).
- The fixed-gain recipe in its Jaguar3 form, then an IGI sweep.
- A read-only check of whether the board supports the runtime USB 3 switch. On USB 2 the estimate is
  about 10 Msps (not yet measured).
- Two-path output with the ≈7.4-sample skew compensated.
- Streaming port: entry contract, memory map and transfer registers (FINDINGS §3 and §4 unknown 7).

**Phase 6, 8812EU:** repeat Phase 1 and the Phase 3 groundwork once an adapter is available (N5).

**Analysis threads that need no new hardware:**
- Decode the [31:24] receive-state bits.
- Explain the BW40 tap and the path-B out-of-band floor.
- Use the 8822C IQK read-out for offline IQ-imbalance correction.
- Check whether devourer's background threads disturb captures.

**Finish:** a final REPORT.md with the streaming measurements, a tuning table and per-chip
reproduction steps.

## 8. Upstreaming to devourer (proposed, not opened)

Small, independently reviewable pieces, each with its evidence:
1. **docs/la-capture.md:**
   - CCK freezes the 8822B bus;
   - `0x808[29] = 0` freezes the writer;
   - debug port 0x0 is live on an 8812BU cut 3;
   - the 8822C path-B skew (≈7.4 samples at 20 Msps) and two-path coherence;
   - the IGI gain map.
2. **LaCapture: optional fixed-gain mode for the 8822B** (CCA off, CCK off, pinned IGI), with snapshot
   and restore of every register it touches.
3. **LaCapture: free-running ring and contiguous-snapshot mode** (the `La8822b::snap()` semantics).
4. Later, after Phase 3: the streaming path as a separate, optional component with its integrity
   counters.

## 9. Licensing and handling

- Code in `src/` and `tools/` is GPL-2.0, matching devourer, which it builds on.
- Vendor firmware and driver files are extracted locally into `vendor/` and never committed;
  regeneration is scripted.
- ac54_sdr has no license, so it is used only as documentation of a technique. No code is copied
  (NEEDS N4).
- No permanent writes were made to any adapter: no efuse, flash or VID/PID changes, and on-chip code
  lives in RAM only.
