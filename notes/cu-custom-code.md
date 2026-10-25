# RTL8812CU (RTL8822C cut 5): custom on-chip code

Status 2026-10-25: **custom code runs on the 8822C's embedded CPU** (RAM only). C2H transport not yet
working. Tool: `build-cu/wsdr_cu` (`cmake -S . -B build-cu && cmake --build build-cu --target wsdr_cu`;
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

## C2H (`wsdr_cu c2h`, `evidence/phase5-cu-c2h.txt`) — partly working
- [V] Staging is fine: the RX-buffer window round trip is exact (`memtest` H1r) and the descriptor
  reads back. The C2H engine consumes the packet: REG 0x298 moves from 0x5f00 to 0x1000 + 0x418 (end of
  a 1048-byte frame). FWFF is not involved (0x601 FWEN already clear, 0x29c/0x2a0 unchanged).
- [V] RX filtered (8822B recipe: 0x10c[2] RX-agg cleared): the trigger clears but **no packet reaches
  bulk-IN** — not even with the CPU running or MAC RX on.
- [V] RX accepting all frames, CPU halted: **15 of 64 1 KiB C2H packets arrive** (199 frames as
  strays); 16 KiB: 0. With the CPU running, the vendor firmware issues its own C2H (0x294 = 0x5f00)
  and collides with ours — halt it for tests.
- [V] RX-agg re-enabled (0x10c[2] = 1): the request is no longer accepted (0x294 BIT16 stays set).
- [I] Reading: on Jaguar3 the RX DMA only flushes to bulk-IN under aggregation (devourer
  HalmacJaguar3MacInit::init_usb_cfg says the same for the 8822E), so with aggregation off a C2H packet
  waits for later RX traffic and leaves packed with frames; with aggregation on, the C2H request path
  wants something else configured. Next: read halmac 88xx (8822C) `cfg_usb_rx_agg` / C2H / RXDMA
  code and the rtw88 8822cu RX path; try aggregation in DMA mode, a short agg timeout, and parse
  aggregated transfers (descriptor walk) instead of requiring one packet per transfer.

## Toward an 8822C streamer
- Needs: working C2H; the 8822C LA ring location and its C2H aliases (the 8822B's TX-bank aliases
  0x16000/0x6000 are unverified here); the two-path word layout (path B at Q[43:32]/I[55:44]).
- USB 2 only on this board (480M): ~44 MB/s → 5 MS/s at 8 B/sample, both paths in the same word.
