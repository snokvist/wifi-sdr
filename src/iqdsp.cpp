// SPDX-License-Identifier: GPL-2.0-only
#include "iqdsp.h"

#include <algorithm>
#include <cmath>

namespace wsdr {

void DcBlock::process(cf *x, size_t n) {
  if (!n)
    return;
  cf m = 0;
  for (size_t i = 0; i < n; i++)
    m += x[i];
  m /= static_cast<float>(n);
  _dc = _init ? _dc + _a * (m - _dc) : m;
  _init = true;
  for (size_t i = 0; i < n; i++)
    x[i] -= _dc;
}

void Nco::set(double f_hz, double fs) {
  _on = f_hz != 0;
  _ph = 1;
  _rot = std::polar(1.0, -2 * M_PI * f_hz / fs);
}

void Nco::process(cf *x, size_t n) {
  if (!_on)
    return;
  for (size_t i = 0; i < n; i++) {
    x[i] *= cf(static_cast<float>(_ph.real()), static_cast<float>(_ph.imag()));
    _ph *= _rot;
  }
  _ph /= std::abs(_ph);
}

void Resampler::configure(double in_rate, double out_rate) {
  _pass = std::abs(in_rate - out_rate) < 1e-6 * in_rate;
  _buf.clear();
  _h.clear();
  if (_pass)
    return;
  _step = in_rate / out_rate;
  const double ratio = std::min(1.0, out_rate / in_rate);
  _taps = static_cast<int>(std::ceil(40 / ratio / 2)) * 2;
  const double fc = 0.45 * ratio; // cycles per input sample
  const int half = _taps / 2;
  _h.assign(static_cast<size_t>((kPhases + 1) * _taps), 0.f);
  for (int p = 0; p <= kPhases; p++) {
    const double frac = static_cast<double>(p) / kPhases;
    double sum = 0;
    float *h = &_h[static_cast<size_t>(p * _taps)];
    for (int j = 0; j < _taps; j++) {
      const double tau = frac + (half - 1) - j; // t - n for tap j
      const double s = tau == 0 ? 2 * fc : std::sin(2 * M_PI * fc * tau) / (M_PI * tau);
      const double u = (tau + half) / _taps; // window position in [0, 1]
      const double w = u <= 0 || u >= 1 ? 0 : 0.42 - 0.5 * std::cos(2 * M_PI * u) + 0.08 * std::cos(4 * M_PI * u);
      h[j] = static_cast<float>(s * w);
      sum += s * w;
    }
    for (int j = 0; j < _taps; j++)
      h[j] = static_cast<float>(h[j] / sum);
  }
  _buf.assign(static_cast<size_t>(half), cf(0));
  _t = half - 1;
}

void Resampler::process(const cf *x, size_t n, std::vector<cf> &out) {
  if (_pass) {
    out.insert(out.end(), x, x + n);
    return;
  }
  _buf.insert(_buf.end(), x, x + n);
  const int half = _taps / 2;
  for (;;) {
    const double fl = std::floor(_t);
    const size_t i0 = static_cast<size_t>(fl);
    if (i0 + static_cast<size_t>(half) >= _buf.size())
      break;
    const int p = static_cast<int>(std::lround((_t - fl) * kPhases));
    const float *h = &_h[static_cast<size_t>(p * _taps)];
    const cf *b = &_buf[i0 + 1 - static_cast<size_t>(half)];
    float re = 0, im = 0;
    for (int j = 0; j < _taps; j++) {
      re += b[j].real() * h[j];
      im += b[j].imag() * h[j];
    }
    out.emplace_back(re, im);
    _t += _step;
  }
  const double drop = std::floor(_t) - (half - 1);
  if (drop > 0) {
    _buf.erase(_buf.begin(), _buf.begin() + static_cast<std::ptrdiff_t>(drop));
    _t -= drop;
  }
}

size_t to_u8(const cf *x, size_t n, float scale, uint8_t *out) {
  size_t clip = 0;
  auto q = [&](float v) {
    float y = v * scale + 127.5f;
    if (y < 0.f || y > 255.f) {
      clip++;
      y = std::clamp(y, 0.f, 255.f);
    }
    return static_cast<uint8_t>(std::lrint(y)); // round: truncation would bias DC by -0.5 LSB
  };
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = q(x[i].real());
    out[2 * i + 1] = q(x[i].imag());
  }
  return clip;
}

} // namespace wsdr
