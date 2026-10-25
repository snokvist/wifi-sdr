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
  int dfir = 0;              // 20/40/80: RX DFIR as vendor phydm_rxdfirpar_by_bw_8822b (0x948/0x94c/0xc20/0xe20);
                             // 0 = leave. devourer always writes the BW20 set, which limits dma 0/1 to ~+-10 MHz
                             // (and doubles every sample at rate_sel 0) at every channel width. MEASURED.
};

struct SnapInfo {
  bool done = false;   // engine stopped within the poll budget
  bool wrap = false;   // 0x7c0[31]: the ring wrapped, so all of it is valid
  uint32_t r7c0 = 0;   // 0x7c0 at stop: [30:16] finish address (8-byte units, TXFF)
  size_t samples = 0;  // valid samples unrolled into the output
  double read_ms = 0;  // EP0 readback time of the ring
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
  // One contiguous snapshot: arm, let the ring fill for settle_us, fire the manual trigger (0x7c0[5]
  // edge, post-trigger time 0), wait for the engine to stop, read the ring and unroll it oldest-first
  // from the finish address into `out` (devourer LaCapture::readback semantics). `raw`, if given,
  // receives the ring as read (for freshness checks). Leaves the engine stopped.
  SnapInfo snap(std::vector<uint64_t> &out, unsigned settle_us, std::vector<uint64_t> *raw = nullptr);

private:
  Reg &_r;
  std::vector<std::pair<std::pair<uint16_t, int>, uint32_t>> _saved; // ((addr, width), value)
};

// The 8822C (Jaguar3) dialect of the same free-running ring (devourer LaCapture setup_bb /
// setup_dbg_port, la_regs_jgr3): engine 0x1CE4 ([5:0] dma, [7:6] MAC-PHY timing, [10:8] rate,
// [17:13] trig sel, [26] edge, [27]/[31:28] adv trigger), 0x1CF4[23] LA on, 0x1EB4[23] report
// update, 0x1C3C[19:8] dbg port, 0x1CE8/0x1CF0 adv-trigger AND gates. MAC side (0x7C0) as on the
// 8822B; 0x7CC[30] = 0 keeps the half-size ring at TXFF 0x20000..0x40000. Rate index as devourer's
// (0 = 80M, 1 = 40M, 2 = 20M ...); measured with latime8822c. igi pins 0x1D70[6:0]/[14:8]; no_cca
// sets 0x1D58[11:3] = 0x1FF and cck_off 0x1A14[9:8] = 3 (Jaguar3 phydm_stop_ic_trx /
// phydm_dis_cck_trx). Ignores the 11AC-only dbgport_clk / dfir / ofdm_off.
class La8822c {
public:
  static constexpr uint32_t kRingOcp = 0x187A0000;
  static constexpr uint32_t kRingBytes = 0x20000;

  explicit La8822c(Reg &r) : _r(r) {}
  void snapshot();
  void configure(const LaRingCfg &c);
  void arm() { _r.w8(0x07C0, 0xCB); }
  void stop() { _r.w8(0x07C0, 0x00); }
  void restore();
  bool saved() const { return !_saved.empty(); }

private:
  Reg &_r;
  std::vector<std::pair<uint16_t, uint32_t>> _saved; // 32-bit (addr, value)
};

} // namespace wsdr
