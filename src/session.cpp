// SPDX-License-Identifier: GPL-2.0-only
#include "session.h"

#include <stdexcept>

#include "UsbOpen.h"
#include "WiFiDriver.h"

namespace wsdr {

// Jaguar3 (8822C/E): devourer starts a coex/phydm runtime thread after bring-up that keeps a bulk-IN
// read posted (it steals C2H packets from our reader) and rewrites DIG/IGI every ~2 s. All wifi-sdr
// tools own bulk-IN and want fixed gain, so turn it off where devourer has the switch
// (tuning.j3_runtime_thread, proposed upstream); older devourer builds compile without it.
template <class T> auto no_j3_runtime(T &t, int) -> decltype(t.j3_runtime_thread = false, void()) {
  t.j3_runtime_thread = false;
}
template <class T> void no_j3_runtime(T &, long) {}

static ChannelWidth_t width_of(int bw) {
  switch (bw) {
  case 40: return CHANNEL_WIDTH_40;
  case 80: return CHANNEL_WIDTH_80;
  default: return CHANNEL_WIDTH_20;
  }
}

Session::Session(Logger_t logger) : _log(std::move(logger)), _s(_log) {}

Session::~Session() {
  _reg.reset();
  _s.close();
}

void Session::open(const OpenOpts &o) {
  if (!o.pid)
    throw std::invalid_argument("pid required");
  libusb_context *ctx = nullptr;
  if (libusb_init(&ctx) < 0)
    throw std::runtime_error("libusb_init failed");
  _s.adopt_context(ctx);
  libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, o.vid, o.pid);
  if (!h)
    throw std::runtime_error("adapter not found");
  std::shared_ptr<devourer::UsbDeviceLock> lock;
  if (devourer::claim_interface_then_reset(h, devourer::find_wifi_interface(h), _log, o.reset,
                                           lock) != 0) {
    _s.adopt_handle(h);
    throw std::runtime_error("claim failed (adapter busy?)");
  }
  _s.adopt_handle(h);
  _s.adopt_lock(lock);
  _ss = libusb_get_device_speed(libusb_get_device(h)) >= LIBUSB_SPEED_SUPER;

  devourer::DeviceConfig cfg;
  cfg.tuning.thermal_track = o.thermal_track;
  no_j3_runtime(cfg.tuning, 0);
  WiFiDriver driver(_log);
  auto dev = driver.CreateRadio(h, ctx, lock, cfg);
  if (!dev)
    throw std::runtime_error("CreateRadio failed");
  _s.adopt_device(std::move(dev));
  _chip = radio()->GetAdapterCaps().chip_name;
  radio()->InitWrite(SelectedChannel{.Channel = static_cast<uint8_t>(o.channel),
                                     .ChannelOffset = static_cast<uint8_t>(o.bw > 20 ? 1 : 0),
                                     .ChannelWidth = width_of(o.bw)});
  _reg = std::make_unique<Reg>(h, _ss);
}

void Session::tune(int channel, int bw) {
  radio()->SetMonitorChannel(SelectedChannel{.Channel = static_cast<uint8_t>(channel),
                                             .ChannelOffset = static_cast<uint8_t>(bw > 20 ? 1 : 0),
                                             .ChannelWidth = width_of(bw)});
}

} // namespace wsdr
