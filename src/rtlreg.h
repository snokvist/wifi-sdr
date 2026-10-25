// SPDX-License-Identifier: GPL-2.0-only
// Thin register / on-chip-SRAM / DDMA access for Realtek 88xx USB parts (8822B, 8822C).
//
// All register traffic is the Realtek vendor control request 0x05 (wValue = address, wIndex = 0),
// the same path devourer's UsbTransport and the vendor drivers use. On SuperSpeed the chip STALLs
// vendor reads of >= 512 bytes, so block transfers are capped at 504 bytes there (4096 on USB 2).
//
// On-chip SRAM in the 0x18xxxxxx OCP region (TX packet buffer 0x18780000, RX buffer 0x18700000)
// is reached through the packet-buffer debug window: 0x0106 = 0x69, 0x0140 = (ocp >> 12) & 0xfff,
// then 4 KiB at register 0x8000. IMEM/DMEM are reached only via DDMA channel 0 (0x1200 src,
// 0x1204 dst, 0x1208 = OWN[31] | len), staged through the TX buffer.
#pragma once

#include <libusb.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace wsdr {

struct UsbError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Reg {
public:
  Reg(libusb_device_handle *h, bool superspeed);

  uint8_t r8(uint16_t a) { return static_cast<uint8_t>(rd(a, 1)); }
  uint16_t r16(uint16_t a) { return static_cast<uint16_t>(rd(a, 2)); }
  uint32_t r32(uint16_t a) { return rd(a, 4); }
  void w8(uint16_t a, uint8_t v) { wr(a, v, 1); }
  void w16(uint16_t a, uint16_t v) { wr(a, v, 2); }
  void w32(uint16_t a, uint32_t v) { wr(a, v, 4); }
  // Read-modify-write: bits in `mask` take the corresponding bits of `val` (already shifted).
  void m32(uint16_t a, uint32_t mask, uint32_t val) { w32(a, (r32(a) & ~mask) | (val & mask)); }
  void m8(uint16_t a, uint8_t mask, uint8_t val) {
    w8(a, static_cast<uint8_t>((r8(a) & ~mask) | (val & mask)));
  }

  // One vendor control transfer of up to cap() bytes.
  void block_read(uint16_t a, void *buf, size_t n);
  void block_write(uint16_t a, const void *buf, size_t n);
  size_t cap() const { return _cap; }
  // Lower the block-transfer size (the 8822C hangs on large packet-buffer window reads; 4 is safe).
  void set_cap(size_t n) { _cap = n ? n : 4; }

  // OCP SRAM (0x18xxxxxx) through the debug window; any length, page-split internally.
  void sram_read(uint32_t ocp, void *buf, size_t n);
  void sram_write(uint32_t ocp, const void *buf, size_t n);

  // DDMA channel 0 copy between OCP addresses (IMEM 0x0, DMEM 0x200000, buffers 0x18xxxxxx).
  // Throws on timeout. len must be a multiple of 4 and < 2^24.
  void ddma(uint32_t src, uint32_t dst, uint32_t len, unsigned timeout_ms = 20);

  // Chip still answering? (SYS_CFG1 read is neither 0 nor all-ones.)
  bool alive();

  unsigned long long transfers() const { return _xfers; }

private:
  uint32_t rd(uint16_t a, int len);
  void wr(uint16_t a, uint32_t v, int len);

  libusb_device_handle *_h;
  size_t _cap;
  unsigned long long _xfers = 0;
};

std::string hex32(uint32_t v);

} // namespace wsdr
