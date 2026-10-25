// SPDX-License-Identifier: GPL-2.0-only
// wsdr_probe — incremental hardware probes for the streaming path (8822B first).
//
//   wsdr_probe ring   --pid 0xb812 [--channel 36] [--rate 2] [--dbg 0x880] [--cck 0|1] [--filter 0|1]
//                     [--dump PREFIX]
//       Arm the LA engine as a free-running ring (never triggered), read the 128 KiB ring twice over
//       EP0 and report how much changed: a live writer changes ~every word between reads; a frozen
//       one (ac54's trap: MAC RX off, or CCK holding the bus) reads back identical.
//   wsdr_probe marker --pid 0xb812 [--channel 36]
//       Load the MIPS16e2 marker program behind the vendor prologue, run it, and read its DMEM block
//       twice: proves upload, execution, DMEM writes and the REG 0x568 microsecond counter.
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "cpu88xx.h"
#include "la88xx.h"
#include "logger.h"
#include "marker8822b.h"
#include "session.h"

using namespace wsdr;
using clk = std::chrono::steady_clock;

namespace {

struct Args {
  std::string cmd;
  OpenOpts open;
  LaRingCfg la;
  std::string dump;
  int reads = 2;
  int count = 1;      // snap: number of snapshots
  int settle_ms = 50; // snap: wait after arm before the first trigger
};

unsigned long num(const char *s) { return std::strtoul(s, nullptr, 0); }

bool parse(int argc, char **argv, Args &a) {
  if (argc < 2)
    return false;
  a.cmd = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string k = argv[i];
    auto v = [&]() -> const char * { return i + 1 < argc ? argv[++i] : "0"; };
    if (k == "--pid") a.open.pid = static_cast<uint16_t>(num(v()));
    else if (k == "--vid") a.open.vid = static_cast<uint16_t>(num(v()));
    else if (k == "--channel") a.open.channel = static_cast<int>(num(v()));
    else if (k == "--no-reset") a.open.reset = false;
    else if (k == "--rate") a.la.rate_sel = static_cast<int>(num(v()));
    else if (k == "--dbg") a.la.dbg_port = static_cast<uint32_t>(num(v()));
    else if (k == "--cck") a.la.cck_off = num(v()) == 0;
    else if (k == "--filter") a.la.filter_rx = num(v()) != 0;
    else if (k == "--dbgclk") a.la.dbgport_clk = num(v()) != 0;
    else if (k == "--dump") a.dump = v();
    else if (k == "--igi") a.la.igi = static_cast<int>(std::strtol(v(), nullptr, 0));
    else if (k == "--ofdm") a.la.ofdm_off = num(v()) == 0;
    else if (k == "--nocca") a.la.no_cca = true;
    else if (k == "--reads") a.reads = static_cast<int>(num(v()));
    else if (k == "--count") a.count = static_cast<int>(num(v()));
    else if (k == "--settle") a.settle_ms = static_cast<int>(num(v()));
    else { std::fprintf(stderr, "unknown arg %s\n", k.c_str()); return false; }
  }
  return a.open.pid != 0;
}

double ms_since(clk::time_point t) {
  return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

int cmd_ring(Session &s, const Args &a) {
  Reg &r = s.reg();
  La8822b la(r);
  la.snapshot();
  int rc = 0;
  try {
    la.configure(a.la);
    la.arm();
    std::printf("{\"ev\":\"ring.cfg\",\"igi_a\":\"0x%02x\",\"igi_b\":\"0x%02x\",\"r808\":\"0x%08x\",\"r95c\":\"0x%08x\"}\n",
                r.r32(0x0C50) & 0x7F, r.r32(0x0E50) & 0x7F, r.r32(0x0808), r.r32(0x095C));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const size_t n = La8822b::kRingBytes / 8;
    std::vector<std::vector<uint64_t>> rd(static_cast<size_t>(a.reads), std::vector<uint64_t>(n));
    for (int k = 0; k < a.reads; k++) {
      const uint32_t c0 = r.r32(0x07C0);
      auto t0 = clk::now();
      r.sram_read(La8822b::kRingOcp, rd[k].data(), La8822b::kRingBytes);
      const double ms = ms_since(t0);
      const uint32_t c1 = r.r32(0x07C0);
      std::printf("{\"ev\":\"ring.read\",\"k\":%d,\"ms\":%.1f,\"mbps\":%.2f,\"r7c0_before\":\"0x%08x\","
                  "\"r7c0_after\":\"0x%08x\"}\n",
                  k, ms, La8822b::kRingBytes / ms / 1e3, c0, c1);
      if (!a.dump.empty()) {
        std::string fn = a.dump + "-read" + std::to_string(k) + ".u64";
        if (FILE *f = std::fopen(fn.c_str(), "wb")) {
          std::fwrite(rd[k].data(), 8, n, f);
          std::fclose(f);
        }
      }
    }
    for (int k = 0; k < a.reads; k++) {
      std::map<uint64_t, unsigned> hi;
      size_t adj = 0, zero = 0;
      for (size_t i = 0; i < n; i++) {
        hi[rd[k][i] >> 24]++;
        if (i && (rd[k][i] & 0xFFFFFF) == (rd[k][i - 1] & 0xFFFFFF)) adj++;
        if ((rd[k][i] & 0xFFFFFF) == 0) zero++;
      }
      uint64_t top = 0;
      unsigned topn = 0;
      for (auto [v, c] : hi)
        if (c > topn) { top = v; topn = c; }
      double si = 0, sq = 0, si2 = 0, sq2 = 0;
      for (size_t i = 0; i < n; i++) {
        const double I = static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(rd[k][i] & 0xFFF) << 20) >> 20);
        const double Q =
            static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>((rd[k][i] >> 12) & 0xFFF) << 20) >> 20);
        si += I; sq += Q; si2 += I * I; sq2 += Q * Q;
      }
      const double rms = std::sqrt((si2 - si * si / n + sq2 - sq * sq / n) / n);
      std::printf("{\"ev\":\"ring.stats\",\"k\":%d,\"rms\":%.1f,\"dc_i\":%.1f,\"dc_q\":%.1f,\"adj_dup\":%.4f,"
                  "\"zero\":%.4f,\"hi_distinct\":%zu,\"hi_top\":\"0x%010" PRIx64 "\",\"hi_top_frac\":%.4f}\n",
                  k, rms, si / n, sq / n, double(adj) / n, double(zero) / n, hi.size(), top, double(topn) / n);
    }
    for (int k = 1; k < a.reads; k++) {
      size_t same = 0;
      for (size_t i = 0; i < n; i++)
        same += (rd[k][i] & 0xFFFFFF) == (rd[k - 1][i] & 0xFFFFFF);
      std::printf("{\"ev\":\"ring.fresh\",\"pair\":\"%d-%d\",\"identical\":%.4f,\"verdict\":\"%s\"}\n", k - 1, k,
                  double(same) / n, double(same) / n >= 0.95 ? "FROZEN" : "LIVE");
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "ring: %s\n", e.what());
    rc = 1;
  }
  la.restore();
  std::printf("{\"ev\":\"ring.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
  return rc;
}

// Contiguous snapshot: run the ring free, fire the manual trigger (0x7c0[5] edge; the post-trigger
// time 0x7c0[14:8] is 0), wait for the engine to stop, read the ring and unroll it oldest-first
// from the finish address (devourer LaCapture::readback semantics). Output: DVLA v1 files.
int cmd_snap(Session &s, const Args &a) {
  Reg &r = s.reg();
  La8822b la(r);
  la.snapshot();
  int rc = 0;
  try {
    la.configure(a.la);
    const size_t n = La8822b::kRingBytes / 8;
    std::vector<uint64_t> ring(n), out(n);
    for (int k = 0; k < a.count; k++) {
      la.arm();
      std::this_thread::sleep_for(std::chrono::milliseconds(k ? 5 : a.settle_ms));
      r.w8(0x07C0, 0xCB | 0x20); // manual trigger edge after arm (devourer: ordering matters on 8822B)
      bool done = false;
      for (int i = 0; i < 200 && !done; i++)
        done = !(r.r8(0x07C0) & 0x02);
      const uint32_t v = r.r32(0x07C0);
      auto t0 = clk::now();
      r.sram_read(La8822b::kRingOcp, ring.data(), La8822b::kRingBytes);
      const double ms = ms_since(t0);
      la.stop();
      const uint32_t fin = (v & 0x7FFF0000u) >> 16; // 8-byte units, TXFF address
      const bool wrap = (v >> 31) & 1;
      size_t start = 0, cnt = n;
      if (wrap)
        start = (((fin + 2) << 3) - 0x20000u) / 8 % n;
      else
        cnt = fin > (0x20000u >> 3) ? fin - (0x20000u >> 3) : 0;
      for (size_t i = 0; i < cnt; i++)
        out[i] = ring[(start + i) % n];
      std::printf("{\"ev\":\"snap\",\"k\":%d,\"done\":%d,\"r7c0\":\"0x%08x\",\"wrap\":%d,\"samples\":%zu,\"read_ms\":%.1f}\n",
                  k, done ? 1 : 0, v, wrap ? 1 : 0, cnt, ms);
      if (!a.dump.empty() && cnt) {
        uint8_t hdr[32] = {'D', 'V', 'L', 'A', 1, 1, 3, static_cast<uint8_t>(a.la.rate_sel),
                           static_cast<uint8_t>(a.la.dma_type), static_cast<uint8_t>(wrap)};
        const uint32_t n32 = static_cast<uint32_t>(cnt);
        std::memcpy(hdr + 12, &n32, 4);
        std::memcpy(hdr + 16, &fin, 4);
        std::string fn = a.dump + "-snap" + std::to_string(k) + ".bin";
        if (FILE *f = std::fopen(fn.c_str(), "wb")) {
          std::fwrite(hdr, 1, 32, f);
          std::fwrite(out.data(), 8, cnt, f);
          std::fclose(f);
        }
      }
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "snap: %s\n", e.what());
    rc = 1;
  }
  la.restore();
  std::printf("{\"ev\":\"snap.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
  return rc;
}

int cmd_marker(Session &s, const Args &) {
  Reg &r = s.reg();
  Cpu8822b cpu(r);
  int rc = 0;
  try {
    const uint32_t r80 = r.r16(0x0080);
    cpu.load(marker8822b_bin, marker8822b_bin_len);
    std::printf("{\"ev\":\"cpu.loaded\",\"bytes\":%zu,\"prog\":%u,\"r80_before\":\"0x%04x\"}\n",
                cpu.loaded_bytes(), marker8822b_bin_len, r80);
    const uint32_t zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    cpu.dmem_write(0x1000, zero, sizeof zero);
    cpu.start();
    uint32_t a[8], b[8];
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cpu.dmem_read(0x1000, a, sizeof a);
    auto t0 = clk::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cpu.dmem_read(0x1000, b, sizeof b);
    const double wall_us = ms_since(t0) * 1e3;
    const bool ok = a[0] == 0xCAFEF00Du && b[1] > a[1] && b[2] != a[2];
    std::printf("{\"ev\":\"cpu.marker\",\"ok\":%d,\"magic\":\"0x%08x\",\"count_a\":%u,\"count_b\":%u,"
                "\"loops_per_s\":%.0f,\"timer_a\":%u,\"timer_b\":%u,\"timer_delta\":%u,\"wall_us\":%.0f,"
                "\"r80\":\"0x%04x\",\"lui_ori\":\"0x%08x\",\"lui_xori\":\"0x%08x\",\"lui_andi\":\"0x%08x\"}\n",
                ok, a[0], a[1], b[1], (b[1] - a[1]) / (wall_us / 1e6), a[2], b[2], b[2] - a[2], wall_us,
                r.r16(0x0080), b[4], b[5], b[6]);
    rc = ok ? 0 : 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "marker: %s\n", e.what());
    rc = 1;
  }
  try {
    cpu.halt();
  } catch (...) {
  }
  std::printf("{\"ev\":\"marker.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
  return rc;
}

} // namespace

int main(int argc, char **argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: wsdr_probe ring|marker --pid 0xNNNN [options] (see source header)\n");
    return 2;
  }
  auto logger = std::make_shared<Logger>();
  Session s(logger);
  try {
    s.open(a.open);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "open/bring-up failed: %s\n", e.what());
    return 3;
  }
  std::printf("{\"ev\":\"session\",\"chip\":\"%s\",\"superspeed\":%d,\"channel\":%d}\n", s.chip().c_str(),
              s.superspeed() ? 1 : 0, a.open.channel);
  if (a.cmd == "ring")
    return cmd_ring(s, a);
  if (a.cmd == "snap")
    return cmd_snap(s, a);
  if (a.cmd == "marker")
    return cmd_marker(s, a);
  std::fprintf(stderr, "unknown command %s\n", a.cmd.c_str());
  return 2;
}
