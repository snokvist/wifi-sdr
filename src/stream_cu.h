// SPDX-License-Identifier: GPL-2.0-only
// Continuous LA streaming from the 8822C (RTL8812CU): stream8822c.S on the embedded CPU ships each
// completed 64 KiB ring bank as five C2H data packets + one trailer (the 8822C's C2H window splits
// every bank at offset 0x6000, and no packet may cross the split; see the .S header). Same checks as
// Stream8822b (framing, trailer seq, deadline, staleness / synthetic pattern) and the same StreamCfg /
// BankInfo / StreamStats. Each LA word carries both RX paths (A: I[11:0] Q[23:12], B: Q[43:32]
// I[55:44]). USB 2 only on this board: ~44.7 MB/s C2H, so rate_sel 5 (2.5 MS/s) or 4 (5 MS/s).
// Preconditions (notes/cu-custom-code.md): devourer's Jaguar3 runtime thread off (Session does it),
// RX-DMA aggregation off, EP0 block cap 4.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "session.h"
#include "stream88xx.h"

namespace wsdr {

class Stream8822c {
public:
  static constexpr size_t kBankBytes = 0x10000;
  static constexpr size_t kBankSamples = kBankBytes / 8;
  static constexpr size_t kPrefix = 128;
  static constexpr size_t kSegB = 0x6000;     // C2H segment split inside a bank
  static constexpr size_t kSegBHead = 24;     // [kSegB, kSegB + 24): chunk 2's descriptor
  static constexpr int kChunks = 5;
  static constexpr std::array<size_t, kChunks> kChunkOff = {128, 12352, 0x6018, 0x9568, 0xCAB8};
  static constexpr std::array<size_t, kChunks> kChunkLen = {12224, 12224, 13648, 13648, 13640};
  static constexpr size_t kTrailerPayload = kPrefix + kSegBHead + 16;

  Stream8822c(Session &s, const StreamCfg &c) : _s(s), _c(c) {}
  double fs() const { return 80e6 / (1u << _c.la.rate_sel); }
  double period_us() const { return kBankSamples / fs() * 1e6; }
  // Gapped mode (StreamCfg::gapped): each bank is a complete capture of kBankSamples, stamped; the
  // gap between banks is the shipping time (~1.5 ms on USB 2). For rates USB 2 cannot carry.
  StreamStats run(const std::function<bool(const uint8_t *bank, const BankInfo &)> &on_bank,
                  const std::atomic<bool> *stop = nullptr);

private:
  Session &_s;
  StreamCfg _c;
};

// LA word -> 12-bit path B I / Q (8822C, dma_type 0).
inline int la_ib(uint64_t w) { return static_cast<int>(static_cast<int32_t>(static_cast<uint32_t>((w >> 44) & 0xFFF) << 20) >> 20); }
inline int la_qb(uint64_t w) { return static_cast<int>(static_cast<int32_t>(static_cast<uint32_t>((w >> 32) & 0xFFF) << 20) >> 20); }

} // namespace wsdr
