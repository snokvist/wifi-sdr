# LOG — engineering journal (append-only)

Times are local (CET, +01:00).

## 2026-10-25

**06:36** — Workspace created: `~/wifi-sdr/` (git, `main`). `vendor/`, `ref/`, `captures/` gitignored.

**06:36** — Recon.
- `lsusb`: one Realtek adapter, `0bda:b812` "RTL88x2bu [AC1200 Techkey]", bus 2 dev 2, sysfs
  `/sys/devices/pci0000:00/0000:00:08.1/0000:65:00.3/usb2/2-1` (AMD xHCI root port), **5000M (USB 3.0)**,
  bcdUSB 3.00, serial `123456`, MaxPower 504 mA. Interface 0, 5 EPs: bulk IN 0x84, bulk OUT 0x05/0x06/0x08,
  interrupt IN 0x87 (64 B). Bulk wMaxPacketSize 1024.
- Other USB: Cypress keyboard/hub, SteelSeries mouse, MediaTek BT (`0e8d:0616`, btusb).
- PCIe: MT7922 Wi-Fi (`wlp2s0`, **rfkill soft-blocked** — left alone), 2× RTL8125 2.5GbE.
- **Host uplink = `enp3s0`** (192.168.2.242/24, default via 192.168.2.2). Protected: never touched.
- `uhubctl`: "No compatible devices detected" → no VBUS switching available.
- Installed already: uhubctl, usbreset, usb_modeswitch, cmake, gcc, python3 (numpy 2.3.5, matplotlib 3.10.7,
  scipy 1.16.3). DKMS: rtl88x2cu 5.15.8-52, rtl88x2eu 5.15.0.1, rtl8821cu (out-of-tree vendor drivers).
- `/etc/modprobe.d/blacklist-rtw88.conf` already blacklists `rtw88_8822cu` (pre-existing, owner's).
  `rtw88_8822bu` was bound to the 8812BU (`wlx40a5ef515661`, down).
- Kernel 7.0.0-38-generic, Ubuntu 26.04.1.

**06:37** — Cloned refs into `ref/`: djObsidian/ac54_sdr (HEAD 2b41f98; **no LICENSE file**),
djObsidian/wifi-sdr, plaingca/rtl8922-sdr (depth 1), OpenIPC/devourer (HEAD 0bf1957, #496).
OpenIPC master is 37 commits ahead of the local snokvist clone (fc66aa9, #440).

**06:45** — Joakim (mid-session message): OK to bring the snokvist clone up to upstream head and work in it.
`/home/snokvist/dev/snokvist/devourer`: added remote `upstream` = OpenIPC/devourer, `git merge --ff-only
upstream/master` → 0bf1957. Not pushed. Submodules inited: reference/rtl88x2bu, rtl88x2cu, rtl88x2eu.
Built Release (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`) — clean.
From here "devourer" = that tree (`$DVR`).

**06:50** — `tools/rtw88_guard.sh on`: temp blacklist `/etc/modprobe.d/zz-wifi-sdr-temp-blacklist.conf`
(rtw88_8822bu, rtw88_8822cu, rtw88_8821cu) + `modprobe -r`. Adapter now driverless; enp3s0 still up.
**Must run `sudo tools/rtw88_guard.sh off` at the end of the work** to restore.

**06:52** — `doctor --pid 0xb812 --channel 6` → SUSPECT (0x40): init OK, EFUSE 4/4 stable (ID 0x8129),
FW ready, but RX 0 ok / 12 CRC. Ch 6 turned out to be RF-quiet here, not a deaf radio (see below).

**06:55** — rxdemo channel survey (9–12 s dwells, `DEVOURER_PID=0xb812`): frames seen —
ch1 1811, ch6 0, ch11 3, ch36 1011, ch44 646, ch100 0, ch149 74 (max RSSI 95), ch157 0.

**06:58** — `doctor --pid 0xb812 --channel 1` → **HEALTHY** (exit 0): rx_ok 1899, rx_crc 636, EFUSE stable,
fw_ready. Chip: 8822B cut 3 (UMC) 2T2R, SYS_CFG1=0x0c493d37, EFUSE rfe_type=0x03.
Evidence: `evidence/phase0/doctor-8812bu-ch{6,1}.{jsonl,stderr}`, `evidence/phase0/rxdemo-8812bu-ch*.jsonl`.

**06:58** — Test signal: no second adapter, no reference SDR → ambient APs (ch1, ch36) for now. NEEDS N1–N5 filed.
