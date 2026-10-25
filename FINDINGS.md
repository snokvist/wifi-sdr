# FINDINGS

Register maps, protocol notes, firmware behaviour and quirks, written for someone who has not read the
journal. Each claim cites its evidence (a file in `evidence/`, a capture in `captures/` — regenerable,
not in git — or a source file:line). Verified vs inferred is marked.

## Conventions
- "devourer" = OpenIPC/devourer at 0bf1957 (local clone `~/dev/snokvist/devourer`, `$DVR`).
- "ac54" = djObsidian/ac54_sdr at 2b41f98 (`ref/ac54_sdr`). **No LICENSE** → used as documentation of a
  technique only; no code copied (NEEDS N4). Full read-out with file:line citations:
  [`notes/ac54-analysis.md`](notes/ac54-analysis.md).
- LA = phydm "logic analyzer" / ADC-sampling mode. Sample word = one u64 in the TX-FIFO ring.

---

## 1. Bench units

| | RTL8812BU `0bda:b812` | RTL8812CU `0bda:c812` |
|---|---|---|
| die / cut | 8822B cut 3 (UMC), SYS_CFG1 0x0c493d37 | 8822C cut 5 (UMC), SYS_CFG1 0x0c495d37 |
| chains | 2T2R | 2T2R |
| EFUSE | id 0x8129, rfe_type 3 (stable 4/4 reads) | id 0x8129, rfe_type 3 (stable 4/4) |
| USB as found | **SuperSpeed 5000M**, bcdUSB 3.00, bulk MPS 1024 | **High-Speed 480M**, bcdUSB 2.00, bulk MPS 512 |
| vendor FW boot (devourer DLFW) | OK, 0x80 = 0xC078 | OK, 0x80 = 0xC078 |
| doctor | HEALTHY ch1 (1899 ok / 636 crc) | HEALTHY ch36 (2676 ok / 0 crc) |

Evidence: `evidence/phase0/doctor-8812{bu,cu}-*.{jsonl,stderr}`.

- The 8812BU was already in USB 3 mode because `rtw88_8822bu` performs the USB2→USB3 mode switch at probe
  and it persists until power loss. A cold-plugged unit enumerates USB 2 first (ac54 `tools/usb3.c`;
  rtw88 `rtw_usb_switch_mode_new`). **Not a permanent change** — a register write, lost on power-off.
- The 8812CU came up on a USB 2 root port with bcdUSB 2.00. Whether this die/board supports the same
  runtime switch is **open** (to check: PAD_CTRL2 0xc4 bit 20 / 0xff, read-only).
- USB-2 ceiling: ~35–40 MB/s of bulk payload in practice → 20 Msps of 16-bit IQ (80 MB/s) cannot fit;
  12-bit packed at 20 Msps (60 MB/s) cannot either. Without USB 3 the 8812CU tops out near 10 Msps (24-bit)
  — to be measured, not assumed.

## 2. LA-mode sample path, measured on these units (Phase 1)

Tool: devourer `rxdemo` with `DEVOURER_LA_CAPTURE` (src/LaCapture.cpp); analysis `tools/la_analyze.py`
(burst detection, PSD, L-LTF correlation for x and conj(x), CFO), `tools/phase1_figure.py`.

### Layout (verified on both chips)
- `dma0` + dbg port `0x880`: **one complex sample per u64**, 12-bit two's complement,
  **path A: I = [11:0], Q = [23:12]**. 8822C additionally carries **path B: Q = [43:32], I = [55:44]**.
  8822B high dword is all zero at this setting.
- Orientation: with this layout, ambient-frame L-LTFs correlate against the reference L-LTF and **not**
  against its mirror. 8812BU: 4/4 detections correct (peak 0.69–0.78 vs ≤0.44 conjugated). 8812CU: 9/9
  correct on both paths (0.78–0.97 vs ≤0.46). The L-LTF tone set is asymmetric in k, so a spectrally
  inverted capture would correlate as conj(x). Evidence: `evidence/phase1-*-cca-20M*.json`.
- Frequency placement: tuned to ch37 (5185 MHz) at 40 Msps, the ch36 AP (5180) is detected **only** after a
  −5 MHz shift, never at 0 or +5 — both chips, both 8822C paths. Figures:
  `evidence/phase1-bu-offcentre.png`, `evidence/phase1-cu-offcentre.png`.
- Rates: at BW20, the `40M` LA rate gives **genuine 40 Msps** (adjacent-duplicate fraction ≤1.5 %; L-LTF
  found with the 40 Msps reference; same CFO as at 20 Msps). ac54 reports the BW20 ADC is 40 Msps and that
  lower LA rates are sample *selection* (no anti-alias filter at that tap: a +11 MHz tone folds to
  −8.96 MHz at 20 Msps) — consistent with the ~±11 MHz passband seen here at 40 Msps.
- CFO of the ch36 AP vs each unit: 8812BU +77…+95 kHz, 8812CU +28…+48 kHz (≈15 and ≈7 ppm at 5180 MHz;
  partly the AP's own offset).

### 8822C two-path coherence (verified)
- Path B **leads** path A by a fixed **≈7.4 samples at 20 Msps (≈370 ns)** in every capture (integer
  part 7 or 8, per-tone phase-slope residual −0.35 / +0.6 respectively). Not physical (≈110 m of path);
  a capture-pipeline skew → **delay B by ~7.4 samples** before any coherent combining.
- After aligning, the per-tone ratio B/A is stable between the two L-LTF symbols to **0.4–4.3° rms**
  (9 frames, 5 captures): the chains share an LO and the capture is phase-coherent.
- Observation (unexplained): at 40 Msps path B's out-of-band floor (|f| > 12 MHz) is ~20 dB higher than
  path A's — path B looks less filtered. Open.

### Quirks
- **CCK freezes the 8822B sample bus.** At ch1 (2.4 GHz) a `crcok`-triggered capture held 16104 of 16384
  words at one constant (0x3d5c57) followed by ~14 µs of live data. On 5 GHz (no CCK) the ring is live
  throughout (max repeated word 112/16384). ac54 independently saw "whole banks of constant IQ" with CCK on
  and streams with `0x808[28]` (CCK block enable) cleared (`ac54-analysis.md` §4). → the streamer must clear
  `0x808[28]`. Not yet tested on the 8822C.
- **Post-AGC bus.** Captured frames sit only a few dB above the idle noise: the gain is switched per frame
  (visible as AGC steps / few-µs holds in |x|). An SDR needs a frozen gain; ac54 lists this as unsolved.
- **BW40 tap unexplained.** With `DEVOURER_BW=40` (RF at ch38 / 5190 MHz, `rf18=0x10926`) the 40M tap still
  shows a ±11 MHz passband centred at 0 and no L-LTF at 0/±10 MHz shifts (7 captures). Either 0x880 is a
  post-DFE primary-channel signal in BW40, or the filter/ADC config differs. Open; streaming targets BW20.
- Readback cost over EP0 (two reads per sample): 16384 samples in **1.22 s on USB 3** (8812BU) vs
  **12.4 s on USB 2** (8812CU). devourer's "~1 s per 1K" figure is a USB-2 number for a different host.
- Manual trigger fires immediately after arm → a manual capture is a random 0.2–0.8 ms window; at
  ≈100–200 ambient frames/s it usually holds no frame. `cca` (frame start) + `t<us>` post-trigger
  reliably holds a preamble.

## 3. Firmware images and the 88xx embedded CPU

Extracted locally by `tools/extract_vendor_fw.sh` / `tools/rtkfw88xx.py` into gitignored `vendor/`
(hashes in `vendor/SHA256SUMS`). Never committed.

- **ac54's "vendor IMEM" is not from the Windows driver.** `ac54/tools/fw_inspect.py:7-16` cuts it from
  `array_mp_8822b_fw_nic` in morrownr/88x2bu-20210702 (d31ffa82) `hal/rtl8822b/hal8822b_fw.c`. That array
  is byte-identical to `/lib/firmware/rtw88/rtw8822b_fw.bin` and devourer `hal/hal8822b_fw.c`
  (8822B NIC v30.20.0, 2020-05-14, image sha256 a72da690…). `rtkfw88xx.py info
  /lib/firmware/rtw88/rtw8822b_fw.bin.zst --ac54 DIR` reproduces ac54's `vendor-imem.bin` exactly
  (149952 B, sha256 f09acd6f6d047ef31bd4c25c950afd5a9d8527331385e752918b1f2f8de17d3f).
- Only **IMEM[0:0x230]** is used (sha256 7a2b616d4c1312dd48dccddb897ed148d5012aa36faa538581922cbcf1ba059d).
  It is: `0x000 jal 0x80000200`; an exception stub at 0x080 jumping to 0x80000329; at 0x200 SP =
  0x8020C000 (DMEM), CP0 Status = 0xff00 (IE=0), CP0 $20 clear 0x800 / set 0x400; fall-through to 0x230.
  Identical in every 8822B build examined (Linux v30.20 NIC/AP/WoW, Windows v28.5 NIC). Hazard: the
  exception vector lands inside whatever code is appended at 0x230 (+0xF8).
- **Header (88xx "3081" format, LE):** sig u16 @0x00, version @0x04, subversion @0x06, build date
  @0x10–0x15, mem_usage @0x18, dmem_addr/size @0x20/0x24, imem_size @0x30, emem_size/addr @0x34/0x38,
  imem_addr @0x3C. Body = DMEM, IMEM, [EMEM], each + 8-byte trailer (u16 XOR of LE halfwords + 6 zero
  bytes). Load address = header address with bit 31 cleared. **No signature field; no secure boot on the
  host download path** (halmac `chk_fw_size` demands the exact size).
- **CPU:** little-endian MIPS executing **MIPS16 / MIPS16e2** (8822B IMEM: 0 MIPS32 `jr ra`/`eret`; 3213
  in-image MIPS16 JALs, 0 JALX). Opcode 10111 is repurposed as CP0 moves. Inferred: Lexra/RLX-class core
  (R3000-style vector at 0x80000080). ac54 found only MIPS16e2 code executes after the vendor prologue.
- **Address map:** OCP/DMA view — 8822B IMEM 0x0, DMEM 0x200000, TX buffer 0x18780000; CPU kseg0 =
  0x80000000 + OCP; uncached kseg1 MAC registers 0xB86x_xxxx (0xB8640568 = REG 0x568), RX SRAM 0xB8700000,
  TX SRAM 0xB8780000.
- **8822C differs:** IMEM at **0x30000**, EMEM at **0x100000** (holds most code), DMEM 0x200000; IROM field
  0x10000; IMEM starts directly with SP/CP0 init (SP 0x80210000, Status 0xfe00) and has **no reset or
  exception vectors** → inferred mask ROM owns reset/exceptions. ac54's "keep vendor prologue, append code"
  does not map 1:1; the 0x09 clock-sync toggle is 8822B-only. Linux images: rtw88 rtw8822c_fw.bin v9.9.15;
  DKMS 88x2cu / devourer hal8822c_fw.c v9.17.0.
- **Download protocol (halmac_fw_88xx.c, identical for B and C):** CPU off (0x03 b2, 0x1D b0) → pltfm reset
  (0x1082 b0; +0x09 b6 on 8822B/8821C) → 0x80 b0 FWDL_EN → each section in 8192-B chunks as reserved page,
  DDMA ch0 (0x1200 SA = 0x18780000+48, 0x1204 DA, 0x1208 = OWN b31 | CHKSUM_EN b29 | len [| CONT b24]) →
  host writes DW_OK/CHKSUM_OK into 0x80 → FW_DW_RDY, CPU on, poll 0x80 == 0xC078 (failure: 0x10FC ==
  0xFAAAAA00 "illegal key").

### Static firmware RE (`notes/fw-disasm.md`): what was cross-checked
A subagent's read-only study of the NIC images; it tags claims [V]/[I]. Checked here against the
vendor sources (`~/dev/snokvist/devourer/reference/rtl88x2cu/hal/`) on 2026-10-25:
- **Register names hold:** `REG_C2H_PKT` 0x0294, `REG_FREERUN_CNT` 0x0568 (the µs timer measured in §4),
  `REG_WMAC_FWPKT_CR` 0x0601 at the cited `halmac_reg2.h` lines.
- **Hardware PSD engine, 8822B (11AC_2 branch):** `phydm_psd.c:461-462` sets control 0x910 and report
  0xf44; a tone index goes in 0x910's low bits, BIT(22) pulses the trigger, it settles
  `10 << (fft_points >> 7)` µs, and the report converts to dB **plus the current IGI**. That gives one
  tone per trigger, magnitude only, no phase: a spectrum-occupancy sensor, not an IQ source.
  **Correction to the study:** on the **8822C** (JGR3 branch) this vendor tree assigns no PSD
  registers. Its 0x1e8c/0x2d90 assignment sits under `#if 0`, so "0x910 on 8822C" is wrong.
- **IQK read-out, 8822C (`halrf_iqk_8822c.c`):** writing 0x40000000 to BB 0x1b38 triggers (:464, :601).
  The result reads as x = 0x1b38[31:20] and y = 0x1b38[19:8] (:439-440): scalar IQ-mismatch estimates,
  not a sample buffer. They could post-correct LA captures offline. The 8822B equivalent is not checked.
- **Stock firmware and gain:** none of the LA, dbg-port or IGI registers appears as a full 32-bit
  constant in either image's literal pool. Per the study's own caveat that is suggestive, not proof,
  since base+offset addressing escapes the scan. It is consistent with §4: the 8822B fixed-gain
  recipe is host-side register writes. The study's §7(b) lists AGC freezing as open; on our 8822B it
  is solved (§4); on the 8822C it is still untested.
- **Not cross-checked:** H2C table locations and index map, call-graph leaf identities, the 8822C entry
  stub, and the §7 leads. Treat them as the study's own claims.

## 4. Continuous streaming data path (ac54, 8822B) — Phase 2

Source: ac54 (`notes/ac54-analysis.md` §2–§8). Status: **transport reproduced on our 8812BU; the
large-packet path is not yet sustainable (see the C2H subsection below).**

```
 ADC (BW20: 40 Msps) ─LA block (free-running, never triggered)─► 128 KiB ring in TX-FIFO SRAM
   0x20000..0x40000 (OCP 0x18780000+off), 8 B/sample, two 64 KiB banks (bank period at 20 Msps = 409.6 µs)
        │  embedded MIPS16e2 program (vendor 0x230 prologue + custom code at IMEM 0x230)
        │  waits on free-running µs counter REG 0x568 (CPU 0xB8640568) for "bank complete"
        ▼  per bank: saves 224-B prefix, writes 24-B RX descriptors in-place, issues 4 overlapping
        │  C2H requests (16352 B each, 24 B overlap, first at bank+200) + 1 prefix/trailer packet
        │  via REG 0x294 (poll bit 16); TX-SRAM→C2H address alias is non-trivial (banks at 0x16000/0x6000)
        ▼
   USB bulk-IN EP 0x84: each transfer = 24-B RX desc (len 14b in [0..1], OWN 0x80 @3, C2H 0x10 @11) + payload
        ▼
   host: ≥32 queued async 64 KiB transfers; checks header, length, overlap bytes, trailer seq ==
   bank, deadline ordering, full 64 KiB coverage, one-ring-lag freshness (≥95 % identical = frozen)
        ▼
   output: interleaved LE int16 I,Q (12-bit × 16)
```

Key preconditions (ac54): MAC RX **must stay enabled** (0x100 |= 0x8a) — disabling it freezes the ADC
writer (their earlier "live" streams were frozen-ring false positives); frames filtered instead
(0x6a0/0x6a2/0x6a4 = 0, 0x608 |= 0x80000, low 16 cleared, RXDMA 0x290 = 14, 0x10c &= ~4); CCK off
(0x808[28] = 0); LA: 0x7c0 = 0 stop, 0x8f8 &= ~0x3c00000, 0x8b4 |= 0x80, 0x95c [11:8] dma_type, [7:5]
rate (fs = 80 MHz >> rate), [4:0] = 3; dbg port 0x8fc = 0; arm 0x7c0 = 0xcb. CPU load: halt → clock-sync
reset (0x1082/0x09) → 0x1080 |= 0x10100 → 0x80 = (old & 0x3800) | 1 → stage image in TXFF at 0x1000 →
DDMA to IMEM 0 → read back and compare → 0x80 = (old & 0x3800) | 0x4078, 0x1d |= 1, 0x03 |= 4.
EP0 reads on SuperSpeed are capped at 504 B per transfer.

Their measured results: 20 Msps for 60 s with 0 faults (single unit, quiet host); 40 Msps only in bursts
(best 0.66 s) — the limit is C2H latency outliers (normal 38–45 µs, worst 369 µs) against a 204.8 µs
opposite-bank window, not USB average throughput. On-chip packing tops out at 3.8 Msps; DDMA at ~15 MB/s.

### Measured on our 8812BU (Phase 3 groundwork)
- Free-running ring (0x7c0 = 0xCB, never triggered) is **live** after devourer `InitWrite` bring-up, with
  MAC RX on and all frames filtered (≈1 % of words identical between two reads; frozen ≥ 95 %). Both dbg
  port 0x0 and 0x880 are live (0x0 tags the upper word bits with a constant; 0x880 leaves them 0) —
  resolves unknown 1. A 128 KiB ring read over EP0 takes 12.5 ms (10.4 MB/s) on USB 3.
- `InitWrite` leaves IGI (0xc50/0xe50 [6:0]) at 0x40 → ≈4 LSB RMS. Pinning IGI is the gain control:
  **≈0.95 dB per step**, noise-floor RMS 198 @0x1c … 9.3 @0x38 (`evidence/phase3-igi-sweep-bu-ch36.txt`).
- **Fixed-gain mode (resolves unknown 3 for the 8822B):** OFDM RX CCA off (`0x838[1] = 1`, the 11AC
  branch of phydm `phydm_stop_ic_trx`, used by phydm's own PSD routine) + CCK off (`0x808[28] = 0`) +
  pinned IGI. No packet detection → no AGC steps; the LA writer stays live; the [31:24] state bits stay 0.
  Contiguous snapshots: CCA on → frame shows the AGC spike then a gain step-down; CCA off → frame flat at
  +22 dB over noise, no clipping at IGI 0x20 (`evidence/phase3-snap-{cca,nocca}-frame.png`).
- Negative: `0x808[29] = 0` (OFDM block off) **freezes** the LA writer.
- With CCA on, word bits [31:24] are non-zero only during frames (0x40, 0x80, 0x83–0x86, 0xc5, 0xc7, 0xc8,
  0xd2) — BB receive-state flags; undecoded lead.
- **Embedded CPU:** custom code at IMEM 0x230 runs (`wsdr_probe marker`): ≈4.2 M simple loop iterations/s;
  REG 0x568 (CPU 0xB8640568) is a µs counter (200,256 vs 200,232 µs host wall). Only **LUI** of the
  MIPS16e2 extended-LI group is implemented — ORI/XORI behave as LI, ANDI 0xffff as LUI 0xffff — so 32-bit
  constants must be LUI + ADDIU. After our code starts, 0x80 reads 0x4078 (no vendor ready bit 15).

### C2H large-packet transport on our 8812BU (Phase 3, 2026-10-25)
- **Working:** host-driven C2H and the on-chip bench. RX SRAM and both TX-bank aliases (TX0 0x16000,
  TX1 0x6000) deliver 8/8 packets; on-chip 16 KiB latency is 38–46 µs (min/p50/p99/max =
  38/41/45/46, mean 41.4) — inside the 204.8 µs 40 Msps bank window. One 1024-request run completed
  1024/1024 at 387 MB/s; it is **not reproducible**.
- **Blocked:** sustained 16 KiB C2H wedges after a random 3–1563 requests. Ten of eleven 1600-request
  runs failed (720, 0, 166, 813, 394, 56, 369, 428, 405, 323 completed); two signatures: the 0x294
  BIT16 trigger stays set with no packet, or the trigger clears and the packet never arrives. Retry,
  abort, restage, gap and descriptor checks do not recover; only a fresh bring-up does.
  `r288` overflow (BIT7) appears only after retries; `r280=0x0a03`, `r10c=0xf5a0`, `r284=0`.
- **Size dependence:** 256 B and 1024 B were 2000/2000 (two runs each); 4 KiB and 8 KiB were flaky
  (one 2000 pass, one early fail each); 16 KiB is unusable.
- **Ruled out (all negative):** CPU-driven vs host-driven, single vs 16 outstanding URBs, unique
  payload per request, rotating 16 KiB addresses, TX source, MAC RX on/off, CR force, RXDMA modes
  0/2/12/14, USB LPM clear, agg-threshold low byte, FWFF off, thermal thread off, RW_RELEASE_EN=0.
  Forcing Rw release (0x284 BIT18) sets RXDMA_IDLE and blocks C2H entirely (0 completions).
- **Handoff / next experiments:** `notes/c2h-wedge-handoff.md` (USB2 replug test first, then a
  devourer-vs-rtw88 register diff, a second host controller/board, and a wedge recovery without
  re-open). Matrix: `evidence/phase3-c2h-wedge-matrix.txt`.
- **USB2 (2026-10-25, E1):** after a replug with the rtw88 guard on, the 8812BU enumerates at 480M and
  the same 16 KiB C2H is reliable: 1600/1600 ×3, a 20000/20000 soak, TX0/TX1 1600/1600 ×2, 4/8 KiB
  2000/2000, c2hbench 1024/1024 ×3 at 347–365 µs/packet = 44.7 MB/s (wire-limited). Open: on USB2 the
  engine is paced ~9× slower, so "USB3 link" vs "back-to-back rate" is not yet separated (the matrix
  has no `--gap-us` run). Evidence: `evidence/phase3-c2h-usb2-*.txt`.
- **USB3, rate separated (2026-10-25):** paced to the USB2 cadence (`--gap-us 1000`) 16 KiB C2H still
  wedges on USB3 (102/150/15), so the failure belongs to the USB3 path, not to back-to-back requests.
  RX DMA burst size 512/64 B (0x290 = 0x1e/0x2e) is also negative. Smaller packets only lower the
  per-packet hazard (1 KiB: 5751/20000; 2 KiB: 243/1600). The streamer on USB3 ships a bank in 181 µs
  (enough for 20 MS/s) but wedges after 5–26 banks. No xHCI errors are logged. Remaining leads:
  rtw88-initialized chip without devourer init (E3), another host controller (E2), another board (E6).
- **USB3, driver ruled out:** with rtw88 owning the chip (its init, its kernel URBs; requests via debugfs,
  usbmon counting) 16 KiB C2H wedges at 287 and 43 requests, and rtw88's normal RX dies with it until
  the MAC is re-initialized. Applying rtw88's RX aggregation registers (0x10c = 0xf5a5, 0x280 = 0x100)
  to our path does not help either. The fault is in the chip's RX DMA → USB3 bulk-IN path when it
  carries C2H packets, on this board / this AMD xHCI (1022:15b9). Next separators: another xHCI
  controller, another 8822BU board, a USB3 hub in between. `evidence/usb3-regdiff/`.
- **USB3 resolved — it is the host controller/port.** On this host's other xHCI (0000:67:00.3, AMD
  1022:15c0) the same adapter, code and init run 16 KiB C2H 1600/1600 ×3, a 50k soak, c2hbench at
  391 MB/s, and the streamer at **20 MS/s for 60 s** and **40 MS/s for 60 s** (×2) with zero faults.
  The failing controller is 0000:65:00.3 (AMD 1022:15b9). At 40 MS/s the host needs a deep URB queue
  (256 × 16 KiB ≈ 13 ms); with 64 one bank once took 560 µs (> 202.8 µs budget) after 39 s.
  `evidence/phase3-c2h-usb3-xhci2.txt`, `evidence/phase3-stream-usb3-20m*`.

### Continuous streaming over USB2 (Phase 3, 2026-10-25)
- **Working:** `wsdr_probe stream` + `src/fw/stream8822b.S` stream the LA ring continuously. 60 s at
  **2.5 MS/s** (rate_sel 5, 20 MB/s) and 60 s at **5 MS/s** (rate_sel 4, 40 MB/s): 18311 / 36621 banks,
  zero strays, zero stale banks, every bank's framing and trailer sequence checked; on-chip shipping
  start is 0–0.8 µs after the bank is due; a bank ships in 1454–1475 µs (4 × 16376 B + 168 B trailer,
  USB2 wire time) against a 3256.8 / 1618.4 µs budget (45 % / 91 %). `evidence/phase3-stream-*`.
- **Offline continuity** (`tools/stream_check.py`, 60 s 2.5 MS/s capture, sha256 in evidence): 455 beacons
  of the ch36 AP, all intervals exact multiples of 102.4 ms (median residual 0 samples, max 320 µs
  CSMA jitter), 0 bank slips; no duplicate banks; sample steps at bank/chunk boundaries equal those at
  control offsets (paired log-ratio −0.07 ± 0.004 everywhere). The ~1.7 % "identical to one ring
  earlier" rate is lag-independent: it is the narrow noise-floor value distribution, not stale data.
- **Rate selectors:** 0x95c[7:5] = 4, 5, 6 all give live, non-duplicated data at BW20. `latime` (CPU
  arms, waits D µs on REG 0x568, triggers) measures rate 5 as exactly 2.5 MS/s; the writer starts at
  TXFF 0x20000 within ~1 µs of the arm write; after 5 s (12.5 M samples) the predicted ring position is
  exact (no ADC/REG 0x568 drift). So an open-loop bank schedule from the µs counter is exact.
- **Hazard (measured):** CPU 32-bit stores into the ring bank the ADC is *currently filling* are
  corrupted even far from the write pointer: a descriptor's bytes 8..15 came back as a copy of bytes
  0..7 (consistent with a 64-bit read-modify-write conflict). Shipping 1 ms early reproduces it every
  time (framing fault, run stops). CPU writes into a bank are therefore only safe while the ADC is in
  the other bank; the schedule keeps descriptor and sentinel writes inside that window (at 5 MS/s with
  ~160 µs to spare).
- **Integrity controls:** a frozen LA writer (`--ofdm 0`) is flagged stale on 303/305 banks (descriptor
  + last-sample sentinel survive a revolution); the synthetic mode (host-filled banks, LA stopped)
  matches the fill pattern byte for byte. 10 Msps (80 MB/s) cannot fit USB2.
- **rtl_tcp front end (`wsdr_tcp`):** the protocol's 32-bit frequency word cannot express 5 GHz; gr-osmosdr
  sends 5180 MHz as 885 032 704 (mod 2^32), and gqrx clamps tuning to the advertised R820T range. Both
  are handled (unwrap of 0.78–1.97 GHz; `--freq-offset` + client LNB LO). gr-osmosdr centres u8 at 127.4,
  which leaves a small DC line at this radio's low noise floor (gqrx "DC remove" clears it). Retune or
  gain change costs ~150 ms of samples (stream restart + devourer retune).

### Unknowns (Phase 2 exit list)
1. Does dbg port 0x8fc = 0 give a live bus on *our* unit? devourer reports dbg_port 0 as dead and uses
   0x880 (`docs/la-capture.md`). Conflicting; test both. **Resolved (8822B):** both are live.
2. Does the free-running ring stay fresh with devourer's bring-up (DIG/watchdog threads touching BB regs)?
   **Resolved (8822B):** live, ≈1 % identical between reads. Long-run disturbance is not yet measured.
3. Gain: no manual AGC freeze path known (ac54 open item; devourer has IGI control — test pinning it).
   **Resolved (8822B):** the fixed-gain recipe above. The 8822C is untested.
4. The TX-SRAM→C2H address alias (bank bases 0x16000/0x6000) — empirical on one ASUS unit; confirm here.
   **Resolved (8822B):** both aliases deliver on our cut; the remaining blocker is the 16 KiB wedge.
5. Linux usbfs tail latency vs WinUSB RAW_IO — whether 20 Msps holds and where 40 Msps breaks.
   **Open:** the 16 KiB wedge is the immediate blocker; a USB2 run and a host-stack comparison are the
   next tests (handoff note). The measured on-chip latency is inside the 40 Msps window, so tail
   latency is not the current limiter.
6. Whether the MIPS16e2-only / 0x230 splice behaviour holds on our cut (same die/cut 3 as ac54: likely).
   **Partly resolved:** code spliced at 0x230 runs on our unit; the implemented MIPS16e2 subset is
   characterized above.
7. 8822C: custom-code entry without vectors (ROM), EMEM layout, C2H register/addresses, LA ring at
   0x20000..0x40000 (devourer: same window), the 7.4-sample A/B skew, USB-2 bandwidth.
8. BW40/80 taps (80/160 Msps?) and dma_type 4–15 formats (dual-path packing) — undecoded.
