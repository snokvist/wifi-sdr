# C2H large-packet wedge — progress and handoff (2026-10-25)

> **RESOLVED (2026-10-25, later the same day).** The wedge is the host port: on xHCI 0000:65:00.3
> (AMD 1022:15b9) large C2H packets lock the chip's RX DMA → USB 3 bulk-IN path, even with rtw88 owning
> the chip; on 0000:67:00.3 (AMD 1022:15c0) the same adapter streams 20 Msps and 40 Msps for 60 s with
> zero faults. USB 2 works on any port. The sections below are kept as the record of the investigation;
> their hypotheses and experiment list are superseded. See FINDINGS §4 and REPORT.md §3 (Phase 3).

This note is a self-contained brief for a fresh investigator (Kimi K3.1 Pro, with Flash subagents if
useful). It records what was built, what is proven, what has been ruled out, and the experiments most
likely to unblock continuous IQ streaming from the RTL8822B.

## 0. TL;DR

- **The C2H transport works and is fast enough.** On-chip latency for a 16 KiB C2H packet is
  **38–46 µs** (min/p50/p99/max = 38/41/45/46), well inside the 204.8 µs opposite-bank window at
  40 Msps. One 1024-request run completed entirely (1024/1024, 387 MB/s).
- **Sustained 16 KiB C2H is not reproducible.** Runs wedge after a random 3–1563 requests. Two
  signatures: (a) the request trigger at `0x294` BIT16 stays set and no packet arrives; (b) the
  trigger clears but the packet never reaches the host. Only a fresh process/bring-up recovers.
- **It is not the host loop or the benchmark's synthetic framing.** The embedded CPU issuing the
  requests (no host control traffic between requests) wedges too. Host-driven with a single
  outstanding URB, a unique payload per request, and no trigger poll also wedges.
- **It is not the source SRAM or address reuse.** RX SRAM and both TX-bank aliases (the real
  streamer's source) wedge; rotating over 3–4 different 16 KiB addresses wedges.
- **Payload size is the strongest correlate.** 256 B and 1024 B were 2000/2000 (two runs each);
  4 KiB and 8 KiB were flaky (one pass, one fail each); 16 KiB almost always fails early.
- **Recommendation:** do not keep the continuous 20 Msps streamer on the critical path while this is
  unresolved. Move the already-validated **snapshot-mode front end** (Phase 4's first bullet) ahead of
  Phase 3 completion. Run the C2H investigation as a parallel research lane. If C2H is reliable on
  **USB 2**, ship a 5 MS/s continuous mode as the intermediate milestone.

The top experiment is deliberately cheap: **replug the 8812BU with the rtw88 guard ON so it
enumerates at 480 Mbps, then repeat the 16 KiB C2H test**. Devourer does not perform the USB2→USB3
switch (the kernel driver did it before it was blacklisted); a replug therefore gives a USB2 link with
no code change, no USB3 link power management, and a different host controller path.

## 1. Context, hardware and constraints

- Workspace: `/home/snokvist/wifi-sdr` (separate git repo; devourer is a subdirectory dependency from
  `~/dev/snokvist/devourer`). Two agent sessions share it via `coord/`:
  - Protocol: `coord/README.md`. Messages are append-only `coord/from-opencode.md` (opencode) and
    `coord/from-claude.md` (Claude). Locks: `tools/coord.sh lock|unlock|say|locks`.
  - Resources: `8812bu` (0bda:b812), `8812cu` (0bda:c812), `rtw88-guard`, `git`.
  - Build dirs: Claude uses `build/`; opencode uses `build-opencode/`. Do not build in the other's.
- Hardware:
  - `0bda:b812` — "AC1200 Techkey", **RTL8822B cut 3 (UMC), 2T2R, rfe_type 3**, currently USB3
    (5000M, bus 2 port 1, `0000:65:00.3`). This is the C2H DUT.
  - `0bda:c812` — RTL8812CU / RTL8822C cut 5, USB2 (480M, bus 5 port 1, `0000:67:00.3`). Not used
    in this work; its LA snapshot readback is ~12.4 s per 16K samples, so it is not a snapshot
    viewer target either.
- `tools/rtw88_guard.sh status` is **ON**: the in-tree `rtw88_8822bu/cu/21cu` and the DKMS `88x2cu`
  are temp-blacklisted. Keep it on. Restore with `sudo tools/rtw88_guard.sh off` only when done.
- Bench rules (`HUMAN-INVESTIGATION.md`): on-chip code is RAM-only (no efuse/flash/VID/PID writes);
  snapshot and restore every register touched; recovery ladder is `libusb_reset_device` → `usbreset`
  → VBUS cycle/replug; RX is the product, TX only for test signals at minimum power; never touch the
  host uplink `enp3s0`; restore the driver guard afterwards.
- ac54's ASUS USB-AC54 is the **same die/cut (8822B cut 3)** but rfe_option 2 (ours is 3). Their
  streamer worked at 20 Msps on Windows/WinUSB RAW_IO. Their code has **no license**: use
  `notes/ac54-analysis.md` as documentation of register/protocol facts only, never copy code.
- `sudo -n true` works. There is no uhubctl-capable hub, so a true VBUS cold needs a manual replug.

## 2. Code delivered (uncommitted)

All paths are under `/home/snokvist/wifi-sdr`.

| File | Purpose |
|---|---|
| `src/fw/c2hbench8822b.S` | MIPS16 on-chip latency bench. Linked at `0x80000230` behind the vendor prologue. DMEM block at `0x80201000`: magic `0x0C2B0001`, issued, done, error, t0, N at `+24`, durations at `+0x40`. Writes request `0x11000` to `0xb8640294`, polls BIT16, times with `0xb8640568`. 112 bytes. |
| `src/c2h88xx.h/.cpp` | `C2hPacket` (24 B RX descriptor + xorshift PRBS payload; `make`, `verify` bytes 0..11 + payload), `c2h_request(addr)`, `c2h_stage`, `C2hRxPath` (snapshot/apply/restore of `0x0100/0x0608/0x06A0/2/4/0x0290/0x010C/0x07C0/0x0601/0x029C/0x02A0`), `C2hReader` (async bulk EP `0x84`, 1..64 URBs, 500 ms per-transfer timeout, verified/stray/timeout/error counters). |
| `src/wsdr_probe.cpp` | New `c2h` and `c2hbench` commands. `c2h` flags: `--source rx\|tx0\|tx1`, `--bytes`, `--depth`, `--xfer`, `--count`, `--gap-us`, `--no-poll`, `--retry-ms`, `--abort`, `--restage`, `--check-desc`, `--cr`, `--rx-off`, `--rx-on`, `--agg-th`, `--fwff-off`, `--rxdma-mode`, `--no-lpm`, `--vary`, `--rw-release`, `--unique`, `--no-thermal`, `--halt-cpu`. |
| `src/session.h/.cpp` | `OpenOpts.thermal_track` (default true) → `DeviceConfig::tuning.thermal_track`. |
| `CMakeLists.txt` | `add_fw(c2hbench8822b)`, `src/c2h88xx.cpp` in `wsdrcore`. |

Build and run:

```sh
cd ~/wifi-sdr
cmake -S . -B build-opencode -DCMAKE_BUILD_TYPE=Release
cmake --build build-opencode -j --target wsdr_probe
tools/coord.sh lock 8812bu opencode "C2H test"
sudo -n env DEVOURER_LOG_LEVEL=silent build-opencode/wsdr_probe \
  c2h --pid 0xb812 --source rx --count 1600 --depth 16 --no-thermal \
      --xfer 16384 --bytes 16352
tools/coord.sh unlock 8812bu opencode
```

`c2h` emits one `c2h.host` line on success or `c2h.fail` with `completed/desc_bad/stray/timeouts/
retries/r294/r288/r280/r10c/r284`, then a diagnostic on stderr. `c2hbench` seeds N in DMEM, starts the
CPU, waits for `done`, halts it, reads the duration array, and prints min/p50/p90/p99/max/mean,
`over_204us`, wall time and MB/s. Exit code is 0 only if magic, done, issued and verified all match.

Evidence already in the tree:
- `evidence/phase3-c2h-bench.jsonl`, `evidence/phase3-c2h-bench-durations.csv` — the clean 1024 run.
- `evidence/phase3-c2h-tail.jsonl`, `evidence/phase3-c2h-tail-r1..r10.csv` — ten 1600-request runs.
- `evidence/phase3-c2h-wedge-matrix.txt` — the newer matrix (TX aliases, varying addresses, unique
  payloads, RW_RELEASE, RXDMA modes, LPM).

`git status` at handoff: modified `CMakeLists.txt`, `src/session.{h,cpp}`, `src/wsdr_probe.cpp`;
untracked `src/c2h88xx.{h,cpp}`, `src/fw/c2hbench8822b.S`, the new evidence files and this note. The
code is not committed; use the `git` lock and a clear message if you commit it.

## 3. What is verified

- **Host C2H smoke test** (8 requests, 16352 B, channel 36): RX 8/8, TX0 8/8, TX1 8/8, zero
  stray/timeouts; ~114/130/138 MB/s. **Both ac54 TX→C2H aliases hold on this cut.**
- **On-chip latency** (one clean 1024-request run, 16 KiB): 1024/1024 verified, min 38, p50 41, p90
  43, p99 45, max 46 µs, mean 41.4, wall 43.2 ms, 387 MB/s, `over_204us=0`, stray 2. This proves the
  per-packet latency and the CPU loop; it is **not** a reproducible sustained result.
- **Small packets are reliable**: 256 B and 1024 B both 2000/2000 in two runs each (~68 ms per 2000;
  ~7.5 / 30 MB/s).
- **The LA path and snapshot path are independent and working.** `wsdr_probe snap` (819 µs capture,
  ~12.5 ms EP0 readback per 16K samples) remains the known-good IQ source. `La8822b::snap()` exists
  and is the basis for the recommended snapshot front end.

## 4. The failure

At 16 KiB payload (frame 16376 B), the engine stops after a random number of requests. Exact
signatures:

| Signature | Registers | Meaning |
|---|---|---|
| (a) trigger stuck | `r294 = 0x00011000` (RX), `0x01016000` (TX0), `0x00016000` (TX1), or the current varying-slot request, BIT16 set | The engine never accepted/cleared the request; no packet. |
| (b) packet lost | `r294 = 0x00001000` (BIT16 clear) | The request completed internally, but no bulk-IN packet arrived; the reader's URBs time out. |
| overflow after retry | `r288` becomes `0x00000080` (BIT_C2H_PKT_OVF) | Only seen when the test retries a stuck request; retrying does not recover. |
| normal failure state | `r280=0x00000a03`, `r10c=0x0000f5a0`, `r284=0x00000000`, `r288=0` | No other obvious error bit. |

- `desc_bad` is always 0: descriptor bytes 0..11 are never corrupted. The payload is intact whenever a
  packet arrives (`C2hPacket::verify` checks bytes 0..11 and the full payload).
- **Recovery:** abort+write 0+retrigger, re-writing the same request, gap 1 ms, retry 10 ms, and
  descriptor restage all fail. Only a fresh process (new `CreateRadio`/`InitWrite`) recovers.
- **Rate:** failures at 3, 13, 26, 47, 63, 70, 72, 77, 99, 100, 128, 132, 166, 248, 252, 286, 321,
  331, 381, 405, 428, 469, 500, 655, 720, 797, 813 and 1563 requests across runs. A 20 Msps stream
  needs 5 packets per 409.6 µs bank; a wedge after ~100 packets is ~8 ms of data. This is unusable.

## 5. What has been tested and ruled out

Do not repeat these without a new reason. All are negative for the 16 KiB wedge.

- Host-driven vs embedded-CPU-driven (with `--halt-cpu` and with the bench CPU program).
- RX SRAM source (`0x1000`, offsets `0x2000/0x4000/0x8000/0xC000`) and TX0/TX1 bank aliases.
- Address reuse vs rotating 3 RX / 4 TX 16 KiB slots.
- Identical payloads vs a fresh unique payload per request (rules out stale/duplicate completion
  ambiguity).
- Queue depth 1, 2, 4, 16, 32-ish, 48; transfer size 16 KiB and 64 KiB.
- Trigger poll vs no-poll (membench-style request → bulk wait).
- Descriptor restage, `--check-desc`, `--gap-us 1000`, `--retry-ms 10`, `--abort`.
- MAC RX on (`0x100 |= 0x8a`, `0x608 |= 0x80000`, filters zeroed) vs RX off (`0x100 &= ~0xC0`), and
  forced `REG_CR=0x3f`.
- RX aggregation low-byte threshold 5, 0, 255 (vendor USB3 value is `0x0a05`; devourer leaves
  `0x0a03`).
- FWFF off (`0x601` BIT_FWEN clear, `0x29c=0x2a0`) — the drive-preserved firmware packet engine.
- RXDMA mode `0x290` = 14, 12, 2, 0.
- USB LPM clear (`CLEAR_FEATURE` 48/49).
- Thermal-track thread off (`--no-thermal`).
- `RW_RELEASE_EN` (`0x0284` BIT18): baseline is 0; forcing 1 sets `RXDMA_IDLE` and **blocks C2H
  entirely** (0 completions). So C2H needs the RX DMA released/running, and the failure is not a
  paused RX DMA.
- U1/U2, thermal, host controller link errors are **not** proven; see experiments.

## 6. Technical facts worth keeping

**Request encoding and descriptor.**
- `REG_C2H_PKT = 0x294`: `BIT_R_C2H_PKT_REQ = BIT(16)`, `R_C2H_STR_ADDR = bits 15:0`,
  `R_C2H_STR_ADDR_16_TO_19 = bits 27:24`.
- `c2h_request(addr) = (addr & 0xFFFF) | ((addr >> 16) << 24) | 0x10000`.
  RX `0x1000` → `0x11000`; TX0 `0x16000` → `0x1016000`; TX1 `0x6000` → `0x16000`.
- 24-byte RX descriptor: `p[0..1]` = **payload** length (14-bit, max 0x3FFF), `p[3]=0x80` OWN,
  `p[11]=0x10` C2H. The chip emits **descriptor + payload** (`payload + 24` bytes; 16376 for a 16352
  payload). Hardware mutates bytes 12..23, so verify only 0..11 and the payload.
- `REG_RXDMA_STATUS = 0x288`: `BIT_C2H_PKT_OVF = BIT(7)`.
- `REG_RXPKT_NUM = 0x0284`: `RXDMA_IDLE = BIT(17)`, `RW_RELEASE_EN = BIT(18)`.
- `REG_WMAC_FWPKT_CR = 0x0601`: `BIT_FWEN = BIT(7)`. `FWFF_C2H=0x298`, `FWFF_CTRL=0x29c`,
  `FWFF_PKT_INFO=0x2a0`, `REG_FWFF=0x1114`. `REG_FREERUN_CNT=0x568` (µs).
- `REG_RXDMA_MODE=0x290`, `REG_RXDMA_AGG_PG_TH=0x280` (low byte size, next byte timeout,
  `EN_PRE_CALC=BIT(29)`), `REG_TXDMA_PQ_MAP=0x010C` (`BIT_RXDMA_AGG_EN=BIT(2)`).

**Devourer bring-up state.** `InitWrite` leaves `REG_CR=0x06ff`, `0x0280=0x0a03` (deliberately
lowered from vendor `0x0a05`), `0x0290=0x0e`, `0x010C` bit2 set; `set_trx_fifo_info` preserves the
vendor firmware's FWFF engine if enabled. `C2hRxPath::apply` writes `0x0290=14`, clears `0x010C`
bit2, stops the LA ring (`0x7c0=0`), and optionally sets the ac54 streamer RX-on state. The 2 s
thermal tick is a devourer thread; `--no-thermal` disables it.

**Vendor USB RX aggregation** (`halmac_usb_88xx.c::cfg_usb_rx_agg_88xx`): mode none clears
`0x010C` bit2; DMA mode sets it and `0x0280` byte3 bit7; USB mode sets it and clears byte3 bit7.
USB3 threshold defaults are size 5 / timeout 0xA; the 32-bit `0x280` carries `EN_PRE_CALC`. Only the
low byte of `0x280` was varied in our tests; a full 32-bit experiment is still open.

**ac54 facts (same cut).** Two 64 KiB TX banks, OCP `0x187A0000`/`0x187B0000`; direct C2H alias
`TX 0x20000..0x2ffff → 0x16000..0x25fff`, `TX 0x30000..0x3ffff → 0x06000..0x15fff`; C2H latency
normally 38–45 µs, outliers 325/369 µs; 20 Msps 60 s on Windows/WinUSB RAW_IO; 40 Msps only 0.66 s
bursts; QDEPTH=32; host buffers 16384; 4 overlapping chunks + trailer per bank; largest packet
16376 B to avoid a zero-length USB completion. Their engine did **not** wedge; ours does.

## 7. Hypotheses, ranked

1. **USB3-specific transport/board issue.** The same cut works on ac54's ASUS over USB3/Windows. Our
   cheap Techkey board, its USB3 SS link, or the Linux xHCI/libusb scheduling may be the difference.
   The USB2 test (E1) and another controller/board (E2/E6) separate these.
2. **Devourer init state differs from ac54's rtw88 init in a way that matters only for large
   packets.** A full register diff (E3) and an "init with rtw88, detach, open without reset" test are
   the direct checks. The payload-size dependence is consistent with a FIFO/credit or DMA threshold
   difference.
3. **C2H engine/FIFO large-packet limitation on this specific board** (signal integrity, USB PHY,
   board layout). Another 8822BU board (E6) is the clean test.
4. **RX FIFO/aggregation/DMA interaction.** Large C2H packets share the RX FIFO/DMA; a full 32-bit
   `0x280` = 0 (and vendor `0x0a05`), `0x284` flushes, and a full RX-DMA stop/start may help. This is
   the least explored register cluster.
5. **Host scheduling / URB completion.** A sync `libusb_bulk_transfer` loop, `usbmon`, xHCI event
   counters, URB sizes 8/16/32 KiB, CPU affinity/RT priority. The on-chip bench wedge weakens this,
   but the host still reaps the completions.
6. **Thermal / elapsed time.** The one clean 1024 run was early; later runs failed. The thermal meter
   was disabled in all matrix runs. Log `thermal` events (remove `--no-thermal`,
   `DEVOURER_THERMAL_POLL_MS=250`) and repeat cold vs warm. Unlikely but cheap.
7. **Firmware/FWFF interference.** The CPU was halted and FWFF was cleared in tests, so low.

## 8. Recommended experiments, in order

**E1 — USB2 first (decisive, no code).**
1. Keep `rtw88_guard.sh on`; unplug/replug the 8812BU (or VBUS-cycle it). It should enumerate at
   480 Mbps because no driver performs the USB2→USB3 switch. Confirm with `lsusb -t` / sysfs
   `speed`; it may re-enumerate as `0bda:b812` on bus 2.
2. Run the 16 KiB C2H test and the 1024 bench:
   `c2h --source rx --count 1600 --depth 16 --bytes 16352 --no-thermal` and
   `c2hbench --count 1024 --depth 16 --bytes 16352 --no-thermal`.
3. Measure 1/2/4/8/16 KiB at 1600 each. If USB2 is reliable, the chip's C2H engine is fine and the
   problem is USB3/host-side. Record the throughput ceiling: USB2 ~40 MB/s → 10 Msps complex max,
   ~5 MS/s comfortable. **If reliable, this is the intermediate product**: a 5 MS/s continuous SDR.
4. Restore USB3 by replugging with a driver probe (or the documented USB3 switch) when done.

**E2 — Different host controller/port.** Test the 8812BU on the 8812CU's port/controller
(`0000:67:00.3`, USB2 only) and, if available, on another USB3 controller (PCIe card / front-panel
port). Capture `dmesg` and `usbmon` for a failing run.

**E3 — Register-state diff.**
- Add a `regs` command or use `rtlreg` to dump the full `0x0100..0x0300`, `0x0600..0x0700`,
  `0x1000..0x1300`, and USB3 interface/PHY registers (`0x10c3`, `0xc4`, `0xc5`, `0x04`, `0xf050`)
  after (a) devourer `InitWrite`, (b) a cold rtw88 probe (temporarily unblacklist under the
  `rtw88-guard` lock; the driver may switch the chip to USB3), and (c) ac54's documented init facts
  where available. Diff. Snapshot/restore every register you touch.
- The strongest variant: let rtw88 probe/init the chip, detach the kernel driver, open with libusb
  **without** `libusb_reset_device` and without devourer re-init (a "register-only" session), then run
  the C2H test. If it works, the difference is devourer's init, not the silicon.

**E4 — Find a wedge recovery that does not re-open the device.** Try, one at a time: write 0 to
`0x294`, `0x29c=0x2a0`, clear/set `0x601` BIT_FWEN, `0x0284` BIT18 pulse, `0x0290` cycle, `0x010C`
bit2 cycle, `0x0100` CR cycle, `0x1082`/`0x09` platform reset (with the LA stopped), and a USB
`libusb_reset_device`. A streamer watchdog needs this. If none works, the only recovery is a full
re-init (~1 s), which rules out sustained streaming after any wedge.

**E5 — 8822C C2H isolation.** If the 8822C's C2H block at `0x294` works reliably at 16 KiB over USB2,
the problem is 8822B/USB3; if it also wedges, the host/libusb/xHCI side is implicated. The 8822C
memory map/aliases differ; verify before requesting.

**E6 — Another 8822BU board.** A TP-Link Archer T3U, ASUS USB-AC54, or any 8822BU dongle would
separate board/USB3 signal integrity from the chip. This is the one hardware need (see §11).

**E7 — Formalize the size/latency curve.** For 1/2/4/8/12/16 KiB: 2000 requests each, 5 runs,
record verified count, latency percentiles, and wedge index. Also log the thermal meter and run one
cold (idle 5 min) vs warm pair per size.

**E8 — Only then** reproduce ac54's actual bank-scheduled streamer (four chunks + trailer) as a
diagnostic. If E1–E7 do not yield a reliable transport, do not build the full streamer.

## 9. Re-sequencing recommendation (holistic)

The project's own plan (`REPORT.md` §7, Phase 4) says: "rtl_tcp server: snapshot mode first,
continuous once Phase 3 holds." Make that explicit and move it ahead of Phase 3 completion:

- **P0 (now): snapshot-mode front end.** A small TCP server that repeatedly calls `La8822b::snap()`
  and streams/decimates IQ (a SpyServer-lite or a simple raw-IQ protocol), plus a `.cs16` + JSON
  sidecar recorder. This is the only path that yields a user-visible SDR (SDR++/gqrx/waterfall)
  today. It uses the verified snapshot path and does not depend on C2H. The prior attempt was stopped
  by a safety classifier mid-write; the work is ordinary (no register writes beyond the existing
  snapshot logic) and should be done in small, reviewable steps. Acceptance: SDR++ or a local viewer
  shows a live waterfall at ~10–17 snapshots/s, with frequency/IGI controls and a recorder.
- **P1: USB2 continuous mode, if E1 passes.** 5 MS/s (20 MB/s) over USB2 is a genuine continuous SDR
  and a much stronger deliverable than snapshots. Reuse the direct-C2H bank scheduler; start with
  2.5 MS/s and scale to the measured USB2 ceiling.
- **P2: C2H/USB3 research lane** (this note). Run in parallel with P0/P1; do not block them.
- **P3: 20/40 Msps streamer** only after the transport is stable. Note that even ac54 saw 40 Msps
  fail after 0.66 s from a 369 µs tail outlier; 20 Msps is the realistic target.
- **Deprioritize the 8812CU port.** Its LA readback is 12.4 s / 16K samples on USB2 and its streaming
  entry contract/memory map is separate work. It is useful only if E5/E6 need a second host path.
- **Tuning table** can proceed with snapshots (per-channel IGI) and is independent; do it after the
  viewer, not before it.

The one thing **not** to do is keep iterating the synthetic C2H bench. It has already established the
latency and the wedge; the remaining work is hardware/init/host isolation.

## 10. Suggested fan-out for Flash agents

Each agent should work in `build-opencode/` unless it owns a different build dir, take the appropriate
coord lock, and write evidence under `evidence/phase3-c2h-*`. They must not copy ac54 code.

| Agent | Task | Inputs | Output / acceptance |
|---|---|---|---|
| A (hardware) | E1 USB2 replug test; E2 alternate controller/port; record throughput/latency/wedge. | this note, `build-opencode/wsdr_probe`, `rtw88_guard` | `evidence/phase3-c2h-usb2-*`, a table, and a go/no-go for P1. |
| B (vendor source) | Audit halmac/rtw88 for C2H preconditions, max payload, USB3 interface/burst/FIFO setup, and any `0x294`/FWFF/RXDMA sequencing. | `ref/devourer/reference/rtl88x2bu/hal/...`, `ref/88x2bu-20210702` | A fact list with file:line; no code changes; flag any register devourer doesn't set. |
| C (ac54 docs) | Re-read `notes/ac54-analysis.md`, `HANDOFF.md`-derived facts, and ac54 source for any wedge, recovery, size limit, URB/queue detail, or USB3 switch state; distinguish facts from guesses. | `notes/ac54-analysis.md`, `ref/ac54_sdr` (read-only) | A delta list of facts not in this note; explicit "no license" reminder. |
| D (host/USB) | E3/E5 host side: `usbmon` of a failing run, xHCI errors, sync bulk loop, URB sizes 8/16/32 KiB, CPU affinity/RT. | this note, `usbmon`, `dmesg` | A trace + a verdict on host scheduling vs device. |
| E (front end) | P0 snapshot server + recorder. | `src/la88xx.*`, `src/wsdr_probe.cpp`, `tools/coord.sh` | A working TCP IQ/SpyServer-lite feed and `.cs16` recorder, plus a README section. |
| F (register diff) | E3 dump/`regs` command and devourer-vs-rtw88 diff. | `src/rtlreg.*`, devourer `InitWrite`, kernel driver | A diff table and the state that changes C2H behavior. |

## 11. Needs / open questions

- **N6 (new):** a second RTL8822BU board (TP-Link Archer T3U, ASUS USB-AC54, etc.) to separate board
  from chip. This is the highest-value hardware ask.
- **N3 (existing):** a uhubctl-capable hub for a true VBUS cold; not required for E1 (a replug works).
- A Windows host or a second Linux host with a different xHCI controller would test the host-stack
  hypothesis directly.
- N4 (ac54 license) remains: use as documentation only.
- Open: does the 8812BU's USB3 link enter U1/U2 during the 41 µs gaps? Does the cheap board's SS link
  have signal-integrity issues? Does a full 32-bit `0x280`/`0x284` flush change the wedge? Can the
  C2H block be reset without a full re-init?

## 12. Rules to preserve

- RAM-only on-chip experiments; snapshot+restore every register; no efuse/flash/VID/PID writes.
- Take the `8812bu` lock before opening the adapter; release it with a trap. Build only in
  `build-opencode/`. Keep `rtw88_guard.sh on` for C2H tests (it also prevents the driver from
  switching the chip back to USB3 after a replug).
- Never commit without the `git` lock and a clear message; the current work is uncommitted.
- Keep the host uplink `enp3s0` untouched; no regulatory/TX-power experiments.
