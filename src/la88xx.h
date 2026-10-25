// SPDX-License-Identifier: GPL-2.0-only
// LA-mode (phydm ADC sampling) configured as a FREE-RUNNING ring: armed with the manual (REG) trigger
// selected and never triggered, the baseband keeps writing samples into the top 128 KiB of the TX
// packet buffer (TXFF 0x20000..0x40000 = OCP 0x187A0000..0x187C0000), 8 bytes per sample, wrapping.
// Register facts: devourer src/LaCapture.cpp (phydm_adc_sampling.c port) and ac54_sdr (facts only).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "rtlreg.h"

namespace wsdr {

struct LaRingCfg {
  int rate_sel = 2;          // 8822B 0x95c[7:5]: fs = 80 MHz >> rate_sel (1 = 40, 2 = 20, 3 = 10 Msps)
  int dma_type = 0;          // 0x95c[11:8]: 0 = path A 12+12-bit IQ, 1 = path B
  uint32_t dbg_port = 0x880; // 0x8fc selector (devourer: 0x880 = ADC path A; ac54 used 0)
  bool cck_off = true;       // 0x808[28] = 0: CCK block freezes the bus while it holds a frame
  bool filter_rx = true;     // keep MAC RX enabled (the LA writer needs it) but accept no frames
  bool dbgport_clk = true;   // 0x198c[2:0] = 7 (11AC-2 dbg-port clock, devourer does this)
  int igi = -1;              // >= 0: pin the initial gain index, 0xc50[6:0] / 0xe50[6:0] (paths A/B)
  bool ofdm_off = false;     // 0x808[29] = 0: OFDM block off. MEASURED: freezes the LA writer — do not use
  bool no_cca = false;       // 0x838[1] = 1: OFDM RX CCA off (phydm_stop_ic_trx) -> no AGC events
};

class La8822b {
public:
  static constexpr uint32_t kRingOcp = 0x187A0000; // TXFF byte 0x20000
  static constexpr uint32_t kRingBytes = 0x20000;  // 128 KiB = 16384 samples

  explicit La8822b(Reg &r) : _r(r) {}
  void snapshot();           // save every register configure()/arm() touches
  void configure(const LaRingCfg &c);
  void arm() { _r.w8(0x07C0, 0xCB); } // enable | poll | MAC-trigger mode | REG trigger, never fired
  void stop() { _r.w8(0x07C0, 0x00); }
  void restore();            // stop, then write the snapshot back (idempotent)
  bool saved() const { return !_saved.empty(); }

private:
  Reg &_r;
  std::vector<std::pair<std::pair<uint16_t, int>, uint32_t>> _saved; // ((addr, width), value)
};

} // namespace wsdr
