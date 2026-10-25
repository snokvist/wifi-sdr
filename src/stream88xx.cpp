// SPDX-License-Identifier: GPL-2.0-only
#include "stream88xx.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <thread>

#include "c2h88xx.h"
#include "cpu88xx.h"
#include "stream8822b.h"

namespace wsdr {

namespace {
using clk = std::chrono::steady_clock;

constexpr uint32_t kStateOff = 0x1000;  // DMEM offset of the program's state block
constexpr uint32_t kRecOff = 0x1100;    // bank records (0x40 apart)
constexpr uint32_t kMagic = 0x57535431u;
constexpr uint32_t kTrailerOcp = 0x18701000u; // RX SRAM; C2H address 0x1000
constexpr uint32_t kBankCpu[2] = {0xB87A0000u, 0xB87B0000u};
constexpr uint32_t kBankC2h[2] = {0x16000u, 0x06000u}; // TXFF 0x20000 / 0x30000 aliases
constexpr uint8_t kSentinel[8] = {0xE1, 0xF1, 0x47, 0x5E, 0xA5, 0xA5, 0xA5, 0xA5}; // stream8822b.S

size_t chunk_off(int k) { return Stream8822b::kPrefix + static_cast<size_t>(k) * Stream8822b::kChunk; }

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

// Bulk-IN EP 0x84 transfers reaped strictly in submission order (one transfer = one C2H packet).
class InOrderReader {
public:
  InOrderReader(libusb_device_handle *h, libusb_context *ctx, int depth) : _h(h), _ctx(ctx) {
    _slots.resize(static_cast<size_t>(depth));
  }
  ~InOrderReader() { stop(); }

  void start() {
    for (size_t i = 0; i < _slots.size(); i++) {
      Slot &s = _slots[i];
      s.buf.resize(16384);
      s.x = libusb_alloc_transfer(0);
      if (!s.x)
        throw std::runtime_error("libusb_alloc_transfer");
      libusb_fill_bulk_transfer(s.x, _h, 0x84, s.buf.data(), static_cast<int>(s.buf.size()), cb, &s, 0);
      submit(i);
    }
  }
  // Next completed transfer (head of the queue); nullptr on timeout.
  struct Done {
    const uint8_t *data;
    int len;
    int status;
  };
  bool next(Done &d, unsigned timeout_ms) {
    const size_t i = _fifo.front();
    const auto dl = clk::now() + std::chrono::milliseconds(timeout_ms);
    while (_slots[i].busy) {
      if (clk::now() >= dl)
        return false;
      timeval tv{0, 2000};
      const int rc = libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
      if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED)
        throw std::runtime_error(std::string("libusb events: ") + libusb_error_name(rc));
    }
    d = {_slots[i].buf.data(), _slots[i].x->actual_length, _slots[i].x->status};
    return true;
  }
  void release() { // resubmit the head transfer at the tail
    const size_t i = _fifo.front();
    _fifo.pop_front();
    submit(i);
  }
  void drain(unsigned ms) {
    const auto end = clk::now() + std::chrono::milliseconds(ms);
    Done d;
    while (clk::now() < end)
      if (next(d, 1))
        release();
  }
  void stop() {
    for (auto &s : _slots)
      if (s.x && s.busy)
        libusb_cancel_transfer(s.x);
    for (int n = 0; n < 500; n++) {
      bool any = false;
      for (auto &s : _slots)
        any |= s.x && s.busy;
      if (!any)
        break;
      timeval tv{0, 2000};
      libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
    }
    for (auto &s : _slots)
      if (s.x && !s.busy) {
        libusb_free_transfer(s.x);
        s.x = nullptr;
      }
    _fifo.clear();
  }

private:
  struct Slot {
    libusb_transfer *x = nullptr;
    std::vector<uint8_t> buf;
    bool busy = false;
  };
  static void LIBUSB_CALL cb(libusb_transfer *x) { static_cast<Slot *>(x->user_data)->busy = false; }
  void submit(size_t i) {
    _slots[i].busy = true;
    if (const int rc = libusb_submit_transfer(_slots[i].x)) {
      _slots[i].busy = false;
      throw std::runtime_error(std::string("bulk-IN submit: ") + libusb_error_name(rc));
    }
    _fifo.push_back(i);
  }

  libusb_device_handle *_h;
  libusb_context *_ctx;
  std::vector<Slot> _slots;
  std::deque<size_t> _fifo;
};
} // namespace

StreamStats Stream8822b::run(const std::function<bool(const uint8_t *, const BankInfo &)> &on_bank,
                             const volatile bool *stop) {
  StreamStats st;
  Reg &r = _s.reg();
  C2hRxPath rx(r);
  La8822b la(r);
  Cpu8822b cpu(r);
  InOrderReader rd(_s.handle(), _s.context(), _c.depth);
  const uint32_t period5 = 512u << _c.la.rate_sel;
  const int32_t guard5 = _c.guard_us * 5;

  uint8_t data_desc[24], trl_desc[24];
  desc(data_desc, kChunk);
  desc(trl_desc, kTrailerPayload);
  std::vector<uint8_t> bank(kBankBytes);
  bool seen[2] = {false, false};
  const auto t0 = clk::now();
  try {
    cpu.load(stream8822b_bin, stream8822b_bin_len);
    rx.apply(true); // MAC RX on, no frames accepted: the LA writer needs it
    la.configure(_c.la);
    if (_c.synthetic) {
      std::vector<uint8_t> pat(kBankBytes);
      for (int b = 0; b < 2; b++) {
        for (size_t i = 0; i < kBankBytes; i++)
          pat[i] = fill(b, i);
        r.sram_write(La8822b::kRingOcp + static_cast<uint32_t>(b) * kBankBytes, pat.data(), pat.size());
      }
    }
    uint8_t trl[24 + kTrailerPayload] = {};
    std::memcpy(trl, trl_desc, 24);
    r.sram_write(kTrailerOcp, trl, sizeof trl);

    uint32_t state[9] = {0, 0, 0, 0, 0, _c.synthetic ? 0u : 1u, period5, static_cast<uint32_t>(guard5), 0};
    std::memcpy(&state[8], data_desc, 4);
    cpu.dmem_write(kStateOff, state, sizeof state);
    uint32_t rec[2][16] = {};
    for (int b = 0; b < 2; b++) {
      rec[b][0] = kBankCpu[b];
      for (int k = 0; k < 4; k++) {
        const uint32_t d = static_cast<uint32_t>(chunk_off(k) - 24);
        rec[b][1 + 2 * k] = kBankCpu[b] + d;
        rec[b][2 + 2 * k] = c2h_request(kBankC2h[b] + d);
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
      while (part < 5) {
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
        const bool trailer = part == 4;
        const uint8_t *want = trailer ? trl_desc : data_desc;
        const size_t want_len = 24 + (trailer ? kTrailerPayload : kChunk);
        if (static_cast<size_t>(d.len) != want_len || std::memcmp(d.data, want, 12) != 0)
          throw std::runtime_error("framing: bank " + std::to_string(seq) + " part " + std::to_string(part) +
                                   " len " + std::to_string(d.len) + " desc " + hex12(d.data));
        if (!trailer) {
          std::memcpy(bank.data() + chunk_off(part), d.data + 24, kChunk);
        } else {
          std::memcpy(bank.data(), d.data + 24, kPrefix);
          uint32_t stamp[4];
          std::memcpy(stamp, d.data + 24 + kPrefix, sizeof stamp);
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
      // CPU-written windows: descriptors ([104,128) and the 24 bytes before chunks 1..3) and the
      // sentinel in the last sample. In live data the ADC must have overwritten all of them.
      uint8_t *tail = bank.data() + kBankBytes - 8;
      if (_c.synthetic) {
        for (size_t i = 0; i < kBankBytes - 8 && !bi.pattern_bad; i++) {
          bool win = false;
          for (int k = 0; k < 4; k++)
            win |= i >= chunk_off(k) - 24 && i < chunk_off(k);
          bi.pattern_bad = !win && bank[i] != fill(b, i);
        }
      } else if (seen[b]) {
        for (int k = 0; k < 4; k++)
          bi.stale |= std::memcmp(bank.data() + chunk_off(k) - 24, data_desc, 12) == 0;
        bi.stale |= std::memcmp(tail, kSentinel, 8) == 0;
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
