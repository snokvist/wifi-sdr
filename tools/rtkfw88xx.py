#!/usr/bin/env python3
"""Parse, verify, carve and extract Realtek HALMAC "88xx" WLAN firmware images.

Offline only: never touches hardware. Run with `python3 -I` because inputs
(vendor drivers, firmware blobs) are untrusted data.

Image layout (halmac_fw_info.h WLAN_FW_HDR_*, halmac_fw_88xx.c start_dlfw_88xx):

    [0x00..0x40)  64-byte header (all fields little-endian)
    DMEM body  (hdr.dmem_size bytes)  + 8-byte checksum trailer
    IMEM body  (hdr.imem_size bytes)  + 8-byte checksum trailer
    EMEM body  (hdr.emem_size bytes)  + 8-byte checksum trailer   only if mem_usage & 0x10

Trailer: u16 = XOR of every little-endian 16-bit halfword of the body, then 6
zero bytes (equivalently: XOR16 over body+trailer == 0). This is what the DDMA
engine's CHKSUM_EN mode checks; it is not a signature.

Load addresses are the header's dmem/imem/emem_addr with BIT31 cleared
(halmac: `addr &= ~BIT(31)`), i.e. OCP addresses for the DDMA destination.

Subcommands:
  info  FILE...                 parse .bin / .bin.zst images and print a report
  carray CFILE [--array NAME]   extract u8 arrays from a vendor hal*_fw.c
  scan  BLOB                    find embedded 88xx images in any blob (e.g. a .sys)
Common options:
  --out DIR     write the image(s) and per-section bodies there
  --json        machine-readable report on stdout
  --ac54 DIR    also write ac54_sdr-style vendor-imem.bin / vendor-dmem.bin
                (bodies WITHOUT checksum trailer, exactly as tools/fw_inspect.py)
                from the first image after --select filtering
  --select STR  keep only images whose name contains STR

Examples (see tools/extract_vendor_fw.sh for the full regeneration):
  rtkfw88xx.py info /lib/firmware/rtw88/rtw8822b_fw.bin.zst --ac54 vendor/ac54
  rtkfw88xx.py carray hal8822b_fw.c --select fw_nic --out vendor/88x2bu
  rtkfw88xx.py scan rtwlanu.sys --select 0x005ba7e0 --ac54 vendor/win/ac54
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

HDR_SIZE = 64
CHK_SIZE = 8
KNOWN_SIGS = {0x8822: "8822B/8822C family", 0x8821: "8821C", 0x8812: "8812F",
              0x8814: "8814A/8814B", 0x8197: "8197F", 0x88E2: "8822E?"}


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def xor16(b: bytes) -> int:
    x = 0
    for (w,) in struct.iter_unpack("<H", b[: len(b) & ~1]):
        x ^= w
    return x


def read_any(path: Path) -> bytes:
    raw = path.read_bytes()
    if path.suffix == ".zst":
        from compression import zstd  # Python >= 3.14
        return zstd.decompress(raw)
    return raw


def parse_header(d: bytes, base: int = 0) -> dict:
    u8 = lambda o: d[base + o]
    u16 = lambda o: struct.unpack_from("<H", d, base + o)[0]
    u32 = lambda o: struct.unpack_from("<I", d, base + o)[0]
    h = {
        "signature": u16(0x00),
        "category": u8(0x02),
        "function": u8(0x03),
        "version": u16(0x04),
        "subversion": u8(0x06),
        "subindex": u8(0x07),
        "word_0x08": u32(0x08),       # rtw88: rsvd (svn/id on some builds)
        "word_0x0c": u32(0x0C),       # rtw88: feature
        "month": u8(0x10), "day": u8(0x11), "hour": u8(0x12), "minute": u8(0x13),
        "year": u16(0x14),
        "mem_usage": u8(0x18),
        "h2c_fmt_ver": u16(0x1C),
        "dmem_addr": u32(0x20),
        "dmem_size": u32(0x24),
        "word_0x28": u32(0x28),
        "word_0x2c": u32(0x2C),
        "imem_size": u32(0x30),
        "emem_size": u32(0x34),
        "emem_addr": u32(0x38),
        "imem_addr": u32(0x3C),
    }
    h["has_emem"] = bool(h["mem_usage"] & 0x10)
    return h


def layout(h: dict) -> list[tuple[str, int, int, int]]:
    """(name, body_offset_rel, body_size, ocp_load_addr) for each section."""
    secs = []
    off = HDR_SIZE
    secs.append(("dmem", off, h["dmem_size"], h["dmem_addr"] & 0x7FFFFFFF))
    off += h["dmem_size"] + CHK_SIZE
    secs.append(("imem", off, h["imem_size"], h["imem_addr"] & 0x7FFFFFFF))
    off += h["imem_size"] + CHK_SIZE
    if h["has_emem"]:
        secs.append(("emem", off, h["emem_size"], h["emem_addr"] & 0x7FFFFFFF))
        off += h["emem_size"] + CHK_SIZE
    return secs


def total_size(h: dict) -> int:
    n = HDR_SIZE + h["dmem_size"] + CHK_SIZE + h["imem_size"] + CHK_SIZE
    if h["has_emem"]:
        n += h["emem_size"] + CHK_SIZE
    return n


def plausible(h: dict) -> bool:
    if (h["dmem_addr"] & 0x7FFFFFFF) != 0x200000:
        return False
    if not (0 < h["dmem_size"] < 0x40000 and 0 < h["imem_size"] < 0x80000):
        return False
    if (h["imem_addr"] & 0x7FFFFFFF) >= 0x200000:
        return False
    if h["has_emem"] and not (0 < h["emem_size"] < 0x100000):
        return False
    return True


def analyse(img: bytes, name: str) -> dict:
    h = parse_header(img)
    rep = {"name": name, "size": len(img), "sha256": sha256(img), "header": h,
           "expected_size": total_size(h), "size_ok": total_size(h) == len(img),
           "sections": []}
    for sname, off, size, addr in layout(h):
        body = img[off:off + size]
        trailer = img[off + size:off + size + CHK_SIZE]
        calc = xor16(body)
        stored = struct.unpack_from("<H", trailer, 0)[0] if len(trailer) >= 2 else None
        rep["sections"].append({
            "name": sname, "file_offset": off, "size": size,
            "load_addr_ocp": addr, "body_sha256": sha256(body),
            "trailer": trailer.hex(), "xor16_calc": calc, "xor16_stored": stored,
            "checksum_ok": stored == calc and trailer[2:] == b"\0" * 6,
        })
    return rep


def fmt_report(r: dict) -> str:
    h = r["header"]
    sig = h["signature"]
    out = [f"== {r['name']}",
           f"  size {r['size']} (expected {r['expected_size']}, {'OK' if r['size_ok'] else 'MISMATCH'})"
           f"  sha256 {r['sha256']}",
           f"  signature 0x{sig:04x} ({KNOWN_SIGS.get(sig, '?')})  category {h['category']} function {h['function']}",
           f"  version {h['version']}.{h['subversion']}.{h['subindex']}"
           f"  (raw ver=0x{h['version']:04x} sub=0x{h['subversion']:02x} idx=0x{h['subindex']:02x})"
           f"  h2c_fmt_ver {h['h2c_fmt_ver']}",
           f"  build {h['year']:04d}-{h['month']:02d}-{h['day']:02d} {h['hour']:02d}:{h['minute']:02d}"
           f"  mem_usage 0x{h['mem_usage']:02x} (EMEM {'yes' if h['has_emem'] else 'no'})",
           f"  word@0x08 0x{h['word_0x08']:08x}  word@0x0c 0x{h['word_0x0c']:08x}"
           f"  word@0x28 0x{h['word_0x28']:08x}  word@0x2c 0x{h['word_0x2c']:08x}",
           f"  hdr dmem_addr 0x{h['dmem_addr']:08x} imem_addr 0x{h['imem_addr']:08x}"
           f" emem_addr 0x{h['emem_addr']:08x}"]
    if "imem_prefix" in r:
        out.append(f"  imem[0:0x{r['imem_prefix']['len']:x}] sha256 {r['imem_prefix']['sha256']}")
    for s in r["sections"]:
        out.append(f"  {s['name']}: off 0x{s['file_offset']:06x} size {s['size']:7d} (0x{s['size']:05x})"
                   f" -> OCP 0x{s['load_addr_ocp']:08x}  xor16 0x{s['xor16_calc']:04x}"
                   f" {'OK' if s['checksum_ok'] else 'BAD(stored ' + str(s['xor16_stored']) + ')'}"
                   f"  sha256 {s['body_sha256']}")
    return "\n".join(out)


def write_outputs(img: bytes, rep: dict, out: Path | None, ac54: Path | None, stem: str) -> None:
    if out:
        out.mkdir(parents=True, exist_ok=True)
        (out / f"{stem}.bin").write_bytes(img)
        for s in rep["sections"]:
            (out / f"{stem}.{s['name']}.bin").write_bytes(
                img[s["file_offset"]:s["file_offset"] + s["size"]])
    if ac54:
        ac54.mkdir(parents=True, exist_ok=True)
        sec = {s["name"]: s for s in rep["sections"]}
        for n in ("imem", "dmem"):
            s = sec[n]
            (ac54 / f"vendor-{n}.bin").write_bytes(img[s["file_offset"]:s["file_offset"] + s["size"]])


ARRAY_RE = re.compile(r"\bu8\s+(array_mp_[A-Za-z0-9_]+)\s*\[\s*\]\s*=\s*\{", re.S)


def carrays(text: str) -> dict[str, bytes]:
    res = {}
    for m in ARRAY_RE.finditer(text):
        end = text.index("};", m.end())
        body = text[m.end():end]
        res[m.group(1)] = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", body))
        lm = re.search(r"array_length_" + re.escape(m.group(1)[len("array_"):]) + r"\s*=\s*(\d+)", text)
        if lm and int(lm.group(1)) != len(res[m.group(1)]):
            print(f"WARNING {m.group(1)}: parsed {len(res[m.group(1)])} bytes, "
                  f"declared {lm.group(1)}", file=sys.stderr)
    return res


def scan(blob: bytes) -> list[int]:
    hits = []
    for sig in KNOWN_SIGS:
        pat = struct.pack("<H", sig)
        i = blob.find(pat)
        while i != -1:
            if i + HDR_SIZE <= len(blob):
                h = parse_header(blob, i)
                if plausible(h) and i + total_size(h) <= len(blob):
                    img = blob[i:i + total_size(h)]
                    if all(s["checksum_ok"] for s in analyse(img, "")["sections"]):
                        hits.append(i)
            i = blob.find(pat, i + 1)
    return sorted(hits)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["info", "carray", "scan"])
    ap.add_argument("inputs", nargs="+", type=Path)
    ap.add_argument("--array", action="append", help="carray: only these array names")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--ac54", type=Path, help="write vendor-imem.bin/vendor-dmem.bin (first/selected image)")
    ap.add_argument("--prefix", default="", help="filename prefix for --out")
    ap.add_argument("--select", help="only images whose name contains this substring")
    ap.add_argument("--boot-prefix", type=lambda s: int(s, 0), default=0,
                    help="also hash IMEM[0:N) (ac54_sdr keeps N=0x230 of the vendor IMEM)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    reports = []
    images: list[tuple[str, bytes]] = []
    for p in a.inputs:
        if a.cmd == "info":
            images.append((p.name.removesuffix(".zst").removesuffix(".bin"), read_any(p)))
        elif a.cmd == "carray":
            arrs = carrays(p.read_text(errors="replace"))
            for n, b in arrs.items():
                if a.array and n not in a.array:
                    continue
                images.append((n, b))
        else:
            blob = read_any(p)
            for off in scan(blob):
                h = parse_header(blob, off)
                images.append((f"{p.stem}@0x{off:08x}", blob[off:off + total_size(h)]))
    if a.select:
        images = [(n, b) for n, b in images if a.select in n]
    ac54_done = False
    for name, img in images:
        rep = analyse(img, name)
        if a.boot_prefix:
            im = next(s for s in rep["sections"] if s["name"] == "imem")
            body = img[im["file_offset"]:im["file_offset"] + min(a.boot_prefix, im["size"])]
            rep["imem_prefix"] = {"len": len(body), "sha256": sha256(body)}
        reports.append(rep)
        stem = a.prefix + name.replace("@", "_at_")
        write_outputs(img, rep, a.out, None if ac54_done else a.ac54, stem)
        if a.ac54:
            ac54_done = True
    if a.json:
        json.dump(reports, sys.stdout, indent=1)
        print()
    else:
        for r in reports:
            print(fmt_report(r))
        if a.cmd == "scan":
            print(f"-- {len(reports)} embedded 88xx image(s) found")
    return 0 if all(r["size_ok"] and all(s["checksum_ok"] for s in r["sections"]) for r in reports) else 1


if __name__ == "__main__":
    sys.exit(main())
