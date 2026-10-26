// SPDX-License-Identifier: GPL-2.0-only
#include "c2h88xx.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "bulkin88xx.h"

namespace wsdr {

namespace {
constexpr int kXferTimeoutMs = 500;
using clk = std::chrono::steady_clock;

// (addr, width) of the RX-path registers the C2H tools program; 0x07C0 is the LA engine control.
const std::pair<uint16_t, int> kRxTouched[] = {
    {0x0100, 2}, // REG_CR
    {0x0608, 4}, // RCR
    {0x06A0, 2}, {0x06A2, 2}, {0x06A4, 2}, // RX filter maps
    {0x0290, 1}, // RXDMA mode
    {0x010C, 1}, // RXDMA control
    {0x07C0, 1}, // LA enable/trigger
    {0x0601, 1}, // WMAC_FWPKT_CR (BIT_FWEN)
    {0x029C, 2}, // FWFF_CTRL
    {0x02A0, 2}, // FWFF_PKT_INFO
};
} // namespace

C2hPacket C2hPacket::make(size_t payload, uint32_t seed) {
  if (payload == 0 || payload > 0x3FFF)
    throw std::invalid_argument("C2H payload out of range");
  C2hPacket p;
  p.frame.assign(24 + payload, 0);
  p.frame[0] = static_cast<uint8_t>(payload & 0xFF);
  p.frame[1] = static_cast<uint8_t>((payload >> 8) & 0x3F);
  p.frame[3] = 0x80; // OWN
  p.frame[11] = 0x10; // C2H flag
  uint32_t rng = seed;
  for (size_t i = 24; i < p.frame.size(); i++) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    p.frame[i] = static_cast<uint8_t>(rng);
  }
  return p;
}

bool C2hPacket::verify(const uint8_t *data, size_t len, bool check_payload) const {
  if (len != frame.size())
    return false;
  if (std::memcmp(data, frame.data(), 12) != 0)
    return false;
  if (check_payload && std::memcmp(data + 24, frame.data() + 24, frame.size() - 24) != 0)
    return false;
  return true;
}

uint32_t c2h_request(uint32_t addr) {
  return (addr & 0xFFFFu) | ((addr >> 16) << 24) | 0x10000u;
}

C2hRxPath::C2hRxPath(Reg &r) : _r(r) {
  for (auto [a, w] : kRxTouched) {
    uint32_t v = w == 1 ? _r.r8(a) : w == 2 ? _r.r16(a) : _r.r32(a);
    _saved.push_back({a, w, v});
  }
}

C2hRxPath::~C2hRxPath() { restore(); }

void C2hRxPath::apply(bool rx_on, bool fwff_off) {
  if (!rx_on) {
    _r.w16(0x0100, static_cast<uint16_t>(_r.r16(0x0100) & ~0xC0u));
  } else {
    // ac54 cpustream: the LA writer (and this C2H path in the real streamer) needs MAC RX on;
    // accept no frames instead of disabling RX.
    _r.w16(0x0100, static_cast<uint16_t>(_r.r16(0x0100) | 0x8Au));
    _r.w32(0x0608, (_r.r32(0x0608) | 0x80000u) & ~0xFFFFu);
    _r.w16(0x06A0, 0);
    _r.w16(0x06A2, 0);
    _r.w16(0x06A4, 0);
  }
  _r.w8(0x0290, 14);
  _r.w8(0x010C, static_cast<uint8_t>(_r.r8(0x010C) & ~4u));
  _r.w8(0x07C0, 0);
  if (fwff_off) {
    // Match ac54's rtw88 init state: devourer preserves the vendor firmware's FWFF engine,
    // which shares the firmware-packet/C2H datapath.
    _r.m8(0x0601, 0x80, 0);                 // clear BIT_FWEN
    _r.w16(0x029C, _r.r16(0x02A0));         // FWFF_CTRL = FWFF_PKT_INFO (empty)
  }
}

void C2hRxPath::restore() {
  if (_restored)
    return;
  _restored = true;
  for (auto &s : _saved) {
    try {
      if (s.width == 1)
        _r.w8(s.addr, static_cast<uint8_t>(s.value));
      else if (s.width == 2)
        _r.w16(s.addr, static_cast<uint16_t>(s.value));
      else
        _r.w32(s.addr, s.value);
    } catch (...) {
    }
  }
}

void c2h_stage(Reg &r, uint32_t ocp, const C2hPacket &p) {
  r.sram_write(ocp, p.frame.data(), p.frame.size());
}

C2hReader::C2hReader(libusb_device_handle *h, libusb_context *ctx, size_t transfer_bytes, int depth)
    : _h(h), _ctx(ctx), _bytes(transfer_bytes), _depth(std::clamp(depth, 1, 64)) {}

C2hReader::~C2hReader() { stop(); }

std::atomic<unsigned> C2hReader::g_callbacks{0}, C2hReader::g_cb_completed{0};

void LIBUSB_CALL C2hReader::cb(libusb_transfer *x) {
  auto *s = static_cast<Slot *>(x->user_data);
  s->status = x->status;
  s->length = x->actual_length;
  s->done = true;
  g_callbacks++;
  if (x->status == LIBUSB_TRANSFER_COMPLETED)
    g_cb_completed++;
}

bool C2hReader::start() {
  if (_started)
    return true;
  _slots.resize(static_cast<size_t>(_depth));
  for (auto &s : _slots) {
    s.buf.resize(_bytes);
    s.x = libusb_alloc_transfer(0);
    if (!s.x)
      return false;
    s.done = false;
    libusb_fill_bulk_transfer(s.x, _h, 0x84, s.buf.data(), static_cast<int>(_bytes), cb, &s,
                              kXferTimeoutMs);
    if (libusb_submit_transfer(s.x)) {
      s.done = true;
      _errors++;
      return false;
    }
  }
  _started = true;
  return true;
}

bool C2hReader::resubmit(Slot &s) {
  s.done = false;
  s.status = 0;
  s.length = 0;
  if (libusb_submit_transfer(s.x)) {
    s.done = true;
    _errors++;
    return false;
  }
  return true;
}

int C2hReader::wait(const C2hPacket &expect, bool check_payload, unsigned timeout_ms) {
  const auto deadline = clk::now() + std::chrono::milliseconds(timeout_ms);
  while (clk::now() < deadline) {
    for (auto &s : _slots) {
      if (!s.done)
        continue;
      s.done = false;
      if (s.status == LIBUSB_TRANSFER_COMPLETED) {
        if (expect.verify(s.buf.data(), static_cast<size_t>(s.length), check_payload)) {
          _verified++;
          if (!resubmit(s))
            return -1;
          return 1;
        }
        _stray++;
        if (!resubmit(s))
          return -1;
        continue;
      }
      if (s.status == LIBUSB_TRANSFER_TIMED_OUT) {
        _timeouts++;
        if (!resubmit(s))
          return -1;
        continue;
      }
      _errors++;
      return -1;
    }
    timeval tv{0, 5000};
    int rc = libusb_handle_events_timeout(_ctx, &tv);
    if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
      _errors++;
      return -1;
    }
  }
  return -1;
}

void C2hReader::drain(unsigned ms) {
  const auto end = clk::now() + std::chrono::milliseconds(ms);
  while (clk::now() < end) {
    timeval tv{0, 2000};
    libusb_handle_events_timeout(_ctx, &tv);
    for (auto &s : _slots) {
      if (!s.done)
        continue;
      s.done = false;
      _stray++;
      if (!resubmit(s))
        return;
    }
  }
}

void C2hReader::stop() {
  // Also after a start() that failed part-way: the slots it had submitted are in flight.
  for (auto &s : _slots)
    if (s.x && !s.done)
      libusb_cancel_transfer(s.x);
  for (int i = 0; i < 200; i++) {
    bool pending = false;
    for (auto &s : _slots)
      if (s.x && !s.done)
        pending = true;
    if (!pending)
      break;
    timeval tv{0, 2000};
    libusb_handle_events_timeout(_ctx, &tv);
  }
  for (auto &s : _slots) {
    if (!s.x)
      continue;
    if (!s.done)
      orphan_transfer(s.x, std::move(s.buf));
    else
      libusb_free_transfer(s.x);
    s.x = nullptr;
  }
  _started = false;
}

} // namespace wsdr
