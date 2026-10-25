// SPDX-License-Identifier: GPL-2.0-only
// Small streaming IQ DSP blocks for the front ends: DC removal, NCO frequency shift, arbitrary-ratio
// polyphase resampling (windowed sinc) and rtl_tcp-style u8 packing. Complex float throughout.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wsdr {

using cf = std::complex<float>;

// Subtracts a slowly tracked DC estimate (one IIR step per block).
class DcBlock {
public:
  explicit DcBlock(float alpha = 0.05f) : _a(alpha) {}
  void process(cf *x, size_t n);
  void reset() { _dc = 0; _init = false; }

private:
  float _a;
  cf _dc = 0;
  bool _init = false;
};

// Multiplies by exp(-j 2 pi f t): content at +f moves to 0 Hz.
class Nco {
public:
  void set(double f_hz, double fs);
  void process(cf *x, size_t n);

private:
  std::complex<double> _ph = 1, _rot = 1;
  bool _on = false;
};

// Arbitrary-ratio resampler: output sample k sits at input time k * in/out. Blackman-windowed sinc,
// cut off at 0.45 * min(in, out), 128 phases, 40 taps scaled up when decimating. Flat (< 0.2 dB) to
// about 0.4 * min(in, out), i.e. ~80 % of the output Nyquist band.
class Resampler {
public:
  void configure(double in_rate, double out_rate);
  bool passthrough() const { return _pass; }
  void process(const cf *x, size_t n, std::vector<cf> &out); // appends to out

private:
  bool _pass = true;
  double _step = 1;
  int _taps = 0;
  static constexpr int kPhases = 128;
  std::vector<float> _h;   // (kPhases + 1) x _taps
  std::vector<cf> _buf;    // history + pending input
  double _t = 0;           // next output position in _buf
};

// value * scale + 127.5, clipped to [0, 255]; returns the number of clipped components.
size_t to_u8(const cf *x, size_t n, float scale, uint8_t *out);

} // namespace wsdr
