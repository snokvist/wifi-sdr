// SPDX-License-Identifier: GPL-2.0-only
#include "la88xx.h"

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
  _r.m32(0x08F8, 0x3C00000u, 0);  // dbg header 0
  _r.m32(0x08B4, 0x80u, 0x80u);   // report update
  _r.w32(0x08FC, c.dbg_port);
  _r.m32(0x095C, 0x80000FFFu,
         (static_cast<uint32_t>(c.dma_type & 0xF) << 8) | (static_cast<uint32_t>(c.rate_sel & 7) << 5) |
             3u); // trig sel 3 (REG), edge 0
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

} // namespace wsdr
