#!/usr/bin/env bash
# Build the MT7612U research probe into build-mt/ (devourer's mt7612u library
# compiled from its source tree + src/mt76sdr/*.cpp).  No CMake needed.
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
MT="${DEVOURER_MT7612U_SRC:-$HOME/dev/snokvist/devourer/src/mt7612u}"
B="$R/build-mt"; mkdir -p "$B/obj"
CXXF="-O2 -g -std=c++20 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -I$MT -I$MT/include"
objs=()
for s in "$MT"/*.cpp; do
  [[ "$(basename "$s")" == Mt7612uRadio.cpp ]] && continue
  o="$B/obj/$(basename "${s%.cpp}").o"
  [[ "$o" -nt "$s" ]] || g++ $CXXF -c "$s" -o "$o"
  objs+=("$o")
done
for t in "$R"/src/mt76sdr/*.cpp; do
  n="$(basename "${t%.cpp}")"
  g++ $CXXF "$t" "${objs[@]}" -o "$B/$n" -lusb-1.0 -lpthread
  echo "built $B/$n"
done
