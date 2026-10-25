// SPDX-License-Identifier: GPL-2.0-only
// wsdr_tcp — rtl_tcp-compatible IQ server on the 8822B continuous LA stream (USB 2).
//
//   sudo wsdr_tcp --pid 0xb812 [--bind 127.0.0.1] [--port 1234] [--freq 5180e6] [--rate 2.4e6]
//                 [--gain 30] [--digital-gain-db 0] [--freq-offset 0] [--keep-root]
//
// Clients (SDR++, gqrx, anything that speaks rtl_tcp) see an "R820T" and its gain table. Mapping:
//   frequency    rtl_tcp sends it as a u32, so 5 GHz arrives wrapped mod 2^32 (5180 MHz -> 885.03 MHz);
//                values that only make sense unwrapped (0.78..1.97 GHz -> 5.08..6.27 GHz) are unwrapped.
//                --freq-offset HZ is added first (for clients set up with an LNB/converter offset).
//                Then snapped to the 5 MHz Wi-Fi synthesizer grid (2.4 GHz ch1..14, 5 GHz ch16..233);
//                the residual (<= 2.5 MHz) is shifted out digitally, from the 5 MS/s stream when the
//                2.5 MS/s one is too narrow for it
//   sample rate  any; resampled from the 2.5 MS/s (or 5 MS/s) stream (flat to ~80 % of Nyquist)
//   gain         manual: IGI in fixed-gain mode (OFDM CCA off), ~0.95 dB per step;
//                auto: the chip's own AGC (CCA on; gain steps during frames)
// Every setting change restarts the stream (a gap of tens of ms), so the chip is never touched while
// it streams. One client at a time. Output: u8 I,Q = value * scale + 127.5, DC removed.
// After the adapter is open the server drops root (to SUDO_UID/SUDO_GID) unless --keep-root.
#include <arpa/inet.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
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

// R820T gain table in tenths of a dB, as rtl_tcp clients expect for tuner type 5.
constexpr int kR820t[] = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                          280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
constexpr int kGains = static_cast<int>(sizeof kR820t / sizeof kR820t[0]);

std::atomic<bool> g_quit{false};

struct Settings {
  double freq = 5180e6;
  double rate = 2.4e6;
  bool manual = true;
  int gain = 300; // tenths of a dB
};

struct Plan {
  int channel = 36;
  double f_ch = 5180e6;
  double shift = 0;    // requested - channel centre
  int rate_sel = 5;
  double src = 2.5e6;
  bool fits = true;    // requested span lies inside the flat captured band
  int igi = -1;
  bool no_cca = false;
};

// rtl_tcp frequency word -> Hz. 5 GHz settings arrive wrapped by 2^32; nothing this radio tunes lies
// in 0.78..1.97 GHz, so those values are unambiguous.
double unwrap_freq(uint32_t v, double offset) {
  double f = static_cast<double>(v) + offset;
  if (f >= 0.78e9 && f < 1.98e9)
    f += 4294967296.0;
  return f;
}

int igi_for(int tenths) { return std::clamp(0x50 - static_cast<int>(std::lround(tenths / 9.5)), 0x1C, 0x50); }

Plan make_plan(const Settings &s) {
  Plan p;
  const double mhz = s.freq / 1e6;
  if (mhz < 3700) {
    const int g = std::clamp(static_cast<int>(std::lround((mhz - 2407) / 5)), 1, 13);
    p.channel = std::abs(mhz - 2484) < std::abs(mhz - devourer::chan_to_freq(g)) ? 14 : g;
  } else {
    p.channel = std::clamp(static_cast<int>(std::lround((mhz - 5000) / 5)), 16, 233);
  }
  p.f_ch = devourer::chan_to_freq(p.channel) * 1e6;
  p.shift = s.freq - p.f_ch;
  // Usable band of a stream at src: +-0.45 src (resampler-flat); the client needs +-0.4 rate.
  p.fits = false;
  for (int rs : {5, 4}) {
    p.rate_sel = rs;
    p.src = 80e6 / (1 << rs);
    if (std::abs(p.shift) + 0.4 * s.rate <= 0.45 * p.src) {
      p.fits = true;
      break;
    }
  }
  if (s.manual) {
    p.igi = igi_for(s.gain);
    p.no_cca = true;
  }
  return p;
}

// Bounded FIFO of u8 IQ blocks between the stream callback and the socket sender. A full queue
// drops the newest block (the client is too slow), never blocks the stream.
class ByteQueue {
public:
  explicit ByteQueue(size_t cap) : _cap(cap) {}
  bool push(std::vector<uint8_t> &&b) {
    std::lock_guard<std::mutex> l(_m);
    if (_closed || _bytes + b.size() > _cap)
      return false;
    _bytes += b.size();
    _q.push_back(std::move(b));
    _cv.notify_one();
    return true;
  }
  bool pop(std::vector<uint8_t> &b) {
    std::unique_lock<std::mutex> l(_m);
    _cv.wait(l, [&] { return _closed || !_q.empty(); });
    if (_q.empty())
      return false;
    b = std::move(_q.front());
    _q.pop_front();
    _bytes -= b.size();
    return true;
  }
  void close() {
    std::lock_guard<std::mutex> l(_m);
    _closed = true;
    _cv.notify_all();
  }
  size_t bytes() {
    std::lock_guard<std::mutex> l(_m);
    return _bytes;
  }

private:
  std::mutex _m;
  std::condition_variable _cv;
  std::deque<std::vector<uint8_t>> _q;
  size_t _bytes = 0, _cap;
  bool _closed = false;
};

bool send_all(int fd, const uint8_t *p, size_t n) {
  while (n) {
    const ssize_t k = ::send(fd, p, n, MSG_NOSIGNAL);
    if (k <= 0)
      return false;
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool recv_all(int fd, uint8_t *p, size_t n) {
  while (n) {
    const ssize_t k = ::recv(fd, p, n, 0);
    if (k <= 0)
      return false;
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

struct Args {
  OpenOpts open;
  std::string bind = "127.0.0.1";
  int port = 1234;
  Settings init;
  double digital_gain_db = 0;
  double freq_offset = 0;
  bool keep_root = false;
};

bool parse(int argc, char **argv, Args &a) {
  for (int i = 1; i < argc; i++) {
    std::string k = argv[i];
    auto v = [&]() -> const char * { return i + 1 < argc ? argv[++i] : "0"; };
    if (k == "--pid") a.open.pid = static_cast<uint16_t>(std::strtoul(v(), nullptr, 0));
    else if (k == "--bind") a.bind = v();
    else if (k == "--port") a.port = std::atoi(v());
    else if (k == "--freq") a.init.freq = std::strtod(v(), nullptr);
    else if (k == "--rate") a.init.rate = std::strtod(v(), nullptr);
    else if (k == "--gain") a.init.gain = static_cast<int>(std::lround(std::strtod(v(), nullptr) * 10));
    else if (k == "--digital-gain-db") a.digital_gain_db = std::strtod(v(), nullptr);
    else if (k == "--freq-offset") a.freq_offset = std::strtod(v(), nullptr);
    else if (k == "--keep-root") a.keep_root = true;
    else { std::fprintf(stderr, "unknown arg %s\n", k.c_str()); return false; }
  }
  return a.open.pid != 0;
}

void drop_root() {
  const char *u = std::getenv("SUDO_UID"), *g = std::getenv("SUDO_GID");
  if (getuid() != 0 || !u || !g)
    return;
  const uid_t uid = static_cast<uid_t>(std::strtoul(u, nullptr, 10));
  const gid_t gid = static_cast<gid_t>(std::strtoul(g, nullptr, 10));
  if (setgroups(0, nullptr) || setgid(gid) || setuid(uid)) {
    std::perror("drop root");
    std::exit(1);
  }
  std::printf("{\"ev\":\"tcp.privs\",\"uid\":%u,\"gid\":%u}\n", uid, gid);
}

// One connected client: header, command reader, sender, and the stream loop on this thread.
void serve(Session &s, int fd, const Args &a, int &cur_channel) {
  uint8_t hdr[12] = {'R', 'T', 'L', '0', 0, 0, 0, 5, 0, 0, 0, static_cast<uint8_t>(kGains)};
  if (!send_all(fd, hdr, sizeof hdr))
    return;
  std::mutex mset;
  Settings set = a.init;
  std::atomic<bool> alive{true}, restart{false};
  std::atomic<int64_t> last_cmd{0};
  ByteQueue q(8u << 20);
  auto now_ns = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count(); };

  std::thread reader([&] {
    uint8_t c[5];
    while (recv_all(fd, c, 5)) {
      const uint32_t v = (uint32_t(c[1]) << 24) | (uint32_t(c[2]) << 16) | (uint32_t(c[3]) << 8) | c[4];
      bool changed = true;
      {
        std::lock_guard<std::mutex> l(mset);
        switch (c[0]) {
        case 0x01: set.freq = unwrap_freq(v, a.freq_offset); break;
        case 0x02: set.rate = v; break;
        case 0x03: set.manual = v != 0; break;
        case 0x04: set.gain = static_cast<int>(static_cast<int32_t>(v)); break;
        case 0x0d: set.gain = kR820t[std::min<uint32_t>(v, kGains - 1)]; break;
        default: changed = false; break;
        }
      }
      std::printf("{\"ev\":\"tcp.cmd\",\"cmd\":%u,\"param\":%u,\"applied\":%d}\n", c[0], v, changed ? 1 : 0);
      std::fflush(stdout);
      if (changed) {
        last_cmd = now_ns();
        restart = true;
      }
    }
    alive = false;
    restart = true;
    q.close();
  });
  std::thread sender([&] {
    std::vector<uint8_t> b;
    while (q.pop(b))
      if (!send_all(fd, b.data(), b.size())) {
        alive = false;
        restart = true;
        break;
      }
  });

  const float base = static_cast<float>(std::pow(10.0, a.digital_gain_db / 20) / 8); // 12-bit -> u8
  uint64_t drops = 0, faults = 0, blocks = 0, clips = 0, comps = 0;
  while (alive && !g_quit) {
    while (now_ns() - last_cmd < 40'000'000 && alive)
      std::this_thread::sleep_for(std::chrono::milliseconds(10)); // let a burst of commands settle
    restart = false;
    Settings cur;
    {
      std::lock_guard<std::mutex> l(mset);
      cur = set;
    }
    const Plan p = make_plan(cur);
    if (p.channel != cur_channel) {
      s.tune(p.channel);
      cur_channel = p.channel;
    }
    DcBlock dc;
    Nco nco;
    nco.set(p.shift, p.src);
    Resampler rs;
    rs.configure(p.src, cur.rate);
    std::printf("{\"ev\":\"tcp.config\",\"freq\":%.0f,\"channel\":%d,\"f_ch\":%.0f,\"shift\":%.0f,\"src\":%.0f,"
                "\"rate\":%.0f,\"fits\":%d,\"manual\":%d,\"gain_db\":%.1f,\"igi\":\"0x%02x\"}\n",
                cur.freq, p.channel, p.f_ch, p.shift, p.src, cur.rate, p.fits ? 1 : 0, cur.manual ? 1 : 0,
                cur.gain / 10.0, p.igi < 0 ? 0 : p.igi);
    if (!p.fits)
      std::printf("{\"ev\":\"tcp.warn\",\"msg\":\"requested span exceeds the captured band; edges alias\"}\n");
    std::fflush(stdout);

    StreamCfg c;
    c.la.rate_sel = p.rate_sel;
    c.la.igi = p.igi;
    c.la.no_cca = p.no_cca;
    Stream8822b st(s, c);
    std::vector<cf> x(Stream8822b::kBankSamples), y;
    auto tick = clk::now();
    const StreamStats ss = st.run(
        [&](const uint8_t *bank, const BankInfo &) {
          for (size_t i = 0; i < Stream8822b::kBankSamples; i++) {
            uint64_t w;
            std::memcpy(&w, bank + 8 * i, 8);
            x[i] = cf(static_cast<float>(la_i(w)), static_cast<float>(la_q(w)));
          }
          dc.process(x.data(), x.size());
          nco.process(x.data(), x.size());
          y.clear();
          rs.process(x.data(), x.size(), y);
          std::vector<uint8_t> b(2 * y.size());
          clips += to_u8(y.data(), y.size(), base, b.data());
          comps += b.size();
          blocks++;
          if (!q.push(std::move(b)) && alive)
            drops++;
          if (clk::now() - tick > std::chrono::seconds(5)) {
            tick = clk::now();
            std::printf("{\"ev\":\"tcp.stats\",\"blocks\":%llu,\"drops\":%llu,\"faults\":%llu,\"queue_kb\":%zu,"
                        "\"clip\":%.5f}\n",
                        static_cast<unsigned long long>(blocks), static_cast<unsigned long long>(drops),
                        static_cast<unsigned long long>(faults), q.bytes() / 1024,
                        comps ? static_cast<double>(clips) / comps : 0.0);
            std::fflush(stdout);
          }
          return alive && !g_quit;
        },
        &restart);
    if (!ss.error.empty()) {
      faults++;
      std::printf("{\"ev\":\"tcp.fault\",\"error\":\"%s\",\"banks\":%llu}\n", ss.error.c_str(),
                  static_cast<unsigned long long>(ss.banks));
      std::fflush(stdout);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  alive = false;
  q.close();
  ::shutdown(fd, SHUT_RDWR);
  reader.join();
  sender.join();
  std::printf("{\"ev\":\"tcp.disconnect\",\"blocks\":%llu,\"drops\":%llu,\"faults\":%llu}\n",
              static_cast<unsigned long long>(blocks), static_cast<unsigned long long>(drops),
              static_cast<unsigned long long>(faults));
  std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: wsdr_tcp --pid 0xNNNN [--bind ADDR] [--port N] [--freq HZ] [--rate SPS] "
                         "[--gain DB] [--digital-gain-db DB] [--freq-offset HZ] [--keep-root] (see source header)\n");
    return 2;
  }
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, [](int) { g_quit = true; });
  std::signal(SIGTERM, [](int) { g_quit = true; });

  const Plan p0 = make_plan(a.init);
  a.open.channel = p0.channel;
  a.open.thermal_track = false; // no host register traffic while streaming
  auto logger = std::make_shared<Logger>();
  Session s(logger);
  try {
    s.open(a.open);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "open/bring-up failed: %s\n", e.what());
    return 3;
  }
  if (s.superspeed())
    std::fprintf(stderr, "warning: adapter is on USB 3, where 16 KiB C2H wedges; replug for USB 2\n");
  int cur_channel = p0.channel;

  const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  const int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<uint16_t>(a.port));
  if (inet_pton(AF_INET, a.bind.c_str(), &sa.sin_addr) != 1 ||
      ::bind(ls, reinterpret_cast<sockaddr *>(&sa), sizeof sa) || ::listen(ls, 1)) {
    std::perror("listen");
    return 4;
  }
  if (!a.keep_root)
    drop_root();
  std::printf("{\"ev\":\"tcp.listen\",\"bind\":\"%s\",\"port\":%d,\"chip\":\"%s\",\"superspeed\":%d}\n",
              a.bind.c_str(), a.port, s.chip().c_str(), s.superspeed() ? 1 : 0);
  std::fflush(stdout);

  while (!g_quit) {
    pollfd pf{ls, POLLIN, 0};
    if (::poll(&pf, 1, 200) <= 0)
      continue;
    sockaddr_in ca{};
    socklen_t cl = sizeof ca;
    const int fd = ::accept(ls, reinterpret_cast<sockaddr *>(&ca), &cl);
    if (fd < 0)
      continue;
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof ip);
    std::printf("{\"ev\":\"tcp.connect\",\"peer\":\"%s:%u\"}\n", ip, ntohs(ca.sin_port));
    std::fflush(stdout);
    serve(s, fd, a, cur_channel);
    ::close(fd);
  }
  ::close(ls);
  std::printf("{\"ev\":\"tcp.exit\",\"alive\":%d}\n", s.reg().alive() ? 1 : 0);
  return 0;
}
