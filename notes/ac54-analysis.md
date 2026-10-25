# ac54_sdr (ASUS USB-AC54 / RTL8822BU) — read-only port analysis

Repository analyzed: `/home/snokvist/wifi-sdr/ref/ac54_sdr` (djObsidian/ac54_sdr).
All code is Windows/WinUSB + libusb-1.0; there is no license, so this note
describes mechanisms, registers, sequences and constants with file:line
citations rather than copying code. Russian docs/commits are translated inline.

The device is an ASUS USB-AC54 B1, USB `0b05:1841`, bcdDevice `0x0210`,
Realtek **RTL8822BU** (2T2R 11ac). The chip was first mis-identified as
RTL8812BU in `PLAN.md` and `docs/usb-map.md` titles; corrected to the 8822B
family in `HANDOFF.md:606-616` ("CORRECTIONS to session 1"). It enumerates as
USB2 High-Speed first and is switched to SuperSpeed by the tool (see §7).

---

## 1. Overall architecture and the evolution of approaches

The project's goal (`PLAN.md:5-13`) was to expose raw baseband I/Q from the
Wi-Fi chip and use it as an SDR. The driver `rtwlanu.sys` was reverse-engineered
in Ghidra (`docs/command-map.md`, `analysis/ghidra/*.java`) to find an internal
"LA mode" (Logic-Analyzer / ADC-sampling) diagnostic that dumps 12-bit I/Q from
the baseband ADC. The host never uses the vendor driver at runtime: the chip is
brought up from scratch over libusb (a port of Linux `rtw88`, `tools/rtwinit.h`)
and the LA block is driven directly. The approaches, in chronological order
(git log order: `273b45b`, `51fcd65`, `2aacb30`, `6326700`, `9860002`,
`eb1aa8e`, `d7d5439`, `fd97d0f`, `1102e65`, `2b41f98`):

1. **Stage 0/1 — driver RE (`docs/command-map.md`).** Found two command tables
   reached from one NDIS dispatcher: a 35-entry "debug" console and a 49-entry
   phydm console. IQ capture is phydm cmd `lamode` index 0x11, handler
   `FUN_1403a4770` (`docs/command-map.md:190`). Samples are read from BB regs
   `0x8000`/`0x8004` in an 8-byte stride (`docs/command-map.md:250-262`), staged
   into a TX-packet buffer, and emitted as RX MPDUs over USB bulk. Conclusion:
   IQ capture is *pure host-driver* — no custom firmware and no H2C needed
   (`PLAN.md:144-148`).

2. **LA-mode snapshots via EP0 control transfers (session 2, `9860002`).**
   `tools/la.c`/`tools/la.h` arm the LA block (never continuous), trigger, poll
   `0x7c0` for completion, and read the 128 KiB TX-FIFO ring through the 4 KiB
   EP0 register window (`la.h:91-162`). One snapshot = 16384 samples @ 40 MS/s
   (BW20), ~410 µs of air; the full read-out cycle is ~100 ms, so ~10-17
   snapshots/s (`HANDOFF.md:638`, `:499-501`). This is a "snapshot SDR", not
   continuous. Superseded because EP0 read-out is ~1-2.3 MB/s and the duty cycle
   is tiny.

3. **SpyServer path (`tools/spyserver.c`, sessions 3-5).** A fake-SpyServer TCP
   server for SDR++. Capture thread: retune to a 5-MHz channel raster → LA
   snapshot → DC removal → digital shift by the residual → windowed-sinc FIR
   decimation → loop buffer; the network thread replays the newest loop buffer
   (`tools/spyserver.c:8-15`, `:111-170`). `live=1` was later added to stream
   the continuous 1.25 MS/s DDMA path, but the final CPU streamer is **not** wired
   into SpyServer (`README.md:70-71`, `HANDOFF.md:476-483`). SpyServer remains a
   viewer only; it was never the bottleneck.

4. **"Bank"/direct-C2H streaming (`faststream.c`, `bankstream.c`).** First
   continuous experiments kept the LA ring armed and read it live. The key trap:
   disabling MAC RX (`CR 0x100 bit7`) **freezes the ADC writer**, so earlier
   "live" 2.5/5 MS/s claims were frozen-ring false positives (99.98% of samples
   equalled the sample one 16384-ring earlier, `HANDOFF.md:182-189`). Fixed by
   keeping MAC RX enabled and rejecting ordinary Wi-Fi frames via RX filters,
   plus a freshness check (`tools/iqfresh.h`). `faststream.c` reads LA SRAM via a
   *direct C2H alias* (no DDMA) but corrupts individual samples while the ADC
   runs, so it is diagnostic-only (`faststream.c:1-7`, `HANDOFF.md:210-231`).
   `bankstream.c` schedules host reads of the *opposite* 64 KiB bank from the one
   the ADC is writing; validated at 2.5 MS/s/30 s (`bankstream.c`,
   `HANDOFF.md:161-170`).

5. **DDMA path (`tools/iqstream.h`, session 5, `d7d5439`).** LA ring (TX SRAM)
   → on-chip DDMA copy to RX SRAM → C2H request → USB bulk-IN EP 0x84. Validated
   continuous at **1.25 MS/s / 30 s** (`HANDOFF.md:413-474`, `docs/usb-map.md:3-8`).
   Bottleneck is DDMA throughput: TX→RX DDMA runs ~964 µs per 16128-byte block =
   ~15.1 MB/s (`HANDOFF.md:356-382`), far below the raw LA rate.

6. **Final "CPU stream"/DDMA path (`tools/cpustream.c` + `cpufw.h`, `fd97d0f`).**
   The decisive change: load a tiny custom program onto the chip's embedded MIPS
   CPU. The CPU itself issues the C2H requests from on-chip memory (no per-bank
   host EP0 round-trips) while the host only reaps bulk-IN completions. Combined
   with WinUSB RAW_IO this reaches a validated **20 MS/s for 60 s**
   (`HANDOFF.md:101-107`, `cpu-20-60s.txt`). 40 MS/s still fails on USB
   scheduling outliers (§8). This is the current best path.

Net result (`README.md:64-73`): 20 MS/s verified 60 s; 40 MS/s only in bursts;
RF validated at 2.4 GHz for tone/aliasing; gain control/calibration still open.

---

## 2. The custom RAM firmware

- **Source / language.** There is no separate .asm/.c firmware file. The MIPS16e2
  program is generated **in C at runtime** by a hand-written assembler in
  `tools/cpufw.h`: `struct m16` holds halfwords, with emit/ext/li32/addi/lw16/
  sw16/jump16/branch helpers (`cpufw.h:23-34`). The actual program bytes are
  built inline in each tool — e.g. the stream scheduler in `cpustream.c`
  (`cpustream.c:17-79`), the C2H throughput loop in `cpubench.c:110-121`, and the
  packing loop in `cpupackbench.c:170-177`. So the "firmware" is per-tool,
  emitted by host C, uploaded into IMEM.
- **Toolchain.** None for the firmware itself — it is assembled by the host tool
  with MSVC (the project build is `tools/mk.bat`: `cl /nologo /O2 /W3 /MD`,
  libusb linked, MSVC + newest Windows 10 SDK auto-discovered,
  `mk.bat:74-90`). Opcode encodings were cross-checked against GNU binutils
  (`cpuprobe.c:4` references `mips16-jalx.d`).
- **Target CPU/core.** Realtek 8822B embedded MCU is **MIPS, little-endian**, and
  only **MIPS16e2** stubs ran. `cpufw.h:3-4` states plain MIPS32 and JALX/
  PC-relative-load stubs did *not* execute; only MIPS16e2 appended after the
  vendor CP0 startup worked. `cpuprobe.c:1-5,22-50` tried MIPS16 JALX
  trampolines + MIPS32 and failed to produce a marker; the resolution was to
  preserve vendor CP0 init (see below). Addresses are little-endian 32-bit;
  uncached CPU view of SRAM is base `0xb8000000` (e.g. timer `0xb8640568`,
  C2H register `0xb8640294`, RX SRAM `0xb8700000`/`0xb8701000`, TX/LA SRAM
  `0xb8780000`/`0xb87a0000`, DMEM `0x80200000`/CPU-view `0x80201000`).
- **Memory map (from `tools/fw_inspect.py` parsing the vendor blob header,
  confirmed offline against devourer's vendored copy of the same image):**
  - IMEM load address `0x80000000`, size **149952 bytes** (0x24A40).
  - DMEM load address `0x80200000`, size **11208 bytes** (0x2BC8).
  - No EMEM (emem size field = 0; a secondary addr field reads `0x80121000`).
  - Firmware header: 64-byte header, then DMEM, then an 8-byte gap, then IMEM
    (`fw_inspect.py:11-16`: `off = 64 + dmem_size + 8`). Version 30, subversion 20,
    total image 161240 bytes.
- **Entry point.** The CPU is held in reset, IMEM is loaded, then started by
  `cpu_start()` (`cpufw.h:20-22`): write MCUFW_CTRL `0x80` = `(old & 0x3800)|0x4078`,
  set `0x1d` bit0, set `0x03` bit2. Boot handshake success is MCUFW_CTRL
  `0x80 == 0xC078` (vendor NIC image boots to that; `HANDOFF.md:176`,
  `cpuprobe.c:81`; observed FW register `0x80604078` in bring-up logs,
  `cpu-bringup.txt`). Custom code begins executing at IMEM offset **0x230**
  (`cpu_load` copies vendor bytes `[0..0x230)` then appends the program,
  `cpufw.h:7-18`; `jump16` targets are computed as `(0x230 + target*2)>>2`,
  `cpufw.h:34`).
- **Coexistence with vendor firmware — the "vendor IMEM".** The custom program
  **does not replace** the vendor image and **does not call** vendor routines as
  a library. What it needs is only the vendor's **CP0/stack/startup prologue**:
  the first **0x230 bytes** of the vendor IMEM are prepended verbatim, then
  execution falls through into the appended MIPS16e2 code at 0x230
  (`cpufw.h:7-18`, `HANDOFF.md:68-70`, `:180`). Early stubs failed precisely
  because they skipped that CP0 init (`HANDOFF.md:174-180`). The rest of the
  vendor IMEM beyond 0x230 is *not* loaded in the streaming path (`cpu_load`
  reads only `0x230` from the file and then the custom program, `cpufw.h:10-12`);
  `cpuprobe.c` can optionally load the full vendor IMEM+DMEM as a positive
  control that boots to 0xC078 (`cpuprobe.c:63`).
- **Where `tmp/vendor-imem.bin` comes from.** `tools/fw_inspect.py` reads the
  Realtek NIC firmware C array `array_mp_8822b_fw_nic[]` from the pinned vendor
  source `tmp/vendor88x2bu/hal/rtl8822b/hal8822b_fw.c` (morrownr/88x2bu-20210702,
  pinned at commit `d31ffa827bb95b8a436c2a469b5163b634ac4333`,
  `HANDOFF.md:261-262`), decodes the hex bytes, slices IMEM/DMEM by the header
  fields and writes `tmp/vendor-imem.bin` (149952 B) and `tmp/vendor-dmem.bin`
  (11208 B) (`fw_inspect.py:7-16`). `cpu_load` only ever reads the first 0x230
  bytes of `tmp/vendor-imem.bin` and requires that exact length
  (`cpufw.h:10-11`). No flash/efuse writes anywhere (`HANDOFF.md:71-72`).
  The identical image is vendored in this devourer tree at
  `hal/hal8822b_fw.c` (`array_mp_8822b_fw_nic`, len 161240), so the 0x230
  prologue can be regenerated here without the external clone.
  (Offline-verified: IMEM length 149952, sha256 of the full IMEM
  `f09acd6f6d047ef31bd4c25c950afd5a9d8527331385e752918b1f2f8de17d3f`, and of the
  needed prologue `imem[:0x230]`
  `7a2b616d4c1312dd48dccddb897ed148d5012aa36faa538581922cbcf1ba059d`.)

---

## 3. Firmware download / boot sequence from the host

All in `cpufw.h`. Register writes use the EP0 vendor path from `rtw.h` (req 0x05).

`cpu_halt()` (`cpufw.h:6`): `0x03 &= ~4` (clear CPU-enable) and `0x1d &= ~1`.

`cpu_load(program, bytes)` (`cpufw.h:7-19`), in order:
1. Read `0x230` bytes of `../tmp/vendor-imem.bin`; abort unless exactly 0x230.
2. Append the program; round total size up to 8 bytes.
3. `cpu_halt()`.
4. HALMAC 8822B/8821C clock-sync reset: `0x1082 &= ~1`, `0x09 &= ~0x40`,
   `0x1082 |= 1`, `0x09 |= 0x40` (vendor `pltfm_reset_88xx`,
   cf. `HANDOFF.md:336-339`).
5. `0x1080 |= 0x10100` — CPU_DMEM_CON: `WL_PLATFORM_RST | DDMA_EN`
   (`rtwinit.h:125` sets BIT16|BIT8 at power-on; here BIT16|BIT8 of the enable).
6. MCUFW_CTRL `0x80` = `(old & 0x3800) | 1`.
7. `txff_xfer(0x1000, code, size, write)` — push the IMEM image into the TX-FIFO
   staging window at TXFF byte 0x1000 (EP0, 504/4096-byte chunks, `la.h:91-113`).
8. `copy(0x18781000, 0, size)` — **DDMA** copy from TX-SRAM `0x18781000` to IMEM
   phys `0x0` (DDMA engine, `iqstream.h:27-39`: write src/dst/`0x80000000|len`
   to `0x1200/0x1204/0x1208` in one 12-byte vendor write; poll `0x1208` bit31).
9. `copy(0, 0x18784000, size)` then `txff_xfer(0x4000, verify, size, read)` and
   `memcmp` — reads IMEM back out via DDMA to TX-SRAM and verifies byte-exact.
   Prints "Custom RAM firmware verified: N bytes".

`cpu_start()` (`cpufw.h:20-22`): MCUFW_CTRL `0x80` = `(old & 0x3800)|0x4078`,
`0x1d |= 1`, `0x03 |= 4`. The CPU then runs from IMEM; host readback of a marker
word confirms boot (`cpuprobe.c:77-82`). DMEM is uploaded the same TXFF→DDMA way
to `0x200000` (`cpuprobe.c:63`, `loadmem`).

`cpu_mem(addr, data, bytes, write)` (`cpufw.h:35-42`) is the slow EP0 path into
on-chip SRAM via the 0x140 page window (`0x8000 + (addr&0xfff)`), used for seeding
descriptors and reading CPU state afterwards.

---

## 4. Chip bring-up before capture

All in `tools/rtwinit.h` (a hand-port of Linux rtw88 `rtw8822b.c`/`mac.c`/
`phy.c`/`efuse.c`, `rtwinit.h:1-9`). Sequence used by every capture tool is
`rtw_power_on(); rtw_read_efuse(); rtw_phy_init(); rtw_set_channel(11, BW20)`.

- **Power-on (`rtw_power_on`, `rtwinit.h:110-134`).** Detect cut/2T2R from
  SYS_CFG1 `0xf0` (observed `0x0c493d37` → cut D (3), 2T2R, `rtw_detect`
  `:102-108`). `mac_pre_system_cfg` (`:84-93`) programs RSV_CTRL, PAD_CTRL1,
  LED, GPIO, disables BB (`0x02 &= ~3`) and RF (`0x1f &= ~7`). If already
  powered, run act→cardemu→carddis transitions first, then
  carddis→cardemu→act power sequences (`pwr_seq`, tables in `t8822b.h`). Then
  `__rtw_mac_init_system_cfg`: CPU_DMEM_CON `0x1080 |= BIT16|BIT8`, SYS_FUNC_EN+1
  `|= 0xDC`, `0x1100[3]` nibble = 0x0C, clear `0x80` BIT20, finally
  `REG_CR 0x100 = 0x00` then `0xff` (MAC_TRX_ENABLE). Note RF/MAC init is a
  straight rtw88 port, **not** the vendor Windows driver.
- **EFUSE (`rtw_read_efuse`, `rtwinit.h:137-172`).** Dumps 1024-byte physical
  efuse via the `0x30`/`0x34` autoload state machine, de-maps to 768-byte
  logical. Reads `xtal_k = log[0xb9]`, **`rfe_option = log[0xca]`**, MAC =
  `log[0x107..]`. On this ASUS unit: id `0x8129`, **rfe_option = 2** (iFEM 2.4G /
  eFEM 5G), xtal_k `0x1d`, MAC `4c:ed:fb:b6:ff:d7` (`HANDOFF.md:621`,
  `artifacts/response-24g/init.txt`).
- **PHY tables (`rtw_phy_init`, `rtwinit.h:326-361`).** BB on (`0x02 |= 3`),
  RF on (`0x1f |= 7`), WLRF1 `0xec |= 7<<24`. Loads MAC/BB/AGC/RF-A (+RF-B if
  2T2R) tables from `tools/t8822b.h` with the rtw88 conditional table walker
  (`load_table`/`cond_ok`, `:194-220`; condition matches rfe_option, INTF_USB=2,
  cut). ~2905 writes, ~0.6-0.75 s (logs). Then xtal-cap into `0x24`/`0x28`,
  `config_trx_mode`, `rfe_init`.
- **TRX mode (`config_trx_mode`, `rtwinit.h:288-324`).** Default antenna state
  `ant_tx = BB_PATH_A`, `ant_rx = BB_PATH_AB` (`rtwinit.h:30`). For a 1T1R cut
  (`!rf_2t2r`) `ant_rx` is forced to PATH_A (`rtw_detect`, `:107`). Writes TX/RX
  path selects (`0xc08`/`0xe08`/`0x80c`/`0x808`/`0xa04`...), then an RF
  mode-table handshake (`rf 0xef/0x33/0x3e/0x3f`), `toggle_igi`, CCA and RFE.
- **Channel / gain / bandwidth (`rtw_set_channel`, `rtwinit.h:380-458`).** BB
  set_channel (per-2.4/5 GHz branches, `0x808/0x454/0xa80/0x814/0x958/0x860/
  0xa24/0xa28/0xcbc`), bandwidth into `0x8ac` (BW20/40/80 plus **BW5/BW10
  narrowband**, `:409-417`), MAC (`set_channel_mac`, `:364-378`, MAC_CLK_SEL=80M),
  RF (`rf 0x18` tracks channel+band+bw, `0xbe`/`0xdf`/`0xb8`, `:422-443`), RX
  DFIR (`0x948/0x94c/0xc20`, `:445-448`), `toggle_igi`, CCA, RFE. **Gain is not
  frozen**: IGI stayed 0x20 but RF0 gain changed across captures
  (`HANDOFF.md:641-643`, `docs/frequency-response.md:22-24`); manual AGC freeze
  is an open item.
- **LA / ADC configuration** (done by each capture tool, e.g.
  `cpustream.c:87-91`, `la.h:120-141`): keep MAC RX **enabled** and filter frames
  — `0x100 |= 0x8a`, `0x608 |= 0x80000` with low 16 bits cleared, RX filter maps
  `0x6a0=0x6a2=0x6a4=0`, RXDMA mode `0x290=14`, `0x10c &= ~4`. LA regs:
  `0x7c0=0` (stop), `0x8f8 &= ~0x3c00000` (dbg header=0), `0x8b4 |= 0x80`
  (rpt enable), `0x95c`: `[11:8]` dma_type, `[7:5]` **sample-rate selector**
  (`f = 80 MHz >> rate`), `[4:0]`=3 (REG trigger sel), `[31]`=0 edge;
  `0x8fc=0` dbg-port. **CCK disable** = clear `0x808` bit28 (`cpustream.c:91`),
  which removes a failure mode where CCK-on produces whole banks of constant IQ
  (`HANDOFF.md:82-83`). Arm = `0x7c0 = 0xcb` and **never trigger** (free-running
  ring, `iqstream.h:79`, `HANDOFF.md:420-421`). Sample-rate selectors used by the
  tools: **rate=1→40M, 2→20M, 3→10M, 4→5M** (`HANDOFF.md:138`;
  `fs = 80e6/(1<<rate)`, `cpustream.c:67`); rate=0 (80 MHz tap) at BW20 just
  duplicates every sample → real ADC = 40 MS/s (`HANDOFF.md:637`). Snapshot tool
  `la.h` uses path A, `dma=0` (12-bit I [11:0] + 12-bit Q [23:12]). Registers
  touched are saved and restored on exit (`iqstream.h:47-57,117-123`;
  `bulkprobe.c:42-47`); the whole chip is re-powered/re-tuned afterward
  (`cpustream.c:148`).

---

## 5. Data path end to end (the CPU-stream path)

Baseband ADC → LA ring in TX-packet SRAM → DDMA/C2H → RX SRAM → USB bulk-IN.

- **LA buffer.** 8822B "half-buffer" mode: the ADC continuously writes the
  128 KiB TX-FIFO region `0x20000..0x40000` (phys `0x18780000 + off`), 8 bytes
  per sample (`la.h:71-74`, `HANDOFF.md:633`). `0x7c0` exposes no live writer
  cursor, so the stream never reads the bank the ADC is in
  (`HANDOFF.md:420-421`). The CPU-stream path treats this as **two 64 KiB banks**
  at TX phys `0xb87a0000 + bank*0x10000` and copies the completed bank while the
  ADC writes the other (`cpustream.c:29-58`).
- **How the CPU learns new data is ready.** There is no interrupt; the embedded
  CPU reads a **free-running hardware microsecond counter** at CPU address
  `0xb8640568` (reg `0x568`) and accumulates exact fractional bank periods
  (`cpustream.c:71`, `:29-35`, `HANDOFF.md:73-77`). `period5 = 1024 << (rate-1)`
  is the bank period ×5 (fifths of a µs) so the CPU can divide/carry exactly
  (`cpustream.c:67`, `bank_program` `:29-34`). It busy-waits until the counter
  reaches the bank's target, then runs a deadline check (counter vs. a deadline
  word) and bails with an error flag if late (`check_deadline`,
  `cpustream.c:17-22`).
- **C2H / DMA engine.** Rather than using the DDMA copy engine per bank, the CPU
  issues **C2H "firmware-fast-forward"-style requests** by writing the request
  word to C2H register `0x294` (CPU addr `0xb8640294`) and polling bit `0x10000`
  for completion (`send_packet`, `cpustream.c:23-28`). Each request points at a
  24-byte RX descriptor placed just before the payload in SRAM; the chip streams
  **descriptor + payload** out the bulk-IN endpoint. The TX→C2H address mapping
  is non-obvious (physical USB SRAM order ≠ OCP order): for TX byte `a` the
  request is `(a & 0xffff) | ((a>>16)<<24) | 0x10000`, and the two 64 KiB banks
  alias to C2H bases `0x6000` (bank 1) and `0x16000` (bank 0)
  (`cpustream.c:46-50`, mapping table `HANDOFF.md:238-250`). The older
  `iqstream.h` path instead uses the real **DDMA** engine: write
  `src,dst,0x80000000|len` to `0x1200/0x1204/0x1208` (one 12-byte vendor write),
  poll `0x1208` bit31, copying LA phys `0x18780000+off` → RX phys `0x18701018`,
  then a C2H `0x294 = 0x11000` to ship RX SRAM `0x18701000` out the bulk pipe
  (`iqstream.h:27-39`, `:93-96`, `docs/usb-map.md:3-8`).
- **USB bulk-IN.** Endpoint **0x84** (IN BULK; USB map `docs/usb-map.md:47`).
  The host pre-submits async bulk transfers (QDEPTH=32 in `cpustream.c:8,104-108`)
  before the CPU publishes, so a completion is waiting when data lands. Each
  completion is `payload + 24` bytes and begins with the RX descriptor the host
  wrote, which it re-validates.
- **Per-bank framing (the CPU scheduler, `cpustream.c:29-58`).** A 64 KiB bank is
  shipped as **five parts**: four overlapping max-length C2H data chunks
  (`CHUNK = 16352`, `SHARED = 24` overlap bytes between consecutive chunks,
  first chunk starts at byte `FIRST_POS = 200`) plus one final "prefix/timing"
  packet. Before the descriptors overwrite the first bytes of the bank, the CPU
  **saves a 224-byte prefix** (`PREFIX = 224`) to RX SRAM `0xb8701028` and sends
  it last with four 32-bit timestamps (sequence, start, end, deadline)
  (`cpustream.c:37-57`). Largest packet is 16376 bytes — deliberately short of a
  full USB transfer to avoid an extra zero-length completion (`HANDOFF.md:126-127`).
- **Host parsing (`cpustream.c:110-136`).** For each expected part: wait for the
  matching queued transfer, check `status==COMPLETED`, check
  `length == size+24` and the first 12 descriptor bytes match the rebuilt header
  (`header()`, `:59`). Data parts: verify the `available-pos` overlap bytes equal
  the previously received copy (`overlaps`), then memcpy into `bankbuf`. Trailer:
  check `stamp[0]==bank`, `(int32)(deadline-end)<0`... wait, `stamp[2]-stamp[3]`
  deadline ordering, and full bank coverage (`available==65536`), then the saved
  prefix overlap. Then freshness check (`iq_check_freshness`), then unpack to
  int16 and write.

---

## 6. Framing, packing, sequence/overlap/freshness, error signalling

- **Sample packing.** LA word is 64 bits (8 bytes). For `dma=0/1` the active bits
  are 12-bit signed I in `[11:0]` and 12-bit signed Q in `[23:12]`
  (`la.h:164-166`: `la_i`/`la_q` sign-extend from bit11; `la2iq.py:9-11`). The
  upper 40 bits are metadata (observed prefixes like `0x7c000000`/`0x7c00000c`,
  `cpu-20-file-check.txt`, `corrupt-isolate.txt`). Output file format: interleaved
  **little-endian signed int16 I,Q**, each 12-bit value **scaled ×16**
  (`cpustream.c:131`, `iqstream.h:107`, `README.md:89`,
  `HANDOFF.md:448`). Other `0x95c[11:8]` dma_type codes are only guessed:
  0/1=24-bit path A/B; 4/5=48-bit (both paths?); 6/7=40-bit; 8/9=20-bit;
  10=32-bit; 11=64-bit dbg; 2,3,12-15 static (`HANDOFF.md:634-636`) — unverified.
- **RX descriptor header.** 24 bytes: `p[0]=len&0xff`, `p[1]=len>>8` (14-bit
  payload length), `p[3]=0x80` (OWN), `p[11]=0x10` (C2H flag); rest zero
  (`cpustream.c:59`, `iqstream.h:70`, `dmaprobe.c:200`). Host matches only the
  first 12 bytes plus length/status, because hardware mutates descriptor bytes
  12..23 (`HANDOFF.md:217-219`).
- **Sequence numbers & overlap.** The trailer carries a 32-bit sequence
  (`stamp[0] == complete`) and start/end/deadline µs timestamps
  (`cpustream.c:123-127`). Each data chunk overlaps the previous by `SHARED=24`
  bytes (CPU-stream) / `OVERLAP=32` samples (iqstream/bankstream, `iqstream.h:13`);
  the host requires **exact byte agreement** of the overlap with the prior fetch,
  else "Overlap failed"/"DISCONTINUITY" and stop (`cpustream.c:120`,
  `iqstream.h:102-104`).
- **Freshness (`tools/iqfresh.h`).** Overlap alone accepts a frozen ring, so
  `iq_check_freshness` compares the low 24 IQ bits at each ring address against
  the previous revolution; if ≥95% of samples are identical at one-ring lag it
  rejects the block as "FROZEN/REPEATING IQ" (`iqfresh.h:135-150`). Near-constant
  input is deliberately treated as unverified. `test_iqfresh.c` unit-tests it.
- **Deadline / flow control.** The CPU marks `error` in its DMEM state if a bank
  slips its opposite-bank window (`cpustream.c:17-21,78`); the host enforces a
  USB completion deadline per part and start/end guards of 2% each
  (`HANDOFF.md:111`). The reader paces on the nominal sample clock (QPC) and
  starts half a ring behind the writer (`iqstream.h:80`); on lateness it stops
  rather than drop/fill/replay (`iqstream.h:84-95`, `HANDOFF.md:454-459`).
- **Overflow / corruption signalling.** The tools never hide gaps: they exit
  nonzero and print the failing bank/part. A frozen bank is dumped to
  `cap/cpu-frozen.bin` for offline analysis (`cpustream.c:128-130`). CPU DMEM
  state (seq/target/frac/deadline/error/start/end/remaining + per-packet µs) is
  read back and printed on exit (`cpustream.c:141-146`).
- **Integrity checker.** The host validates, per bank: descriptor header match,
  exact length, USB status, inter-chunk overlap equality, saved-prefix overlap,
  trailer sequence==bank and deadline ordering, full 64 KiB coverage, and
  one-ring-lag freshness. An independent offline check
  (`tools/check_cpu_capture.py`) recomputes one-ring-lag equality and unique-value
  counts on saved files; the 60 s capture gave 1-ring-lag equality
  7.3e-07 and 6.24M unique IQ values (`cpu-20-file-check.txt`,
  `HANDOFF.md:105-106`).

---

## 7. Host-side tooling / WinUSB specifics and libusb-on-Linux equivalents

- **USB register access (`rtw.h`).** All register R/W is EP0 vendor control:
  `bmRequestType 0xC0` read / `0x40` write, `bRequest 0x05`, `wValue = addr`,
  `wIndex = 0`, 1-4 byte data (`rtw.h:35-52`, `docs/usb-map.md:70-84`). Because
  `wValue` is 16-bit, only addresses `0x0000..0xFFFF` are reachable directly
  (`rtw.h:2-4`); on-chip SRAM beyond that is reached through the TXFF page window
  (`0x106=0x69`, `0x140[15:0]=0x780+(addr>>12)`, window `0x8000..0x8FFF`,
  `la.h:90-113`). This is already libusb; on Linux it is identical
  `libusb_control_transfer`.
- **RAW_IO.** The decisive WinUSB feature. `libusb_endpoint_set_raw_io` is
  resolved dynamically from the loaded DLL (`cpustream.c:84`, `cpubench.c:123`)
  and enabled on EP 0x84. It removes per-transfer buffering so throughput rises
  from ~295 MB/s to **402 MB/s** of verified PRBS (`HANDOFF.md:85-92`,
  `cpu-raw-bench.txt`). RAW_IO requires host transfers that are a multiple of the
  endpoint max packet (65536-byte transfers used). The runtime DLL was upgraded
  from libusb 1.0.23 (no RAW_IO) to **1.0.30** (DLL sha256 in
  `HANDOFF.md:88`). **Linux equivalent:** there is no RAW_IO concept — on Linux
  bulk transfers go straight to usbfs; to approach the same effect submit many
  large (64 KiB) async `libusb_transfer`s and, if needed, raise the usbfs memory
  limit. The deeper problem RAW_IO solved (average throughput) is not the
  blocker; the latency outliers are (§8).
- **Transfer queue depth.** CPU-stream uses QDEPTH=32 outstanding bulk-IN
  transfers (`cpustream.c:8`); queues of 128/256 did not help the outliers
  (`HANDOFF.md:112`, `cpu-raw-40-q256.txt`). `bulkprobe.c` uses 8× 64 KiB
  (`bulkprobe.c:9-10`). On Linux these map 1:1 to async `libusb_submit_transfer`
  rings reaped with `libusb_handle_events_timeout`.
- **USB2→USB3 switch (`tools/usb3.c`).** Realtek USB3 chips enumerate USB2 first.
  `usb3.c` ports rtw88 `rtw_usb_switch_mode_new`: check `0xff==0x20` (already
  USB3), require PAD_CTRL2 `0xc4` bits (BIT20 | 3<<18), then `0x10c3 &= ~1`,
  `0xc4 = (pad & ~(3<<18)) | (2<<18) | BIT16`, `0xc5=4`, `0x04 |= BIT9`, sleep,
  `0xc4 |= BIT17` — device re-enumerates as SuperSpeed (`usb3.c:160-199`,
  `HANDOFF.md:552-563`). After the switch, bcdUSB=0300, EP0=512, EP 0x84
  maxpacket 1024, SS companion present, `0xff=0x20`. **SuperSpeed quirk:** vendor
  EP0 reads ≥512 B STALL/timeout; the tools cap EP0 reads at **504 bytes** on
  USB3 (4096 on USB2) (`la.h:95-97`, `usb-map.md:13-14`). Windows may rebind the
  new enumeration to the Wi-Fi INF, needing a Zadig/WinUSB re-bind
  (`bind_winusb.c`, `HANDOFF.md:491-494`). **Linux:** the mode switch is the same
  register writes; no Zadig — just re-open the device after re-enumeration. The
  504-byte EP0 cap must be reproduced.
- **USB LPM.** `cpustream` optionally disables USB U1/U2 via CLEAR_FEATURE 48/49,
  verified by GET_STATUS 000c→0000 (`cpustream.c:95-101`,
  `cpu-raw-40-nolpm-verified.txt`); it did not fix the 40 MS/s outliers. Linux:
  `libusb_control_transfer` CLEAR_FEATURE, or disable LPM via sysfs.
- **Process/thread priority.** `SetPriorityClass HIGH`, `THREAD_PRIORITY_HIGHEST`
  (`cpustream.c:82-83`). Linux equivalent: `SCHED_FIFO`/`nice`, CPU pinning.
- **Endpoints (`docs/usb-map.md:43-53`):** `0x84` IN BULK (RX/IQ), `0x05`/`0x06`/
  `0x08` OUT BULK (cmd/tx/h2c), `0x87` IN INT (C2H/status). Interface 0,
  vendor-specific class 0xff.

---

## 8. Measured results and adversarial caveats

- **20 MS/s / 60 s PASS** (final): 1,199,996,928 samples, 59.999846 RF s /
  60.001338 wall s, max bank 325 µs, 585,936 overlap + 146,482 freshness checks,
  CPU error 0 (`HANDOFF.md:101-104`, `cpu-20-60s.txt`). Independent file check:
  1-ring-lag equality 7.3e-07, 6.24M unique values — not a frozen replay
  (`HANDOFF.md:105-107`). Recorded 2-second waterfall demo at 2462 MHz/BW20 =
  39,993,344 samples, all checks passed (`README.md:13-15`,
  `docs/media/waterfall-capture.txt`). Caveat: two earlier 3-s takes stopped on
  deadlines; the clean take had no concurrent disk/analysis load; "does not
  establish unconditional long-term streaming reliability."
- **40 MS/s: bursts only.** Best = 0.661504 s / 26,460,160 samples before a
  deadline failure (`HANDOFF.md:108-110`, `cpu-raw-40-guard2.txt`). Explicitly
  **not** sustained.
- **The concrete blocker (on-chip timer).** C2H packets normally take 38-45 µs,
  but one took **369 µs** (`cpu-raw-40-profile.txt`), another 325 µs with 256
  queued reads (`cpu-raw-40-q256.txt`). A single outlier exceeds the entire
  204.8 µs opposite-bank window at 40 MS/s (`HANDOFF.md:116-121`). RAW_IO fixes
  average throughput, not these tail latencies. USB/DMA aggregation modes
  (`0x280` aggregation, `cpubench.c:127`) and U1/U2 disable did not help.
- **CPU software packing ceiling.** Discarding the upper 32 bits of each 64-bit
  word on-chip reaches only **3.803 MSPS** (clock selectors 0/2) / 2.799 MSPS
  (1/3) (`cpupackbench.c`, `cpu-pack-clock1/2.txt`, `HANDOFF.md:129-132`). So
  device-side packing cannot carry 40 MS/s either — the limiter is the on-chip
  memory/C2H bus, not USB.
- **DDMA throughput.** TX→RX DDMA ≈ 964 µs / 16128 B = ~15.1 MB/s;
  ~1.89 MS/s raw; proven live DDMA rate 1.25 MS/s (`HANDOFF.md:356-382`,
  `membench.c`). RX-SRAM→USB memory export alone = 158 MB/s but that is not live
  ADC.
- **Frozen-ring false positives (critical).** Disabling MAC RX froze the ADC
  writer; old 2.5/5 MS/s "live" claims were retracted (99.98% one-ring-lag
  identical, `HANDOFF.md:182-189`). Lesson baked into every tool: keep MAC RX on,
  filter frames, run the freshness check.
- **Live direct-C2H corruption.** With the ADC running, individual words mutate
  (often to zero) between overlapping reads even at 1.25 MS/s; `corruptprobe.c`
  localized a C2H-interval mutation and showed C2H into the *active* bank zeros
  SRAM words, but the *opposite* bank is clean (`HANDOFF.md:141-153`,
  `corrupt-isolate.txt`: mode=2 changed_read=92, bulk_diff=68). Negative fixes:
  disabling MAC TX, holding CPU, clearing descriptor OWN, RXDMA modes 0/2/12/14,
  `0x7cc` bit30 ring-extend — none fixed it (`HANDOFF.md:221-228`).
- **RF-response caveats (§9 data).** OTA response is uncalibrated (antennas +
  multipath + unfrozen RX gain); no −3 dB bandwidth claim; 20 MS/s is **not
  alias-free** (+11 MHz folds to −8.964 MHz, `HANDOFF.md:29-34`,
  `docs/frequency-response.md`, `artifacts/response-validation/cpu.json`).

---

## 9. Tuning: channel centres, the 160 centres, bandwidth

- **How centres are set.** `rtw_set_channel` programs RF reg `0x18` = band bits +
  channel number + bandwidth bits (`rtwinit.h:422-440`), plus 5 GHz band-edge
  tables (`low/middle/high_band`, `:382-384`) into RF `0xbe`, and bandwidth into
  BB `0x8ac` (`:409-417`). Frequency raster is 5 MHz: `freq = 5000 + 5*ch`
  (5 GHz) and the 2.4 GHz raster `2407 + 5*ch` with ch14 = 2484 MHz
  (`spyserver.c:96-110`).
- **The 160 centres.** 2.4 GHz: 2412-2472 MHz in 5-MHz steps (ch 1-13) + 2484
  (ch14); 5 GHz: 5160-5885 MHz in 5-MHz steps (ch 32-177). All 160 confirmed with
  Pluto test tones on 2026-10-17, BW20/path A, 40-MS/s LA snapshots: 159 passed at
  20 dB TX attenuation, 5885 MHz (ch177) needed a 10 dB repeat
  (`README.md:32-47`, `docs/tuning-range.md`, `HANDOFF.md:3-21`,
  `artifacts/response-tuning*/`). This is tone reception on this one unit, **not**
  uniform sensitivity, filter response, or continuous streaming per frequency.
- **SpyServer fractional tuning.** Picks nearest 5-MHz hardware centre and shifts
  the residual digitally (DC removal + complex multiply + windowed-sinc FIR
  decimate, `spyserver.c:8-15,160-170`); for 5 GHz, client freq `< 2 GHz` is
  interpreted as `f + 4000 MHz` (set SDR++ Offset 4000 MHz, `spyserver.c:94`,
  `README.md:49-57`). Verified at 2413.25/5161.25/5883.75 MHz.
- **cpustream is hardcoded to ch11 / 2462 MHz / BW20** — its CLI args set rate and
  seconds, **not** the RF centre (`README.md:59-62`, `cpustream.c:86`,
  `docs/tuning-range.md:105-107`).
- **Bandwidth.** `rtwinit.h:409-417` supports BW20/40/80 and BW5/BW10 narrowband.
  Only **BW20 at 40 MS/s** has established ADC behaviour; filter passbands and ADC
  clocks for other widths are unmeasured (`HANDOFF.md:52-60`). LA rate reduction
  is sample *selection*, not an anti-alias decimator at that tap
  (`HANDOFF.md:57-59`).

---

## 10. UNKNOWNS / open issues

Stated by the author:
1. **Sustained 40 MS/s** is unsolved; limited by C2H/DDMA latency outliers, not
   USB average throughput or host packing (`HANDOFF.md:113-133`, `:300-306`).
2. **Compact hardware packing / alternate ADC destination** — none found in the
   vendor LA source for 8822B (`HANDOFF.md:302-305`, `:397-399`); on-chip packing
   measured too slow (§8).
3. **Manual gain control / AGC freeze** — RX gain still moves; no frozen-gain path
   (`HANDOFF.md:61-64`, `:641-643`, open issue 1 in `usb-map`/session-2 list).
4. **I/Q orientation & imbalance / DC offset / clock calibration** — sign verified
   at 2.4 GHz only; oscillator offset ~+36-38 kHz unattributed
   (`HANDOFF.md:29-34`, `docs/frequency-response.md:17-21`).
5. **dma_type 4-15 formats** (dual-path, 48/40/20/32/64-bit) undecoded; BW40/80 →
   80/160 MS/s untested (`HANDOFF.md:634-636`, `:646`).
6. **5 GHz filter response** never measured (`README.md:28`, `HANDOFF.md:34`).
7. **SpyServer still uses the old DDMA path**; CPU streamer not integrated, no
   channel/width args, retune gaps (~0.7 s) (`HANDOFF.md:62-64`, `:476-480`).
8. **MCUFW_CTRL[13:12] CPU clock selectors** — effect on packing seen, actual MHz
   unverified; "do not assign MHz" (`HANDOFF.md:327-330`).
9. **Exact LA word metadata** (upper 40 bits) and whether data is pre- or
   post-OFDM/digital not fully characterized (`command-map.md` Stage-2 note).

Additional ones I notice:
- `cpu_load` hard-requires a local `../tmp/vendor-imem.bin` of exactly 0x230 bytes
  (`cpufw.h:10-11`); no in-repo copy — the pinned vendor source must be present.
  (The same NIC image is in *this* devourer tree at `hal/hal8822b_fw.c`, so the
  0x230 prologue can be derived here instead.)
- The CPU-view / phys / OCP address maps are only partly documented; the
  TX↔C2H alias table (`HANDOFF.md:238-250`) was found empirically and may be
  cut/bank dependent.
- All validation is on a **single** ASUS 8822BU unit; numbers are not shown to
  generalize across units or hosts.
- MIPS16e2-only execution and the 0x230 CP0 dependency are empirical; the reason
  MIPS32/JALX failed is not understood (`cpufw.h:3-4`).

---

## 11. ASUS 0b05:1841-specific facts vs a generic RTL8812BU

- **VID/PID.** `rtw.h:13-14` hardcodes `0x0b05:0x1841`; `rtw_open` opens exactly
  that (`rtw.h:22`). A generic **RTL8812BU is `0bda:b812`** — change VID/PID
  (and any topology selection) in `rtw.h`.
- **RFE / front-end.** This unit's efuse gives **rfe_option = 2** (iFEM 2.4G /
  eFEM 5G, `rtwinit.h:224` `rfe_is_efem`, observed in `init.txt`). The CCA/RFE
  tables (`set_channel_cca` `:248-277`, `set_channel_rfe` `:226-246`) branch on
  rfe_option (2 vs 3/5 vs ifem_ext). A different board (different front-end
  module) will report a different rfe_option and select different `0x82c/0x830/
  0x838/0xcb0/0xcb4/0xca0` values — **do not hardcode rfe_option=2**; read it
  from efuse (`rtw_read_efuse`, `log[0xca]`). `init.exe` already allows an
  `rfe=N` override (`init.c:23-24`).
- **EFUSE layout.** `xtal_k` at logical `0xb9`, `rfe_option` at `0xca`, MAC at
  `0x107` (`rtwinit.h:165-167`) are rtw88 8822B offsets and should hold for any
  8812BU/8822BU, but values (xtal_k, MAC, efuse id `0x8129`) are per-unit.
- **2T2R vs 1T1R / path A vs B.** This unit is 2T2R (SYS_CFG1 BIT27). A 1T1R
  8811CU-style cut is auto-detected and forces `ant_rx = PATH_A`
  (`rtw_detect`, `:107`). The data path is **path A only** throughout
  (`dma=0`/`BB_PATH_A`); path B is captured for the RF18 channel readback but its
  I/Q is never used (`docs/tuning-range.md:74-75`). A single-path **RTL8812BU**
  would already work on the path-A path; no change needed there. (Note: RTL8812BU
  is the 1T1R/2T2R USB sibling in the same rtw88 `rtw8822b` family — the init
  tables are shared; only the chip detect and efuse differ.)
- **USB3.** 8812BU is natively USB3 too and enumerates USB2-first; the same
  `usb3.c` switch applies. The 504-byte SuperSpeed EP0 read cap (`la.h:95-97`) is
  a chip-family property, keep it.
- **PID checks.** `rd8(0xff)!=0x20` ("Run usb3 first") gates the streamers
  (`iqstream.c:35`, `bankstream.c:26`); that is mode-based, not PID-based, so it
  carries over. No other PID-specific logic beyond the open.

---

## Porting checklist for Linux / libusb

1. **Open / claim.** Replace the Windows `rtw.h` open with the devourer
   `UsbTransport`/`claim_interface_then_reset` path; parameterize VID/PID
   (`0b05:1841` here, `0bda:b812` for generic 8812BU). EP0 vendor req 0x05,
   type 0xC0/0x40, wValue=addr, wIndex=0 is unchanged.
2. **USB3 switch.** Port `usb3.c` register writes (`0x10c3`,`0xc4`,`0xc5`,`0x04`);
   after re-enumeration, re-open (no Zadig on Linux). Keep the **504-byte EP0
   read cap** on SuperSpeed; TXFF page window (`0x106=0x69`, `0x140`) unchanged.
3. **Bring-up.** Port `rtwinit.h` (power seq, efuse — read rfe_option/xtal_k/MAC,
   PHY tables from `t8822b.h`, trx mode, set_channel). Devourer already has the
   Jaguar2/8822B HAL (`src/jaguar2/`) and the identical FW blob
   (`hal/hal8822b_fw.c`) — prefer reusing that bring-up over re-porting rtw88.
4. **Vendor IMEM prologue.** Extract IMEM[0:0x230] from `hal/hal8822b_fw.c`
   (`array_mp_8822b_fw_nic`, header at offset 64+dmem_size+8; IMEM size 149952,
   DMEM 11208, IMEM@0x80000000, DMEM@0x80200000). Prepend it, append the
   MIPS16e2 program, round to 8 bytes.
5. **IMEM upload + verify via DDMA.** Reproduce `cpu_load`: halt CPU, HALMAC
   clock-sync reset (`0x1082`,`0x09`), CPU_DMEM_CON `0x1080 |= 0x10100`,
   MCUFW_CTRL `0x80=(old&0x3800)|1`, TXFF-stage at 0x1000, DDMA `0x1200/04/08`
   copy TX→IMEM, read back and memcmp. Start with MCUFW_CTRL=0x4078 + `0x1d`,`0x03`.
   Boot = MCUFW_CTRL==0xC078.
6. **LA config.** Keep MAC RX enabled (`0x100|=0x8a`), filter frames
   (`0x6a0/0x6a2/0x6a4=0`, `0x608`, `0x290=14`, `0x10c&=~4`), CCK off
   (`0x808 bit28=0`), LA regs `0x8f8/0x8b4/0x95c/0x8fc`, arm `0x7c0=0xcb`
   (never trigger). Pick rate selector in `0x95c[7:5]`.
7. **Data path.** Emit the CPU's per-bank C2H scheduler (free-running timer
   `0x568`, opposite-bank reads, 4 overlapping chunks + prefix/timing trailer,
   24-byte RX descriptor `len/OWN0x80/C2H0x10`, C2H request to `0x294`).
   Pre-submit a ring of 64 KiB async bulk-IN transfers on EP 0x84.
8. **RAW_IO substitute.** No RAW_IO on Linux — use many large async transfers;
   consider raising the usbfs buffer limit and `SCHED_FIFO` to chase the latency
   tails. Expect the same 40 MS/s outlier wall; target 20 MS/s.
9. **Integrity.** Port exact-overlap, freshness (`iqfresh.h`), sequence and
   deadline checks verbatim; **never** drop/fill/replay on lateness — fail.
10. **Output.** Interleaved LE int16 I,Q, 12-bit ×16 (or scale to float). Add
    channel/BW selection the Windows tool lacks (cpustream is fixed at ch11/BW20).
11. **Teardown.** Halt CPU (`0x03&=~4`, MCUFW_CTRL=0), restore every saved
    register, re-power/re-tune the chip; honour devourer's IRadio-first teardown
    order.
