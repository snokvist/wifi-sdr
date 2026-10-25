#!/usr/bin/env bash
# Regenerate every firmware-RE artifact under vendor/fwre/ (gitignored,
# vendor-derived). OFFLINE ONLY -- no hardware, no USB, no kernel modules.
# Requires: mipsel-linux-gnu-objdump (binutils), python3.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cd "$root"
for chip in 8822b 8822c; do
    python3 -I tools/fwre/fwre.py all "$chip"
done
echo "artifacts in $root/vendor/fwre/"
ls -1 "$root/vendor/fwre/"
