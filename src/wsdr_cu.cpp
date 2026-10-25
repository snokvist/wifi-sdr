// SPDX-License-Identifier: GPL-2.0-only
// wsdr_cu — custom on-chip code probes for the RTL8812CU (RTL8822C). See notes/cu-custom-code.md.
//
//   wsdr_cu marker   --pid 0xc812 [--channel 36]
//       Load marker8822c.S behind the vendor IMEM entry stub, run it, read its DMEM block twice:
//       proves upload, execution, DMEM writes, the REG 0x568 microsecond counter and the ISA subset.
//   wsdr_cu c2h      --pid 0xc812 [--bytes 16352] [--count 64] [--depth 16] [--rx-off 0x1000]
//       Host-issued C2H (no CPU code): stage one PRBS packet in RX SRAM, write REG 0x294, verify the
//       bulk-IN completion.
//   wsdr_cu c2hbench --pid 0xc812 [--bytes 16352] [--count 1024] [--depth 16]
//       Run c2hbench8822c.S: per-request C2H latency from REG 0x568, read back from DMEM.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "c2h88xx.h"
#include "c2hbench8822c.h"
#include "cpu8822c.h"
#include "logger.h"
#include "marker8822c.h"
#include "session.h"

using namespace wsdr;
using clk = std::chrono::steady_clock;

namespace {

constexpr uint32_t kBlock = 0x8000; // DMEM offset of the programs' state block (CPU 0x80208000)

struct Args {
  bool no_halt = false; // c2h: leave the vendor CPU running
  bool rx_on = false;   // c2h: MAC RX on with frames filtered (streamer RX-path state)
  std::vector<std::pair<uint16_t, uint32_t>> sets; // c2h: --set ADDR=VAL (8-bit if ADDR=0x290, else 32-bit) after setup
  std::string source = "rx"; // c2h: rx | tx0 (TXFF 0x20000 -> C2H 0x16000) | tx1 (TXFF 0x30000 -> 0x6000)
  bool pipelined = false; // c2h: wait for packet i-1 after request i (8822C one-packet hold)
  int burst = 0; // c2h: fire this many requests back to back, then count what arrives
  bool agg = false; // c2h: keep RX-DMA aggregation on (0x10c[2]); Jaguar3 RX DMA only flushes to bulk-IN with it
  bool accept_all = false; // c2h: MAC RX on and accept every frame (does the reader see anything?)
  bool fwff_off = false; // c2h: clear 0x601 BIT_FWEN (firmware-packet FIFO) before requesting
  size_t cap = 4; // Reg block-transfer cap: the 8822C hangs on 4 KiB window reads (memtest)
  std::string cmd;
  OpenOpts open;
  int bytes = 16352;
  int count = 0;
  int depth = 16;
  int rx_off = 0x1000;
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
    else if (k == "--no-halt") a.no_halt = true;
    else if (k == "--rx-on") a.rx_on = true;
    else if (k == "--fwff-off") a.fwff_off = true;
    else if (k == "--accept-all") a.accept_all = true;
    else if (k == "--agg") a.agg = true;
    else if (k == "--pipelined") a.pipelined = true;
    else if (k == "--source") a.source = v();
    else if (k == "--set") {
      std::string sv = v();
      const size_t eq = sv.find('=');
      if (eq == std::string::npos) { std::fprintf(stderr, "--set needs ADDR=VAL\n"); return false; }
      a.sets.push_back({static_cast<uint16_t>(num(sv.substr(0, eq).c_str())),
                        static_cast<uint32_t>(num(sv.substr(eq + 1).c_str()))});
    }
    else if (k == "--burst") a.burst = static_cast<int>(num(v()));
    else if (k == "--cap") a.cap = static_cast<size_t>(std::strtoul(v(), nullptr, 0));
    else if (k == "--channel") a.open.channel = static_cast<int>(num(v()));
    else if (k == "--bytes") a.bytes = static_cast<int>(num(v()));
    else if (k == "--count") a.count = static_cast<int>(num(v()));
    else if (k == "--depth") a.depth = static_cast<int>(num(v()));
    else if (k == "--rx-off") a.rx_off = static_cast<int>(num(v()));
    else { std::fprintf(stderr, "unknown arg %s\n", k.c_str()); return false; }
  }
  return a.open.pid != 0;
}

double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

uint32_t pctl(const std::vector<uint32_t> &v, unsigned p) { return v.empty() ? 0 : v[(v.size() - 1) * p / 100]; }

// Hop-by-hop memory test with the vendor CPU halted: which leg of the stage -> DDMA -> IMEM/DMEM
// path works on the 8822C. Each hop writes a fresh pattern and compares what comes back.
int cmd_memtest(Session &s, const Args &) {
  Reg &r = s.reg();
  Cpu8822c cpu(r);
  int rc = 0;
  auto pattern = [](uint32_t seed, size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (auto &b : v) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      b = static_cast<uint8_t>(x);
    }
    return v;
  };
  auto report = [&](const char *hop, const std::vector<uint8_t> &want, const std::vector<uint8_t> &got,
                    const char *err) {
    size_t bad = 0, first = SIZE_MAX;
    for (size_t i = 0; i < want.size() && i < got.size(); i++)
      if (want[i] != got[i]) {
        bad++;
        if (first == SIZE_MAX)
          first = i;
      }
    char sample[64] = "";
    if (first != SIZE_MAX && first + 8 <= got.size())
      std::snprintf(sample, sizeof sample, "want %02x%02x%02x%02x got %02x%02x%02x%02x", want[first],
                    want[first + 1], want[first + 2], want[first + 3], got[first], got[first + 1],
                    got[first + 2], got[first + 3]);
    std::printf("{\"ev\":\"memtest\",\"hop\":\"%s\",\"bytes\":%zu,\"bad\":%zu,\"first_bad\":%lld,"
                "\"sample\":\"%s\",\"error\":\"%s\"}\n",
                hop, want.size(), bad, first == SIZE_MAX ? -1LL : static_cast<long long>(first), sample, err);
    std::fflush(stdout);
    if (bad || *err)
      rc = 1;
  };
  constexpr size_t n = 4096;
  const uint32_t tx_a = 0x18781000, tx_b = 0x18788000;
  try {
    cpu.halt();
    r.w32(0x1080, r.r32(0x1080) | 0x100u); // DDMA_EN
    // H1: debug-window write + read of the TX buffer.
    {
      auto w = pattern(1, n);
      std::vector<uint8_t> g(n);
      r.sram_write(tx_a, w.data(), n);
      r.sram_read(tx_a, g.data(), n);
      report("H1 txbuf window write/read", w, g, "");
    }
    // H1r: the same through the window, but in the RX packet buffer (where c2h stages its packet).
    {
      auto w = pattern(11, n);
      std::vector<uint8_t> g(n);
      r.sram_write(0x18701000, w.data(), n);
      r.sram_read(0x18701000, g.data(), n);
      report("H1r rxbuf window write/read", w, g, "");
    }
    // H2: DDMA TX -> TX.
    {
      auto w = pattern(2, n);
      std::vector<uint8_t> g(n);
      r.sram_write(tx_a, w.data(), n);
      std::string e;
      try { r.ddma(tx_a, tx_b, n); } catch (const std::exception &x) { e = x.what(); }
      r.sram_read(tx_b, g.data(), n);
      report("H2 ddma txbuf->txbuf", w, g, e.c_str());
    }
    // H3/H4/H5: DDMA to a memory and back (DMEM scratch, IMEM entry, IMEM +0x1000).
    struct M { const char *name; uint32_t ocp; } mems[] = {
        {"H3 ddma txbuf->DMEM+0x8000->txbuf", 0x208000},
        {"H4 ddma txbuf->IMEM+0x0->txbuf", 0x030000},
        {"H5 ddma txbuf->IMEM+0x1000->txbuf", 0x031000},
    };
    uint32_t seed = 3;
    for (auto &m : mems) {
      auto w = pattern(seed++, n);
      std::vector<uint8_t> g(n);
      r.sram_write(tx_a, w.data(), n);
      std::string e;
      try {
        r.ddma(tx_a, m.ocp, n);
        r.sram_write(tx_b, std::vector<uint8_t>(n, 0xEE).data(), n);
        r.ddma(m.ocp, tx_b, n);
      } catch (const std::exception &x) { e = x.what(); }
      r.sram_read(tx_b, g.data(), n);
      report(m.name, w, g, e.c_str());
    }
    // H6: DDMA into IMEM with the hardware checksum (devourer's verify path), no read-back.
    {
      auto w = pattern(9, n);
      r.sram_write(tx_a, w.data(), n);
      r.w32(0x1208, 1u << 25); // RESET_CHKSUM_STS
      uint32_t ctrl = (1u << 29) | (1u << 31) | static_cast<uint32_t>(n);
      r.w32(0x1200, tx_a);
      r.w32(0x1204, 0x030000);
      r.w32(0x1208, ctrl);
      int k = 0;
      while ((r.r32(0x1208) & (1u << 31)) && k++ < 1000) {}
      std::printf("{\"ev\":\"memtest\",\"hop\":\"H6 ddma txbuf->IMEM chksum\",\"ch0ctrl\":\"0x%08x\","
                  "\"own_cleared\":%d,\"chksum_sts_bit27\":%u}\n",
                  r.r32(0x1208), k < 1000 ? 1 : 0, (r.r32(0x1208) >> 27) & 1);
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "memtest: %s\n", e.what());
    rc = 1;
  }
  std::printf("{\"ev\":\"memtest.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
  return rc;
}

int cmd_marker(Session &s, const Args &) {
  Reg &r = s.reg();
  Cpu8822c cpu(r);
  int rc = 1;
  try {
    const uint32_t r80 = r.r16(0x0080);
    cpu.load(marker8822c_bin, marker8822c_bin_len);
    std::printf("{\"ev\":\"cpu.loaded\",\"bytes\":%zu,\"prog\":%u,\"r80_before\":\"0x%04x\"}\n", cpu.loaded_bytes(),
                marker8822c_bin_len, r80);
    const uint32_t zero[12] = {};
    cpu.dmem_write(kBlock, zero, sizeof zero);
    cpu.start();
    uint32_t a[12], b[12];
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cpu.dmem_read(kBlock, a, sizeof a);
    const auto t0 = clk::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cpu.dmem_read(kBlock, b, sizeof b);
    const double wall_us = ms_since(t0) * 1e3;
    const bool ok = a[0] == 0xCAFE0C22u && b[1] > a[1] && b[2] != a[2];
    std::printf("{\"ev\":\"cpu.marker\",\"ok\":%d,\"magic\":\"0x%08x\",\"count_a\":%u,\"count_b\":%u,"
                "\"loops_per_s\":%.0f,\"timer_a\":%u,\"timer_b\":%u,\"timer_delta\":%u,\"wall_us\":%.0f,"
                "\"r80\":\"0x%04x\",\"lui_ori\":\"0x%08x\",\"lui_xori\":\"0x%08x\",\"lui_andi\":\"0x%08x\","
                "\"alu_probe\":\"0x%x\",\"cpu_r80\":\"0x%08x\"}\n",
                ok, a[0], a[1], b[1], (b[1] - a[1]) / (wall_us / 1e6), a[2], b[2], b[2] - a[2], wall_us,
                r.r16(0x0080), b[4], b[5], b[6], b[7], b[8]);
    rc = ok ? 0 : 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "marker: %s\n", e.what());
  }
  try {
    cpu.halt();
  } catch (...) {
  }
  std::printf("{\"ev\":\"marker.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
  return rc;
}

int cmd_c2h(Session &s, const Args &a) {
  Reg &r = s.reg();
  C2hRxPath rx(r);
  Cpu8822c cpu(r);
  C2hReader rd(s.handle(), s.context(), 16384, a.depth);
  int rc = 1, completed = 0;
  try {
    if (!a.no_halt)
      cpu.halt(); // the vendor firmware is not needed for host-issued C2H; keep it from interfering
    std::printf("{\"ev\":\"c2h.fwff_before\",\"r601\":\"0x%02x\",\"r29c\":\"0x%08x\",\"r2a0\":\"0x%08x\",\"r298\":\"0x%08x\"}\n",
                r.r8(0x0601), r.r32(0x029C), r.r32(0x02A0), r.r32(0x0298));
    rx.apply(a.rx_on || a.accept_all, a.fwff_off);
    for (auto [addr, val] : a.sets) {
      if (addr == 0x0290)
        r.w8(addr, static_cast<uint8_t>(val));
      else
        r.w32(addr, val);
    }
    if (a.agg) // C2hRxPath clears it for the 8822B; restored by C2hRxPath
      r.m8(0x010C, 0x04, 0x04);
    if (a.accept_all) { // RCR accept bits + all RX filter maps (restored by C2hRxPath)
      r.w32(0x0608, r.r32(0x0608) | 0x0000F00Fu);
      r.w16(0x06A0, 0xFFFF);
      r.w16(0x06A2, 0xFFFF);
      r.w16(0x06A4, 0xFFFF);
    }
    const size_t payload = static_cast<size_t>(std::clamp(a.bytes, 64, 16352));
    const int count = a.count > 0 ? a.count : 64;
    // C2H address and OCP of the staged packet. The TX aliases are the 8822B's (ac54/FINDINGS §4);
    // on the 8822C they are what this test checks.
    uint32_t off = static_cast<uint32_t>(a.rx_off & ~7), ocp = 0x18700000u + off;
    if (a.source == "tx0") {
      off = 0x16000;
      ocp = 0x187A0000u;
    } else if (a.source == "tx1") {
      off = 0x06000;
      ocp = 0x187B0000u;
    } else if (a.source != "rx") {
      throw std::runtime_error("--source must be rx, tx0 or tx1");
    }
    const C2hPacket pkt = C2hPacket::make(payload);
    c2h_stage(r, ocp, pkt);
    std::vector<uint8_t> chk(24);
    r.sram_read(ocp, chk.data(), chk.size());
    const bool staged = std::memcmp(chk.data(), pkt.frame.data(), 24) == 0;
    std::printf("{\"ev\":\"c2h.stage\",\"ocp\":\"0x%08x\",\"descriptor_readback_ok\":%d,\"r100\":\"0x%04x\","
                "\"r290\":\"0x%02x\",\"r10c\":\"0x%08x\",\"r284\":\"0x%08x\"}\n",
                ocp, staged ? 1 : 0, r.r16(0x0100), r.r8(0x0290), r.r32(0x010C), r.r32(0x0284));
    std::fflush(stdout);
    if (!rd.start())
      throw std::runtime_error("bulk-IN submit failed");
    rd.drain(30);
    if (a.burst > 0) {
      // Burst probe: does packet N leave only when something follows it? Fire K requests (each must
      // be accepted), then collect for 1.5 s. A one-packet hold shows up as K-1 arrivals.
      int accepted = 0;
      for (int i = 0; i < a.burst; i++) {
        r.w32(0x0294, c2h_request(off));
        bool clear = false;
        const auto dl = clk::now() + std::chrono::milliseconds(50);
        while (!clear && clk::now() < dl)
          clear = (r.r32(0x0294) & 0x10000u) == 0;
        if (!clear)
          break;
        accepted++;
      }
      const uint32_t r284 = r.r32(0x0284), r288 = r.r32(0x0288), r298 = r.r32(0x0298);
      int got = 0;
      while (rd.wait(pkt, true, 1500) == 1)
        got++;
      std::printf("{\"ev\":\"c2h.burst\",\"requested\":%d,\"accepted\":%d,\"arrived\":%d,\"stray\":%u,"
                  "\"timeouts\":%u,\"r284_after_burst\":\"0x%08x\",\"r288\":\"0x%08x\",\"r298\":\"0x%08x\","
                  "\"r294_end\":\"0x%08x\"}\n",
                  a.burst, accepted, got, rd.stray(), rd.timeouts(), r284, r288, r298, r.r32(0x0294));
      rd.stop();
      rx.restore();
      std::printf("{\"ev\":\"c2h.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
      return got > 0 ? 0 : 1;
    }
    const auto t0 = clk::now();
    for (int i = 0; i < count; i++) {
      r.w32(0x0294, c2h_request(off));
      bool clear = false;
      const auto dl = clk::now() + std::chrono::milliseconds(2000);
      while (!clear && clk::now() < dl)
        clear = (r.r32(0x0294) & 0x10000u) == 0;
      if (!clear)
        throw std::runtime_error("C2H request did not complete (r294 " + hex32(r.r32(0x0294)) + ")");
      // MEASURED: the 8822C RX DMA releases packet N only when packet N+1 arrives, so in pipelined
      // mode request i flushes packet i-1 and the last packet stays held.
      if (a.pipelined && i == 0)
        continue;
      if (rd.wait(pkt, true, 2000) != 1)
        throw std::runtime_error("no verified bulk-IN completion");
      completed = a.pipelined ? i : i + 1;
    }
    const double ms = ms_since(t0);
    std::printf("{\"ev\":\"c2h.host\",\"staged\":%d,\"bytes\":%zu,\"count\":%d,\"verified\":%u,\"stray\":%u,"
                "\"timeouts\":%u,\"ms\":%.1f,\"mbps\":%.2f}\n",
                staged ? 1 : 0, payload, count, rd.verified(), rd.stray(), rd.timeouts(), ms,
                ms > 0 ? payload * count / ms / 1e3 : 0.0);
    rc = 0;
  } catch (const std::exception &e) {
    uint32_t r294 = 0, r288 = 0;
    try {
      r294 = r.r32(0x0294);
      r288 = r.r32(0x0288);
    } catch (...) {
    }
    try {
      std::printf("{\"ev\":\"c2h.fwff_after\",\"r601\":\"0x%02x\",\"r29c\":\"0x%08x\",\"r2a0\":\"0x%08x\",\"r298\":\"0x%08x\"}\n",
                  r.r8(0x0601), r.r32(0x029C), r.r32(0x02A0), r.r32(0x0298));
    } catch (...) {
    }
    std::printf("{\"ev\":\"c2h.fail\",\"completed\":%d,\"stray\":%u,\"timeouts\":%u,\"r294\":\"0x%08x\","
                "\"r288\":\"0x%08x\",\"callbacks\":%u,\"cb_completed\":%u,\"verified\":%u}\n",
                completed, rd.stray(), rd.timeouts(), r294, r288, C2hReader::g_callbacks.load(),
                C2hReader::g_cb_completed.load(), rd.verified());
    std::fprintf(stderr, "c2h: %s\n", e.what());
  }
  rd.stop();
  rx.restore();
  std::printf("{\"ev\":\"c2h.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
  return rc;
}

// Map the 8822C's C2H address space onto the TX buffer: plant a small tagged C2H packet every 4 KiB
// in TXFF 0x20000..0x3f000, then fire one request per 4 KiB-aligned C2H address and see which tag
// (if any) comes back. Stops at the first request that does not clear (a wedge).
int cmd_c2hmap(Session &s, const Args &) {
  Reg &r = s.reg();
  C2hRxPath rx(r);
  Cpu8822c cpu(r);
  int found = 0;
  try {
    cpu.halt();
    rx.apply(false);
    constexpr size_t kPay = 64;
    for (uint32_t t = 0x20000; t < 0x40000; t += 0x1000) {
      uint8_t f[24 + kPay] = {};
      f[0] = kPay;
      f[3] = 0x80;
      f[11] = 0x10;
      const uint32_t tag = 0xC2C00000u | t;
      for (size_t i = 24; i < sizeof f; i += 4)
        std::memcpy(f + i, &tag, 4);
      r.sram_write(0x18780000u + t, f, sizeof f);
    }
    std::vector<uint8_t> buf(16384);
    for (uint32_t c = 0x6000; c < 0x40000; c += 0x1000) {
      r.w32(0x0294, c2h_request(c));
      // The engine only completes once a bulk-IN read is posted, so read first, then check the bit.
      int got = 0, len = 0;
      uint32_t tag = 0;
      for (int k = 0; k < 4 && !tag; k++) {
        int n = 0;
        if (libusb_bulk_transfer(s.handle(), 0x84, buf.data(), static_cast<int>(buf.size()), &n, 40) != 0 || n <= 0)
          break;
        got++;
        len = n;
        if (n == static_cast<int>(24 + kPay) && (buf[11] & 0x10))
          std::memcpy(&tag, buf.data() + 24, 4);
      }
      bool clear = false;
      const auto dl = clk::now() + std::chrono::milliseconds(30);
      while (!clear && clk::now() < dl)
        clear = (r.r32(0x0294) & 0x10000u) == 0;
      if (tag)
        found++;
      std::printf("{\"ev\":\"c2hmap\",\"c2h\":\"0x%05x\",\"cleared\":%d,\"packets\":%d,\"last_len\":%d,"
                  "\"tag\":\"0x%08x\"}\n",
                  c, clear ? 1 : 0, got, len, tag);
      std::fflush(stdout);
      if (!clear)
        break;
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "c2hmap: %s\n", e.what());
  }
  rx.restore();
  std::printf("{\"ev\":\"c2hmap.done\",\"mapped\":%d,\"alive\":%d}\n", found, r.alive() ? 1 : 0);
  return found ? 0 : 1;
}

int cmd_c2hbench(Session &s, const Args &a) {
  Reg &r = s.reg();
  C2hRxPath rx(r);
  Cpu8822c cpu(r);
  const int count = std::clamp(a.count > 0 ? a.count : 1024, 1, 1600);
  const size_t payload = static_cast<size_t>(std::clamp(a.bytes, 64, 16352));
  C2hReader rd(s.handle(), s.context(), 16384, a.depth);
  int rc = 1;
  try {
    const C2hPacket pkt = C2hPacket::make(payload);
    cpu.load(c2hbench8822c_bin, c2hbench8822c_bin_len);
    rx.apply(false);
    c2h_stage(r, 0x18701000, pkt);
    const uint32_t seed = static_cast<uint32_t>(count);
    cpu.dmem_write(kBlock + 0x18, &seed, sizeof seed);
    if (!rd.start())
      throw std::runtime_error("bulk-IN submit failed");
    rd.drain(20);
    const auto t0 = clk::now();
    cpu.start();
    unsigned ok = 0;
    while (ok < static_cast<unsigned>(count) && rd.wait(pkt, true, 1500) == 1)
      ok++;
    const double wall_ms = ms_since(t0);
    cpu.halt();
    rd.stop();
    uint32_t hdr[16] = {0};
    cpu.dmem_read(kBlock, hdr, sizeof hdr);
    const uint32_t issued = std::min<uint32_t>(hdr[1], static_cast<uint32_t>(count));
    std::vector<uint32_t> dur(issued);
    if (issued)
      cpu.dmem_read(kBlock + 0x40, dur.data(), dur.size() * 4);
    std::vector<uint32_t> sorted = dur;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (uint32_t d : dur)
      sum += d;
    std::printf("{\"ev\":\"c2h.bench\",\"n\":%d,\"issued\":%u,\"verified\":%u,\"stray\":%u,\"timeouts\":%u,"
                "\"magic\":\"0x%08x\",\"done\":%u,\"min_us\":%u,\"p50_us\":%u,\"p99_us\":%u,\"max_us\":%u,"
                "\"mean_us\":%.1f,\"wall_ms\":%.1f,\"mbps\":%.2f}\n",
                count, hdr[1], ok, rd.stray(), rd.timeouts(), hdr[0], hdr[2], sorted.empty() ? 0 : sorted.front(),
                pctl(sorted, 50), pctl(sorted, 99), sorted.empty() ? 0 : sorted.back(),
                dur.empty() ? 0.0 : sum / dur.size(), wall_ms, wall_ms > 0 ? payload * ok / wall_ms / 1e3 : 0.0);
    rc = (hdr[0] == 0x0C2B0001u && hdr[2] == 1u && ok == static_cast<unsigned>(count)) ? 0 : 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "c2hbench: %s\n", e.what());
    try {
      cpu.halt();
    } catch (...) {
    }
  }
  rx.restore();
  std::printf("{\"ev\":\"c2h.bench.done\",\"alive\":%d}\n", r.alive() ? 1 : 0);
  return rc;
}

} // namespace

int main(int argc, char **argv) {
  Args a;
  if (!parse(argc, argv, a)) {
    std::fprintf(stderr, "usage: wsdr_cu marker|c2h|c2hbench --pid 0xc812 [options] (see source header)\n");
    return 2;
  }
  a.open.thermal_track = false; // no host register traffic from devourer's thermal tick
  auto logger = std::make_shared<Logger>();
  Session s(logger);
  try {
    s.open(a.open);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "open/bring-up failed: %s\n", e.what());
    return 3;
  }
  std::printf("{\"ev\":\"session\",\"chip\":\"%s\",\"superspeed\":%d,\"channel\":%d,\"r80\":\"0x%04x\"}\n",
              s.chip().c_str(), s.superspeed() ? 1 : 0, a.open.channel, s.reg().r16(0x0080));
  s.reg().set_cap(a.cap);
  std::printf("{\"ev\":\"cu.cap\",\"block_bytes\":%zu}\n", a.cap);
  if (a.cmd == "c2hmap")
    return cmd_c2hmap(s, a);
  if (a.cmd == "memtest")
    return cmd_memtest(s, a);
  if (a.cmd == "marker")
    return cmd_marker(s, a);
  if (a.cmd == "c2h")
    return cmd_c2h(s, a);
  if (a.cmd == "c2hbench")
    return cmd_c2hbench(s, a);
  std::fprintf(stderr, "unknown command %s\n", a.cmd.c_str());
  return 2;
}
