# TX feasibility on the RTL8812CU (8822C) — deferred

_2026-10-25. Investigation complete; no TX implementation is merged._

## Question

Can wifi-sdr add SDR transmit? The repo is RX-only (raw LA streaming plus
SoapySDR/rtl_tcp front ends), so the question was whether the 8822C could
become a general-purpose transmitter.

## Bottom line

- **Arbitrary-IQ TX is not available.** The TX FIFO is 802.11 packet memory
  feeding the MAC/PHY (scrambler, FEC, interleaver, mapper, IFFT), not a sample
  ring to a DAC. The LA path is the reverse debug tap (ADC -> TX SRAM capture);
  there is no DAC playback path. The vendor MP code exposes only packet TX,
  single tone, carrier suppression and continuous TX — no waveform playback.
- **Three real TX modes do exist**, all in devourer:
  1. a bare RF carrier (`StartCwTone`);
  2. a fixed legacy-6M OFDM carrier (`StartContinuousTx`);
  3. raw 802.11 MPDU injection through the normal TX FIFO (`send_packet`).
- A prototype `wsdr::Transmitter` + `wsdr_tx` CLI was built and CW-tested, then
  removed before merge. The findings below are from devourer's demos plus that
  one integration check.

## What was verified

Hardware: RTL8812CU (0bda:c812), RTL8822C cut 5, bus 1 port 1, USB 2, channel
36. All commands used `DEVOURER_PID=0xc812 DEVOURER_USB_BUS=1
DEVOURER_USB_PORT=1 DEVOURER_CHANNEL=36`; the 8812cu coord lock was held and
released around each run. The 8812BU was not used: its coord lock was held by
the desktop session, so no on-air RX check was performed.

| Mode | Devourer call | Result |
|---|---|---|
| CW, gain 2 | `StartCwTone` via `DEVOURER_CW_TONE=1` | `CW single-tone armed @ ch36 gain=2 (8822c)`, then clean `CW single-tone stopped — chip restored`; `tx.stats submitted=50 failed=0` (bring-up only) |
| Continuous | `StartContinuousTx` via `DEVOURER_CONT_TX=1` | `Modulated continuous TX armed @ ch36 (Jaguar3 PMAC 6M HW 100%-duty carrier)`, idle-hold, clean stop; `submitted=114 failed=0` |
| Injection, 100 frames, 2 ms gap, power index 8 | `IRadio::send_packet` via `DEVOURER_TX_FRAMES=100` | `tx.stats submitted=150 failed=0 last_rc=0 txdma_status=0 final=1` (50 firmware-download transfers + 100 frames); clean de-init |

`tx.stats submitted` counts USB bulk-OUT submissions, not on-air frames; a
successful submission proves the chip accepted the descriptor and accepted the
data, not that a peer decoded it. No spectrum-analyzer or second-adapter
measurement was made.

Adapter caps reported by devourer on this unit: RTL8822C, 2 TX chains, 2 spatial
streams, 80 MHz TX bandwidth, TX-power index max 127 at 0.25 dB/index, and
CW/continuous/inject all supported.

The prototype path used `wsdr::Session`, which sets
`tuning.j3_runtime_thread=false` for the 8822C. CW armed and stopped cleanly
through that path. Continuous and injection were only verified through
devourer's `txdemo`, not through `Session`, so that combination remains
untested.

Cautions:

- devourer's `txdemo` ignores `--help` and opens the first matching adapter.
  Always pin `DEVOURER_PID`, `DEVOURER_USB_BUS` and `DEVOURER_USB_PORT`.
- Do not run two TX demos on the same adapter; one will fail to open.
- CW gain is an RF register value (0..31), not a calibrated dBm control.

## Why arbitrary IQ is not possible

- The TX FIFO is the 802.11 packet buffer. A frame handed to `send_packet` is
  parsed (radiotap plus MPDU), described, and fed to the normal MAC/PHY chain.
  There is no host-visible sample stream at the DAC input.
- The LA tap captures RX ADC samples into TX SRAM for debug readout. That is the
  opposite direction; it cannot drive the DAC. Devourer's `LaCapture` notes the
  capture occupies the top of TXFF and must not run concurrently with heavy TX.
- The vendor MP API has `PACKETS_TX`, `SINGLE_TONE`, `CARRIER_SUPPRESSION` and
  `CONTINUOUS_TX`. None is a waveform-playback engine.
- A true vector-signal generator needs a different radio (B210, Pluto, HackRF,
  ...) or a new reverse-engineering project on the LA/TX-SRAM block.

## What the bare carrier is useful for

The CW mode is an unmodulated carrier at the tuned Wi-Fi channel centre, path A,
gain 0..31. Its bandwidth is roughly the LO linewidth, not 20 MHz. It is a
diagnostic/test mode, not a data link:

| Use | What it gives you | Detector |
|---|---|---|
| TX-chain smoke test | Bring-up, PA and antenna path are alive | Second adapter or spectrum analyzer |
| Channel/filter validation | FFT spike at centre; ch36 vs ch40 shows filter roll-off | 8812BU RX or SDR |
| Gain/power curve | Step gain 0..31 and measure RSSI; relative TX power | Second adapter or power meter |
| Antenna/field test | Nulls, polarization, orientation | Second adapter |
| Narrowband interferer | Raise the noise floor on one channel; CCA/ED, rate adaptation, coexistence | Wi-Fi link or RX |
| RF characterization | LO leakage, phase noise, spurs, harmonics | Spectrum analyzer / SDR |
| Bistatic Doppler | CW TX plus a separate RX; coarse Doppler only (independent clocks) | Two adapters |

Limits: 5 MHz channel grid only, uncalibrated gain, path A only, half-duplex,
and regulatory limits apply. The 8812CU is USB 2, which is irrelevant for these
low-rate modes but rules out a high-throughput TX stream.

## If TX is revisited

Keep it a thin, honest wrapper around devourer rather than a new PHY:

- `Transmitter` API + `wsdr_tx` CLI with `cw`, `cont` and `frames`.
- `cw`: `StartCwTone` / `StopCwTone`.
- `cont`: `StartContinuousTx` / `StopContinuousTx`; Jaguar3 is fixed 6M.
- `frames`: `send_packet` with a radiotap header plus MPDU; expose rate, width,
  power and a raw-MPDU file input.
- Keep RX and TX mutually exclusive. TX needs the vendor firmware; the custom
  LA streamer replaces the vendor CPU. Do not claim a SoapySDR TX stream, since
  that API implies arbitrary IQ.
- On-air verification plan (deferred until the 8812bu lock is free): 8812CU
  sends CW on ch36 at several gain steps, 8812BU receives on ch36/ch40 and
  records FFT/RSSI. This confirms radiation and gives a relative gain curve.

## Open items

1. On-air verification of all three modes (8812BU lock was held by the desktop
   session).
2. Continuous and packet injection through `wsdr::Session` with the Jaguar3
   runtime thread disabled.
3. CW gain/power calibration and channel-to-channel output variation.
4. Regulatory limits and a safe dummy-load/attenuator setup for any future
   on-air work.
