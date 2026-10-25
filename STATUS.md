# STATUS

_Last updated: 2026-10-25 12:45 CET_

**Current phase:** **Phase 3 done** (2026-10-25): continuous streaming at **20 MS/s for 60 s** and
**40 MS/s for 60 s** with zero faults over USB3, once the 8812BU is on the right port (xHCI
0000:67:00.3; the other controller, 65:00.3, wedges C2H). USB2 gives 2.5/5 MS/s on any port. Front ends:
`wsdr_tcp` (rtl_tcp, 2.5/5 MS/s) and the SoapySDR module (2.5/5 MS/s on USB2, up to 40 MS/s on USB3).
Bandwidth: ~18.4 MHz by default; **40 MHz at 40 MS/s with `--bw 40 --dfir 40`** (streams over USB3);
80 MHz / 80 MS/s with `--bw 80 --dfir 80 --rate 0` (snapshot only). **8812CU: continuous two-path streaming at 2.5 and 5 MS/s for 60 s, zero faults** (USB2, `build-cu/wsdr_cu stream`). Interim write-up:
[REPORT.md](REPORT.md).

### Using it
```sh
git submodule update --init external/devourer   # pinned devourer (not --recursive: its submodules are reference trees)
cmake -S . -B build && cmake --build build -j
sudo build/wsdr_tcp --pid 0xb812 --freq-offset 5e9      # take the 8812bu lock first
gqrx: device "rtl_tcp=127.0.0.1:1234", Input controls → LNB LO 5000 MHz, then tune 5180 MHz
sudo build/wsdr_probe stream --pid 0xb812 --seconds 60 --out captures/x   # record .cs16 + .json
```
Clients that send 5 GHz as a wrapped u32 (gr-osmosdr with "No limits", custom tools) need no offset.
**Better: SoapySDR** (true 5 GHz, 16-bit samples): `cmake -S . -B build-soapy -DWSDR_SOAPY=ON && cmake
--build build-soapy -j --target wsdrSupport`, then `SOAPY_SDR_PLUGIN_PATH=$PWD/build-soapy gqrx` with device
`soapy=0,driver=wsdr` (or SDR++'s SoapySDR source). Non-root access: `tools/99-wsdr.rules` (not installed).

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
- **Phase 5, 8812CU (RTL8822C) streaming** (FINDINGS §4 "Continuous streaming on the 8822C"):
  `wsdr_cu stream` + `src/fw/stream8822c.S`, five C2H packets per bank around the 0x6000 segment
  split. 60 s at 5 MS/s and 60 s at 2.5 MS/s, zero faults; offline 567/552 beacons on the grid, 0
  slips. Path B rides in the same word: B leads A by 401 ns, coherent in bursts
  (`evidence/phase5-cu-stream*`). Fixed gain (CCA off, ~1.0 dB per IGI step). **SoapySDR:**
  `soapy=0,driver=wsdr,pid=0xc812`, two coherent channels (A/B), 2.5/5 MS/s.
- **Firmware study** `notes/fw-disasm.md`: the 8822B PSD engine, the 8822C IQK read-out and the
  register names are cross-checked into FINDINGS §3, with one correction (no PSD registers on the
  8822C in this vendor tree).

## Not done yet
- Front ends do not yet pass `dfir`/channel width (Soapy/wsdr_tcp are BW20); centre→primary-channel
  mapping at 40/80 MHz. 80 MS/s and raw-ADC (dma 11) snapshot modes are not exposed.
- 8812CU: not behind `wsdr_tcp` (rtl_tcp) yet; wide-tap/DFIR untested on the 8822C; no USB 3 on this
  board. Upstream:
  OpenIPC/devourer#497 (8822B DFIR) and #498 (optional Jaguar3 runtime thread), both open.
- The tuning table across channels and bands. Only the ch36 noise-floor gain map exists.
- Phase 4: SoapySDR/SpyServer front end (rtl_tcp and the recorder exist). Phase 5 (8812CU port),
  Phase 6 (8812EU).
- 8812CU streaming is USB-2-limited (44.7 MB/s C2H → 5 MS/s, both paths in the same word) unless
  the board supports the runtime USB 3 switch (open).

## Open needs (NEEDS.md)
N2 test transmitter · N3 uhubctl hub · N4 ac54 license judgment · N5 (stretch) RTL8812EU. N1 is resolved
(8812CU plugged in).

## Next steps
Updated 2026-10-25 12:40: (1) front ends on USB3: 10/20/40 MS/s in the SoapySDR module (and wsdr_tcp),
deep URB queue; (2) BW40 at 40 MS/s for a full 40 MHz span; (3) characterize the 40 MS/s tail over long
runs; (4) decode the LA dma_type formats for a denser word (the only route to 80 MHz on USB3 Gen1).
Keep the 8812BU on the xHCI 67:00.3 port for USB3.
