#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""C2H large-packet test with the in-kernel rtw88 driver owning the 8822B (run as root).

Separates "devourer init / libusb host path" from "chip / board / xHCI": rtw88 does its own full
init and keeps its bulk-IN URBs posted; this script only stages one C2H packet and fires requests
through rtw88's debugfs register interface, and usbmon counts the packets that reach the host.

Requires: rtw88_8822bu bound, interface up in monitor mode (MAC powered, no TX), usbmon loaded.
Source: TXFF 0x20000 (C2H alias 0x16000), which rtw88 does not use while it does not transmit.
Payload: 24-byte C2H descriptor + zeros (C2H id 0, which rtw88's handler ignores).
Usage: rtw88_c2h_test.py PHY BUS [--count 1600] [--bytes 16352]
"""
import argparse
import re
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument("phy")
ap.add_argument("bus", type=int)
ap.add_argument("--count", type=int, default=1600)
ap.add_argument("--bytes", type=int, default=16352)
a = ap.parse_args()
D = f"/sys/kernel/debug/ieee80211/{a.phy}/rtw88"


def wr(addr, val, n=4):
    with open(f"{D}/write_reg", "w") as f:
        f.write(f"{addr:x} {val:x} {n}")


def rd(addr, n=4):
    with open(f"{D}/read_reg", "w") as f:
        f.write(f"{addr:x} {n}")
    with open(f"{D}/read_reg") as f:
        return int(re.search(r"0x([0-9a-fA-F]+)\s*$", f.read().strip()).group(1), 16)


# usbmon: count bulk-IN completions of exactly descriptor+payload bytes on this bus.
frame = 24 + a.bytes
seen = {"n": 0}
stop = threading.Event()


def mon():
    with open(f"/sys/kernel/debug/usb/usbmon/{a.bus}u") as f:
        while not stop.is_set():
            line = f.readline()
            if not line:
                continue
            p = line.split()
            # tag ts C Bi:bus:dev:ep status length ...
            if len(p) > 5 and p[2] == "C" and p[3].startswith("Bi:") and p[5] == str(frame):
                seen["n"] += 1


t = threading.Thread(target=mon, daemon=True)
t.start()
time.sleep(0.3)

# Stage the packet in TXFF 0x20000 (OCP 0x187A0000) through the packet-buffer debug window.
ocp = 0x187A0000
desc = bytearray(24)
desc[0] = a.bytes & 0xFF
desc[1] = (a.bytes >> 8) & 0x3F
desc[3] = 0x80
desc[11] = 0x10
pkt = bytes(desc) + bytes(a.bytes)
pkt += bytes((-len(pkt)) % 4)
saved106 = rd(0x106, 1)
saved140 = rd(0x140, 2)
wr(0x106, 0x69, 1)
for off in range(0, len(pkt), 4):
    a_ocp = ocp + off
    if off % 4096 == 0 or (a_ocp & 0xFFF) == 0:
        wr(0x140, (a_ocp >> 12) & 0xFFF, 2)
    wr(0x8000 + (a_ocp & 0xFFF), int.from_bytes(pkt[off:off + 4], "little"))
wr(0x140, saved140, 2)
wr(0x106, saved106, 1)

req = (0x16000 & 0xFFFF) | ((0x16000 >> 16) << 24) | 0x10000
done = 0
wedge = None
t0 = time.monotonic()
for i in range(a.count):
    wr(0x294, req)
    dl = time.monotonic() + 0.2
    while rd(0x294) & 0x10000:
        if time.monotonic() > dl:
            wedge = i
            break
    if wedge is not None:
        break
    done += 1
el = time.monotonic() - t0
time.sleep(0.3)
stop.set()
print({"requests_completed": done, "count": a.count, "wedge_at": wedge, "packets_seen_usbmon": seen["n"],
       "r294": hex(rd(0x294)), "r288": hex(rd(0x288)), "seconds": round(el, 2),
       "ms_per_request": round(el / max(done, 1) * 1e3, 2)})
