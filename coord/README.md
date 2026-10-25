# coord/: coordination between the two agent sessions working in ~/wifi-sdr

Two sessions share this workspace: **claude** (Claude Code) and **opencode** (the OpenCode harness).
This directory is how they avoid stepping on each other. Joakim reads it too.

## Messages: one writer per file, append only

| File | Written by | Read by |
|---|---|---|
| `from-claude.md` | claude only | opencode, Joakim |
| `from-opencode.md` | opencode only | claude, Joakim |

Append with `tools/coord.sh say <you> "text"`, or append a `### <date time>` heading plus text yourself.
Never edit or truncate the other side's file. Claude watches `from-opencode.md` and is notified when
it grows.

## Locks for shared resources: atomic `mkdir`

```sh
tools/coord.sh lock   <resource> <you> "why"   # exit 1 + current owner if busy
tools/coord.sh unlock <resource> <you>
tools/coord.sh locks                           # who holds what
```

| Resource | Take it before you… |
|---|---|
| `8812bu` | open the RTL8812BU (0bda:b812) at all. Only one process can claim it, and chip state is shared. |
| `8812cu` | open the RTL8812CU (0bda:c812) at all. |
| `mt7612u` | open the MT7612U (0e8d:7612) at all. |
| `rtw88-guard` | run `tools/rtw88_guard.sh on/off`, or modprobe/rmmod the rtw88 or 88x2cu drivers. |
| `git` | `git add` / `git commit` in ~/wifi-sdr. Stage only your own files, and release right after. |

Release a lock as soon as you're done. A stale lock (owner gone) can be cleared by Joakim, or by
asking the owner in your message file first.

## Other house rules

- **Build directories:** claude uses `build/`. Use your own (for example `build-opencode/`) so the two
  sessions never run cmake in the same tree.
- **Files:** before editing a shared file (STATUS.md, LOG.md, FINDINGS.md, NEEDS.md), check
  `git status` and re-read it. Announce a large edit in your message file first.
- The bench rules in `HUMAN-INVESTIGATION.md` ("Bench rules in force") apply to both sessions.

## Lanes

- **claude:** read-only analysis, offline visualization from existing captures, documentation. Claude
  does not work on, or review, the on-chip transfer program or an rtl_tcp server. Don't route those to
  Claude through this channel.
- **opencode:** whatever Joakim assigns it. Post your lane and the files you'll touch in
  `from-opencode.md` so claude can stay clear of them.

The message files and `locks/` are gitignored; only this README and `tools/coord.sh` are tracked.
