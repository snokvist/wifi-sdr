#!/usr/bin/env python3
"""Static reverse-engineering helpers for Realtek 8822B/8822C WLAN firmware.

OFFLINE ONLY. Never touches hardware, USB, or kernel modules. Inputs (the
extracted firmware section .bin files under vendor/) are untrusted data, so
run with `python3 -I`.

The embedded MCU is a little-endian MIPS executing MIPS16/MIPS16e2
(Lexra/RLX-class). The strategy here is ISA-robust and does not depend on a
perfect MIPS16 decode:

  * MMIO census  -- the firmware's MIPS16 code loads 32-bit absolute pointers
    (register addresses, SRAM bases, code/function pointers) from a literal
    pool that lives in DMEM.  We scan every section for 32-bit LE words and
    bucket them by high half-word (0xb864 = uncached MAC/BB MMIO, 0xb870 = RX
    SRAM, 0xb878 = TX/LA SRAM, 0x8000/0x8003/0x8010/0x8011 = OCP code/data).
    The register offset a firmware touches == (word & 0xffff) for 0xb864/0xb860
    words.  This enumerates the register surface directly, ISA-independently.

  * Code-pointer tables -- odd 32-bit words whose high half lands in a code
    region (ISA bit set => MIPS16 target) are function pointers; runs of them
    in DMEM are dispatch/handler tables (H2C handlers, calibration step tables).

  * Disassembly -- objdump -m mips:16 against the raw section at its load VMA.
    Used for call-graph (jal/jalx targets) and for reading individual routines.
    Data (literal pools) interleaved with code will mis-decode; that is fine
    for a census and expected when reading.

Subcommands:
  layout                     print the section/load-address table
  disasm  CHIP SEC           write objdump MIPS16 listing to --out
  mmio    CHIP               MMIO pointer-constant census (all sections)
  ptrs    CHIP               code-pointer tables in DMEM (candidate dispatch)
  calls   CHIP SEC           jal/jalx target histogram from the MIPS16 listing
  find    CHIP HEXWORD       locate a 32-bit LE word / register addr everywhere
  all     CHIP               regenerate every artifact under --out

CHIP in {8822b, 8822c}. SEC in {imem, dmem, emem}.
"""
from __future__ import annotations

import argparse
import collections
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # .../wifi-sdr
OBJDUMP = "mipsel-linux-gnu-objdump"

# section file + OCP load addr + CPU (cached) VMA used for disassembly.
# CPU cached VMA == 0x80000000 + OCP (confirmed from fw headers).
IMAGES = {
    "8822b": {
        "title": "8822B NIC v30.20 (array_mp_8822b_fw_nic / rtw8822b_fw)",
        "dir": ROOT / "vendor/88x2bu",
        "stem": "array_mp_8822b_fw_nic",
        "sections": {
            # name: (suffix, ocp, vma, has_code)
            "dmem": ("dmem.bin", 0x200000, 0x80200000, False),
            "imem": ("imem.bin", 0x000000, 0x80000000, True),
        },
    },
    "8822c": {
        "title": "8822C NIC v9.17 (devourer array_mp_8822c_fw_nic)",
        "dir": ROOT / "vendor/devourer",
        "stem": "devourer_array_mp_8822c_fw_nic",
        "sections": {
            "dmem": ("dmem.bin", 0x200000, 0x80200000, False),
            "imem": ("imem.bin", 0x030000, 0x80030000, True),
            "emem": ("emem.bin", 0x100000, 0x80100000, True),
        },
    },
}

# High half-word -> meaning for the pointer-constant census.
HI_MEANING = {
    0xB864: "uncached MAC/BB MMIO  (phys 0x18640000 + reg; EP0 0x0000..0xffff)",
    0xB860: "uncached MMIO base 2  (phys 0x18600000 + off)",
    0xB866: "uncached MMIO base 3  (phys 0x18660000 + off)",
    0xB870: "RX SRAM (uncached, phys 0x18700000)",
    0xB878: "TX/LA SRAM (uncached, phys 0x18780000)",
    0xB87A: "TX/LA SRAM bank (uncached, phys 0x187a0000)",
    0x1864: "MAC/BB MMIO (phys/OCP 0x18640000)",
    0x1870: "RX SRAM (phys 0x18700000)",
    0x1878: "TX/LA SRAM (phys 0x18780000)",
    0x8000: "OCP IMEM/code or data ptr (0x80000000 region)",
    0x8001: "OCP IMEM code/data ptr (0x80010000)",
    0x8002: "OCP DMEM ptr (0x80020000)",
    0x8003: "OCP IMEM code ptr (0x80030000, 8822C imem base)",
    0x8010: "OCP EMEM code ptr (0x80100000, 8822C emem base)",
    0x8011: "OCP EMEM code ptr (0x80110000)",
    0x8012: "OCP EMEM code ptr (0x80120000)",
    0x8020: "OCP DMEM ptr (0x80200000, dmem base)",
    0x8021: "OCP DMEM ptr (0x80210000)",
}

# Known 8822x MAC register names (offset within the 0xb864 window). Sourced
# from halmac/phydm reg definitions and the ac54 analysis; only labels we are
# reasonably sure of are included, the rest print raw.
REGNAMES = {
    0x0002: "SYS_FUNC_EN",
    0x0003: "SYS_FUNC_EN+1 (CPU_EN bit2)",
    0x0008: "AFE_CTRL",
    0x001d: "9346CR/EE (CPU bit0)",
    0x001f: "RF_CTRL",
    0x0024: "AFE_XTAL_CTRL",
    0x0028: "AFE_PLL_CTRL",
    0x0080: "MCUFW_CTRL",
    0x0088: "RSV",
    0x0100: "CR (MAC TRX enable)",
    0x0106: "TXDMA page (TXFF window hi)",
    0x010c: "PBP",
    0x0140: "TXFF page window index",
    0x0208: "RXDMA mode/agg",
    0x0280: "RXDMA aggregation (DMA_AGG)",
    0x0284: "RXDMA agg timeout/threshold",
    0x0286: "RXPKT_NUM / RXDMA agg",
    0x0288: "RXDMA status",
    0x0290: "RXDMA mode (RXPKT mode)",
    0x0294: "C2H / FW2HOST request+poll (bit16 go, ac54 fast-forward)",
    0x0296: "C2H+2",
    0x0300: "FIFOPAGE",
    0x0368: "RXDMA PRO",
    0x03a7: "RXFF",
    0x0550: "TSFTR",
    0x0558: "TSFTR",
    0x0568: "free-running us timer (ac54 bank clock)",
    0x05fc: "PS timer",
    0x07c0: "LA mode ctrl/arm (0x7c0=0xcb arm, 0x7c0=0 stop)",
    0x07c4: "LA mode",
    0x07c8: "LA mode",
    0x07cc: "LA mode (bit30 ring-extend)",
    0x0808: "BB OFDM CCA / CCK enable (bit28)",
    0x08ac: "BB bandwidth (BW20/40/80/5/10)",
    0x08b4: "BB LA report enable (bit7)",
    0x08f8: "BB dbg header select (bits 22..)",
    0x08fc: "BB dbg-port select",
    0x095c: "BB LA: dma_type[11:8], rate sel[7:5], trig sel[4:0], edge[31]",
    0x0948: "BB RX DFIR",
    0x094c: "BB RX DFIR",
    0x1080: "CPU_DMEM_CON (WL_PLATFORM_RST BIT16 | DDMA_EN BIT8)",
    0x1082: "CPU clock-sync reset (bit0)",
    0x1100: "WLAN_ACT_MASK / sched",
    0x1200: "DDMA ch source addr (SA)",
    0x1204: "DDMA ch dest addr (DA)",
    0x1208: "DDMA ch ctrl (0x80000000|len; poll bit31)",
    0x120c: "DDMA ch status",
    0x1210: "DDMA ch1 SA",
    0x1214: "DDMA ch1 DA",
    0x1218: "DDMA ch1 ctrl",
    0x1c3c: "BB dbg-port read-back (phydm dbg port)",
    0x1ce4: "BB dbg-port read-back",
    0x1d00: "BB",
    0xfe00: "USB core",
    0xfe11: "USB core",
    0xfe17: "USB core",
    0xfe48: "USB core",
    0xfe50: "USB core",
}

TARGET_REGS = [0x0294, 0x0568, 0x07c0, 0x07c4, 0x07c8, 0x07cc, 0x08fc, 0x095c,
               0x08b4, 0x08f8, 0x1c3c, 0x1ce4, 0x1200, 0x1204, 0x1208, 0x120c,
               0x1080, 0x1082, 0x0280, 0x0284, 0x0290, 0x0100]


def sec_path(chip: str, sec: str) -> Path:
    info = IMAGES[chip]
    suffix, *_ = info["sections"][sec]
    return info["dir"] / f"{info['stem']}.{suffix}"


def read_words(path: Path):
    b = path.read_bytes()
    n = len(b) & ~3
    return list(struct.unpack(f"<{n//4}I", b[:n])), b


def code_regions(chip: str):
    """OCP ranges that hold executable code (for ISA-bit pointer detection)."""
    out = []
    for sec, (suffix, ocp, vma, has_code) in IMAGES[chip]["sections"].items():
        if has_code:
            sz = sec_path(chip, sec).stat().st_size
            out.append((sec, 0x80000000 + ocp, 0x80000000 + ocp + sz))
    return out


def cmd_layout(_a):
    for chip, info in IMAGES.items():
        print(f"== {chip}: {info['title']}")
        for sec, (suffix, ocp, vma, has_code) in info["sections"].items():
            p = sec_path(chip, sec)
            print(f"   {sec:5s} {p.name:40s} size {p.stat().st_size:7d}"
                  f"  OCP 0x{ocp:06x}  CPU-VMA 0x{vma:08x}  {'code' if has_code else 'data'}")


def objdump_mips16(path: Path, vma: int) -> str:
    r = subprocess.run(
        [OBJDUMP, "-D", "-b", "binary", "-m", "mips:16", "-EL",
         f"--adjust-vma=0x{vma:x}", str(path)],
        check=True, capture_output=True, text=True)
    return r.stdout


def cmd_disasm(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    vma = IMAGES[a.chip]["sections"][a.sec][2]
    txt = objdump_mips16(sec_path(a.chip, a.sec), vma)
    dst = out / f"{a.chip}.{a.sec}.mips16.asm"
    dst.write_text(txt)
    print(f"wrote {dst} ({len(txt)} bytes)")


def cmd_mmio(a):
    lines = [f"# MMIO / pointer-constant census -- {a.chip}: {IMAGES[a.chip]['title']}",
             "# 32-bit LE words bucketed by high half-word. Register offset == (word & 0xffff).",
             ""]
    allregs = collections.Counter()
    for sec in IMAGES[a.chip]["sections"]:
        w, b = read_words(sec_path(a.chip, sec))
        hi = collections.Counter(x >> 16 for x in w)
        lines.append(f"== section {sec}  ({len(b)} bytes, {len(w)} words)")
        for h, c in hi.most_common():
            if c < 3 and h not in HI_MEANING:
                continue
            meaning = HI_MEANING.get(h, "")
            lines.append(f"   hi 0x{h:04x}  x{c:<4d} {meaning}")
            if h in (0xB864, 0xB860, 0xB866):
                offs = sorted(set(x & 0xffff for x in w if (x >> 16) == h))
                for o in offs:
                    allregs[o] += sum(1 for x in w if x == (h << 16 | o))
        lines.append("")
    lines.append("== 0xb864 register offsets touched (merged over sections), with labels")
    for o in sorted(allregs):
        lab = REGNAMES.get(o, "")
        lines.append(f"   0x{o:04x}  x{allregs[o]:<3d} {lab}")
    lines.append("")
    lines.append("== targeted registers of interest (present? / count)")
    for r in TARGET_REGS:
        c = allregs.get(r, 0)
        lines.append(f"   0x{r:04x}  {'PRESENT x'+str(c) if c else 'absent':12s} {REGNAMES.get(r,'')}")
    txt = "\n".join(lines) + "\n"
    if a.out:
        dst = Path(a.out); dst.mkdir(parents=True, exist_ok=True)
        (dst / f"{a.chip}.mmio.txt").write_text(txt)
        print(f"wrote {dst / (a.chip + '.mmio.txt')}")
    else:
        print(txt)


def cmd_ptrs(a):
    """Find runs of code pointers (odd words into a code region) in DMEM/EMEM."""
    regions = code_regions(a.chip)

    def is_codeptr(x):
        if not (x & 1):
            return False
        t = x & ~1
        return any(lo <= t < hi for _, lo, hi in regions)

    lines = [f"# Candidate code-pointer tables (odd words -> code region) -- {a.chip}",
             f"# code regions: " + ", ".join(f"{s}[0x{lo:08x},0x{hi:08x})" for s, lo, hi in regions),
             ""]
    for sec in IMAGES[a.chip]["sections"]:
        if sec == "imem" and len(IMAGES[a.chip]["sections"]) > 2:
            # imem for 8822c is tiny stub; still scan but label
            pass
        suffix, ocp, vma, _ = IMAGES[a.chip]["sections"][sec]
        w, b = read_words(sec_path(a.chip, sec))
        # find runs of >=3 consecutive code pointers
        runs = []
        i = 0
        while i < len(w):
            if is_codeptr(w[i]):
                j = i
                while j < len(w) and is_codeptr(w[j]):
                    j += 1
                if j - i >= 3:
                    runs.append((i, j))
                i = j
            else:
                i += 1
        total = sum(1 for x in w if is_codeptr(x))
        lines.append(f"== section {sec}  (code pointers total: {total}; tables of >=3:)")
        for i, j in runs:
            addr = vma + i * 4
            lines.append(f"   table @ DMEM/OCP 0x{addr:08x} (file off 0x{i*4:04x})  {j-i} entries:")
            for k in range(i, j):
                lines.append(f"       [{k-i:2d}] -> 0x{w[k] & ~1:08x} (0x{w[k]:08x})")
        lines.append("")
    txt = "\n".join(lines) + "\n"
    if a.out:
        dst = Path(a.out); dst.mkdir(parents=True, exist_ok=True)
        (dst / f"{a.chip}.ptrs.txt").write_text(txt)
        print(f"wrote {dst / (a.chip + '.ptrs.txt')}")
    else:
        print(txt)


JAL_RE = re.compile(r"\b(jal|jalx)\s+0x([0-9a-f]+)")
TARGET_RE = re.compile(r"^\s+([0-9a-f]+):")


def cmd_calls(a):
    vma = IMAGES[a.chip]["sections"][a.sec][2]
    txt = objdump_mips16(sec_path(a.chip, a.sec), vma)
    targets = collections.Counter()
    for line in txt.splitlines():
        m = JAL_RE.search(line)
        if m:
            targets[int(m.group(2), 16)] += 1
    lines = [f"# jal/jalx target histogram -- {a.chip} {a.sec}  ({sum(targets.values())} calls, "
             f"{len(targets)} distinct targets)",
             "# target (ISA bit stripped) : call count", ""]
    for t, c in sorted(targets.items(), key=lambda kv: -kv[1]):
        lines.append(f"   0x{t & ~1:08x}  x{c}")
    out = "\n".join(lines) + "\n"
    if a.out:
        dst = Path(a.out); dst.mkdir(parents=True, exist_ok=True)
        (dst / f"{a.chip}.{a.sec}.calls.txt").write_text(out)
        print(f"wrote {dst / (a.chip + '.' + a.sec + '.calls.txt')}")
    else:
        print(out)


def cmd_find(a):
    target = int(a.word, 0)
    for sec in IMAGES[a.chip]["sections"]:
        suffix, ocp, vma, _ = IMAGES[a.chip]["sections"][sec]
        w, b = read_words(sec_path(a.chip, sec))
        for i, x in enumerate(w):
            if x == target or (x >> 16) == 0xB864 and (x & 0xffff) == (target & 0xffff) and target <= 0xffff:
                print(f"{sec} off 0x{i*4:05x}  OCP 0x{vma + i*4:08x}  = 0x{x:08x}")


def cmd_all(a):
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    for sec in IMAGES[a.chip]["sections"]:
        a.sec = sec
        cmd_disasm(a)
        if IMAGES[a.chip]["sections"][sec][3]:
            cmd_calls(a)
    cmd_mmio(a)
    cmd_ptrs(a)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("layout"); p.set_defaults(fn=cmd_layout)
    p = sub.add_parser("disasm"); p.add_argument("chip"); p.add_argument("sec")
    p.add_argument("--out", default=str(ROOT / "vendor/fwre")); p.set_defaults(fn=cmd_disasm)
    p = sub.add_parser("mmio"); p.add_argument("chip")
    p.add_argument("--out", default=str(ROOT / "vendor/fwre")); p.set_defaults(fn=cmd_mmio)
    p = sub.add_parser("ptrs"); p.add_argument("chip")
    p.add_argument("--out", default=str(ROOT / "vendor/fwre")); p.set_defaults(fn=cmd_ptrs)
    p = sub.add_parser("calls"); p.add_argument("chip"); p.add_argument("sec")
    p.add_argument("--out", default=str(ROOT / "vendor/fwre")); p.set_defaults(fn=cmd_calls)
    p = sub.add_parser("find"); p.add_argument("chip"); p.add_argument("word")
    p.set_defaults(fn=cmd_find, out=None)
    p = sub.add_parser("all"); p.add_argument("chip")
    p.add_argument("--out", default=str(ROOT / "vendor/fwre")); p.set_defaults(fn=cmd_all)
    a = ap.parse_args()
    return a.fn(a) or 0


if __name__ == "__main__":
    sys.exit(main())
