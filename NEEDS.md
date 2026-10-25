# NEEDS — requests for Joakim

Newest at the bottom. Resolved entries are marked, never deleted.

---

### N1 — 2026-10-25 — Plug in an RTL8812CU (target 2)
- **Need:** an RTL8812CU / RTL8822CU adapter connected to this machine, ideally on a USB 3 port.
- **Why:** only one Realtek adapter is attached (0bda:b812, RTL8812BU). Phase 1 asks for an LA baseline on
  the 8812CU too, and Phase 5 is the 8812CU port.
- **Unblocks:** Phase 1 (8812CU half), Phase 5.
- **Meanwhile:** all 8812BU work (Phases 1–4) proceeds; the 8822C firmware/LA dialect is being studied offline.
- **Status:** OPEN

### N2 — 2026-10-25 — A controllable test transmitter
- **Need (any one of):**
  1. a second devourer-supported Realtek adapter (e.g. an 8812AU/8812EU) to run `txdemo` as the test source; or
  2. a Pluto / HackRF / B210 (devourer's own tests reference a B210 on this bench) for a CW tone; or
  3. permission to `rfkill unblock` the internal MT7922 (`wlp2s0`, currently soft-blocked — I assumed that is
     deliberate and have not touched it) and use it as a low-power monitor-mode injector on a standard channel.
- **Why:** a known, controllable signal makes layout/orientation/frequency verification and the streaming
  continuity test unambiguous. Ambient APs work for now (strong traffic on ch 1, 36, 44, 149), and an
  off-centre tune against an ambient AP gives an asymmetric spectrum for the I/Q-orientation test, but
  ambient frames have unknown timing and level.
- **Unblocks:** a coded-continuity test for Phase 3, a CW-based tuning table, gain characterization.
- **Meanwhile:** using ambient Wi-Fi as the signal source; L-LTF correlation as the orientation check.
- **Status:** OPEN

### N3 — 2026-10-25 — A USB 3 hub with per-port power switching (uhubctl-compatible)
- **Need:** a uhubctl-supported hub (e.g. a USB 3 hub on the uhubctl compatibility list).
- **Why:** `uhubctl` finds no switchable hub, so the wedge-recovery ladder stops at `usbreset`; a wedged
  chip (likely during custom-firmware work) then needs a manual replug. A real VBUS cycle is also the only
  true cold-boot.
- **Unblocks:** unattended recovery during Phase 3/5 firmware experiments.
- **Meanwhile:** recovery is libusb reset → usbreset → (ask for replug, logged here).
- **Status:** OPEN

### N4 — 2026-10-25 — Judgment call: ac54_sdr has no license
- **Need:** your call on whether that is acceptable as handled below, or whether you want to contact the
  author (djObsidian) to ask for a license.
- **Why:** `djObsidian/ac54_sdr` has no LICENSE file, so its code is all-rights-reserved by default.
- **What I'm doing:** using it only as documentation of a technique (register addresses, sequences and
  protocol facts). All code under `~/wifi-sdr/` is written fresh; nothing is copied from it. Its Windows
  driver artifact (`rtwlanu.sys`) and anything extracted from it stay in the gitignored `vendor/`.
- **Status:** OPEN (proceeding as described)

### N5 — 2026-10-25 — (stretch) an RTL8812EU module
- **Need:** an RTL8812EU (waybeam air-unit radio) on USB, only when Phases 3–5 are done.
- **Unblocks:** Phase 6.
- **Status:** OPEN (not urgent)
