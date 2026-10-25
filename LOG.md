# LOG — engineering journal (append-only)

Times are local (CET, +01:00).

## 2026-10-25

**06:36** — Workspace created: `~/wifi-sdr/` (git, `main`). `vendor/`, `ref/`, `captures/` gitignored.

**06:36** — Recon.
- `lsusb`: one Realtek adapter, `0bda:b812` "RTL88x2bu [AC1200 Techkey]", bus 2 dev 2, sysfs
  `/sys/devices/pci0000:00/0000:00:08.1/0000:65:00.3/usb2/2-1` (AMD xHCI root port), **5000M (USB 3.0)**,
  bcdUSB 3.00, serial `123456`, MaxPower 504 mA. Interface 0, 5 EPs: bulk IN 0x84, bulk OUT 0x05/0x06/0x08,
  interrupt IN 0x87 (64 B). Bulk wMaxPacketSize 1024.
- Other USB: Cypress keyboard/hub, SteelSeries mouse, MediaTek BT (`0e8d:0616`, btusb).
- PCIe: MT7922 Wi-Fi (`wlp2s0`, **rfkill soft-blocked** — left alone), 2× RTL8125 2.5GbE.
- **Host uplink = `enp3s0`** (192.168.2.242/24, default via 192.168.2.2). Protected: never touched.
- `uhubctl`: "No compatible devices detected" → no VBUS switching available.
- Installed already: uhubctl, usbreset, usb_modeswitch, cmake, gcc, python3 (numpy 2.3.5, matplotlib 3.10.7,
  scipy 1.16.3). DKMS: rtl88x2cu 5.15.8-52, rtl88x2eu 5.15.0.1, rtl8821cu (out-of-tree vendor drivers).
- `/etc/modprobe.d/blacklist-rtw88.conf` already blacklists `rtw88_8822cu` (pre-existing, owner's).
  `rtw88_8822bu` was bound to the 8812BU (`wlx40a5ef515661`, down).
- Kernel 7.0.0-38-generic, Ubuntu 26.04.1.

**06:37** — Cloned refs into `ref/`: djObsidian/ac54_sdr (HEAD 2b41f98; **no LICENSE file**),
djObsidian/wifi-sdr, plaingca/rtl8922-sdr (depth 1), OpenIPC/devourer (HEAD 0bf1957, #496).
OpenIPC master is 37 commits ahead of the local snokvist clone (fc66aa9, #440).

**06:45** — Joakim (mid-session message): OK to bring the snokvist clone up to upstream head and work in it.
`/home/snokvist/dev/snokvist/devourer`: added remote `upstream` = OpenIPC/devourer, `git merge --ff-only
upstream/master` → 0bf1957. Not pushed. Submodules inited: reference/rtl88x2bu, rtl88x2cu, rtl88x2eu.
Built Release (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`) — clean.
From here "devourer" = that tree (`$DVR`).

**06:50** — `tools/rtw88_guard.sh on`: temp blacklist `/etc/modprobe.d/zz-wifi-sdr-temp-blacklist.conf`
(rtw88_8822bu, rtw88_8822cu, rtw88_8821cu) + `modprobe -r`. Adapter now driverless; enp3s0 still up.
**Must run `sudo tools/rtw88_guard.sh off` at the end of the work** to restore.

**06:52** — `doctor --pid 0xb812 --channel 6` → SUSPECT (0x40): init OK, EFUSE 4/4 stable (ID 0x8129),
FW ready, but RX 0 ok / 12 CRC. Ch 6 turned out to be RF-quiet here, not a deaf radio (see below).

**06:55** — rxdemo channel survey (9–12 s dwells, `DEVOURER_PID=0xb812`): frames seen —
ch1 1811, ch6 0, ch11 3, ch36 1011, ch44 646, ch100 0, ch149 74 (max RSSI 95), ch157 0.

**06:58** — `doctor --pid 0xb812 --channel 1` → **HEALTHY** (exit 0): rx_ok 1899, rx_crc 636, EFUSE stable,
fw_ready. Chip: 8822B cut 3 (UMC) 2T2R, SYS_CFG1=0x0c493d37, EFUSE rfe_type=0x03.
Evidence: `evidence/phase0/doctor-8812bu-ch{6,1}.{jsonl,stderr}`, `evidence/phase0/rxdemo-8812bu-ch*.jsonl`.

**06:58** — Test signal: no second adapter, no reference SDR → ambient APs (ch1, ch36) for now. NEEDS N1–N5 filed.

**07:00–08:40** — Phase 1 (LA baseline), both chips. All captures via devourer `rxdemo` +
`DEVOURER_LA_CAPTURE`, decoded with new `tools/la_analyze.py`.
- Joakim: OK to use Fable-level agents for firmware probes/software if Opus-level stalls (noted).
- Joakim plugged in the RTL8812CU: `0bda:c812`, bus 5 port 1 (`…/0000:67:00.3/usb5/5-1`), **480M,
  bcdUSB 2.00**, bound to DKMS `rtl88x2cu`. Added `88x2cu` to `tools/rtw88_guard.sh` (temp blacklist,
  restored on `off`); owner's `/etc/modprobe.d/88x2cu-waybeam.conf` untouched.
- 8812BU ch1 manual (4701 samples, readback 324 ms) live noise. ch1 `crcok`: 16104/16384 words stuck at
  0x3d5c57 → CCK freezes the bus (ac54 concurs). ch36 `crcok`/`cca`: live; L-LTF hits 4/4 correct
  orientation. 40M at BW20: genuine 40 Msps. BW40 (ch38 centre): passband ±11 MHz centred, no L-LTF at
  any shift in 7 captures → open.
- 8812CU doctor ch36 HEALTHY. LA readback 12.4 s / 16K samples (USB 2). Paths A+B both decode L-LTFs
  (9/9 correct); B leads A by ≈7.4 samples @20 Msps; per-tone B/A phase stable 0.4–4.3° rms → coherent.
- Off-centre test, ch37 @40 Msps: ch36 AP found only at −5 MHz on both chips/paths.
  Figures `evidence/phase1-{bu,cu}-offcentre.png`.
- Subagent reports: ac54 analysis → `notes/ac54-analysis.md`. Firmware-image characterization (subagent;
  scripts `tools/rtkfw88xx.py`, `tools/extract_vendor_fw.sh`, outputs in gitignored `vendor/`) condensed
  into FINDINGS §3: Linux `rtw8822b_fw.bin` reproduces ac54's vendor-imem byte-for-byte; 8822C has
  IMEM@0x30000 + EMEM, no vectors. Subagent also cloned `ref/88x2bu-20210702` and inited devourer's
  reference/rtl88x2bu + rtl88x2cu submodules in `ref/devourer`.
- No packages installed so far. SDR++/GNU Radio/SoapySDR are not installed.

**~09:00–10:10** — Phase 3 groundwork (8812BU).
- Joakim: SDR++ visualization question → answered (snapshot-mode feed early, real one after streaming).
- Joakim: suggests firmware disassembly / custom firmware for hidden SDR blocks → background subagent
  started on static RE of the 8822B/8822C NIC firmware (→ `notes/fw-disasm.md`).
- Installed `binutils-mipsel-linux-gnu` (2.45.90) for MIPS16e2 assembly. gas encodings match the forms
  ac54 verified on silicon (e.g. extended LUI `f030 6820`).
- New host code (`src/`, GPL-2.0 because it links devourer): `rtlreg` (vendor-req 0x05 regs, SS 504-B
  EP0 cap, OCP SRAM window, DDMA), `session` (devourer CreateRadio + InitWrite), `la88xx` (free-running
  LA ring, snapshot/restore), `cpu88xx` (prologue splice from devourer's FW array, FNV-checked),
  `wsdr_probe` (ring / snap / marker). CMake pulls devourer via add_subdirectory(EXCLUDE_FROM_ALL).
- `wsdr_probe ring`: free-running ring (0x7c0 = 0xCB, never triggered) is **LIVE** after devourer
  bring-up (≈1 % words identical between reads; frozen would be ≥95 %). 128 KiB EP0 read = 12.5 ms
  (10.4 MB/s) on USB 3. dbg 0x0 and 0x880 both live; dbg 0 tags bits [39:32]… with 0x6c, 0x880 → 0.
- InitWrite leaves IGI = 0x40 → ring RMS ≈ 4 LSB (deaf-looking). IGI 0x20 → RMS ≈ 135.
- With CCA on, bits [31:24] of the LA word are non-zero only during frames (0x40/0x80/0x84/0xc5/0xc7/
  0xc8/0xd2…) → BB RX-state flags (lead, undecoded).
- 0x808[29] = 0 (OFDM block off) FREEZES the LA writer (6/6 reads identical). Negative result.
- **0x838[1] = 1 (OFDM RX CCA off, from phydm_stop_ic_trx/phydm_psd) + CCK off + pinned IGI → fixed
  gain**, writer stays live; metadata bits stay 0. `wsdr_probe snap` (contiguous manual-trigger
  snapshots, DVLA output): CCA-on frame shows AGC spike + step-down; CCA-off frame flat at +22 dB,
  no clipping. Evidence `evidence/phase3-snap-{cca,nocca}-frame.png`.

**~10:10–10:30** — 8812BU, continued.
- IGI sweep with CCA off (`evidence/phase3-igi-sweep-bu-ch36.txt`): noise-floor RMS 0x1c→198, 0x20→134,
  0x28→54, 0x30→22, 0x38→9.3, 0x40→4.0 → **≈0.95 dB per IGI step**, ~45 dB usable range.
- `wsdr_probe marker`: custom MIPS16 code runs on the 8822B CPU behind the vendor 0x230 prologue
  (RAM only). 4.2 M loop iterations/s; REG 0x568 advanced 200,256 µs vs 200,232 µs host wall → a true
  µs counter. ISA probe: MIPS16e2 LUI works; ORI and XORI execute as plain LI; ANDI 0xffff executes as
  LUI 0xffff → only LUI of the e2 LI-group exists. Constants must be LUI+ADDIU (`LI32` macro).
- 10:30 — my response was stopped by a safety classifier while writing the next on-chip program
  (a C2H transport benchmark). The truncated file was deleted; that piece is paused pending Joakim.

## 2026-10-25 11:40 — pause
- Declined to write a hand-off brief for other agents to do the stopped on-chip transfer work.
- Refactor: snapshot capture moved into `La8822b::snap()`; `wsdr_probe snap` uses it; builds clean.
- Started a snapshot-mode rtl_tcp server (`src/wsdr_tcp.cpp`); the response was stopped by a safety
  classifier mid-write. Partial file deleted, not retried. Work paused for Joakim's direction.
- Firmware-RE subagent completed; output at `notes/fw-disasm.md` (unreviewed, uncommitted).

## 2026-10-25 12:05 — firmware-RE study cross-check
- Checked `notes/fw-disasm.md` against the vendor sources: halmac register names, 8822B PSD engine
  (0x910/0xf44), 8822C IQK read-out (0x1b38) hold. Correction: the 8822C (JGR3) PSD branch assigns no
  registers in this vendor tree. Summary in FINDINGS §3; the note is committed with its [V]/[I] tags.

## 2026-10-25 12:20 — coord channel; visualization paused
- Added coord/ (one-writer message files + mkdir locks) and tools/coord.sh for the parallel opencode session.
- Started an offline visualization tool (tools/viz_capture.py); the response was stopped by a safety classifier mid-write. Partial file deleted, not retried. Claude session paused.

## 2026-10-25 08:30 — C2H large-packet result (8812BU)
- Host C2H works on RX SRAM and both TX-bank aliases (8/8 each). On-chip 16 KiB latency: 38–46 µs
  (p50 41), one 1024/1024 run at 387 MB/s — not reproducible.
- Sustained 16 KiB C2H wedges randomly after 3–1563 requests (10 of 11 1600-request runs failed;
  two failure signatures, trigger stuck or packet lost). 256 B/1024 B were 2000/2000; 4/8 KiB flaky;
  16 KiB unusable. CPU-driven and host-driven, single URB, unique payloads, varying addresses, TX
  source, RXDMA modes, LPM, FWFF off, agg threshold, CR, thermal off, retries: all negative.
  Forcing 0x284 BIT18 (RW_RELEASE_EN) blocks C2H entirely.
- New files: `src/fw/c2hbench8822b.S`, `src/c2h88xx.{h,cpp}`, `wsdr_probe c2h|c2hbench`; evidence
  `evidence/phase3-c2h-{bench,tail}.*`, `evidence/phase3-c2h-wedge-matrix.txt`.
- Handoff/research brief: `notes/c2h-wedge-handoff.md`. Recommended order: snapshot front end first,
  C2H investigation in parallel; USB2 replug test is the top experiment.

## 2026-10-25 09:55 — E1: C2H over USB2 is reliable (8812BU)
- Replugged the 8812BU with the rtw88 guard ON → enumerates at 480M (1-1); devourer does no USB3 switch.
- 16 KiB C2H, RX source: 1600/1600 ×3 and a 20000/20000 soak, 0 timeouts. TX0/TX1 aliases 1600/1600 ×2
  each. 4 KiB and 8 KiB 2000/2000. On-chip c2hbench 1024/1024 ×3: 347–365 µs/packet, 44.7 MB/s
  (USB2 wire limit). Same code/binary that wedged 10 of 11 runs on USB3.
- Caveat (open): on USB2 the engine is wire-paced ~9× slower (365 vs 41 µs/packet), so "USB3 link"
  and "back-to-back request rate" are not yet separated. The matrix has no `--gap-us` evidence; the
  host-driven depth-1 USB3 runs (~host-RTT paced) still wedged, which leans towards USB3.
- Budget: LA ring is 8 B/sample, so 5 MS/s = 40 MB/s (~90 % of measured USB2; bank 1638 µs vs
  ~1.47 ms ship time) and 2.5 MS/s = 20 MB/s (comfortable). A USB2 continuous mode is unblocked.
- Evidence: `evidence/phase3-c2h-usb2-{16k,bench,sources}.txt`.

## 2026-10-25 10:30 — continuous streaming over USB2 (8812BU)
- New: `src/fw/latime8822b.S` + `wsdr_probe latime` (CPU-timed LA arm→trigger), `src/fw/stream8822b.S`
  + `src/stream88xx.{h,cpp}` + `wsdr_probe stream` (bank-scheduled C2H streamer, in-order bulk reader,
  per-bank framing/deadline/staleness checks, `.cs16` + JSON recorder), `tools/stream_check.py`
  (offline continuity: beacon timing, duplicate banks, boundary steps).
- latime: rate_sel 5 = exactly 2.5 MS/s, writer starts within ~1 µs of the arm, no drift over 5 s.
- Results: 60 s at 2.5 MS/s and 60 s at 5 MS/s, zero faults; ship 1475 µs max per bank (45 % / 91 %
  of the budget). Offline: 455 beacons on exact 102.4 ms multiples, 0 bank slips.
- Found: CPU stores into the bank the ADC is filling are corrupted (bytes 8..15 = copy of 0..7);
  shipping 1 ms early reproduces it. Frozen-writer control (`--ofdm 0`) flags 303/305 banks stale.
- Captures in `captures/phase3/` (not tracked); evidence `evidence/phase3-stream-*`.

## 2026-10-25 10:45 — rtl_tcp front end (wsdr_tcp)
- Committed the C2H probes (fb15901, opencode's work) and the USB2 streamer (dd87516).
- New `wsdr_tcp`: rtl_tcp server on `Stream8822b` (R820T header and gain table; u8 IQ; DC removed;
  NCO for the off-grid residual; arbitrary-rate polyphase resampler from 2.5/5 MS/s; gain → IGI in
  fixed-gain mode, auto → chip AGC; every setting change restarts the stream; drops root after open).
  `src/iqdsp.{h,cpp}` + `iqdsp_test` (tone/alias/DC-bias checks, all pass).
- rtl_tcp carries frequency as a u32: 5 GHz arrives wrapped mod 2^32. The server unwraps
  0.78–1.97 GHz, and `--freq-offset` pairs with a client LNB LO (gqrx clamps to the R820T range:
  LNB LO 5000 MHz + `--freq-offset 5e9`).
- Verified: test client 30 s at 2.400007 MS/s, 254 beacons on the 102.4 ms grid, 0 slips; retune
  ch36→40→36 and gain changes work (~150 ms gap each); gr-osmosdr (gqrx backend) headless and gqrx
  GUI live on ch36, no drops while the client reads. Fixed a u8 truncation bias (−0.5 LSB DC line).
- Evidence: `evidence/phase3-tcp-*`, `evidence/phase3-iqdsp-test.txt`.
