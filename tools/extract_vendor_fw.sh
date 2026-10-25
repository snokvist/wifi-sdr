#!/usr/bin/env bash
# Regenerate every vendor-derived firmware file under vendor/ (gitignored).
# Offline only: reads local files, never touches USB devices or kernel modules.
#
# Inputs (all optional; missing ones are skipped with a note):
#   /lib/firmware/rtw88/rtw8822{b,c}_fw.bin.zst, rtw8822c_wow_fw.bin.zst  (linux-firmware)
#   ref/ac54_sdr/artifacts/rtwlanu.sys                    (Windows driver, ac54_sdr repo)
#   ref/88x2bu-20210702  (git clone https://github.com/morrownr/88x2bu-20210702;
#                         ac54_sdr pins d31ffa827bb95b8a436c2a469b5163b634ac4333)
#   ref/devourer/reference/rtl88x2cu  (git -C ref/devourer submodule update --init reference/rtl88x2cu)
#   /usr/src/rtl88x2cu-*            (DKMS vendor driver, 8822C)
#   ref/devourer/hal/hal8822{b,c}_fw.c  (devourer's embedded NIC blobs)
#
# Outputs: vendor/<source>/<image>.bin + per-section .dmem/.imem/.emem.bin + report.json,
#          vendor/*/ac54/vendor-{imem,dmem}.bin (ac54_sdr tmp/ format), vendor/SHA256SUMS.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
V=$ROOT/vendor
T=$ROOT/tools/rtkfw88xx.py
PY="python3 -I"
mkdir -p "$V"
run() { echo "+ $*" >&2; "$@"; }

# 1) linux-firmware (rtw88) .bin.zst -> .bin
if ls /lib/firmware/rtw88/rtw8822[bc]*_fw.bin.zst >/dev/null 2>&1; then
  mkdir -p "$V/linux-firmware"
  for f in rtw8822b_fw rtw8822c_fw rtw8822c_wow_fw; do
    [ -f /lib/firmware/rtw88/$f.bin.zst ] && zstd -q -d -f /lib/firmware/rtw88/$f.bin.zst -o "$V/linux-firmware/$f.bin"
  done
  run $PY "$T" info "$V"/linux-firmware/*.bin --out "$V/linux-firmware/sections" --boot-prefix 0x230 --json > "$V/linux-firmware/report.json"
  run $PY "$T" info "$V/linux-firmware/rtw8822b_fw.bin" --ac54 "$V/linux-firmware/ac54" > /dev/null
else echo "skip: no rtw88 firmware in /lib/firmware" >&2; fi

# 2) Windows driver: carve embedded 88xx images by header + XOR16 checksum
SYS=$ROOT/ref/ac54_sdr/artifacts/rtwlanu.sys
if [ -f "$SYS" ]; then
  mkdir -p "$V/win"
  sha256sum "$SYS" > "$V/win/rtwlanu.sys.sha256"
  run $PY "$T" scan "$SYS" --out "$V/win" --boot-prefix 0x230 --json > "$V/win/scan.json"
  # NIC image (inferred: 2nd of the ap/nic/wowlan length triple at file offset 0x5ba7d0)
  run $PY "$T" scan "$SYS" --select 0x005ba7e0 --ac54 "$V/win/ac54" > /dev/null
else echo "skip: $SYS missing" >&2; fi

# 3) Vendor Linux driver C arrays
B=$ROOT/ref/88x2bu-20210702/hal/rtl8822b/hal8822b_fw.c
if [ -f "$B" ]; then
  mkdir -p "$V/88x2bu"
  run $PY "$T" carray "$B" --out "$V/88x2bu" --boot-prefix 0x230 --json > "$V/88x2bu/report.json"
  # exact ac54_sdr tools/fw_inspect.py equivalent (array_mp_8822b_fw_nic)
  run $PY "$T" carray "$B" --select fw_nic --ac54 "$V/88x2bu/ac54" > /dev/null
else echo "skip: $B missing" >&2; fi
for src in "$ROOT/ref/devourer/reference/rtl88x2cu:88x2cu-devourer-submodule" /usr/src/rtl88x2cu-*; do
  dir=${src%%:*}; name=${src#*:}; [ "$name" = "$src" ] && name="88x2cu-usrsrc-$(basename "$dir" | sed 's/^rtl88x2cu-//')"
  [ -f "$dir/hal/rtl8822c/hal8822c_fw.c" ] || { echo "skip: $dir" >&2; continue; }
  mkdir -p "$V/$name"
  run $PY "$T" carray "$dir/hal/rtl8822c/hal8822c_fw.c" --out "$V/$name" --boot-prefix 0x230 --json > "$V/$name/report.json"
  [ -f "$dir/hal/rtl8822c/hal8822c_fw_10M.c" ] && \
    run $PY "$T" carray "$dir/hal/rtl8822c/hal8822c_fw_10M.c" --out "$V/$name" --prefix 10M_ --json > "$V/$name/report_10M.json"
done

# 4) devourer's embedded NIC blobs
for c in hal8822b_fw hal8822c_fw; do
  f=$ROOT/ref/devourer/hal/$c.c
  [ -f "$f" ] || continue
  mkdir -p "$V/devourer"
  run $PY "$T" carray "$f" --out "$V/devourer" --prefix devourer_ --json > "$V/devourer/$c.report.json"
done

(cd "$V" && find . -type f -name '*.bin' | sort | xargs sha256sum > SHA256SUMS)
echo "done: $(wc -l < "$V/SHA256SUMS") files hashed in $V/SHA256SUMS" >&2
