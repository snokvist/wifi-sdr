# RTL8812BU (RTL8822B) TX capability report

Date: 2026-10-27. Bench: TX RTL8812BU (USB 7-1) -> 50 dB -> RX RTL8812BU
(USB 6-1), cabled, no antennas. Raw captures, run logs and register journals
are generated evidence kept locally under `tx_research/` and gitignored; this
document and the scripts that produced them are tracked. All TX runs used `lab/test-setup.conf`
(`cabled=yes`), an allowed channel, a <=10 s hold, a JSONL register journal,
reverse restore, a forced TX-off/BB-reset at exit, and an adapter reset by
`tx_research/tx_run.py`. No eFuse/EEPROM/flash writes and no persisted firmware
changes.

## Capability table

| Capability | Method | Achieved rate / length / fidelity | Evidence | Limits |
|---|---|---|---|---|
| Analog CW at the channel LO | RF 0x58[1] LO enable, RF 0x00 TX mode + gain, 0x808 modulators off (vendor single-tone) | Continuous tone at +62.45 kHz from RX LO; 80.3 dB SNR; 2.005 s burst; +27.4 dB on/off | `captures/cw-ch36-g0-20261027c-rx.cs16`, `...-summary.json`, `regs/cw-ch36-g0-20261027c.jsonl` | Frequency is the channel LO only; no phase register; amplitude is the RF gain index |
| OOK of that carrier | Toggle RF 0x58[1] from host | 1 kHz clean: 996.6 toggle/s, 50.1 % duty, 351x on/off magnitude; sustained limit ~1.33 kHz | `captures/ook-lo-1k-ch36-g0-20261027a-rx.cs16`, `captures/ook-lo-2k-ch36-g0-20261027a-rx.cs16`, `regs/ook-*.jsonl` | USB read-modify-write rate; no pulse shaping; no phase control |
| Continuous 0x914 OFDM test signal | 0x914[18:16]=1 **plus** 0xb00[8]=1 (PMAC on) and 0xb04[3:0]=4 (TX OFDM on) | 100 % duty, 1.500 s burst, 19.0 MHz 99 % bandwidth, +23.5 dB on/off, 50.4 dB in-burst SNR | `captures/cont-ch36-20261027b-rx.cs16`, `...-summary.json`, `regs/cont-ch36-20261027b.jsonl` | Fixed internal test pattern; no host data input; content not programmable |
| 0x914 single-carrier test mode | 0x914[18:16]=2 with PMAC/TX-OFDM | No usable signal above the floor (+1.1 dB "burst") | `captures/raw-singlecarrier-ch36-20261027a-rx.cs16`, `regs/raw-singlecarrier-ch36-20261027a.jsonl` | Dead end in this setup |
| 0x914 single-tone test mode | 0x914[18:16]=4 with PMAC/TX-OFDM | Strong on-period signal (RMS ~100-190 vs ~20-30 baseline at TX power 0) but a multi-line/comb spectrum; peak moved between runs and did not follow 0x914[15:0] linearly | `captures/raw-singletone-ch36-20261027a-rx.cs16`, `captures/st-igi80-ch36-20261027a-rx.cs16`, `captures/st-p0-igiA-ch36-20261027a-rx.cs16`, `captures/st-low1/low2-...` | Not a qualified programmable tone; RX AGC stays elevated after TX stop; no phase/amplitude sample control |
| IQK tone engine (0x1bxx) | Selfcheck-style setup; full IQK AFE/RFE/RF prep + RF TX mode/LO; 0x1b34 one-shot variants | No held on-channel tone above the floor; only a short calibration/prep transient | `captures/tone-hold-i8-ch36-20261027a-rx.cs16`, `captures/tone-full-i8-ch36-20261027a-rx.cs16`, `captures/tone-full-lo-i8-ch36-20261027b-rx.cs16`, `captures/raw-tone-start-i8-ch36-20261027a-rx.cs16` | Vendor calls it a one-shot TX-IMR measurement; `_iqk_start_tximr_test_8822b` is compiled out; not a sample buffer |
| Host IQ samples to the DAC | No path found | None | Source review in `source_review.md`; hardware attempts above | No writable waveform RAM, sample FIFO, replay pointer or DAC sample clock found. TX FIFO is packet memory; LA SRAM is RX capture; CFIR/DPK are coefficients/scalars |
| Frame injection (context only) | 802.11 PSDU through devourer `send_packet` / `precoder` | Existing tooling can shape legacy-6M OFDM payload bytes | Not run on the 8822B in this study; `external/devourer/examples/precoder` | Packet-based, not raw IQ; needs a bus/port-safe 8822B integration |

## Custom-firmware follow-up (rule lifted)

- **WCPU register throughput is high enough to matter:** `src/fw/txbench8822b.S`
  wrote BB `0x1bcc` 1,000,000 times in 200,033 us => **~5.0 M writes/s**
  (`fwbench` run, 2026-10-27). The 8822B embedded CPU can therefore drive a
  register-fed source at a few MS/s.
- **0x910 is a test-comb control, not a clean NCO:** host raw runs with
  `0x914[18:16]=4` and `0x910[6:0]` = 0/8/16 produced dominant lines at
  -1.818 / -3.068 / -4.318 MHz (156.25 kHz per step), but the on-period spectra
  contain a comb of comparable lines spaced ~0.625 MHz
  (`st-910-08`, `st-910-10`, `st-p0-igiA`). The 0x910 word steers the comb;
  it does not give a single clean tone.
- **Live updates are not clean:** firmware and host writes to 0x910 while the
  mode is active produce mixed/spurious spectra
  (`fwfsk-*`, `fsk-host-500`, `fsk-rearm-100`). Stopping/re-arming 0x914 or
  0xb04 introduces transients; no repeatable clean two-tone FSK was obtained.
- The firmware probes (`txbench`, `tx914sweep`, `txfsk`) are retained as
  evidence of the attempt; they are not a usable IQ transmitter.

## Conclusion

The RTL8822B has real RF output paths (analog CW, OOK, the fixed 0x914
continuous test waveform, and the 0x910-steered test comb), but **no
host-accessible arbitrary-IQ/DAC playback path** was found, even with custom
firmware. The IQK tone engine is a one-shot calibration source; the CFIR/DPK
blocks hold coefficients or scalars; the LA SRAM captures RX samples; the TX
FIFO is 802.11 packet memory. The WCPU can write registers at ~5 M writes/s,
but there is no writable sample memory or DAC sample port for it to drive. The
requested "host IQ samples reaching the DAC" is a dead end on this silicon.

## Most promising next step

If raw IQ is mandatory, move to a chip/platform with a documented DAC or
template-RAM interface. The closest Realtek precedent in this workspace is the
RTL8922 work (`ref/rtl8922-sdr`), which demonstrated bounded complex-envelope
transmission through calibration/NCO controls but still no general raw-DAC
replay. On the 8822B, the remaining practical options are CW/OOK, the fixed
0x914 test waveform, and 802.11 frame injection.

## Biggest unknown

Whether the 8822B mask ROM contains an undocumented test path that feeds the
DAC from a writable buffer. The vendor driver exposes none, the hardware probes
above found none, and the custom-firmware follow-up (including the 5 M writes/s
WCPU benchmark and the `0x910` comb steering) found no such port. Resolving the
remaining ROM question would require silicon-level reverse engineering, not
another register-level experiment.
