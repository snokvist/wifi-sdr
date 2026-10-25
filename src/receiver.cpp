// SPDX-License-Identifier: GPL-2.0-only
#include "receiver.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "ChannelFreq.h"
#include "stream88xx.h"
#include "stream_cu.h"

namespace wsdr {

using clk = std::chrono::steady_clock;

const char *rx_state_name(RxState s) {
  switch (s) {
  case RxState::Closed: return "closed";
  case RxState::Opening: return "opening";
  case RxState::Starting: return "starting";
  case RxState::Streaming: return "streaming";
  case RxState::Lost: return "lost";
  case RxState::Error: return "error";
  }
  return "?";
}

// Primary 20 MHz channel for a requested centre. devourer's Channel is the primary channel with the
// 40 MHz channel above it, so at 40 MHz width the centre is primary + 10 MHz (measured, wide-tap.md).
int Receiver::channel_for(double freq, int width) {
  const double mhz = freq / 1e6;
  if (width == 40) {
    if (mhz < 3700)
      return std::clamp(static_cast<int>(std::lround((mhz - 10 - 2407) / 5)), 1, 9);
    return std::clamp(static_cast<int>(std::lround((mhz - 10 - 5000) / 5)), 16, 229);
  }
  if (mhz < 3700) {
    const int g = std::clamp(static_cast<int>(std::lround((mhz - 2407) / 5)), 1, 13);
    return std::abs(mhz - 2484) < std::abs(mhz - devourer::chan_to_freq(g)) ? 14 : g;
  }
  return std::clamp(static_cast<int>(std::lround((mhz - 5000) / 5)), 16, 233);
}

double Receiver::centre_of(int channel, int width) {
  return devourer::chan_to_freq(channel) * 1e6 + (width == 40 ? 10e6 : 0);
}

int Receiver::igi_for(double db, bool cu) {
  return std::clamp(0x50 - static_cast<int>(std::lround(db / gain_step(cu))), 0x1C, 0x50);
}

Receiver::Receiver(Logger_t log) : _log(std::move(log)) {}

Receiver::~Receiver() { close(); }

void Receiver::set_state(RxState s, const std::string &err) {
  std::lock_guard<std::mutex> l(_m);
  _st.state = s;
  if (!err.empty())
    _st.error = err;
}

void Receiver::open(uint16_t pid, const RxSettings &initial) {
  close();
  {
    std::lock_guard<std::mutex> l(_m);
    _set = initial;
    _st = {};
    _st.state = RxState::Opening;
  }
  const bool cu = pid == kPid8812cu;
  if (initial.rate_sel < 1 || initial.rate_sel > 5)
    throw std::invalid_argument("rate_sel must be 1..5");
  OpenOpts o;
  o.pid = pid;
  o.bw = width_of(initial, cu);
  o.channel = channel_for(initial.freq, o.bw);
  o.thermal_track = false; // no host register traffic while streaming
  auto s = std::make_unique<Session>(_log);
  try {
    s->open(o);
  } catch (...) {
    set_state(RxState::Closed);
    throw;
  }
  if (cu)
    s->reg().set_cap(4); // the 8822C hangs on >= 4 KiB EP0 block transfers (notes/cu-custom-code.md)
  _caps = {};
  _caps.pid = pid;
  _caps.cu = cu;
  _caps.superspeed = s->superspeed();
  _caps.chip = s->chip();
  // USB 2 carries ~44 MB/s of C2H (5 MS/s); USB 3 carries 40 MS/s. The 8812CU is verified on USB 2 only.
  _caps.min_rate_sel = s->superspeed() && !cu ? 1 : 4;
  _caps.paths = cu ? 2 : 1;
  _caps.gain_step = gain_step(cu);
  _caps.gain_max = gain_max(cu);
  _hw_channel = o.channel;
  _hw_width = o.bw;
  _s = std::move(s);
  std::lock_guard<std::mutex> l(_m);
  _set.rate_sel = std::max(_set.rate_sel, _caps.min_rate_sel);
  _st.state = RxState::Closed;
  _st.channel = _hw_channel;
  _st.width = _hw_width;
}

void Receiver::close() {
  stop();
  _s.reset();
  std::lock_guard<std::mutex> l(_m);
  _st.state = RxState::Closed;
}

void Receiver::start(BankSink sink) {
  if (!_s)
    throw std::logic_error("Receiver::start before open");
  if (_worker.joinable())
    return;
  _sink = std::move(sink);
  _active = true;
  _restart = false;
  _worker = std::thread([this] { worker(); });
}

void Receiver::stop() {
  _active = false;
  _restart = true;
  if (_worker.joinable())
    _worker.join();
  std::lock_guard<std::mutex> l(_m);
  if (_st.state != RxState::Lost)
    _st.state = RxState::Closed;
}

void Receiver::set(const RxSettings &s) {
  {
    std::lock_guard<std::mutex> l(_m);
    _set = s;
    _set.rate_sel = std::clamp(_set.rate_sel, _caps.min_rate_sel, 5);
    _set.gain_db = std::clamp(_set.gain_db, 0.0, _caps.gain_max);
    _last_change = clk::now();
  }
  _restart = true;
}

RxSettings Receiver::settings() const {
  std::lock_guard<std::mutex> l(_m);
  return _set;
}

RxStatus Receiver::status() const {
  std::lock_guard<std::mutex> l(_m);
  return _st;
}

void Receiver::worker() {
  uint64_t seq = 0;
  int consecutive_faults = 0;
  while (_active) {
    // Let a burst of setter calls settle before restarting.
    for (;;) {
      clk::time_point last;
      {
        std::lock_guard<std::mutex> l(_m);
        last = _last_change;
      }
      if (!_active || clk::now() - last >= std::chrono::milliseconds(30))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!_active)
      break;
    _restart = false;
    RxSettings cur;
    uint32_t gen;
    {
      std::lock_guard<std::mutex> l(_m);
      cur = _set;
      gen = ++_gen;
      _st.state = RxState::Starting;
    }
    const int width = width_of(cur, _caps.cu);
    const int ch = channel_for(cur.freq, width);
    const double fs = rate_of(cur.rate_sel);
    const double centre = centre_of(ch, width);
    try {
      if (ch != _hw_channel || width != _hw_width) {
        _s->tune(ch, width);
        _hw_channel = ch;
        _hw_width = width;
      }
    } catch (const std::exception &e) {
      if (!_s->reg().alive()) {
        set_state(RxState::Lost, std::string("adapter lost during retune: ") + e.what());
        break;
      }
      set_state(RxState::Error, std::string("retune to channel ") + std::to_string(ch) + " failed: " + e.what());
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    StreamCfg c;
    c.la.rate_sel = cur.rate_sel;
    // >= 20 MS/s needs ~13 ms of posted URBs; the 8812CU wants ~100 ms (FINDINGS §4)
    c.depth = _caps.cu || cur.rate_sel <= 2 ? 256 : 64;
    c.la.igi = cur.agc ? -1 : igi_for(cur.gain_db, _caps.cu);
    c.la.no_cca = !cur.agc;
    c.la.dfir = !_caps.cu && width == 40 ? 40 : 0; // 0 leaves devourer's BW20 DFIR (the 20 MHz path)
    BankMeta meta;
    meta.gen = gen;
    meta.fs = fs;
    meta.centre_hz = centre;
    meta.shift_hz = cur.freq - centre;
    meta.paths = _caps.paths;
    meta.cu = _caps.cu;
    {
      std::lock_guard<std::mutex> l(_m);
      _st.channel = ch;
      _st.width = width;
      _st.centre_hz = centre;
      _st.fs = fs;
      _st.state = RxState::Streaming;
    }
    bool clean_start = false;
    const auto on_bank = [&](const uint8_t *bank, const BankInfo &) {
      if (!clean_start) {
        clean_start = true;
        consecutive_faults = 0;
        std::lock_guard<std::mutex> l(_m);
        _st.error.clear();
      }
      meta.seq = seq++;
      _sink(bank, Stream8822b::kBankBytes, meta);
      {
        std::lock_guard<std::mutex> l(_m);
        _st.banks++;
      }
      return _active.load();
    };
    StreamStats ss;
    try {
      ss = _caps.cu ? Stream8822c(*_s, c).run(on_bank, &_restart) : Stream8822b(*_s, c).run(on_bank, &_restart);
    } catch (const std::exception &e) {
      ss.error = e.what();
    }
    if (ss.error.empty() || _restart || !_active)
      continue; // settings change or stop
    {
      std::lock_guard<std::mutex> l(_m);
      _st.restarts++;
    }
    if (!_s->reg().alive()) {
      set_state(RxState::Lost, "adapter lost: " + ss.error);
      break;
    }
    if (++consecutive_faults >= 5) {
      set_state(RxState::Error, "stream keeps failing: " + ss.error);
      break;
    }
    set_state(RxState::Starting, "stream fault (restarting): " + ss.error);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

} // namespace wsdr
