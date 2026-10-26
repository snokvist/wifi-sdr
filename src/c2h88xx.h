// SPDX-License-Identifier: GPL-2.0-only
// Host-side C2H transport helpers (8822B and 8822C): one packet = 24-byte RX descriptor + payload,
// emitted by the chip out bulk-IN EP 0x84 when a request word is written to REG 0x294.
//
// The request encoding is ac54's routeprobe/lamap finding (facts only; see
// notes/ac54-analysis.md §5): `(addr & 0xffff) | ((addr >> 16) << 24) | 0x10000`, where addr is
// the C2H-mapped byte address. RX SRAM 0x1000 -> 0x11000; the two LA TX banks map to 0x16000
// (TX 0x20000..0x2ffff) and 0x06000 (TX 0x30000..0x3ffff).
#pragma once

#include <libusb.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtlreg.h"

namespace wsdr {

// Descriptor (24 B) + payload, as staged in SRAM and as the chip emits it. The chip mutates
// descriptor bytes 12..23 in flight, so verification only trusts bytes 0..11 and the payload.
struct C2hPacket {
  std::vector<uint8_t> frame;
  static C2hPacket make(size_t payload, uint32_t seed = 0x67a72b31u);
  size_t payload() const { return frame.size() - 24; }
  bool verify(const uint8_t *data, size_t len, bool check_payload) const;
};

// ac54/routeprobe request encoding for a C2H-mapped address.
uint32_t c2h_request(uint32_t addr);

// RX-path state the host/C2H tools program (apply: MAC RX off, or on with no frames accepted;
// RXDMA mode 14, no RW release; after ac54 cpubench/cpustream). Snapshot on construction, restore in the
// destructor (or explicitly).
class C2hRxPath {
public:
  explicit C2hRxPath(Reg &r);
  ~C2hRxPath();
  C2hRxPath(const C2hRxPath &) = delete;
  C2hRxPath &operator=(const C2hRxPath &) = delete;

  void apply(bool rx_on, bool fwff_off = false);
  void restore();

private:
  struct Saved {
    uint16_t addr;
    int width;
    uint32_t value;
  };
  Reg &_r;
  std::vector<Saved> _saved;
  bool _restored = false;
};

void c2h_stage(Reg &r, uint32_t ocp, const C2hPacket &p);

// Ring of async bulk-IN transfers on EP 0x84. One completion is one C2H packet.
class C2hReader {
public:
  C2hReader(libusb_device_handle *h, libusb_context *ctx, size_t transfer_bytes, int depth);
  ~C2hReader();
  C2hReader(const C2hReader &) = delete;
  C2hReader &operator=(const C2hReader &) = delete;

  bool start();
  // Reap until one verified packet arrives (1) or the wait deadline/error (-1). Strays and
  // per-transfer timeouts are counted and resubmitted.
  int wait(const C2hPacket &expect, bool check_payload, unsigned timeout_ms);
  void drain(unsigned ms); // discard whatever is already queued (start-up junk)
  void stop();

  unsigned verified() const { return _verified; }
  unsigned stray() const { return _stray; }
  unsigned timeouts() const { return _timeouts; }
  unsigned errors() const { return _errors; }
  // Diagnostics: every transfer callback / every COMPLETED one, process-wide.
  static std::atomic<unsigned> g_callbacks, g_cb_completed;

private:
  struct Slot {
    libusb_transfer *x = nullptr;
    std::vector<uint8_t> buf;
    bool done = false;
    int status = 0;
    int length = 0;
  };
  static void LIBUSB_CALL cb(libusb_transfer *x);
  bool resubmit(Slot &s);

  libusb_device_handle *_h;
  libusb_context *_ctx;
  size_t _bytes;
  int _depth;
  std::vector<Slot> _slots;
  bool _started = false;
  unsigned _verified = 0, _stray = 0, _timeouts = 0, _errors = 0;
};

} // namespace wsdr
