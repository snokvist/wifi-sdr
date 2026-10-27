# RTL8822B (RTL8812BU) TX-path source review

Scope: vendor driver `ref/88x2bu-20210702/`, devourer `external/devourer/`, and the
earlier notes in `notes/tx-feasibility.md` (8822C) and `notes/fw-disasm.md`.
Status tags match `LOG.md`: [S] source, [V] verified, [H] hypothesis, [X] dead end.

## 1. The MP "single tone" is the analog LO

`hal/hal_mp.c:1784 hal_mpt_SetSingleToneTx()` (8822B branch):

- BB `0x808[29:28]=0`: both OFDM and CCK modulators off.
- RF path A `0x00[19:16]=2` (TX mode), `0x00[4:0]=gain` (0..31).
- RF `0x58[1]=1`: LO enable.
- RFE pinmux forced: `0xCB0=0x77777777`, `0xEB0=0x77777777`,
  `0xCB4[15:0]=0x7777`, `0xEB4[15:0]=0x7777`, `0xCBC[11:0]=0xb`,
  `0xEBC[11:0]=0x830`.
- Stop path restores the RFE words, RF `0x00`, clears RF `0x58[1]`, and puts
  `0x808[29:28]` back to 3.

There is no digital oscillator, no phase accumulator and no sample buffer in this
path: the output is the free-running channel LO leaking through the TX switch.
Devourer mirrors it in `src/jaguar2/RtlJaguar2Device.cpp:859 StartCwTone()` and
`:924 StopCwTone()`; `StartContinuousTx()` is at `:966`.

Consequences:

- Frequency = channel LO. Changing frequency requires a synthesizer retune
  (`SetMonitorChannel`), which is not phase-continuous and takes milliseconds.
- Amplitude = RF `0x00[4:0]` (and the RFE/PA state).
- On/off = RF `0x58[1]` (fast, verified at 1 kHz OOK), or RF gain/mode.
- Phase = not writable.

## 2. `0x914[18:16]` OFDM test modes

`include/rtw_mp.h:624`:

| value | mode |
|---|---|
| 0 | OFDM_ALL_OFF |
| 1 | OFDM_ContinuousTx |
| 2 | OFDM_SingleCarrier |
| 4 | OFDM_SingleTone |

Vendor `mpt_StartOfdmContTx()` (`hal_mp.c` ~2460-2652) turns the OFDM block on
(`0x800[25]`), turns CCK test mode off, enables the scrambler, and writes
`0x914[18:16]=1`. `hal_mpt_SetContinuousTx()` uses this for rates >= 6M.

Devourer `StartContinuousTx()` additionally writes `0x838[7:0]=0x6d` first,
because without it the USB TX FIFO wedges; it reports a flat ~18 MHz OFDM block
on an SDR. `StopContinuousTx()` clears `0x914`, pulses the BB reset
(`0x100[8]` low then high) and restores `0x838`. This is a modulated
pseudo-random OFDM test signal, not an arbitrary waveform source.

`SingleCarrier`/`SingleTone` values exist in the enum but the 8822B single-tone
path does not use them and no frequency/phase register for them was found.

## 3. IQK / calibration blocks (`0x1bxx`)

The 8822B IQK is host-driven (the 8822C firmware owns its own IQK; the 8822B
firmware MMIO does not contain `0x1b38`). Commands are written to `0x1b00`:

- select: `0xf8000008 | (path << 1)`
- LOK: `0xf8000018`, `0xf8000028`
- TXIQK: `0xf8000418`, `0xf8000518`, `0xf8000618`
- RXIQK: `0xf8000718`, `0xf8000818`
- RXIMR: `0xf8000208 | (1 << (path + 4))`, TXIMR: `0x1b34 = 0x800`
- `0x1b20[31:20]` and `0x1b24[31:20]` carry a 12-bit `tone_index`
- `0x1b2c`, `0x1b38`, `0x1b3c`, `0x1bcc` control/read the engine
- `0x1b38` holds the scalar result

`0x1b80` / `0x1bd8` are the **IQK CFIR coefficient RAM**: eight complex taps
(`iqk_cfir_real/imag[ch][path][TX/RX][8]` in `halrf_iqk.h`), written through an
indirect port (low bits address, high bits data). They are calibration
coefficients, not samples.

`0x90c[15]` is commented "dac_buf reset selection" in
`_iqk_configure_macbb_8822b`; it is an IQK test mux, not a host DAC buffer.
`0xC64`/`0xE64` are dynamic AFE power-control registers, not a buffer.
`0x910` is the PSD (RX power-spectrum) control and `0xf44` its 16-bit report
(`phydm_psd.c`); neither drives the TX DAC.

The 8822B `rf_supportability` table in `halrf.c` enables **IQK + LCK only**;
DPK is commented out for this chip. The 8822C/8821C IQK/DPK blocks similarly
expose scalars/coefficients, not a stream.

Open question: whether the IQK TX tone engine can be held after a calibration
command (rather than bursting and stopping) and whether `tone_index` gives a
usable frequency step. This is the only register-level frequency source found
apart from the channel LO; it is the next hardware test.

## 4. LA / test capture

`notes/fw-disasm.md` and the ac54 work show the LA path is ADC -> TX SRAM
capture (RX samples), not a DAC playback path. The LA ring is a capture engine;
its SRAM is not readable by the TX baseband as a waveform.

## 5. Firmware

`notes/fw-disasm.md` lists H2C commands for IQK (`0x0E`), PSD (`0x10`), DPK
(`0xB7`) and FW_ACCESS_TEST (`0x00`). For the 8822C the IQK `0x1bxx` block is a
tone-correlation engine that reports scalars at `0x1b38`; DPK reads gain-loss
scalars. Nothing in the H2C surface accepts a sample block for playback.

## 6. Vendor MP API

The vendor MP surface only offers packets, single tone, carrier suppression and
continuous TX (`hal_mp.c`); there is no `playback`, `waveform`, `sample buffer`
or `DAC buffer` API in `ref/88x2bu-20210702/`. The TX FIFO feeds the 802.11
MAC/PHY; it is packet memory, not a directly clocked DAC buffer.

## 7. Hardware follow-up on the remaining leads (2026-10-27)

- **0x914 continuous mode is real once PMAC/TX-OFDM are enabled.** With
  `0x914[18:16]=1`, `0xb00[8]=1` and `0xb04[3:0]=4`, the TX adapter emits a
  100 %-duty ~19 MHz-wide test waveform (`cont-ch36-20261027b`). It is a fixed
  internal pattern, not a host sample stream.
- **0x914 single-carrier mode 2 did not produce a clean carrier**
  (`raw-singlecarrier-ch36-20261027a`).
- **0x914 single-tone mode 4 emits a strong but unstable comb/test signal**
  (`raw-singletone-ch36-20261027a`, `st-igi80-ch36-20261027a/b`,
  `st-p0-igiA-ch36-20261027a`, `st-low1/low2`). The peak does not follow
  `0x914[15:0]` linearly and the RX AGC remains elevated after TX stop, so this
  is not a qualified programmable tone.
- **The IQK tone engine did not hold a tone.** Selfcheck-style setup, full
  IQK AFE/RFE/RF preparation, RF TX mode/LO and the `0x1b34` one-shot variants
  were tried (`tone-hold`, `tone-full`, `raw-tone-start`); none produced a
  sustained on-channel tone. The vendor source calls this a one-shot TX-IMR
  measurement, and `_iqk_start_tximr_test_8822b` is compiled out (`#if 0`).

## Current conclusion (source + hardware)

- Analog CW, OOK and slow gain keying: real and verified.
- 0x914 continuous OFDM and single-tone test modes: real, but fixed/internal
  or unstable; not controllable sample streams.
- Host-supplied IQ at the DAC: **no evidence of a path** on the RTL8822B.
  The IQK tone engine is a one-shot calibration source, not a holdable tone
  generator; the CFIR/DPK blocks are coefficients/scalars; the LA SRAM is an RX
  capture buffer; the TX FIFO is packet memory.
- A true IQ-injection path would need a different chip with a documented DAC or
  template-RAM interface, or custom firmware/FPGA work. The frame-injection
  `precoder` fallback remains packet-based, not raw IQ.
