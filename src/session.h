// SPDX-License-Identifier: GPL-2.0-only
// Device session: open + claim the adapter, bring it up with devourer (power, firmware, MAC/BB/RF
// tables, calibration, channel), then hand out raw register access for the SDR work.
// Teardown order is devourer's DeviceSession contract: radio -> interface -> handle -> context.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "DeviceSession.h"
#include "IRadio.h"
#include "logger.h"
#include "rtlreg.h"

namespace wsdr {

struct OpenOpts {
  uint16_t vid = 0x0bda;
  uint16_t pid = 0;   // required
  int bus = -1;       // optional: only the adapter on this USB bus ...
  std::string port;   // ... and port path ("1", "1.2"); empty = any
  int channel = 36;
  int bw = 20;        // 20 / 40 / 80
  bool reset = true;  // libusb_reset_device before claim (devourer default)
  bool thermal_track = true; // devourer's ~2 s RF thermal tick; off for timing-critical probes
};

// Session::open failures the caller can act on (e.g. "install the udev rule" for Permission).
struct OpenError : std::runtime_error {
  enum Kind { NotFound, Permission, Busy, Other } kind;
  OpenError(Kind k, const std::string &what) : std::runtime_error(what), kind(k) {}
};

class Session {
public:
  explicit Session(Logger_t logger);
  ~Session();
  // Open, claim, CreateRadio, InitWrite(channel). Throws OpenError (not found / permission / busy) or
  // std::runtime_error (bring-up failure).
  void open(const OpenOpts &o);
  IRadio *radio() const { return _s.device(); }
  libusb_device_handle *handle() const { return _s.handle(); }
  libusb_context *context() const { return _s.context(); }
  Reg &reg() { return *_reg; }
  bool superspeed() const { return _ss; }
  std::string chip() const { return _chip; }
  // Retune via devourer (full SetMonitorChannel path).
  void tune(int channel, int bw = 20);

private:
  Logger_t _log;
  devourer::DeviceSession _s;
  std::unique_ptr<Reg> _reg;
  bool _ss = false;
  std::string _chip;
};

} // namespace wsdr
