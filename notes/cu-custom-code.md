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

## C2H (`wsdr_cu c2h`, `evidence/phase5-cu-c2h.txt`) — not working yet
- [V] Host-issued request 0x11000 (RX SRAM 0x1000): the trigger bit clears (r294 = 0x00001000) but no
  packet reaches bulk-IN EP 0x84 (64 timeouts, no strays) at 1 KiB and 16 KiB. EP 0x84 is the 8812CU's
  bulk-IN too (lsusb).
- [I] Candidates: the RX-path setup is the 8822B's (`C2hRxPath`: 0x100/0x608/0x290/0x10c), the 8822C
  RX buffer base or RXDMA/HCI enables differ, or C2H on this die needs the FWFF/RX queue configured as
  halmac 88xx v1 does for the 8822C. Next: diff the 8822C RX-path registers against devourer's
  Jaguar3 init and halmac `cfg_usb_rx_agg`/C2H code, then retry with RX on and aggregation off.

## Toward an 8822C streamer
- Needs: working C2H; the 8822C LA ring location and its C2H aliases (the 8822B's TX-bank aliases
  0x16000/0x6000 are unverified here); the two-path word layout (path B at Q[43:32]/I[55:44]).
- USB 2 only on this board (480M): ~44 MB/s → 5 MS/s at 8 B/sample, both paths in the same word.
