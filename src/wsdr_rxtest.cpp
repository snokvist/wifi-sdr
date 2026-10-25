// SPDX-License-Identifier: GPL-2.0-only
// wsdr_rxtest — hardware check of wsdr::Receiver (the app-facing API):
//   wsdr_rxtest --pid 0xb812 [--freq 5190e6] [--rate 1] [--seconds 10] [--retune 5230e6]
// Opens the adapter, streams, optionally retunes half-way, and prints status JSON once a second plus a
// summary: banks, restarts, settings generations seen, sink time per bank (must stay tiny: it runs on
// the libusb event thread).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "receiver.h"

using namespace wsdr;
using clk = std::chrono::steady_clock;

int main(int argc, char **argv) {
  uint16_t pid = 0;
  RxSettings set;
  set.rate_sel = 5;
  double seconds = 10, retune = 0;
  int bus = -1;
  std::string port;
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : "0";
    if (k == "--pid") pid = static_cast<uint16_t>(std::strtoul(v, nullptr, 0)), i++;
    else if (k == "--freq") set.freq = std::strtod(v, nullptr), i++;
    else if (k == "--rate") set.rate_sel = std::atoi(v), i++;
    else if (k == "--seconds") seconds = std::strtod(v, nullptr), i++;
    else if (k == "--retune") retune = std::strtod(v, nullptr), i++;
    else if (k == "--agc") set.agc = true;
    else if (k == "--bus") bus = std::atoi(v), i++;
    else if (k == "--port") port = v, i++;
    else { std::fprintf(stderr, "unknown arg %s\n", k.c_str()); return 2; }
  }
  if (!pid) {
    std::fprintf(stderr, "usage: wsdr_rxtest --pid 0xb812|0xc812 [--freq HZ] [--rate 1..5] [--seconds N] [--retune HZ]\n");
    return 2;
  }
  Receiver rx(std::make_shared<Logger>());
  try {
    rx.open(pid, set, bus, port);
  } catch (const OpenError &e) {
    std::printf("{\"ev\":\"open\",\"ok\":0,\"kind\":%d,\"error\":\"%s\"}\n", static_cast<int>(e.kind), e.what());
    return 3;
  } catch (const std::exception &e) {
    std::printf("{\"ev\":\"open\",\"ok\":0,\"error\":\"%s\"}\n", e.what());
    return 3;
  }
  const RxCaps c = rx.caps();
  std::printf("{\"ev\":\"open\",\"ok\":1,\"chip\":\"%s\",\"cu\":%d,\"superspeed\":%d,\"min_rate_sel\":%d,\"paths\":%d,"
              "\"gain_max\":%.1f}\n", c.chip.c_str(), c.cu, c.superspeed, c.min_rate_sel, c.paths, c.gain_max);
  std::atomic<uint64_t> banks{0}, gens{0}, sink_ns{0}, sink_max_ns{0};
  std::atomic<uint32_t> last_gen{0};
  static uint8_t copy[65536];
  rx.start([&](const uint8_t *bank, size_t n, const BankMeta &m) {
    const auto t = clk::now();
    std::memcpy(copy, bank, n); // what a real consumer does: copy, nothing else
    if (m.gen != last_gen.exchange(m.gen))
      gens++;
    banks++;
    const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t).count();
    sink_ns += ns;
    if (ns > sink_max_ns)
      sink_max_ns = ns;
  });
  const auto t0 = clk::now();
  bool retuned = false;
  while (std::chrono::duration<double>(clk::now() - t0).count() < seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (retune > 0 && !retuned && std::chrono::duration<double>(clk::now() - t0).count() >= seconds / 2) {
      RxSettings s = rx.settings();
      s.freq = retune;
      rx.set(s);
      retuned = true;
    }
    const RxStatus st = rx.status();
    std::printf("{\"ev\":\"status\",\"state\":\"%s\",\"banks\":%llu,\"restarts\":%llu,\"channel\":%d,\"width\":%d,"
                "\"centre\":%.0f,\"fs\":%.0f,\"error\":\"%s\"}\n", rx_state_name(st.state),
                static_cast<unsigned long long>(st.banks), static_cast<unsigned long long>(st.restarts), st.channel,
                st.width, st.centre_hz, st.fs, st.error.c_str());
    std::fflush(stdout);
    if (st.state == RxState::Lost || st.state == RxState::Error)
      break;
  }
  const double el = std::chrono::duration<double>(clk::now() - t0).count();
  rx.stop();
  const RxStatus st = rx.status();
  std::printf("{\"ev\":\"rxtest\",\"banks\":%llu,\"msps\":%.2f,\"gens\":%llu,\"restarts\":%llu,\"sink_mean_us\":%.2f,"
              "\"sink_max_us\":%.1f,\"state\":\"%s\",\"error\":\"%s\"}\n",
              static_cast<unsigned long long>(banks.load()), banks * 8192.0 / el / 1e6,
              static_cast<unsigned long long>(gens.load()), static_cast<unsigned long long>(st.restarts),
              banks ? sink_ns / 1e3 / banks : 0.0, sink_max_ns / 1e3, rx_state_name(st.state), st.error.c_str());
  rx.close();
  return st.state == RxState::Lost || st.state == RxState::Error ? 1 : 0;
}
