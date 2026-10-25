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
