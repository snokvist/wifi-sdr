// SPDX-License-Identifier: GPL-2.0-only
#include "cpu8822c.h"

#include <cstring>
#include <stdexcept>

#include "hal8822c_fw.h" // devourer: array_mp_8822c_fw_nic[] (vendor NIC firmware, v9.17)

namespace wsdr {

namespace {
// FNV-1a 64 of IMEM[0, 0x3c) of devourer's 8822C NIC firmware: sp = 0x80210000 + CP0 setup, ending
// right before the MCUFW_CTRL warm/cold-boot branch (notes/fw-disasm.md §6).
constexpr uint64_t kStubFnv = 0xc113f9dbb3211775ull;

uint64_t fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
} // namespace

std::vector<uint8_t> Cpu8822c::vendor_stub() {
  const uint8_t *fw = array_mp_8822c_fw_nic;
  if (array_mp_8822c_fw_nic_len < 64 || (fw[0] | fw[1] << 8) != 0x8822)
    throw std::runtime_error("devourer 8822C firmware array missing/invalid");
  // Header: DMEM size @36, IMEM addr @60 (0x80030000); sections follow the 64-byte header, each
  // with an 8-byte checksum trailer.
  if (le32(fw + 60) != 0x80030000u)
    throw std::runtime_error("8822C firmware IMEM address is not 0x80030000");
  const uint32_t imem_off = 64 + le32(fw + 36) + 8;
  if (imem_off + kStub > array_mp_8822c_fw_nic_len)
    throw std::runtime_error("8822C firmware image too short");
  std::vector<uint8_t> v(fw + imem_off, fw + imem_off + kStub);
  if (fnv1a(v.data(), v.size()) != kStubFnv)
    throw std::runtime_error("8822C IMEM entry stub differs from the validated build; refusing to splice");
  return v;
}

void Cpu8822c::halt() {
  // wlan_cpu_en(false): SYS_FUNC_EN+1 [2] then RSV_CTRL+1 [0] (HalmacJaguar3Fw).
  _r.m8(0x0003, 0x04, 0x00);
  _r.m8(0x001D, 0x01, 0x00);
}

void Cpu8822c::load(const uint8_t *prog, size_t n, bool verify) {
  std::vector<uint8_t> img = vendor_stub();
  img.insert(img.end(), prog, prog + n);
  img.resize((img.size() + 7) & ~size_t(7), 0);
  if (img.size() > kMaxImage)
    throw std::runtime_error("custom image too large");

  halt();
  // pltfm_reset (8822C/8822E path): REG_CPU_DMEM_CON+2 bit0 low -> high. DDMA_EN (0x1080 bit8).
  _r.m8(0x1082, 0x01, 0x00);
  _r.m8(0x1082, 0x01, 0x01);
  _r.w32(0x1080, _r.r32(0x1080) | 0x100u);
  _r.w16(0x0080, static_cast<uint16_t>((_r.r16(0x0080) & 0x3800) | 1)); // FWDL_EN

  _r.sram_write(kStageImem, img.data(), img.size());
  _r.ddma(kStageImem, kImemOcp, static_cast<uint32_t>(img.size()));
  if (verify) {
    std::vector<uint8_t> back(img.size());
    _r.ddma(kImemOcp, kStageVerify, static_cast<uint32_t>(img.size()));
    _r.sram_read(kStageVerify, back.data(), back.size());
    if (back != img)
      throw std::runtime_error("IMEM read-back mismatch after DDMA load");
  }
  _loaded = img.size();
}

void Cpu8822c::start() {
  // FW_DW_RDY | IMEM/DMEM DW_OK + CHKSUM_OK (host-forged, as halmac sets them), FWDL_EN cleared.
  _r.w16(0x0080, static_cast<uint16_t>((_r.r16(0x0080) & 0x3800) | 0x4078));
  _r.m8(0x001D, 0x01, 0x01);
  _r.m8(0x0003, 0x04, 0x04);
}

void Cpu8822c::dmem_write(uint32_t off, const void *buf, size_t n) {
  if ((off | n) & 3)
    throw std::invalid_argument("dmem_write alignment");
  _r.sram_write(kStageDmem, buf, n);
  _r.ddma(kStageDmem, kDmemOcp + off, static_cast<uint32_t>(n));
}

void Cpu8822c::dmem_read(uint32_t off, void *buf, size_t n) {
  if ((off | n) & 3)
    throw std::invalid_argument("dmem_read alignment");
  _r.ddma(kDmemOcp + off, kStageDmem, static_cast<uint32_t>(n));
  _r.sram_read(kStageDmem, buf, n);
}

} // namespace wsdr
