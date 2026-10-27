# TX capability research: RTL8812BU (RTL8822B)

Goal: find what arbitrary-waveform TX the 8822B silicon has beyond the CW tone. Ultimate target: host IQ samples reaching the DAC.

Rules:
- TX only on the cabled bench (`lab/test-setup.conf` must say `cabled=yes`).
- Only channels in `lab/allowed_channels.txt`, at the lowest measurable power.
- At most 10 s per run, with TX disabled on exit.
- RAM-only register writes, every one logged.
- Every claim backed by an RX capture in this folder, together with the script that made it.

Status tags: [V] verified on hardware with evidence · [S] read in source · [H] hypothesis · [X] dead end.

## 2026-10-26: setup

- **Hardware [V] (lsusb):** one RTL8812BU (bus 6, the RX receiver) and the 8812CU on bus 1, which is never opened.
  - **No TX adapter attached.** The second 8812BU (bus 8 earlier) is unplugged.
- **Lab config:** `lab/test-setup.conf` and `lab/allowed_channels.txt` are created as templates.
  - `cabled=no` and the channel list is empty, so no TX can run until the owner fills them in.
- **Prior work:** `notes/tx-feasibility.md` (8822C, an OpenCode session) found no DAC playback path from a review of the MP API. Treated here as a hypothesis for the 8822B.

## Phase 1: the existing CW tone

- [S] devourer `src/jaguar2/RtlJaguar2Device.cpp` `StartCwTone()` (port of vendor `hal_mpt_SetSingleToneTx`, 8822B branch):
  - BB 0x808[29:28] = 0 (OFDM + CCK modulators off);
  - RF A 0x00[19:16] = 2 (TX mode), 0x00[4:0] = gain;
  - RF A 0x58[1] = 1 (LO enable);
  - RFE pinmux 0xCB0/0xEB0 = 0x77777777, 0xCB4/0xEB4[15:0] = 0x7777, inverse 0xCBC = 0xb, 0xEBC = 0x830.
  - [H] So the tone is the analog LO at the channel centre (LO leakage with the modulator off), not a digital tone. The registers that affect it are the channel (LO frequency), RF 0x00 gain, RF 0x58[1] (on/off) and the RFE pinmux.
  - To verify on the bench: offset 0 from the LO; no image or digital spurs; the offset doesn't move with any BB register.

## Phase 3: source review (running)

- Review of vendor phydm/halrf for tone generators, DC injection and playback: notes in `source_review.md`.

## 2026-10-27: bench armed (supersedes the 2026-10-26 setup note)

- **Hardware [V]:** TX RTL8812BU on USB bus 7 port 1 (Bus 007 Device 008, USB 2.0); RX RTL8812BU on bus 6 port 1 (Bus 006 Device 006, USB 3.0); 8812CU on bus 1 is never opened. `lab/test-setup.conf` now says `cabled=yes`, `tx_adapter=Bus 007.Port 001`, `rx_adapter=6-1`, `attenuation_db=50`, `tx_paths=AB`; `lab/allowed_channels.txt` = 36, 48, 161. No antennas.
- **Guard [V]:** `wsdr_txprobe` refuses channel 1 (not allowed) and bus 6 (not the TX adapter) before opening the device. `reset` (libusb_reset_device on bus 7 port 1) returns `{"ok":1}`.
- **Harness [V]:** `src/wsdr_txprobe.cpp` (modes cw/cont/ook/raw/reset) + `tx_research/tx_run.py`. The supervisor validates the lab files, checks sysfs IDs (0bda:b812 on 7-1 and 6-1), takes `tools/coord.sh lock 8812bu`, brings TX up first (no RF), starts RX, collects a baseline, then arms TX over stdin. Every run is killed at the deadline; the adapter is reset in `finally`, and the reset is logged. Register writes go to `tx_research/regs/*.jsonl` and are restored in reverse order. Each run's commands, TX/RX output and reset status are in `tx_research/logs/*.log`.
- **Bring-up timing [V]:** ~4 s wall for the TX adapter (firmware + BB/RF tables); the RX streamer needs ~4 s before its first samples. Hence the ready-file/stdin handshake and the RX sample-file poll.

## Phase 1: CW characterisation (verified)

- [S] The vendor 8822B single-tone path (`hal_mpt_SetSingleToneTx`, `ref/88x2bu-20210702/hal/hal_mp.c:1784`) and devourer's identical port (`RtlJaguar2Device.cpp:859`) leave the OFDM/CCK modulators off, put RF path A in TX mode, enable the LO (RF 0x58[1]) and force the RFE pinmux. It is an **analog LO carrier**, not a baseband tone.
- **[V] CW at the lowest gain:** `tx_research/captures/cw-ch36-g0-20261027c-rx.cs16` (run `cw-ch36-g0-20261027c`, ch36, RF gain 0, 2 s, 50 dB, RX 2.5 MS/s).
  - Tone at **+62.4 kHz** from the RX LO (LO offset between the two adapters), on-burst SNR **80.3 dB** above the median floor, on-RMS 1240.5 vs off-RMS 53.1 (**+27.4 dB**).
  - Burst exactly **2.005 s** long in the capture; peak drifts ~610 Hz over the burst (independent LO drift).
  - Register journal shows 10 arm writes, 2 key-down/up writes, 9 restores; RF 0x58 bit 1 is set to 0 before the restore and `lo_off=1` at exit.
  - Earlier run `cw-ch36-g0-20261027b` (no pre-roll) repeats it: 2.005 s burst, +30.5 dB on/off, peak +64.2 kHz, SNR 81.4 dB.
- [V] On/off is controlled by RF 0x58[1]; the carrier disappears completely when it is cleared (OOK off-RMS 4.6 vs on 1251, 48.7 dB power ratio).
- [H] Amplitude is RF 0x00[4:0] (gain index); phase has no register in this path (LO phase is free-running). Frequency is the channel LO only; changing it means a synthesizer retune, not a phase-continuous step.
- [V] `wsdr_txprobe` reported 21 writes for the CW run and `lo_off=1`; the supervisor's post-run reset also returned ok.

## Phase 2: register-level keying (verified so far)

- **[V] OOK via RF 0x58[1] at 1 kHz:** `tx_research/captures/ook-lo-1k-ch36-g0-20261027a-rx.cs16` (pattern 1010, 3 s, gain 0).
  - 3000 edges inside a 3.01 s burst => **996.6 toggle/s** measured, 50.1% duty, on/off ratio 351 (linear magnitude), burst spectrum peak +61.8 kHz, SNR 63.5 dB.
  - TX journal: 3000 edges in 3.0057 s (998.8 Hz mean, 1.001 ms mean interval, p90 1.002 ms, max 5.25 ms); 6037 EP0 transfers => ~2.0k transfers/s.
- **[V] OOK at 2 kHz requested:** `ook-lo-2k-ch36-g0-20261027a` achieved **1326.4 toggle/s** (2665 edges in 2.007 s), so the sustained read-modify-write limit is ~1.33 kHz (~0.75 ms per edge, two control transfers per edge). 1 kHz is the highest cleanly met request so far.
- [S]/[H] No writable phase or frequency register for this carrier has been found; the only frequency control is the channel LO. OOK is real; PSK/FSK at sample rate is not available through this path.

## Phase 3: source review (running)

- See `tx_research/source_review.md` for the register/function evidence on CW, 0x914 test modes, IQK tone engine, CFIR RAM, PSD, DPK and LA. No host-accessible DAC sample buffer has been found yet; the IQK tone engine is the remaining register-level lead.

## Phase 2: 0x914 continuous and single-carrier/tone modes (verified limits)

- **[V] Continuous OFDM test mode works once the MAC PMAC and TX-OFDM enables are included.** The vendor `mpt_StartOfdmContTx` alone sets `0x914[18:16]=1`; on this adapter that is not enough. Adding `0xb00[8]=1` (PMAC on) and `0xb04[3:0]=4` (TX OFDM on) produces a **100 %-duty modulated signal**:
  - `tx_research/captures/cont-ch36-20261027b-rx.cs16` (run `cont-ch36-20261027b`, ch36, 1.5 s, TX power default, 50 dB, RX 20 MS/s).
  - Burst exactly **1.500 s**, on-RMS 1111 vs off-RMS 74.6 (**+23.5 dB**), 99 % occupied bandwidth **19.0 MHz**, in-burst SNR 50.4 dB.
  - The 10 µs/100 µs envelope is flat (CV 1.5 %/0.9 %), so this is not a packet train; it is a continuous test waveform.
  - Source: `tx_research/regs/cont-ch36-20261027b.jsonl`; `wsdr_txprobe cont` now arms `0x838`, `0x800[25]`, `0xa00`, `0x914[18:16]=1`, `0xb00[8]=1`, `0xb04[3:0]=4` and stops with `0xb04=2`, `0x914=0`, PMAC off and a BB reset.
  - This is a **fixed internal test pattern**, not host IQ. Its content is not a programmable sample stream.
- **[V] Single-carrier mode (`0x914[18:16]=2`) did not produce a clean carrier:** `raw-singlecarrier-ch36-20261027a` showed only baseline spurs and a +1.1 dB "burst"; no usable signal above the RX floor.
- **[V]/[H] Single-tone mode (`0x914[18:16]=4`) produces a strong test signal but not a stable programmable tone:**
  - `raw-singletone-ch36-20261027a`, `st-igi80-ch36-20261027a/b`, `st-p0-igiA-ch36-20261027a` and `st-low1/low2` are the captures. With TX power index 0 and RX IGI 80 the on-period RMS is ~100-190 vs a ~20-30 baseline, so the mode is emitting.
  - The on-period spectrum is not one clean line: peaks at e.g. -1.82, -2.44, -1.19 and -8.07 MHz, with ~0.625 MHz spacing in several runs. The observed peak moves between runs and does not vary linearly with `0x914[15:0]` (low=0 and low=2 both showed ~-1.82 MHz; low=1 showed ~-2.44 MHz in a truncated capture).
  - The RX AGC stays elevated for >1 s after TX stop, so post-TX spectra are not a clean baseline. `st-igi80`/`st-p0` should be read only as "mode 4 emits a broadband/comb test signal", not as a measured single tone.
  - No writable phase or per-sample amplitude control has been identified for this mode; it cannot supply arbitrary IQ.
- **[V] IQK tone-engine test (the remaining register-level lead) did not yield a held tone.**
  - Attempts: `tone-hold-i8-ch36-20261027a` (selfcheck-style setup), `tone-full-i8-ch36-20261027a`, `tone-full-lo-i8-ch36-20261027b` (full IQK AFE/RFE/RF prep plus RF TX mode/LO), and `raw-tone-start-i8-ch36-20261027a` (`0x1b34=0x801` only).
  - No on-channel tone was visible above the floor during the hold in any run; the only transient was a short calibration/prep burst at the arm boundary. The TX journals show the one-shot writes and the final LO/test-mode clear.
  - This is consistent with the source: `_iqk_tximr_selfcheck_8822b` is a one-shot TX-IMR measurement; `_iqk_start_tximr_test_8822b` is disabled (`#if 0`) in the vendor IQK flow, and `0x1b20/0x1b24` carry a calibration tone index, not a stream pointer.
- **Safety note:** the first tone attempts left RF 0x58[1] set because the pre-run state already had it set; `wsdr_txprobe` now always clears RF 0x58[1] on both paths, clears `0x914`, stops `0xb04`, disables PMAC and pulses the BB reset at exit, then the supervisor resets the adapter. The `lo_off=0` runs above predate that fix and are retained as failed experiments, not as accepted results.

## Phase 3: IQ-injection conclusion (hardware + source)

- **[X] No host-accessible arbitrary-IQ/DAC playback path was found on the RTL8822B.** The TX FIFO feeds the 802.11 MAC/PHY packet formatter; the 0x914 modes generate fixed internal test signals; the IQK/DPK blocks are calibration coefficient/scalar engines; the LA SRAM is an RX capture buffer. There is no writable waveform RAM, sample FIFO, replay pointer or sample clock register in the vendor driver, and the hardware tests above did not expose one.
- The practical ways to put energy on the air remain: analog CW/OOK (verified), the fixed 0x914 continuous test waveform (verified), and 802.11 frame injection (not the requested IQ path). None of these accepts host IQ samples at a sample rate.
- A true arbitrary-IQ path would require a different chip with a documented DAC/template-RAM path (the RTL8922 work is the closest Realtek example, and even there only bounded calibration/NCO envelopes were demonstrated), or custom firmware/FPGA work, which is outside the no-firmware-modification rule for this bench.


## 2026-10-27: custom-firmware/ROM follow-up (no-firmware rule lifted)

- [V] `src/fw/txbench8822b.S` + `wsdr_txprobe fwbench`: the 8822B WCPU wrote BB
  `0x1bcc` 1,000,000 times in 200,033 us, i.e. **~5.0 M writes/s**
  (`tx_research/logs/20261026-233311-fwbench.log`). The embedded CPU is fast
  enough to drive a register-fed source.
- [V] `0x910[6:0]` steers the `0x914[18:16]=4` test signal. Host raw runs with
  index 0/8/16 gave dominant lines at -1.818/-3.068/-4.318 MHz
  (`st-910-08`, `st-910-10`), but the on-period spectrum is a comb of lines
  spaced ~0.625 MHz, not a single tone.
- [V]/[X] Live updates are not clean. Firmware (`fwfsk-*`) and host (`fsk-*`)
  writes to 0x910 while the mode is active produced mixed/spurious spectra;
  stop/re-arm of 0x914 or 0xb04 added transients. No repeatable clean FSK.
- [X] Even with firmware, no writable sample memory or DAC sample port was
  found. The WCPU can only toggle/steer the existing test comb; arbitrary IQ
  remains a dead end on the 8822B.
