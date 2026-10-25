// SPDX-License-Identifier: GPL-2.0-only
// RAM-only custom code for the 8822C's embedded MIPS16 CPU (RTL8812CU). Nothing here touches flash or
// efuse: a power cycle (or devourer's next bring-up, which re-downloads the vendor firmware) undoes it.
//
// The 8822C layout differs from the 8822B (notes/fw-disasm.md §0, §6): IMEM is at OCP 0x30000
// (CPU 0x80030000), EMEM at 0x100000, DMEM at 0x200000, and the image carries no reset vectors, so
// a mask ROM is inferred to enter IMEM at 0x80030000. The vendor IMEM entry stub sets sp = 0x80210000
// and does the CP0 setup in its first 0x3c bytes, then branches on MCUFW_CTRL (warm/cold boot).
// Image = that 0x3c-byte stub (from devourer's embedded 8822C NIC firmware, hash-checked) + our
// program linked at 0x8003003c, replacing the warm/cold branch and everything after it.
// Load path (devourer HalmacJaguar3Fw, halmac download flow minus the checksum handshake): CPU off ->
// platform reset (0x1082 bit0) -> FWDL_EN -> stage in TX buffer -> DDMA to IMEM -> read back -> start.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtlreg.h"

namespace wsdr {

class Cpu8822c {
public:
  static constexpr uint32_t kStub = 0x3c;            // vendor IMEM entry stub kept in front of our code
  static constexpr uint32_t kImemOcp = 0x030000;
  static constexpr uint32_t kDmemOcp = 0x200000;
  static constexpr uint32_t kStageImem = 0x18781000; // TX buffer staging, as on the 8822B
  static constexpr uint32_t kStageVerify = 0x18788000;
  static constexpr uint32_t kStageDmem = 0x1878F000;
  static constexpr size_t kMaxImage = 0x4000;

  explicit Cpu8822c(Reg &r) : _r(r) {}

  static std::vector<uint8_t> vendor_stub(); // throws if devourer's copy does not match
  void halt();
  // stub + prog. verify: DDMA IMEM back and compare. MEASURED (memtest): IMEM does not read back over
  // DDMA on this chip even when TXFF/DMEM round trips are exact, so verify defaults to off.
  void load(const uint8_t *prog, size_t n, bool verify = false);
  void start();
  // DMEM access via DDMA through the TX buffer. off/len multiples of 4 (off from DMEM base 0x200000).
  void dmem_write(uint32_t off, const void *buf, size_t n);
  void dmem_read(uint32_t off, void *buf, size_t n);
  size_t loaded_bytes() const { return _loaded; }

private:
  Reg &_r;
  size_t _loaded = 0;
};

} // namespace wsdr
