// SPDX-License-Identifier: GPL-2.0-only
// Offline checks of iqdsp: a tone through NCO + resampler must come out at the expected frequency
// and amplitude, with images and alias products well down. Exit 0 = all pass.
#include <cmath>
#include <cstdio>
#include <vector>

#include "iqdsp.h"

using namespace wsdr;

namespace {

// Power of x at frequency f (Hz) via a single-bin DFT, normalized to |amplitude|^2.
double tone_power(const std::vector<cf> &x, size_t skip, double f, double fs) {
  std::complex<double> acc = 0;
  const size_t n = x.size() - skip;
  for (size_t i = 0; i < n; i++)
    acc += std::complex<double>(x[skip + i]) * std::polar(1.0, -2 * M_PI * f * static_cast<double>(i) / fs);
  return std::norm(acc / static_cast<double>(n));
}

std::vector<cf> run(double in_fs, double out_fs, double tone, double shift) {
  std::vector<cf> in(static_cast<size_t>(in_fs * 0.05)); // 50 ms
  for (size_t i = 0; i < in.size(); i++)
    in[i] = std::polar(1.0f, static_cast<float>(2 * M_PI * tone * static_cast<double>(i) / in_fs));
  Nco nco;
  nco.set(shift, in_fs);
  Resampler rs;
  rs.configure(in_fs, out_fs);
  std::vector<cf> out;
  for (size_t off = 0; off < in.size(); off += 8192) {
    const size_t n = std::min<size_t>(8192, in.size() - off);
    std::vector<cf> blk(in.begin() + static_cast<std::ptrdiff_t>(off), in.begin() + static_cast<std::ptrdiff_t>(off + n));
    nco.process(blk.data(), n);
    rs.process(blk.data(), n, out);
  }
  return out;
}

bool check(const char *name, double in_fs, double out_fs, double tone, double shift, double expect,
           const std::vector<double> &bad) {
  const std::vector<cf> out = run(in_fs, out_fs, tone, shift);
  const double want_n = static_cast<double>(static_cast<size_t>(in_fs * 0.05)) * out_fs / in_fs;
  const double p = tone_power(out, 200, expect, out_fs);
  double worst = 0;
  for (double f : bad)
    worst = std::max(worst, tone_power(out, 200, f, out_fs));
  const bool ok = std::abs(10 * std::log10(p)) < 0.2 && 10 * std::log10(worst / p) < -50 &&
                  std::abs(static_cast<double>(out.size()) - want_n) < 64;
  std::printf("%-34s out %zu/%.0f  tone %+.2f dB  worst spur %+.1f dBc  %s\n", name, out.size(), want_n,
              10 * std::log10(p), 10 * std::log10(worst / p), ok ? "PASS" : "FAIL");
  return ok;
}

// An out-of-band tone must be attenuated: its alias in the output stays below max_db.
bool reject(const char *name, double in_fs, double out_fs, double tone, double alias, double max_db) {
  const std::vector<cf> out = run(in_fs, out_fs, tone, 0);
  const double db = 10 * std::log10(tone_power(out, 200, alias, out_fs));
  const bool ok = db < max_db;
  std::printf("%-34s alias at %+.2f MHz %+.1f dB  %s\n", name, alias / 1e6, db, ok ? "PASS" : "FAIL");
  return ok;
}

// u8 packing must not add a DC bias: zero-mean noise at ~1.5 LSB rms has to average to 127.5.
bool u8_bias() {
  std::vector<cf> x(1 << 20);
  uint32_t r = 1;
  auto u = [&] { r = r * 1664525u + 1013904223u; return (r >> 8) / 16777216.0f - 0.5f; };
  for (auto &v : x)
    v = cf(5.f * u(), 5.f * u());
  std::vector<uint8_t> b(2 * x.size());
  to_u8(x.data(), x.size(), 1.f, b.data());
  double m = 0;
  for (uint8_t v : b)
    m += v;
  m = m / static_cast<double>(b.size()) - 127.5;
  const bool ok = std::abs(m) < 0.02;
  std::printf("%-34s mean - 127.5 = %+.4f LSB  %s\n", "u8 packing DC bias", m, ok ? "PASS" : "FAIL");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= check("passthrough 2.5M, shift 300k", 2.5e6, 2.5e6, 700e3, 300e3, 400e3, {-400e3, 700e3});
  ok &= check("2.5M -> 2.048M, tone 600k", 2.5e6, 2.048e6, 600e3, 0, 600e3, {-600e3, 200e3});
  ok &= check("5M -> 2.4M, shift -0.9M, tone 0", 5e6, 2.4e6, 0, -0.9e6, 0.9e6, {-0.9e6, 0});
  ok &= reject("5M -> 2.4M, 2.0M tone rejected", 5e6, 2.4e6, 2.0e6, 2.0e6 - 2.4e6, -60);
  ok &= check("2.5M -> 3.2M upsample, tone -0.95M", 2.5e6, 3.2e6, -0.95e6, 0, -0.95e6, {0.95e6, 0});
  ok &= u8_bias();
  return ok ? 0 : 1;
}
