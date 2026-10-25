// SPDX-License-Identifier: GPL-2.0-only
#include "stream_cu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "bulkin88xx.h"
#include "c2h88xx.h"
#include "cpu8822c.h"
#include "la88xx.h"
#include "stream8822c.h"

namespace wsdr {

namespace {
using clk = std::chrono::steady_clock;

constexpr uint32_t kStateOff = 0x8000;  // DMEM offset of the program's state block (CPU 0x80208000)
constexpr uint32_t kRecOff = 0x8100;    // bank records (0x80 apart)
constexpr uint32_t kTrailerOcp = 0x18701000u; // RX SRAM; C2H address 0x1000
constexpr uint32_t kBankCpu[2] = {0xB87A0000u, 0xB87B0000u};
// C2H address of bank offset 0 in segment A and of offset 0x6000 in segment B (c2hmap).
constexpr uint32_t kSegAC2h[2] = {0x20000u, 0x10000u};
constexpr uint32_t kSegBC2h[2] = {0x16000u, 0x06000u};
constexpr uint8_t kSentinel[8] = {0xE1, 0xF1, 0x47, 0x5E, 0xA5, 0xA5, 0xA5, 0xA5}; // stream8822c.S

using S = Stream8822c;

uint32_t c2h_addr(int bank, size_t off) {
  return off < S::kSegB ? kSegAC2h[bank] + static_cast<uint32_t>(off)
                        : kSegBC2h[bank] + static_cast<uint32_t>(off - S::kSegB);
}

void desc(uint8_t *d, size_t payload) {
  std::memset(d, 0, 24);
  d[0] = static_cast<uint8_t>(payload & 0xFF);
  d[1] = static_cast<uint8_t>((payload >> 8) & 0x3F);
  d[3] = 0x80;  // OWN
  d[11] = 0x10; // C2H
}

std::string hex12(const uint8_t *p) {
  char b[40];
  for (int i = 0; i < 12; i++)
    std::snprintf(b + 3 * i, 4, "%02x%s", p[i], i < 11 ? "." : "");
  return b;
}

uint8_t fill(int bank, size_t i) { // synthetic pattern
  uint32_t x = static_cast<uint32_t>(i) * 0x9E3779B1u ^ (bank ? 0x5bd1e995u : 0x1b873593u);
  x ^= x >> 15;
  x *= 0x2c1b3c6du;
  return static_cast<uint8_t>(x >> 24);
}

bool cpu_window(size_t i) { // bytes the CPU overwrites: the five descriptors and the sentinel
  for (size_t off : S::kChunkOff)
    if (i >= off - 24 && i < off)
      return true;
  return i >= S::kBankBytes - 8;
}
} // namespace

StreamStats Stream8822c::run(const std::function<bool(const uint8_t *, const BankInfo &)> &on_bank,
                             const std::atomic<bool> *stop) {
  StreamStats st;
  Reg &r = _s.reg();
  C2hRxPath rx(r);
  La8822c la(r);
  Cpu8822c cpu(r);
  InOrderReader rd(_s.handle(), _s.context(), _c.depth);
  const uint32_t period5 = 512u << _c.la.rate_sel;
  const int32_t guard5 = _c.guard_us * 5;

  uint8_t data_desc[kChunks][24], trl_desc[24];
  for (int k = 0; k < kChunks; k++)
    desc(data_desc[k], kChunkLen[k]);
  desc(trl_desc, kTrailerPayload);
  std::vector<uint8_t> bank(kBankBytes);
  bool seen[2] = {false, false};
  const auto t0 = clk::now();
  try {
    cpu.load(stream8822c_bin, stream8822c_bin_len);
    rx.apply(true); // MAC RX on, no frames accepted
    la.configure(_c.la);
    if (_c.synthetic) {
      std::vector<uint8_t> pat(kBankBytes);
      for (int b = 0; b < 2; b++) {
        for (size_t i = 0; i < kBankBytes; i++)
          pat[i] = fill(b, i);
        r.sram_write(La8822c::kRingOcp + static_cast<uint32_t>(b) * kBankBytes, pat.data(), pat.size());
      }
    }
    uint8_t trl[24 + kTrailerPayload] = {};
    std::memcpy(trl, trl_desc, 24);
    r.sram_write(kTrailerOcp, trl, sizeof trl);

    const uint32_t state[8] = {0, 0, 0, 0, 0, _c.synthetic ? 0u : 1u, period5, static_cast<uint32_t>(guard5)};
    cpu.dmem_write(kStateOff, state, sizeof state);
    uint32_t rec[2][32] = {};
    for (int b = 0; b < 2; b++) {
      rec[b][0] = kBankCpu[b];
      rec[b][1] = kBankCpu[b] + static_cast<uint32_t>(kSegB);
      for (int k = 0; k < kChunks; k++) {
        const size_t d = kChunkOff[k] - 24;
        rec[b][2 + 3 * k] = kBankCpu[b] + static_cast<uint32_t>(d);
        rec[b][3 + 3 * k] = c2h_request(c2h_addr(b, d));
        std::memcpy(&rec[b][4 + 3 * k], data_desc[k], 4);
      }
    }
    cpu.dmem_write(kRecOff, rec, sizeof rec);

    rd.start();
    rd.drain(30);
    cpu.start();

    const double budget5 = static_cast<double>(period5) - guard5;
    for (uint32_t seq = 0;; seq++) {
      if (stop && *stop)
        break;
      const int b = static_cast<int>(seq & 1);
      int part = 0;
      BankInfo bi;
      bi.seq = seq;
      bi.bank = b;
      while (part <= kChunks) {
        InOrderReader::Done d;
        if (!rd.next(d, _c.pkt_timeout_ms))
          throw std::runtime_error("no packet within timeout (bank " + std::to_string(seq) + " part " +
                                   std::to_string(part) + ")");
        if (d.status != LIBUSB_TRANSFER_COMPLETED)
          throw std::runtime_error("bulk-IN status " + std::to_string(d.status));
        const bool c2h = d.len >= 24 && (d.data[11] & 0x10);
        if (!c2h) {
          if (st.strays < 8) {
            std::fprintf(stderr, "stream: stray bank %u part %d len %d:", seq, part, d.len);
            for (int i = 0; i < std::min(d.len, 24); i++)
              std::fprintf(stderr, " %02x", d.data[i]);
            std::fprintf(stderr, "\n");
          }
          st.strays++;
          rd.release();
          continue;
        }
        const bool trailer = part == kChunks;
        const uint8_t *want = trailer ? trl_desc : data_desc[part];
        const size_t want_len = 24 + (trailer ? kTrailerPayload : kChunkLen[part]);
        if (static_cast<size_t>(d.len) != want_len || std::memcmp(d.data, want, 12) != 0)
          throw std::runtime_error("framing: bank " + std::to_string(seq) + " part " + std::to_string(part) +
                                   " len " + std::to_string(d.len) + " desc " + hex12(d.data));
        if (!trailer) {
          std::memcpy(bank.data() + kChunkOff[part], d.data + 24, kChunkLen[part]);
        } else {
          std::memcpy(bank.data(), d.data + 24, kPrefix);
          std::memcpy(bank.data() + kSegB, d.data + 24 + kPrefix, kSegBHead);
          uint32_t stamp[4];
          std::memcpy(stamp, d.data + 24 + kPrefix + kSegBHead, sizeof stamp);
          if (stamp[0] != seq)
            throw std::runtime_error("trailer seq " + std::to_string(stamp[0]) + " != " + std::to_string(seq));
          bi.late_us = static_cast<int32_t>(stamp[2] - stamp[1]) / 5.0;
          bi.ship_us = static_cast<int32_t>(stamp[3] - stamp[2]) / 5.0;
          bi.end_us = static_cast<int32_t>(stamp[3] - stamp[1]) / 5.0;
          if (bi.end_us * 5 > budget5)
            throw std::runtime_error("deadline: bank " + std::to_string(seq) + " ended " +
                                     std::to_string(bi.end_us) + " us after due");
        }
        rd.release();
        part++;
      }
      if (_c.synthetic) {
        for (size_t i = 0; i < kBankBytes && !bi.pattern_bad; i++)
          bi.pattern_bad = !cpu_window(i) && bank[i] != fill(b, i);
      } else if (seen[b]) {
        // Live data: the ADC must have overwritten last revolution's descriptors and sentinel.
        for (int k = 0; k < kChunks; k++)
          bi.stale |= std::memcmp(bank.data() + kChunkOff[k] - 24, data_desc[k], 12) == 0;
        bi.stale |= std::memcmp(bank.data() + kBankBytes - 8, kSentinel, 8) == 0;
      }
      seen[b] = true;
      st.banks++;
      st.stale += bi.stale;
      st.pattern_bad += bi.pattern_bad;
      st.late_max_us = std::max(st.late_max_us, bi.late_us);
      st.ship_max_us = std::max(st.ship_max_us, bi.ship_us);
      st.end_max_us = std::max(st.end_max_us, bi.end_us);
      if (!on_bank(bank.data(), bi))
        break;
    }
  } catch (const std::exception &e) {
    st.error = e.what();
  }
  st.wall_s = std::chrono::duration<double>(clk::now() - t0).count();
  try {
    cpu.halt();
  } catch (...) {
  }
  rd.stop();
  la.restore();
  rx.restore();
  return st;
}

} // namespace wsdr
