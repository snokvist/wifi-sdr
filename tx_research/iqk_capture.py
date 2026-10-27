#!/usr/bin/env python3
"""Capture the TX adapter's bring-up IQK emission on the cabled bench.

The RX stream starts first; the TX probe is then opened (bring-up runs the
8822B host-driven IQK) and armed with no explicit test mode, so the only RF it
can emit is whatever the IQK engine produces. The TX adapter is reset in
finally. Intended for the IQK-tone question in tx_research/source_review.md.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROBE = ROOT / "build-opencode" / "wsdr_probe"
TXPROBE = ROOT / "build-opencode" / "wsdr_txprobe"
CAPS = ROOT / "tx_research" / "captures"
LOGS = ROOT / "tx_research" / "logs"
REGS = ROOT / "tx_research" / "regs"

sys.path.insert(0, str(ROOT / "tx_research"))
import tx_run  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", type=int, default=36)
    ap.add_argument("--rx-rate", type=int, default=2)
    ap.add_argument("--rx-bw", type=int, default=40)
    ap.add_argument("--rx-dfir", type=int, default=40)
    ap.add_argument("--rx-seconds", type=float, default=8.0)
    ap.add_argument("--baseline", type=float, default=0.5)
    ap.add_argument("--no-preclear", action="store_true",
                    help="skip the extra TX bring-up/reset before the RX starts")
    ap.add_argument("--tag", default=None)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    lab = tx_run.load_lab()
    if args.channel not in lab["channels"]:
        tx_run.die(f"channel {args.channel} not in allowed channels")
    tag = args.tag or time.strftime("%Y%m%d-%H%M%S") + f"-iqk-ch{args.channel}"
    CAPS.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    REGS.mkdir(parents=True, exist_ok=True)
    cap = CAPS / f"{tag}-rx"
    runlog = LOGS / f"{tag}.log"
    reglog = REGS / f"{tag}.jsonl"
    ready = LOGS / f"{tag}.ready"

    rx_cmd = [str(PROBE), "stream", "--pid", hex(tx_run.TX_PID),
              "--bus", str(tx_run.RX_BUS), "--port", tx_run.RX_PORT,
              "--channel", str(args.channel), "--bw", str(args.rx_bw),
              "--dfir", str(args.rx_dfir), "--rate", str(args.rx_rate),
              "--seconds", str(args.rx_seconds), "--out", str(cap)]
    tx_cmd = [str(TXPROBE), "raw", "--pid", hex(tx_run.TX_PID),
              "--bus", str(lab["tx_bus"]), "--port", lab["tx_port"],
              "--channel", str(args.channel), "--seconds", "0.2",
              "--log", str(reglog), "--wait-stdin", "--ready-file", str(ready)]
    print(f"bench: RX {tx_run.RX_BUS}-{tx_run.RX_PORT}; TX bring-up only on "
          f"{lab['tx_bus']}-{lab['tx_port']}, {lab['atten']} dB, channel {args.channel}")
    print(f"RX : {' '.join(rx_cmd)}")
    print(f"TX : {' '.join(tx_cmd)}")
    if args.dry_run:
        return 0
    for p in (PROBE, TXPROBE):
        if not p.exists():
            tx_run.die(f"{p} missing")
    if not tx_run.coord_lock(tag):
        return 2
    rx = tx = None
    try:
        with open(runlog, "w") as log:
            if not args.no_preclear:
                prelog = REGS / f"{tag}-preclear.jsonl"
                pre = [str(TXPROBE), "raw", "--pid", hex(tx_run.TX_PID),
                       "--bus", str(lab["tx_bus"]), "--port", lab["tx_port"],
                       "--channel", str(args.channel), "--seconds", "0.1",
                       "--log", str(prelog)]
                log.write(f"# pre-clear TX bring-up: $ {' '.join(pre)}\n")
                log.flush()
                r = subprocess.run(pre, cwd=ROOT, capture_output=True, text=True,
                                   timeout=60)
                log.write(r.stdout + r.stderr)
                log.flush()
                if r.returncode != 0:
                    tx_run.die(f"pre-clear TX bring-up failed rc={r.returncode}")
                tx_run.reset_tx(lab["tx_bus"], lab["tx_port"], log)
            ready.unlink(missing_ok=True)
            log.write(f"$ {' '.join(rx_cmd)}\n")
            log.flush()
            rx = subprocess.Popen(rx_cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            cs16 = Path(str(cap) + ".cs16")
            deadline = time.time() + 30
            while time.time() < deadline:
                if rx.poll() is not None:
                    tx_run.die(f"RX exited before TX bring-up; see {runlog}")
                if cs16.exists() and cs16.stat().st_size >= 1_000_000:
                    break
                time.sleep(0.1)
            if not cs16.exists() or cs16.stat().st_size < 1_000_000:
                tx_run.die(f"RX not producing samples; see {runlog}")
            log.write(f"# RX streaming, {cs16.stat().st_size} bytes; baseline\n")
            log.flush()
            time.sleep(max(0.0, args.baseline))
            try:
                import numpy as np
                raw = np.fromfile(cs16, dtype=np.int16)
                if raw.size >= 400000:
                    z = (raw[:400000:2].astype(np.float32) +
                         1j * raw[1:400000:2].astype(np.float32)) / 16.0
                    base_rms = float(np.sqrt(np.mean(np.abs(z) ** 2)))
                    log.write(f"# pre-TX baseline RMS {base_rms:.1f}\n")
                    log.flush()
                    if base_rms > 100.0:
                        tx_run.die(f"baseline RMS {base_rms:.1f} too high; aborting")
            except Exception as e:
                log.write(f"# baseline check skipped: {e}\n")
                log.flush()
            log.write(f"$ {' '.join(tx_cmd)}\n")
            log.flush()
            tx = subprocess.Popen(tx_cmd, cwd=ROOT, stdin=subprocess.PIPE,
                                  stdout=log, stderr=subprocess.STDOUT)
            deadline = time.time() + 90
            while time.time() < deadline:
                if tx.poll() is not None:
                    tx_run.die(f"TX exited early rc={tx.returncode}; see {runlog}")
                if ready.exists():
                    break
                time.sleep(0.05)
            if not ready.exists():
                tx_run.die(f"TX not ready; see {runlog}")
            log.write("# TX brought up (IQK done); arming no-op raw mode\n")
            log.flush()
            assert tx.stdin is not None
            tx.stdin.write(b"\n")
            tx.stdin.flush()
            try:
                tx.wait(timeout=8)
            except subprocess.TimeoutExpired:
                tx.kill()
                tx.wait()
            log.write(f"# TX rc={tx.returncode}\n")
            log.flush()
            try:
                rx.wait(timeout=args.rx_seconds + 10)
            except subprocess.TimeoutExpired:
                rx.kill()
                rx.wait()
            log.write(f"# RX rc={rx.returncode}\n")
    finally:
        for p in (tx, rx):
            if p is not None and p.poll() is None:
                p.kill()
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
        with open(runlog, "a") as log:
            ok = tx_run.reset_tx(lab["tx_bus"], lab["tx_port"], log)
            log.write(f"# TX reset {'ok' if ok else 'FAILED'}\n")
        tx_run.coord_unlock()
    print(f"wrote {cap}.cs16 and {runlog}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
