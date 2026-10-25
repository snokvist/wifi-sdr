#!/usr/bin/env bash
# coord.sh — coordination helper for the two agent sessions sharing ~/wifi-sdr (see coord/README.md).
#   coord.sh lock <resource> <claude|opencode> ["why"]   atomic (mkdir); exit 1 + owner if busy
#   coord.sh unlock <resource> <claude|opencode>          only the owner may release
#   coord.sh locks                                        list held locks
#   coord.sh say <claude|opencode> "message"              append to coord/from-<who>.md
set -euo pipefail
D="$(cd "$(dirname "$0")/.." && pwd)/coord"
mkdir -p "$D/locks"

usage() { sed -n '2,6p' "$0" >&2; exit 2; }
who_ok() { [[ "$1" == claude || "$1" == opencode ]] || { echo "who must be claude|opencode" >&2; exit 2; }; }
res_ok() { [[ "$1" =~ ^[a-z0-9-]+$ ]] || { echo "resource must match [a-z0-9-]+" >&2; exit 2; }; }

case "${1:-}" in
lock)
  [[ $# -ge 3 ]] || usage
  res_ok "$2"; who_ok "$3"
  if mkdir "$D/locks/$2" 2>/dev/null; then
    printf '%s\t%s\t%s\n' "$3" "$(date -Is)" "${4:-}" >"$D/locks/$2/owner"
    echo "locked $2"
  else
    echo "BUSY $2: $(cat "$D/locks/$2/owner" 2>/dev/null || echo '(owner file missing)')"
    exit 1
  fi
  ;;
unlock)
  [[ $# -ge 3 ]] || usage
  res_ok "$2"; who_ok "$3"
  owner=$(cut -f1 "$D/locks/$2/owner" 2>/dev/null || true)
  [[ "$owner" == "$3" ]] || { echo "not released: $2 is held by '${owner:-nobody}'" >&2; exit 1; }
  rm -f "$D/locks/$2/owner" && rmdir "$D/locks/$2"
  echo "unlocked $2"
  ;;
locks)
  for l in "$D"/locks/*/; do
    [[ -d "$l" ]] && printf '%s\t%s\n' "$(basename "$l")" "$(cat "$l/owner" 2>/dev/null)"
  done
  true
  ;;
say)
  [[ $# -ge 3 ]] || usage
  who_ok "$2"
  w=$2; shift 2
  printf '\n### %s\n%s\n' "$(date '+%F %T')" "$*" >>"$D/from-$w.md"
  ;;
*) usage ;;
esac
