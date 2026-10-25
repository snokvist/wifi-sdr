// SPDX-License-Identifier: GPL-2.0-only
#include "rtlreg.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace wsdr {

namespace {
constexpr uint8_t kReq = 0x05;
constexpr unsigned kTimeoutMs = 1000;
constexpr uint16_t kRegPktbufDbgSel = 0x0106; // 0x69 = packet-buffer debug access
constexpr uint16_t kRegPktbufPage = 0x0140;   // [11:0] = OCP[23:12]
constexpr uint16_t kWindow = 0x8000;          // 4 KiB window
constexpr uint16_t kRegDdmaCh0 = 0x1200;      // SA, DA, CTRL (OWN = bit 31)
} // namespace

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}

Reg::Reg(libusb_device_handle *h, bool superspeed) : _h(h), _cap(superspeed ? 504 : 4096) {}

uint32_t Reg::rd(uint16_t a, int len) {
  uint8_t b[4] = {0, 0, 0, 0};
  int r = libusb_control_transfer(_h, 0xC0, kReq, a, 0, b, static_cast<uint16_t>(len), kTimeoutMs);
  _xfers++;
  if (r != len)
    throw UsbError("read " + hex32(a) + " failed: " + libusb_error_name(r));
  uint32_t v = 0;
  std::memcpy(&v, b, static_cast<size_t>(len));
  return v;
}

void Reg::wr(uint16_t a, uint32_t v, int len) {
  uint8_t b[4];
  std::memcpy(b, &v, 4);
  int r = libusb_control_transfer(_h, 0x40, kReq, a, 0, b, static_cast<uint16_t>(len), kTimeoutMs);
  _xfers++;
  if (r != len)
    throw UsbError("write " + hex32(a) + " failed: " + libusb_error_name(r));
}

void Reg::block_read(uint16_t a, void *buf, size_t n) {
  if (n > _cap)
    throw std::invalid_argument("block_read beyond EP0 cap");
  int r = libusb_control_transfer(_h, 0xC0, kReq, a, 0, static_cast<unsigned char *>(buf),
                                  static_cast<uint16_t>(n), kTimeoutMs);
  _xfers++;
  if (r != static_cast<int>(n))
    throw UsbError("block read " + hex32(a) + " failed: " + libusb_error_name(r));
}

void Reg::block_write(uint16_t a, const void *buf, size_t n) {
  if (n > _cap)
    throw std::invalid_argument("block_write beyond EP0 cap");
  int r = libusb_control_transfer(_h, 0x40, kReq, a, 0,
                                  const_cast<unsigned char *>(static_cast<const unsigned char *>(buf)),
                                  static_cast<uint16_t>(n), kTimeoutMs);
  _xfers++;
  if (r != static_cast<int>(n))
    throw UsbError("block write " + hex32(a) + " failed: " + libusb_error_name(r));
}

void Reg::sram_read(uint32_t ocp, void *buf, size_t n) {
  auto *p = static_cast<uint8_t *>(buf);
  uint32_t page = ~0u;
  w8(kRegPktbufDbgSel, 0x69);
  while (n) {
    const uint32_t off = ocp & 0xfff;
    size_t chunk = std::min<size_t>({n, 0x1000 - off, _cap});
    const uint32_t pg = (ocp & 0xffffff) >> 12;
    if (pg != page) {
      w16(kRegPktbufPage, static_cast<uint16_t>(pg));
      page = pg;
    }
    block_read(static_cast<uint16_t>(kWindow + off), p, chunk);
    ocp += static_cast<uint32_t>(chunk);
    p += chunk;
    n -= chunk;
  }
}

void Reg::sram_write(uint32_t ocp, const void *buf, size_t n) {
  auto *p = static_cast<const uint8_t *>(buf);
  uint32_t page = ~0u;
  w8(kRegPktbufDbgSel, 0x69);
  while (n) {
    const uint32_t off = ocp & 0xfff;
    size_t chunk = std::min<size_t>({n, 0x1000 - off, _cap});
    const uint32_t pg = (ocp & 0xffffff) >> 12;
    if (pg != page) {
      w16(kRegPktbufPage, static_cast<uint16_t>(pg));
      page = pg;
    }
    block_write(static_cast<uint16_t>(kWindow + off), p, chunk);
    ocp += static_cast<uint32_t>(chunk);
    p += chunk;
    n -= chunk;
  }
}

void Reg::ddma(uint32_t src, uint32_t dst, uint32_t len, unsigned timeout_ms) {
  if ((len & 3) || len >= (1u << 24))
    throw std::invalid_argument("ddma length");
  // The three channel registers are consecutive; CTRL (with OWN) must land last.
  const uint32_t cmd[3] = {src, dst, 0x80000000u | len};
  if (_cap >= sizeof cmd) {
    block_write(kRegDdmaCh0, cmd, sizeof cmd);
  } else {
    for (int i = 0; i < 3; i++)
      w32(static_cast<uint16_t>(kRegDdmaCh0 + 4 * i), cmd[i]);
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    const uint32_t c = r32(kRegDdmaCh0 + 8);
    if (!(c & 0x80000000u))
      return;
    if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms))
      throw UsbError("DDMA ch0 stuck busy, ctrl=" + hex32(c) + " src=" + hex32(src) +
                     " dst=" + hex32(dst));
  }
}

bool Reg::alive() {
  try {
    const uint32_t v = r32(0x00F0);
    return v != 0 && v != 0xffffffffu;
  } catch (const UsbError &) {
    return false;
  }
}

} // namespace wsdr
