#!/usr/bin/env bash
# Temporarily keep the in-tree rtw88 USB modules away from the adapters under test.
# Why: rtw88_* auto-probes (and downloads firmware into) every matching Realtek dongle at
# each USB enumeration; `modprobe -r` alone does not survive a re-enumeration, so we also
# drop a temp blacklist file (devourer tests/adapter_doctor_cold.sh uses the same recipe).
# Only the per-chip USB modules are listed (incl. the DKMS vendor 88x2cu); nothing the host's own networking uses.
#   sudo tools/rtw88_guard.sh on     # blacklist + unload
#   sudo tools/rtw88_guard.sh off    # remove blacklist (modules reload on next plug / modprobe)
#   tools/rtw88_guard.sh status
set -euo pipefail
CONF=/etc/modprobe.d/zz-wifi-sdr-temp-blacklist.conf
MODS=(rtw88_8822bu rtw88_8822cu rtw88_8821cu 88x2cu)
case "${1:-status}" in
  on)
    printf '# temporary: wifi-sdr bench (remove with tools/rtw88_guard.sh off)\n' > "$CONF"
    for m in "${MODS[@]}"; do echo "blacklist $m" >> "$CONF"; done
    for m in "${MODS[@]}"; do modprobe -r "$m" 2>/dev/null || true; done
    echo "guard ON: $CONF"; ;;
  off)
    rm -f "$CONF"; echo "guard OFF (blacklist removed; replug or 'modprobe rtw88_8822bu' to restore the kernel driver)"; ;;
  status)
    [ -f "$CONF" ] && { echo "guard ON"; cat "$CONF"; } || echo "guard OFF"
    lsmod | grep -E '^rtw88_' || true; ;;
  *) echo "usage: $0 on|off|status" >&2; exit 2 ;;
esac
