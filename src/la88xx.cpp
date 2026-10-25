// SPDX-License-Identifier: GPL-2.0-only
#include "la88xx.h"

#include <chrono>
#include <thread>

namespace wsdr {

namespace {
// (address, width) of everything configure()/arm() may write. 0x07C0 is restored as 0 (stopped).
const std::pair<uint16_t, int> kTouched[] = {
    {0x0100, 2}, // REG_CR: MAC/DMA enables
    {0x0608, 4}, // RCR: accept bits
    {0x0290, 1}, // RXDMA mode
    {0x010C, 1}, // RXDMA control
    {0x0106, 1}, // packet-buffer debug select (Reg::sram_* leave it at 0x69)
    {0x0140, 2}, // packet-buffer debug page
    {0x06A0, 2}, {0x06A2, 2}, {0x06A4, 2}, // RX filter maps (mgmt / ctrl / data)
    {0x0808, 4}, // OFDM/CCK block enables
    {0x08B4, 4}, // [7] report update
    {0x08F8, 4}, // [25:22] dbg-port header
    {0x08FC, 4}, // dbg-port selector
    {0x095C, 4}, // LA engine: [4:0] trig sel, [7:5] rate, [11:8] dma type, [31] edge
    {0x198C, 4}, // [2:0] dbg-port clock
    {0x0C50, 4}, {0x0E50, 4}, // IGI path A / B
    {0x0838, 4}, // [1] OFDM RX CCA disable
    {0x0948, 4}, {0x094C, 4}, {0x0C20, 4}, {0x0E20, 4}, // RX DFIR (dfir option)
};
} // namespace

void La8822b::snapshot() {
  _saved.clear();
  for (auto [a, w] : kTouched) {
    uint32_t v = w == 1 ? _r.r8(a) : w == 2 ? _r.r16(a) : _r.r32(a);
    _saved.push_back({{a, w}, v});
  }
}

void La8822b::configure(const LaRingCfg &c) {
  if (!saved())
    snapshot();
  stop();
  if (c.filter_rx) {
    // MAC RX stays ON: with it off the LA writer freezes (ac54: frozen-ring false positives).
    _r.w16(0x0100, static_cast<uint16_t>(_r.r16(0x0100) | 0x8A)); // MACRXEN | RXDMA_EN | HCI_RXDMA_EN
    _r.w32(0x0608, (_r.r32(0x0608) | 0x80000u) & ~0xFFFFu);      // clear all accept bits
    _r.w16(0x06A0, 0);
    _r.w16(0x06A2, 0);
    _r.w16(0x06A4, 0);
    _r.w8(0x0290, 14);
    _r.w8(0x010C, static_cast<uint8_t>(_r.r8(0x010C) & ~4u));
  }
  if (c.cck_off)
    _r.m32(0x0808, 1u << 28, 0);
  if (c.ofdm_off)
    _r.m32(0x0808, 1u << 29, 0);
  if (c.no_cca)
    _r.m32(0x0838, 1u << 1, 1u << 1);
  if (c.dbgport_clk)
    _r.m32(0x198C, 0x7, 0x7);
  if (c.igi >= 0) {
    _r.m32(0x0C50, 0x7F, static_cast<uint32_t>(c.igi));
    _r.m32(0x0E50, 0x7F, static_cast<uint32_t>(c.igi));
  }
  if (c.dfir == 20 || c.dfir == 40 || c.dfir == 80) {
    // phydm_rxdfirpar_by_bw_8822b: 0x948[29:28] / 0x94c[29:28] / 0xc20[31] = 0xe20[31]
    const uint32_t a = c.dfir == 40 ? 1 : 2, b = c.dfir == 20 ? 2 : c.dfir == 40 ? 0 : 1, n20 = c.dfir == 20 ? 1 : 0;
    _r.m32(0x0948, 0x30000000u, a << 28);
    _r.m32(0x094C, 0x30000000u, b << 28);
    _r.m32(0x0C20, 0x80000000u, n20 << 31);
    _r.m32(0x0E20, 0x80000000u, n20 << 31);
  }
  _r.m32(0x08F8, 0x3C00000u, 0);  // dbg header 0
  _r.m32(0x08B4, 0x80u, 0x80u);   // report update
  _r.w32(0x08FC, c.dbg_port);
  _r.m32(0x095C, 0x80000FFFu,
         (static_cast<uint32_t>(c.dma_type & 0xF) << 8) | (static_cast<uint32_t>(c.rate_sel & 7) << 5) |
             3u); // trig sel 3 (REG), edge 0
}

SnapInfo La8822b::snap(std::vector<uint64_t> &out, unsigned settle_us, std::vector<uint64_t> *raw) {
  const size_t n = kRingBytes / 8;
  std::vector<uint64_t> local;
  std::vector<uint64_t> &ring = raw ? *raw : local;
  ring.resize(n);
  out.resize(n);
  SnapInfo s;
  arm();
  std::this_thread::sleep_for(std::chrono::microseconds(settle_us));
  _r.w8(0x07C0, 0xCB | 0x20); // manual trigger edge after arm (devourer: ordering matters on 8822B)
  for (int i = 0; i < 200 && !s.done; i++)
    s.done = !(_r.r8(0x07C0) & 0x02);
  s.r7c0 = _r.r32(0x07C0);
  const auto t0 = std::chrono::steady_clock::now();
  _r.sram_read(kRingOcp, ring.data(), kRingBytes);
  s.read_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  stop();
  const uint32_t fin = (s.r7c0 & 0x7FFF0000u) >> 16;
  s.wrap = (s.r7c0 >> 31) & 1;
  size_t start = 0;
  if (s.wrap) {
    start = (((fin + 2) << 3) - 0x20000u) / 8 % n;
    s.samples = n;
  } else {
    s.samples = fin > (0x20000u >> 3) ? fin - (0x20000u >> 3) : 0;
  }
  for (size_t i = 0; i < s.samples; i++)
    out[i] = ring[(start + i) % n];
  out.resize(s.samples);
  return s;
}

void La8822b::restore() {
  try {
    stop();
  } catch (...) {
  }
  for (auto &[aw, v] : _saved) {
    auto [a, w] = aw;
    try {
      if (w == 1)
        _r.w8(a, static_cast<uint8_t>(v));
      else if (w == 2)
        _r.w16(a, static_cast<uint16_t>(v));
      else
        _r.w32(a, v);
    } catch (...) {
    }
  }
}

namespace {
const uint16_t kTouchedJ3[] = {
    0x0100, 0x0608, 0x0290, 0x010C, 0x06A0, 0x06A4, // MAC RX path (0x6A0 dword covers 0x6A2)
    0x07CC,                                         // [30] full-buffer mode
    0x1CE4, 0x1CF4, 0x1EB4, 0x1C3C, 0x1CE8, 0x1CF0, // LA engine / dbg port
    0x1D70,                                         // IGI paths A/B
    0x1D58,                                         // [11:3] OFDM RX CCA disable
    0x1A14,                                         // [9:8] CCK RxIQ weighting
};
} // namespace

void La8822c::snapshot() {
  _saved.clear();
  for (uint16_t a : kTouchedJ3)
    _saved.push_back({a, _r.r32(a)});
}

void La8822c::configure(const LaRingCfg &c) {
  if (!saved())
    snapshot();
  stop();
  if (c.filter_rx) {
    _r.w16(0x0100, static_cast<uint16_t>(_r.r16(0x0100) | 0x8A));
    _r.w32(0x0608, (_r.r32(0x0608) | 0x80000u) & ~0xFFFFu);
    _r.w16(0x06A0, 0);
    _r.w16(0x06A2, 0);
    _r.w16(0x06A4, 0);
    _r.w8(0x0290, 14);
    _r.w8(0x010C, static_cast<uint8_t>(_r.r8(0x010C) & ~4u)); // aggregation off: C2H needs it
  }
  if (c.igi >= 0)
    _r.m32(0x1D70, 0x7F7Fu, static_cast<uint32_t>(c.igi) * 0x101u);
  // Fixed gain, Jaguar3 branch of phydm_stop_ic_trx / phydm_dis_cck_trx (rtl88x2cu phydm_api.c):
  // OFDM RX CCA off (no packet detection -> no AGC steps), CCK RxIQ weighting [0,0].
  if (c.no_cca)
    _r.m32(0x1D58, 0xFF8u, 0x1FFu << 3);
  if (c.cck_off)
    _r.m32(0x1A14, 0x300u, 0x300u);
  _r.m32(0x07CC, 1u << 30, 0);
  _r.m32(0x1EB4, 1u << 23, 1u << 23);
  _r.m32(0x1CF4, 1u << 23, 1u << 23);
  _r.m32(0x1CE8, (1u << 5) | 0x3C0u | (1u << 15) | 0xF0000u | (1u << 25) | (1u << 26), 0);
  _r.w32(0x1CF0, 0);
  _r.m32(0x1C3C, 0xFFF00u, (c.dbg_port & 0xFFFu) << 8);
  _r.m32(0x1CE4, 0xFC03E7FFu,
         static_cast<uint32_t>(c.dma_type & 0x3F) | (static_cast<uint32_t>(c.rate_sel & 7) << 8) |
             (3u << 13)); // trig sel 3, MAC-PHY timing 0, edge 0, adv trigger off
}

void La8822c::restore() {
  try {
    stop();
  } catch (...) {
  }
  for (auto it = _saved.rbegin(); it != _saved.rend(); ++it) {
    try {
      _r.w32(it->first, it->second);
    } catch (...) {
    }
  }
}

} // namespace wsdr
