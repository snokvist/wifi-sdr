// SPDX-License-Identifier: GPL-2.0-only
#include "cpu88xx.h"

#include <cstring>
#include <stdexcept>

#include "hal8822b_fw.h" // devourer: array_mp_8822b_fw_nic[] (vendor NIC firmware, v30.20)

namespace wsdr {

namespace {
// FNV-1a 64 of IMEM[0, 0x230) of the 8822B NIC firmware. Same bytes in every 8822B build examined
// (v28.5 Windows, v30.20 Linux); sha256 7a2b616d4c1312dd48dccddb897ed148d5012aa36faa538581922cbcf1ba059d.
constexpr uint64_t kPrologueFnv = 0x51e7eb70efe13d14ull;

uint64_t fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
} // namespace

std::vector<uint8_t> Cpu8822b::vendor_prologue() {
  const uint8_t *fw = array_mp_8822b_fw_nic;
  if (array_mp_8822b_fw_nic_len < 64 || (fw[0] | fw[1] << 8) != 0x8822)
    throw std::runtime_error("devourer 8822B firmware array missing/invalid");
  const uint32_t dmem_size = le32(fw + 0x24);
  const uint32_t imem_off = 64 + dmem_size + 8;
  if (imem_off + kPrologue > array_mp_8822b_fw_nic_len)
    throw std::runtime_error("8822B firmware image too short");
  std::vector<uint8_t> v(fw + imem_off, fw + imem_off + kPrologue);
  if (fnv1a(v.data(), v.size()) != kPrologueFnv)
    throw std::runtime_error("8822B IMEM prologue differs from the validated build; refusing to splice");
  return v;
}

void Cpu8822b::halt() {
  _r.m8(0x0003, 0x04, 0x00); // SYS_FUNC_EN+1 [2]: CPU enable
  _r.m8(0x001D, 0x01, 0x00); // [0]: CPU clock/IO enable
}

void Cpu8822b::load(const uint8_t *prog, size_t n) {
  std::vector<uint8_t> img = vendor_prologue();
  img.insert(img.end(), prog, prog + n);
  img.resize((img.size() + 7) & ~size_t(7), 0);
  if (img.size() > kMaxImage)
    throw std::runtime_error("custom image too large");

  halt();
  // Platform reset (halmac pltfm_reset_88xx; the 0x09 toggle is 8822B/8821C-specific).
  _r.m8(0x1082, 0x01, 0x00);
  _r.m8(0x0009, 0x40, 0x00);
  _r.m8(0x1082, 0x01, 0x01);
  _r.m8(0x0009, 0x40, 0x40);
  _r.w32(0x1080, _r.r32(0x1080) | 0x10100u);                         // WL_PLATFORM_RST | DDMA_EN
  _r.w16(0x0080, static_cast<uint16_t>((_r.r16(0x0080) & 0x3800) | 1)); // FWDL_EN

  _r.sram_write(kStageImem, img.data(), img.size());
  _r.ddma(kStageImem, kImemOcp, static_cast<uint32_t>(img.size()));
  std::vector<uint8_t> back(img.size());
  _r.ddma(kImemOcp, kStageVerify, static_cast<uint32_t>(img.size()));
  _r.sram_read(kStageVerify, back.data(), back.size());
  if (back != img)
    throw std::runtime_error("IMEM read-back mismatch after DDMA load");
  _loaded = img.size();
}

void Cpu8822b::start() {
  // FW_DW_RDY | IMEM/DMEM DW_OK + CHKSUM_OK (host-forged, as halmac does), FWDL_EN cleared.
  _r.w16(0x0080, static_cast<uint16_t>((_r.r16(0x0080) & 0x3800) | 0x4078));
  _r.m8(0x001D, 0x01, 0x01);
  _r.m8(0x0003, 0x04, 0x04);
}

void Cpu8822b::dmem_write(uint32_t off, const void *buf, size_t n) {
  if ((off | n) & 3)
    throw std::invalid_argument("dmem_write alignment");
  _r.sram_write(kStageDmem, buf, n);
  _r.ddma(kStageDmem, kDmemOcp + off, static_cast<uint32_t>(n));
}

void Cpu8822b::dmem_read(uint32_t off, void *buf, size_t n) {
  if ((off | n) & 3)
    throw std::invalid_argument("dmem_read alignment");
  _r.ddma(kDmemOcp + off, kStageDmem, static_cast<uint32_t>(n));
  _r.sram_read(kStageDmem, buf, n);
}

} // namespace wsdr
