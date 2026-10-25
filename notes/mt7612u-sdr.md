# MT7612U as a raw-IQ SDR receiver — Phase 6 (claude-mt)

_2026-10-25 · target: MediaTek MT7612U `0e8d:7612`, MT7662 MAC core, NDS32/Andes MCU, 2T2R, usb 8-1 SuperSpeed_

Convention: **[V]** measured on this bench, **[I]** inferred from source/firmware RE,
**[?]** open. This is a static-RE + partial-hardware result: the adapter dropped off the
USB bus during the first bring-up attempt (see "Hardware status") before any capture ran,
so every hardware claim below is from the kernel driver's own prior bring-up, not ours.

## TL;DR

1. **No ready-made raw-IQ-to-host path exists in the firmware or the mt76 driver.** The one
   classic MediaTek "ADC dump to packet-buffer SRAM" path (`Set_ADCDump_Proc`, PBF_CAP_CTRL
   `0x0440`) is **explicitly compiled out for this MAC generation** (`HIF_RLT`), and the
   newer "ICAP / RBIST_CAPTURE" IQ-capture block is an **mt7615/mt7915-and-later** feature,
   absent from MT76x2. What the MT76x2 baseband *does* expose is calibration-grade I/Q:
   accumulated correlator values (RXIQ/divider cal, BBP `CAL` block `0x2c00`) and indexed
   magnitude read-back (DPD, `BBP_R186/187/188`) — **not** a free-running sample ring. [I]
2. **The PBF packet-buffer capture hardware physically exists on the RLT MAC** (the
   `PBF_SYS_CTRL[14] CAP_MODE` bit and the SMM `0x4000` / PKT `0x8000` buffers are defined in
   the vendor `omac` register map, shared by RT/RLT); only the *driver* refuses to drive it.
   This is the single most promising untested hardware path and the register recipe is known
   (below). **Untested on our hardware.** [I]/[?]
3. **Running our own MCU code, RAM-only, is clearly feasible and does not even need a modified
   firmware image.** The stock running firmware's in-band command dispatcher implements
   `CMD_BURST_WRITE`(8) / `CMD_RANDOM_WRITE`(12) / `CMD_READ_MODIFY_WRITE`(9) as
   **arbitrary 32-bit writes to any MCU-bus address**, including ILM (`0x80000`) and DLM
   (`0x110800`) code/data RAM. That is the same lever the Realtek work used: patch a RAM
   handler to run our loop, read capture memory, push to host. [I]

## Hardware status / caution

- On plug the chip enumerates SuperSpeed, VID:PID `0e8d:7612`, ASIC `0x76120044`, MAC
  `40:a5:ef:5a:32:f8`, EEPROM chip `0x7612`. [V]
- First bring-up via devourer's mt7612u library **failed to apply the ROM patch** — the
  first FCE/bulk vendor requests (`req 07 idx 09a8`, `req 42 idx 0230..0236`) timed out, and
  within seconds the device **fell off the USB bus**: `xhci_hcd 0000:67:00.4: Timeout while
  waiting for setup device command`, `device not accepting address, error -62`,
  `USB disconnect`. [V] Evidence: `evidence/phase6-mt-dropoff-dmesg.txt`.
- Port-disable/enable on `usb8-port1` did **not** bring it back (`unable to enumerate USB
  device`); the controller `0000:67:00.4` then refused a D0→D3hot power-state change. [V]
  **The adapter needs a physical replug.** Rebinding the xHCI PCI controller was declined by
  the environment (it also hosts other work), so this was not attempted.
- Note for next run: our stock firmware was built from the kernel's `/lib/firmware/mediatek/
  mt7662.bin` (v build time `201507311614`, ROM patch `20141115060606a`), copied into
  `vendor/mt7612u/stock/`. The kernel driver itself had brought this exact adapter up cleanly
  minutes earlier (`mt76x2u ... Firmware Version 0.0.00 Build 1`), so the firmware pair is
  good; the failure looks like a USB/xHCI-level wedge on controller `67:00.4`, not a firmware
  problem. devourer's docs already warn this part can wedge below USB level
  (`devourer/src/mt7612u/README.md`, "The receiver must never run undrained"; `docs/
  mt7612u-usb-wedge.md`). Try a different USB3 port / controller, or bring it up with the
  `mt76x2u` kernel driver once and hand the handle to `mt7612u_open_handle()`.

## Q1 — Is there a hardware raw/near-raw baseband-IQ path?

### 1a. PBF packet-buffer "ADC dump" — exists in hardware, disabled in driver [I]

The Ralink/MediaTek lineage has a packet-buffer capture mode that snapshots ADC samples into
on-chip shared memory. In the MT7612U-capable vendor driver (Netgear A6210 = MT7612U, and
openwrt/mtk-wifi-gpl) it is `Set_ADCDump_Proc` (`ate/common/rt_ate.c`). The recipe is fully
documented in source:

```
PBF_SYS_CTRL (0x0400) |= BIT(14)      # CAP_MODE: packet-buffer capture mode
PBF_CAP_CTRL (0x0440):
    bit15 = ADC source select (1 = "ADC 6", 0 = "ADC 8")
    bit31 = 0
    bits[28:16] = trigger offset (0..0x1FFF)
    bit30 = 1  -> START capture;  poll until bit30 clears (done)
    bits[12:0] = CaptureStartAddr after done
MAC_SYS_CTRL (0x1004) = 0x8           # RX on (or 0x4 for TX) during capture
# then read back:
SMM  @ 0x4000 + CaptureStartAddr*2    # shared-memory samples (16-bit halves)
PKT  @ 0x8000 + CaptureStartAddr*4    # packet-buffer samples (4 bytes/word)
```

The read-back structs (`CAPTURE_MODE_SHARE_MEMORY`, `CAPTURE_MODE_PACKET_BUFFER`,
`include/rtmp.h`) show the capture stores raw byte lanes, not magnitudes — i.e. this *is* an
ADC/baseband sample path, analogous to the Realtek LA ring.

**The catch:** the function begins
```c
#ifdef RLT_MAC
    if (pAd->chipCap.hif_type == HIF_RLT)
        return FALSE;      // <-- MT7612U is an RLT-MAC part
#endif
    ... capture only runs under  if (hif_type == HIF_RTMP) ...
```
MT7612U uses the **RLT** MAC (it is driven through `rlt_bbp.h`, `ral_nmac_*`), so the vendor
driver refuses the dump on exactly this chip. But the SMM/PKT buffers and the `CAP_MODE` bit
are part of the **omac/RLT** register map too (`ral_omac_pbf.h`: `PBF_CAP_CTRL 0x0440`;
`RTMP_PBF_SYS_CTRL.CAP_MODE`). So this is a **software** refusal over hardware that is very
likely present. **This is the first thing to try on live hardware.** [I] → [?]

Nuance: the exact capture-control bit layout above is from the **RTMP**-MAC path. The RLT MAC
may place the trigger/offset bits differently; the honest test is to set `CAP_MODE`, write the
trigger, and read SMM/PKT back over the MCU `CMD_RANDOM_READ` (these are MCU-bus addresses,
not host-CSR addresses), checking for liveness and structure.

### 1b. ICAP / RBIST_CAPTURE (the modern IQ-capture) — not on this chip [I]

MediaTek's later parts (mt7615/mt7915/mt7921, `embedded/include/icap.h`, `CR_RBIST_CAPTURE`,
`Set_RBIST_Capture_Start`) have a proper RX-IQ capture engine with firmware commands. **None
of that exists in MT76x2** — neither in the mt76 `mt76x2/` tree nor in the MT7612U firmware
strings. Do not expect an ICAP path here.

### 1c. What the baseband *does* expose (calibration I/Q, not a stream) [I]

From the firmware (NDS32) and the vendor cal code, the only per-sample-ish I/Q the MT76x2 BB
offers is:
- **RXIQ / divider-phase calibration correlator** — BBP `CAL` block at `0x2c00`
  (`CAL_R1 0x2c04` arm/status bit7, `CAL_R11 0x2c2c` result = {phase[23:16], Q[15:8],
  I[7:0]}, `CAL_R2/CAL_R5` path/accum-length). Firmware string `RXIQ mi=.. mq=.. si=.. sq=..`
  at `0x1122c8`, code at ILM `0x89700..0x898d0`. This yields **accumulated** I/Q correlator
  outputs over a calibration tone, not a time series of the air. [I]
- **DPD AM/PM magnitude read-back** — `BBP_R186`(addr), `BBP_R187`(index), `BBP_R188`(data);
  "capture buffer" there holds AM samples for predistortion, **magnitude only**, and that path
  is `HIF_RTMP`-only too. [I]
- No FFT/PSD engine with a host-readable spectrum was found in MT76x2. [I]/[?]

**Conclusion Q1:** no turnkey raw-IQ path. One credible hardware candidate (PBF packet-buffer
capture, §1a) that the driver deliberately skips on this MAC and that we could not test before
the adapter dropped off the bus. Everything else the BB offers is calibration-grade
(correlator/magnitude), not a wideband sample ring like the Realtek LA mode.

## Q2 — Can we run our own code on the MCU, RAM-only? Yes. [I]

### Core / ISA [V-static]
The MCU is an **Andes NDS32** core (AndeStar v3, 16/32-bit mixed ISA — `rasm2 -a nds32`
disassembles the image cleanly: `sethi/ori`, `lmw.adm/smw.adm`, `jral`, `movi55`, etc.). This
confirms the commonly-stated "Andes core". It is **not** MIPS16 like the Realtek 8822B.

### Firmware layout & load [I, cross-checked with mt76]
- Host upload path (`mt76x2/usb_mcu.c`, mirrored in `devourer/src/mt7612u/fw.cpp`): ROM patch
  then ILM then DLM, each shipped as FCE in-band command DMA (`MT_VEND_WRITE_FCE`
  `MT_FCE_DMA_ADDR 0x230` / `MT_FCE_DMA_LEN 0x234`, bulk-OUT EP `0x08`), followed by the IVB
  vendor request (`MT_VEND_DEV_MODE`, wValue `0x12`). Firmware-running is signalled by
  `MT_MCU_COM_REG0` bit0.
- Image header (`struct mt76x02_fw_header`): `ilm_len`, `dlm_len`, versions, build time —
  **no signature/MAC field**, and the loader only reads the two lengths. **No secure boot on
  the download path.** [I] (same finding class as the Realtek images.)
- Load offsets: ILM USB offset `0x80000` (MCU view `0x80000`), DLM USB offset `0x110000`
  (+`0x800` for rev≥E3, so our DLM lands at MCU `0x110800`). ROM-patch offset `0x90000`.
- The 2015 image begins with a self-contained C-runtime startup stub at `0x80000`
  (copies `.data`, zeroes `.bss`, jumps to `main`), so a modified ILM/DLM image would run as
  loaded. [I]

### The lever: arbitrary RAM writes from a running stock firmware [I]
Static RE of the running firmware's in-band command dispatcher (ILM `0x83800`, decodes the
7-bit `CMD_TYPE` field) maps the commands mt76 already uses to arbitrary memory primitives:

| cmd | id | handler | effect |
|---|---|---|---|
| `CMD_BURST_WRITE` | 8 | `0x825cc` | write a run of words to consecutive `[addr]` |
| `CMD_READ_MODIFY_WRITE` | 9 | `0x825f8` | per-word `(*a & ~mask)|val` |
| `CMD_RANDOM_READ` | 10 | `0x8267c` | reply pairs `{addr, *addr}` (≤24 words/cmd) |
| `CMD_BURST_READ` | 11 | `0x826b4` | read N words from `[addr]` |
| `CMD_RANDOM_WRITE` | 12 | `0x82644` | pairs `{addr, val}` → `*addr = val` |

These handlers do **no range check** beyond 4-byte alignment — the destination is whatever the
MCU bus decodes, which includes the ILM/DLM code+data RAM the firmware itself runs from. So,
exactly as on Realtek:
1. Bring the chip up with stock firmware (kernel `mt76x2u` or devourer).
2. Use `CMD_RANDOM_WRITE` to overwrite a RAM handler / an unused command-id slot in the
   dispatcher with a small NDS32 payload we uploaded (also via `CMD_BURST_WRITE` into free
   RAM), then invoke it by sending that command id.
3. Our payload reads the capture memory (§1a SMM/PKT, or the CAL correlator) and ships it to
   the host — either by crafting RX frames onto the data pipe (EP `0x84`) through the FCE, or
   by letting the host poll via `CMD_BURST_READ`.

No modified firmware image is required (though one would also run). RAM-only; a power cycle /
fresh bring-up undoes everything, satisfying the safety rule. [I]

### MCU command transport (for the probe) [I]
In-band MCU framing: 4-byte TXINFO header `{LEN, DPORT=CPU_TX_PORT(2), CMD_SEQ, CMD_TYPE,
TYPE_CMD}` + payload, padded to 4 and a 4-byte zero terminator, on bulk-OUT EP `0x08`;
response (incl. random-read reply pairs) on bulk-IN EP `0x85`, matched on the 4-bit sequence,
`EVT_CMD_DONE`. `MT_INBAND_PACKET_MAX_LEN = 192` → ≤24 read pairs per command. This is already
implemented in the probe (`src/mt76sdr/mtsdr.cpp`, `mcu_rd`/`mcu_wr`).

## Q3 — Hardware proof

**Not achieved**: the adapter wedged at the USB level and disconnected during the first
bring-up (see "Hardware status"), before any capture or marker program ran. Needs a physical
replug (and preferably a different USB3 controller) to retry. The tooling to run the proof is
built and ready (below).

## Q4 — What a streaming/snapshot SDR mode would take, and the budget

- **Front end:** MT7612U ADC is nominally 40/80 MHz-capable (MT76x2 does 20/40/80 MHz
  channels). If the PBF capture (§1a) delivers 12-bit I/Q, a snapshot is bounded by the
  SMM/PKT buffer size (SMM `0x4000..0x8000` = 16 KiB; PKT `0x8000..0x10000` = 32 KiB) — on the
  order of a few thousand complex samples per trigger, i.e. a short snapshot, not continuous.
  Continuous streaming would need the same trick as Realtek: an on-MCU loop that re-arms the
  capture and ships each buffer to the host while the next fills.
- **USB budget:** this unit is **SuperSpeed (5000M)** on xHCI `67:00.4`. 20 Msps × 4 B/complex
  = 80 MB/s; 40 Msps = 160 MB/s — both inside USB3's ~400 MB/s practical ceiling, and far above
  the ~35–40 MB/s USB2 cap that limited the CU Realtek. Throughput is not the limiter here; the
  capture-buffer depth and the MCU's ability to drain-and-re-arm are. [I]
- **Risks:** (a) the PBF capture may be physically fused off or wired differently on RLT than
  the RTMP recipe assumes; (b) the "receiver must never run undrained" wedge is easy to hit and
  recovers only by replug in the hard case; (c) no bench transmitter — the ch36 AP at 5180 MHz
  is the only test signal, as in earlier phases.

## Files created
- `src/mt76sdr/mtsdr.cpp` — MT7612U research probe (RX-only; host EP0 reads/writes, MCU
  `CMD_RANDOM_READ/WRITE` incl. arbitrary-RAM, `diff` liveness, EP5 event dump, EP4 data dump,
  fast/full channel set). Links against devourer's mt7612u library.
- `tools/mt_build.sh` — builds the probe into `build-mt/` (no CMake; compiles devourer's
  `src/mt7612u/*.cpp` + `src/mt76sdr/*.cpp`). `DEVOURER_MT7612U_SRC` overrides the library path.
- `tools/mt_fwre.py` — static NDS32 RE helper: per-block constant propagation over an r2 `pD`
  disassembly (`sethi/ori/addi/movi`, `addi.gp` with `--gp=`), resolves load/store EAs so MAC/
  BBP/peripheral register touches and string xrefs fall out. Usage in the file header.
- `evidence/phase6-mt-dropoff-dmesg.txt` — the USB drop-off kernel log.
- `vendor/mt7612u/stock/{mt7662.bin,mt7662_rom_patch.bin}` — decompressed kernel firmware
  (gitignored), the pair the probe loads.

### Reproduce (after a replug, with the adapter on a working USB3 port)
```sh
tools/coord.sh lock mt7612u claude "claude-mt: <why>"
# verify no kernel driver: readlink /sys/bus/usb/devices/8-1/8-1:1.0/driver
tools/mt_build.sh
sudo -n env MT7612U_DEV=8-1 MTSDR_FW=$PWD/vendor/mt7612u/stock \
    build-mt/mtsdr rr:0x1004 mr:0x410000:2
# PBF capture probe (the §1a experiment), e.g.:
#   wr:0x0400:<sys_ctrl|BIT14>  wr:0x0440:<trigger>  wr:0x1004:0x8
#   mdump:0x4000:4096 /tmp/smm.bin   mdump:0x8000:8192 /tmp/pkt.bin
tools/coord.sh unlock mt7612u claude
```

## Still open [?]
- Whether the RLT MAC's PBF capture actually arms and fills (the whole Q1 payoff) — untested.
- The exact RLT (vs RTMP) `PBF_CAP_CTRL` bit layout and whether SMM/PKT are MCU-readable here.
- Sample format / bit width the capture stores (claim of 12-bit I/Q is by analogy only).
- Whether a wider-than-20 MHz tap is reachable (MT76x2 nominally does 80 MHz; the capture
  front-end bandwidth is unknown).
