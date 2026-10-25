# STATUS

_Last updated: 2026-10-25 10:45 CET_

**Current phase:** Phase 3 on USB2 is done and Phase 4 has its first front end: **`wsdr_tcp` serves the
8812BU as an rtl_tcp device** (gqrx verified live), on top of the 2.5/5 MS/s continuous stream (60 s,
zero faults). 16 KiB C2H wedges on USB3, so 20/40 Msps still waits on the USB3 investigation. The 8812BU
is enumerated at 480M (1-1). Interim write-up: [REPORT.md](REPORT.md).

### Using it
```sh
cmake -S . -B build && cmake --build build -j
sudo build/wsdr_tcp --pid 0xb812 --freq-offset 5e9      # take the 8812bu lock first
gqrx: device "rtl_tcp=127.0.0.1:1234", Input controls → LNB LO 5000 MHz, then tune 5180 MHz
sudo build/wsdr_probe stream --pid 0xb812 --seconds 60 --out captures/x   # record .cs16 + .json
```
Clients that send 5 GHz as a wrapped u32 (gr-osmosdr with "No limits", custom tools) need no offset.

## Hardware

| Adapter | VID:PID | Chip | USB link | Port (sysfs) | devourer RX |
|---|---|---|---|---|---|
| "AC1200 Techkey" | 0bda:b812 | RTL8822B cut 3, 2T2R, rfe 3 | **5000M (USB 3)** | bus 2 port 1 | HEALTHY ([doctor ch1](evidence/phase0/doctor-8812bu-ch1.jsonl)) |
| RTL8812CU | 0bda:c812 | RTL8822C cut 5, 2T2R, rfe 3 | **480M (USB 2, bcdUSB 2.00)** | bus 5 port 1 | HEALTHY ([doctor ch36](evidence/phase0/doctor-8812cu-ch36.jsonl)) |

- No uhubctl-capable hub (NEEDS N3). Host uplink `enp3s0` untouched.
- **In-tree rtw88_8822bu/cu/21cu and DKMS 88x2cu are temp-blacklisted** (`tools/rtw88_guard.sh on`).
  Restore with `sudo ~/wifi-sdr/tools/rtw88_guard.sh off`, then replug or `modprobe`.
- Test signal: ambient Wi-Fi (ch36 AP). No controllable TX (NEEDS N2).
- devourer: local clone fast-forwarded to OpenIPC 0bf1957 (your OK), not pushed.
- Two sessions share the workspace. Before using an adapter, the driver guard or git, take the lock
  (`coord/README.md`, `tools/coord.sh`).

## What works (with evidence)
- **Phase 1, LA baseline on both chips:**
  - Layout: I[11:0]/Q[23:12], plus 8822C path B at Q[43:32]/I[55:44].
  - Orientation correct (L-LTF test, 13/13 detections).
  - Off-centre placement correct ([BU](evidence/phase1-bu-offcentre.png),
    [CU](evidence/phase1-cu-offcentre.png)).
  - The 8822C's two paths are phase-coherent after a fixed ≈7.4-sample A/B skew.

  Details: FINDINGS §2.
- **Phase 2, data path documented** (FINDINGS §3–4, `notes/ac54-analysis.md`). The "vendor IMEM" is
  the Linux firmware, byte-identical, not a Windows extraction.
- **Phase 3 groundwork, 8812BU** (FINDINGS §4 "Measured on our 8812BU"):
  - The free-running LA ring is live after devourer bring-up.
  - **Fixed-gain mode** (0x838[1] + CCK off + IGI): −0.94 dB per IGI step over ≈45 dB. ac54 lists
    this as open.
  - Contiguous snapshot capture (`wsdr_probe snap`): 819 µs per snapshot, ~12.5 ms readback.
  - **Custom code runs on the embedded CPU.** The REG 0x568 µs counter is verified and the ISA subset
    characterized.
- **Phase 3 on USB2, 8812BU:** continuous streaming of path A at 2.5 MS/s and 5 MS/s, 60 s each, zero
  faults, recorded to `.cs16` and checked offline (FINDINGS §4 "Continuous streaming over USB2";
  `evidence/phase3-stream-*`).
- **Firmware study** `notes/fw-disasm.md`: the 8822B PSD engine, the 8822C IQK read-out and the
  register names are cross-checked into FINDINGS §3, with one correction (no PSD registers on the
  8822C in this vendor tree).

## Not done yet
- Continuous streaming at 20 Msps for 60 s and the 40 Msps result: needs USB3, where 16 KiB C2H wedges.
  Next: the USB3 rate-vs-link test (pace C2H to the USB2 rate on USB3), then the devourer-vs-rtw88
  register diff (`notes/c2h-wedge-handoff.md`). Done on USB2: 2.5 and 5 MS/s for 60 s (FINDINGS §4).
- The tuning table across channels and bands. Only the ch36 noise-floor gain map exists.
- Phase 4: SoapySDR/SpyServer front end (rtl_tcp and the recorder exist). Phase 5 (8812CU port),
  Phase 6 (8812EU).
- 8812CU streaming would be USB-2-limited (≈35–40 MB/s) unless the board supports the runtime USB 3
  switch (open).

## Open needs (NEEDS.md)
N2 test transmitter · N3 uhubctl hub · N4 ac54 license judgment · N5 (stretch) RTL8812EU. N1 is resolved
(8812CU plugged in).

## Next steps
Updated 2026-10-25 10:45: (1) **USB3 rate-vs-link test** (switch the adapter to USB3 with a short rtw88 probe
under the `rtw88-guard` lock, then pace 16 KiB C2H to the USB2 rate); (2) front-end polish: a
SpyServer or SoapySDR module (int16, true 5 GHz frequencies, no LNB trick), tuning table per channel;
(3) 20/40 Msps only after the USB3 transport holds. The 8812CU port is deprioritized.
