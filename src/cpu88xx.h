// SPDX-License-Identifier: GPL-2.0-only
// RAM-only custom code for the 8822B's embedded MIPS16e2 CPU. Nothing here touches flash or efuse:
// a power cycle (or devourer's next bring-up, which re-downloads the vendor firmware) undoes it.
//
// Image = vendor IMEM prologue [0, 0x230) (CP0/stack init that falls through) + our program linked at
// 0x80000230. The prologue is taken from devourer's embedded vendor NIC firmware (v30.20, identical to
// linux-firmware rtw88/rtw8822b_fw.bin) and checked against a known hash before use.
// Load path (halmac download flow, minus the vendor checksum handshake): CPU off -> platform reset ->
// FWDL_EN -> stage in TX buffer -> DDMA to IMEM (OCP 0) -> DDMA back + compare -> start.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtlreg.h"

namespace wsdr {

class Cpu8822b {
public:
  static constexpr uint32_t kPrologue = 0x230;
  static constexpr uint32_t kImemOcp = 0x000000;
  static constexpr uint32_t kDmemOcp = 0x200000;
  // TX-buffer staging (below the LA ring at 0x187A0000; nothing else uses the TXFF while we own it).
  static constexpr uint32_t kStageImem = 0x18781000;
  static constexpr uint32_t kStageVerify = 0x18788000;
  static constexpr uint32_t kStageDmem = 0x1878F000;
  static constexpr size_t kMaxImage = 0x7000;

  explicit Cpu8822b(Reg &r) : _r(r) {}

  static std::vector<uint8_t> vendor_prologue(); // throws if devourer's copy does not match
  void halt();
  void load(const uint8_t *prog, size_t n);     // prologue + prog; verified read-back
  void start();
  // DMEM access via DDMA through the TX buffer. off/len multiples of 4.
  void dmem_write(uint32_t off, const void *buf, size_t n);
  void dmem_read(uint32_t off, void *buf, size_t n);
  size_t loaded_bytes() const { return _loaded; }

private:
  Reg &_r;
  size_t _loaded = 0;
};

} // namespace wsdr
