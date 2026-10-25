# RTL8812CU (RTL8822C cut 5): custom on-chip code

Status 2026-10-25: **custom code runs on the 8822C's embedded CPU** (RAM only), C2H works, and
**the LA ring streams continuously** (2.5 / 5 MS/s, 60 s, zero faults; FINDINGS §4). Tool: `build-cu/wsdr_cu` (`cmake -S . -B build-cu && cmake --build build-cu --target wsdr_cu`;
CMake via `cmake/cu.cmake`). [V] = measured here, [I] = inferred.

## Loader (`src/cpu8822c.{h,cpp}`)
- Image = the vendor 8822C NIC firmware's first 0x3c bytes of IMEM (entry stub: sp = 0x80210000 + CP0
  setup, cut before the MCUFW_CTRL warm/cold branch; FNV-checked, taken from devourer's embedded array
  at build time, never committed) + our MIPS16 program, linked at 0x8003003c.
- Path: halt CPU (0x0003[2], 0x001D[0]) → platform reset (0x1082[0] low→high) → DDMA_EN →
  FWDL_EN → stage in TXFF 0x18781000 through the packet-buffer window → DDMA to IMEM OCP 0x30000 →
  forge FW_DW_RDY / DW_OK bits in 0x80 → CPU on.

## What the hop test found (`wsdr_cu memtest`, `evidence/phase5-cu-memtest.jsonl`)
- [V] **The 8822C hangs on large EP0 block transfers.** A 4 KiB read through the 0x8000 window timed
  out and left the chip's register interface dead (efuse autoload fails, every read errors) until a
  physical replug; a USB reset and a port disable/enable did not recover it. Devourer reads 8822C SRAM
  4 bytes at a time for this reason. `wsdr_cu` sets `Reg::set_cap(4)` (default `--cap 4`), and
  `Reg::ddma` falls back to three 32-bit writes when the cap is below 12.
- [V] With 4-byte transfers: TXFF window write/read, DDMA TXFF→TXFF and DDMA TXFF→DMEM→TXFF are exact
  (4096 B each).
- [V] **IMEM does not read back over DDMA** (TXFF→IMEM→TXFF returns unrelated data at 0x30000 and
  0x31000) although the write lands (the marker runs). The loader's IMEM verify is therefore off by
  default; devourer verifies with the DDMA checksum instead.

## Marker (`src/fw/marker8822c.S`, `evidence/phase5-cu-marker.jsonl`)
- [V] magic 0xCAFE0C22 written to DMEM 0x80208000, loop counter 5.33 M iterations/s (8822B: 4.2 M).
- [V] REG 0x568 is a µs counter here too: 206,742 µs over 206,742 µs of host wall time.
- [V] ISA as on the 8822B: LUI works; ORI/XORI act as LI; ANDI 0xffff as LUI 0xffff. Chip alive after.

## C2H (`wsdr_cu c2h|c2hbench|c2hmap`, `evidence/phase5-cu-*`) — working
- [V] **Root cause of the missing packets: devourer's Jaguar3 coex/phydm runtime thread.** With no RX
  loop running it keeps a synchronous 16 KiB bulk-IN read posted on EP 0x84, so our C2H packets were
  split between its reads and ours (usbmon: every request produced its packet within 0.1–0.3 ms;
  our reader saw only as many as it had transfers posted). Earlier "one-packet hold", "needs RX
  traffic" and "depth-limited" readings were all this race. The thread also rewrites DIG/IGI every ~2 s.
  Fix: devourer `tuning.j3_runtime_thread = false` (branch `pr/jaguar3-runtime-thread-optional`);
  `Session` sets it when the field exists.
- [V] Host C2H from RX SRAM 0x1000: 64/64 at 1 KiB, **1600/1600 at 16 KiB** (14.4 MB/s host-driven).
- [V] **CPU-driven bench: 1024/1024 ×3 at 44.7 MB/s**, 365 µs per 16 KiB packet (USB2 wire limit,
  same as the 8822B on USB2).
- [V] Keep RX-DMA aggregation off for C2H: with 0x10c[2] = 1 the request is not accepted.
- [V] **C2H address map** (`c2hmap`, all 58 probes clear, 32/32 ring pages found):
  C2H 0x06000–0x0ffff → TX 0x36000–0x3ffff; 0x10000–0x15fff → TX 0x30000–0x35fff;
  0x16000–0x1ffff → TX 0x26000–0x2ffff; 0x20000–0x25fff → TX 0x20000–0x25fff.
  Each 64 KiB bank maps in two segments split at offset 0x6000; the 8822B aliases (0x16000/0x6000
  for the bank bases) do **not** apply.

## Streamer (`src/fw/stream8822c.S`, `src/stream_cu.*`, `wsdr_cu ring|latime|stream`)
- [V] LA ring, Jaguar3 dialect (`La8822c` in `src/la88xx.*`): 0x1CE4 [5:0] dma / [10:8] rate /
  [17:13] trig sel 3, 0x1CF4[23], 0x1EB4[23], 0x1C3C[19:8] = 0x880, 0x7CC[30] = 0, arm 0x7C0 = 0xCB.
  Live with the vendor CPU running or halted.
- [V] Rate (latime8822c): fs = 80 MHz >> rate (rate 1–5), ring starts at TXFF 0x20000 at the arm.
- [V] Five packets per bank: [128, 12352), [12352, 0x6000), [0x6018, 0x9568), [0x9568, 0xCAB8),
  [0xCAB8, 0x10000); trailer (RX SRAM 0x1000) = 128 B prefix + 24 B at 0x6000 + stamps. The CPU
  reads and writes TXFF at 0xB87A0000/0xB87B0000 and RX SRAM at 0xB8701000 as on the 8822B.
- [V] 60 s at 5 MS/s and at 2.5 MS/s, zero faults, 1483 µs per bank on USB 2; offline beacon grid
  clean. `--path ab` records both paths (B leads A by 401 ns). Use depth 256 (default).
- Run: `cmake -S . -B build-cu && cmake --build build-cu --target wsdr_cu`, then
  `sudo build-cu/wsdr_cu stream --pid 0xc812 --channel 36 --rate 4 --seconds 60 --path ab --out captures/x`.
- [V] Fixed gain: `--no-cca --igi N` (0x1D58[11:3] = 0x1FF, 0x1A14[9:8] = 3, 0x1D70); ~1.0 dB/step.
- [V] SoapySDR: `driver=wsdr,pid=0xc812`, channels 0/1 = paths A/B (`evidence/phase5-cu-soapy.txt`).
- Open: rtl_tcp front end, DFIR / wide tap on the 8822C.
