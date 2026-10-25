// SPDX-License-Identifier: GPL-2.0-only
// SoapySDR driver "wsdr": the RTL8812BU (RTL8822B) LA stream as an SDR receiver.
//
// Build and use (no system install needed):
//   cmake -S . -B build-soapy -DWSDR_SOAPY=ON && cmake --build build-soapy -j --target wsdrSupport
//   export SOAPY_SDR_PLUGIN_PATH=$PWD/build-soapy
//   SoapySDRUtil --find=driver=wsdr
//   gqrx: device string  soapy=0,driver=wsdr      SDR++: SoapySDR source, device "wsdr"
// The client opens the adapter itself, so it needs USB access: install tools/99-wsdr.rules (udev) or
// run the client as root. Keep the rtw88 guard ON so the kernel driver does not claim the adapter.
//
// What the client sees:
//   frequency    a double, so 5 GHz needs no tricks; snapped to the 5 MHz synthesizer grid
//                (2.4 GHz ch1..14, 5 GHz ch16..233), the residual (<= 2.5 MHz) shifted digitally
//   sample rate  2.5 MS/s or 5 MS/s (LA rate_sel 5 / 4), native, no resampling; bandwidth = rate
//   gain         one element "IGI", 0..49 dB in ~0.95 dB steps (fixed-gain mode, OFDM CCA off);
//                gain mode "automatic" hands gain to the chip's AGC (gain steps during frames)
//   format       CS16 (native: 12-bit << 4) or CF32 (/2048); one RX channel (path A); DC removed
//                unless DC offset mode is turned off
// Every setting change restarts the stream (tens of ms; ~150 ms for a retune), so the chip is never
// touched while it streams. The stream needs USB 2 (16 KiB C2H wedges on USB 3; FINDINGS §4).
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ChannelFreq.h"
#include "iqdsp.h"
#include "logger.h"
#include "session.h"
#include "stream88xx.h"

using namespace wsdr;
using clk = std::chrono::steady_clock;

namespace {

constexpr uint16_t kVid = 0x0bda;
constexpr uint16_t kPid8812bu = 0xb812;
constexpr double kGainMax = 49.4;   // (0x50 - 0x1C) * 0.95
constexpr double kGainStep = 0.95;  // measured dB per IGI step (FINDINGS §4)

struct Settings {
  double freq = 5180e6;
  int rate_sel = 5; // 5 = 2.5 MS/s, 4 = 5 MS/s
  bool agc = false;
  double gain_db = 30;
  bool dc_remove = true;
};

double rate_of(int rate_sel) { return 80e6 / (1 << rate_sel); }

int igi_for(double db) {
  return std::clamp(0x50 - static_cast<int>(std::lround(db / kGainStep)), 0x1C, 0x50);
}

// Nearest synthesizer channel (5 MHz grid) for a frequency; same rule as wsdr_tcp.
int channel_for(double freq) {
  const double mhz = freq / 1e6;
  if (mhz < 3700) {
    const int g = std::clamp(static_cast<int>(std::lround((mhz - 2407) / 5)), 1, 13);
    return std::abs(mhz - 2484) < std::abs(mhz - devourer::chan_to_freq(g)) ? 14 : g;
  }
  return std::clamp(static_cast<int>(std::lround((mhz - 5000) / 5)), 16, 233);
}

// Single-producer / single-consumer sample ring (complex float, LA units). Overflow drops the
// incoming block and is reported once to the reader.
class Ring {
public:
  void reset(size_t cap) {
    std::lock_guard<std::mutex> l(_m);
    _buf.assign(cap, cf(0));
    _head = _tail = _count = 0;
    _ovf = false;
  }
  void push(const cf *x, size_t n) {
    {
      std::lock_guard<std::mutex> l(_m);
      if (n > _buf.size() - _count) {
        _ovf = true;
        return;
      }
      for (size_t i = 0; i < n; i++) {
        _buf[_head] = x[i];
        _head = (_head + 1) % _buf.size();
      }
      _count += n;
    }
    _cv.notify_one();
  }
  // >0 samples copied, 0 timeout, -1 overflow pending (cleared), -2 woken by stop.
  int pop(cf *out, size_t n, long timeout_us, const std::atomic<bool> &active) {
    std::unique_lock<std::mutex> l(_m);
    _cv.wait_for(l, std::chrono::microseconds(std::max(0L, timeout_us)),
                 [&] { return _count > 0 || _ovf || !active; });
    if (_ovf) {
      _ovf = false;
      return -1;
    }
    if (!_count)
      return active ? 0 : -2;
    const size_t k = std::min(n, _count);
    for (size_t i = 0; i < k; i++) {
      out[i] = _buf[_tail];
      _tail = (_tail + 1) % _buf.size();
    }
    _count -= k;
    return static_cast<int>(k);
  }
  void wake() { _cv.notify_all(); }

private:
  std::mutex _m;
  std::condition_variable _cv;
  std::vector<cf> _buf;
  size_t _head = 0, _tail = 0, _count = 0;
  bool _ovf = false;
};

struct WsdrStream {
  std::string format;
};

class SoapyWsdr : public SoapySDR::Device {
public:
  explicit SoapyWsdr(const SoapySDR::Kwargs &args) {
    if (args.count("freq"))
      _set.freq = std::stod(args.at("freq"));
    OpenOpts o;
    o.pid = kPid8812bu;
    o.channel = channel_for(_set.freq);
    o.thermal_track = false; // no host register traffic while streaming
    _s = std::make_unique<Session>(std::make_shared<Logger>());
    _s->open(o); // throws on failure; SoapySDR reports it to the client
    _channel = o.channel;
    if (_s->superspeed())
      SoapySDR::log(SOAPY_SDR_WARNING,
                    "wsdr: adapter is on USB 3, where 16 KiB C2H wedges; replug it so it enumerates at USB 2");
  }

  ~SoapyWsdr() override { stop_worker(); }

  // ---- identification ----
  std::string getDriverKey() const override { return "wsdr"; }
  std::string getHardwareKey() const override { return "RTL8822B"; }
  SoapySDR::Kwargs getHardwareInfo() const override {
    return {{"chip", _s->chip()}, {"usb", _s->superspeed() ? "3 (streaming unsupported)" : "2"},
            {"path", "A"}, {"source", "LA ring, C2H bank streamer"}};
  }

  // ---- channels ----
  size_t getNumChannels(const int dir) const override { return dir == SOAPY_SDR_RX ? 1 : 0; }
  bool getFullDuplex(const int, const size_t) const override { return false; }

  // ---- streams ----
  std::vector<std::string> getStreamFormats(const int dir, const size_t) const override {
    if (dir != SOAPY_SDR_RX)
      return {};
    return {SOAPY_SDR_CS16, SOAPY_SDR_CF32};
  }
  std::string getNativeStreamFormat(const int, const size_t, double &fullScale) const override {
    fullScale = 32768;
    return SOAPY_SDR_CS16;
  }
  SoapySDR::Stream *setupStream(const int dir, const std::string &format, const std::vector<size_t> &channels,
                                const SoapySDR::Kwargs &) override {
    if (dir != SOAPY_SDR_RX)
      throw std::runtime_error("wsdr: RX only");
    if (format != SOAPY_SDR_CS16 && format != SOAPY_SDR_CF32)
      throw std::runtime_error("wsdr: format must be CS16 or CF32");
    if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0))
      throw std::runtime_error("wsdr: one channel (0)");
    std::lock_guard<std::mutex> l(_streamm);
    if (_stream)
      throw std::runtime_error("wsdr: stream already open");
    _stream = std::make_unique<WsdrStream>();
    _stream->format = format;
    return reinterpret_cast<SoapySDR::Stream *>(_stream.get());
  }
  void closeStream(SoapySDR::Stream *) override {
    stop_worker();
    std::lock_guard<std::mutex> l(_streamm);
    _stream.reset();
  }
  size_t getStreamMTU(SoapySDR::Stream *) const override { return Stream8822b::kBankSamples; }
  int activateStream(SoapySDR::Stream *, const int flags, const long long, const size_t) override {
    if (flags)
      return SOAPY_SDR_NOT_SUPPORTED;
    if (_active)
      return 0;
    _ring.reset(static_cast<size_t>(rate_of(4) / 2)); // 0.5 s at the highest rate
    _active = true;
    _restart = false;
    _worker = std::thread([this] { worker(); });
    return 0;
  }
  int deactivateStream(SoapySDR::Stream *, const int flags, const long long) override {
    if (flags)
      return SOAPY_SDR_NOT_SUPPORTED;
    stop_worker();
    return 0;
  }
  int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags, long long &timeNs,
                 const long timeoutUs) override {
    flags = 0;
    timeNs = 0;
    auto *st = reinterpret_cast<WsdrStream *>(stream);
    if (_tmp.size() < numElems)
      _tmp.resize(numElems);
    const int n = _ring.pop(_tmp.data(), numElems, timeoutUs, _active);
    if (n == -1)
      return SOAPY_SDR_OVERFLOW;
    if (n == -2)
      return SOAPY_SDR_STREAM_ERROR;
    if (n == 0)
      return SOAPY_SDR_TIMEOUT;
    if (st->format == SOAPY_SDR_CS16) {
      auto *o = static_cast<int16_t *>(buffs[0]);
      for (int i = 0; i < n; i++) {
        o[2 * i] = static_cast<int16_t>(std::clamp(std::lrint(_tmp[i].real() * 16.f), -32768L, 32767L));
        o[2 * i + 1] = static_cast<int16_t>(std::clamp(std::lrint(_tmp[i].imag() * 16.f), -32768L, 32767L));
      }
    } else {
      auto *o = static_cast<float *>(buffs[0]);
      for (int i = 0; i < n; i++) {
        o[2 * i] = _tmp[i].real() / 2048.f;
        o[2 * i + 1] = _tmp[i].imag() / 2048.f;
      }
    }
    return n;
  }

  // ---- antenna ----
  std::vector<std::string> listAntennas(const int dir, const size_t) const override {
    return dir == SOAPY_SDR_RX ? std::vector<std::string>{"RX"} : std::vector<std::string>{};
  }
  void setAntenna(const int, const size_t, const std::string &) override {}
  std::string getAntenna(const int, const size_t) const override { return "RX"; }

  // ---- DC offset (host-side DC removal) ----
  bool hasDCOffsetMode(const int dir, const size_t) const override { return dir == SOAPY_SDR_RX; }
  void setDCOffsetMode(const int, const size_t, const bool automatic) override {
    update([&](Settings &s) { s.dc_remove = automatic; });
  }
  bool getDCOffsetMode(const int, const size_t) const override { return get().dc_remove; }

  // ---- gain ----
  std::vector<std::string> listGains(const int dir, const size_t) const override {
    return dir == SOAPY_SDR_RX ? std::vector<std::string>{"IGI"} : std::vector<std::string>{};
  }
  bool hasGainMode(const int dir, const size_t) const override { return dir == SOAPY_SDR_RX; }
  void setGainMode(const int, const size_t, const bool automatic) override {
    update([&](Settings &s) { s.agc = automatic; });
  }
  bool getGainMode(const int, const size_t) const override { return get().agc; }
  void setGain(const int, const size_t, const double value) override {
    update([&](Settings &s) { s.gain_db = std::clamp(value, 0.0, kGainMax); });
  }
  void setGain(const int dir, const size_t ch, const std::string &, const double value) override {
    setGain(dir, ch, value);
  }
  double getGain(const int, const size_t) const override { return get().gain_db; }
  double getGain(const int dir, const size_t ch, const std::string &) const override { return getGain(dir, ch); }
  SoapySDR::Range getGainRange(const int, const size_t) const override { return {0, kGainMax, kGainStep}; }
  SoapySDR::Range getGainRange(const int dir, const size_t ch, const std::string &) const override {
    return getGainRange(dir, ch);
  }

  // ---- frequency ----
  std::vector<std::string> listFrequencies(const int dir, const size_t) const override {
    return dir == SOAPY_SDR_RX ? std::vector<std::string>{"RF"} : std::vector<std::string>{};
  }
  void setFrequency(const int, const size_t, const double frequency, const SoapySDR::Kwargs &) override {
    update([&](Settings &s) { s.freq = frequency; });
  }
  void setFrequency(const int dir, const size_t ch, const std::string &, const double frequency,
                    const SoapySDR::Kwargs &args) override {
    setFrequency(dir, ch, frequency, args);
  }
  double getFrequency(const int, const size_t) const override { return get().freq; }
  double getFrequency(const int dir, const size_t ch, const std::string &) const override {
    return getFrequency(dir, ch);
  }
  SoapySDR::RangeList getFrequencyRange(const int, const size_t) const override {
    return {SoapySDR::Range(2.400e9, 2.500e9), SoapySDR::Range(5.080e9, 6.165e9)};
  }
  SoapySDR::RangeList getFrequencyRange(const int dir, const size_t ch, const std::string &) const override {
    return getFrequencyRange(dir, ch);
  }

  // ---- sample rate / bandwidth ----
  void setSampleRate(const int, const size_t, const double rate) override {
    const int rs = std::abs(rate - rate_of(4)) < std::abs(rate - rate_of(5)) ? 4 : 5;
    if (std::abs(rate - rate_of(rs)) > 1)
      SoapySDR::logf(SOAPY_SDR_WARNING, "wsdr: %.0f S/s not supported, using %.0f", rate, rate_of(rs));
    update([&](Settings &s) { s.rate_sel = rs; });
  }
  double getSampleRate(const int, const size_t) const override { return rate_of(get().rate_sel); }
  std::vector<double> listSampleRates(const int, const size_t) const override { return {rate_of(5), rate_of(4)}; }
  SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override {
    return {SoapySDR::Range(rate_of(5), rate_of(5)), SoapySDR::Range(rate_of(4), rate_of(4))};
  }
  void setBandwidth(const int, const size_t, const double) override {}
  double getBandwidth(const int dir, const size_t ch) const override { return getSampleRate(dir, ch); }
  std::vector<double> listBandwidths(const int dir, const size_t ch) const override {
    return {getSampleRate(dir, ch)};
  }

private:
  Settings get() const {
    std::lock_guard<std::mutex> l(_setm);
    return _set;
  }
  // Setters only record the change; the worker applies it between stream runs.
  template <class F> void update(F f) {
    {
      std::lock_guard<std::mutex> l(_setm);
      f(_set);
      _last_change = clk::now();
    }
    _restart = true;
  }

  void stop_worker() {
    _active = false;
    _restart = true;
    _ring.wake();
    if (_worker.joinable())
      _worker.join();
  }

  // Owns the device while active: (re)configure, stream until a setting changes or a fault, repeat.
  void worker() {
    std::vector<cf> x(Stream8822b::kBankSamples);
    while (_active) {
      // Let a burst of setter calls settle before restarting.
      for (;;) {
        clk::time_point last;
        {
          std::lock_guard<std::mutex> l(_setm);
          last = _last_change;
        }
        if (!_active || clk::now() - last >= std::chrono::milliseconds(30))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!_active)
        break;
      _restart = false;
      const Settings cur = get();
      const int ch = channel_for(cur.freq);
      const double src = rate_of(cur.rate_sel);
      const double shift = cur.freq - devourer::chan_to_freq(ch) * 1e6;
      if (std::abs(shift) > 0.05 * src)
        SoapySDR::logf(SOAPY_SDR_WARNING,
                       "wsdr: %.3f MHz is %.0f kHz off the 5 MHz grid; the band edge on that side is aliased",
                       cur.freq / 1e6, shift / 1e3);
      try {
        if (ch != _channel) {
          _s->tune(ch);
          _channel = ch;
        }
      } catch (const std::exception &e) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "wsdr: retune to channel %d failed: %s", ch, e.what());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      DcBlock dc;
      Nco nco;
      nco.set(shift, src);
      StreamCfg c;
      c.la.rate_sel = cur.rate_sel;
      c.la.igi = cur.agc ? -1 : igi_for(cur.gain_db);
      c.la.no_cca = !cur.agc;
      Stream8822b st(*_s, c);
      const StreamStats ss = st.run(
          [&](const uint8_t *bank, const BankInfo &) {
            for (size_t i = 0; i < Stream8822b::kBankSamples; i++) {
              uint64_t w;
              std::memcpy(&w, bank + 8 * i, 8);
              x[i] = cf(static_cast<float>(la_i(w)), static_cast<float>(la_q(w)));
            }
            if (cur.dc_remove)
              dc.process(x.data(), x.size());
            nco.process(x.data(), x.size());
            _ring.push(x.data(), x.size());
            return _active.load();
          },
          &_restart);
      if (!ss.error.empty() && !_restart && _active) {
        SoapySDR::logf(SOAPY_SDR_WARNING, "wsdr: stream fault after %llu banks (%s); restarting",
                       static_cast<unsigned long long>(ss.banks), ss.error.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  std::unique_ptr<Session> _s;
  int _channel = 36;
  mutable std::mutex _setm;
  Settings _set;
  clk::time_point _last_change{};
  std::mutex _streamm;
  std::unique_ptr<WsdrStream> _stream;
  std::atomic<bool> _active{false}, _restart{false};
  std::thread _worker;
  Ring _ring;
  std::vector<cf> _tmp;
};

// Enumerate without opening: only USB descriptors, bus and port numbers.
SoapySDR::KwargsList find_wsdr(const SoapySDR::Kwargs &args) {
  SoapySDR::KwargsList out;
  libusb_context *ctx = nullptr;
  if (libusb_init(&ctx) != 0)
    return out;
  libusb_device **list = nullptr;
  const ssize_t n = libusb_get_device_list(ctx, &list);
  for (ssize_t i = 0; i < n; i++) {
    libusb_device_descriptor d{};
    if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != kVid || d.idProduct != kPid8812bu)
      continue;
    uint8_t ports[8];
    const int np = libusb_get_port_numbers(list[i], ports, sizeof ports);
    std::string port;
    for (int k = 0; k < np; k++)
      port += (k ? "." : "") + std::to_string(ports[k]);
    const std::string bus = std::to_string(libusb_get_bus_number(list[i]));
    const bool ss = libusb_get_device_speed(list[i]) >= LIBUSB_SPEED_SUPER;
    if (args.count("bus") && args.at("bus") != bus)
      continue;
    if (args.count("port") && args.at("port") != port)
      continue;
    SoapySDR::Kwargs k;
    k["driver"] = "wsdr";
    k["label"] = "RTL8812BU (RTL8822B) :: bus " + bus + " port " + port + (ss ? " (USB 3: replug for USB 2)" : "");
    k["bus"] = bus;
    k["port"] = port;
    k["usb"] = ss ? "3" : "2";
    out.push_back(k);
  }
  if (list)
    libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  return out;
}

SoapySDR::Device *make_wsdr(const SoapySDR::Kwargs &args) { return new SoapyWsdr(args); }

const SoapySDR::Registry kRegister("wsdr", &find_wsdr, &make_wsdr, SOAPY_SDR_ABI_VERSION);

} // namespace
