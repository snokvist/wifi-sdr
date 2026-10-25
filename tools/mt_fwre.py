#!/usr/bin/env python3
"""mt_fwre.py - static RE helper for the MT7662/MT7612U NDS32 (Andes) firmware.

Input: radare2 disassembly (pD) of the ILM image mapped at 0x80000, e.g.
  r2 -q -e scr.color=0 -e asm.lines=0 -e asm.flags=0 -e asm.comments=0 \
     -a nds32 -b 32 -m 0x80000 -c "pD 0xe7b0" ilm.bin > ilm.dis
Does a crude per-basic-block constant propagation of sethi/ori/addi/movi and
resolves load/store effective addresses.  --gp=0x1012a8 for the 2015
mt7662.bin (gp is set by the mask ROM; inferred from string xrefs).  Prints  "<pc> <op> <ea>"  lines for
every resolved memory access (or, with --consts, every materialised constant).
"""
import re, sys
from collections import defaultdict

LINE = re.compile(r'^\s*0x([0-9a-f]+)\s+([0-9a-f]+)\s+(\S+)\s*(.*)$')
MEM = re.compile(r'\[(\w+)\s*(?:\+\s*(-?\s*\d+|0x[0-9a-f]+))?\]')
GP = None
BR = ('j', 'jr', 'ret', 'b', 'jral', 'iret')  # terminate tracking at control flow

def num(s):
    s = s.replace(' ', '')
    return int(s, 0)

def main():
    args = sys.argv[1:]
    consts = '--consts' in args
    global GP
    for a in args:
        if a.startswith('--gp='): GP = int(a[5:], 0)
    args = [a for a in args if not a.startswith('--')]
    regs = {}
    out = []
    for ln in open(args[0]):
        m = LINE.match(ln)
        if not m:
            continue
        pc = int(m.group(1), 16); op = m.group(3); ops = m.group(4)
        parts = [p.strip() for p in ops.split(',')]
        try:
            if op == 'addi.gp':
                regs[parts[0]] = (GP + num(parts[1])) & 0xffffffff
                if consts: out.append((pc, 'const', regs[parts[0]]))
            elif op.endswith('.gp') and GP is not None:
                mg = re.search(r'\[\s*([+-]?\s*\d+)\s*\]', ops)
                if mg: out.append((pc, op, (GP + num(mg.group(1))) & 0xffffffff))
                regs.pop(parts[0], None)
            elif op == 'sethi':
                regs[parts[0]] = num(parts[1]) << 12
            elif op in ('ori',) and parts[1] in regs:
                regs[parts[0]] = regs[parts[1]] | num(parts[2])
                if consts: out.append((pc, 'const', regs[parts[0]]))
            elif op in ('addi', 'addi333', 'addi45', 'addi10s') and parts[-2 if len(parts)==3 else 0] in regs:
                src = parts[1] if len(parts) == 3 else parts[0]
                regs[parts[0]] = (regs[src] + num(parts[-1])) & 0xffffffff
                if consts: out.append((pc, 'const', regs[parts[0]]))
            elif op in ('movi', 'movi55'):
                regs[parts[0]] = num(parts[1]) & 0xffffffff
            elif op in ('mov55',) and parts[1] in regs:
                regs[parts[0]] = regs[parts[1]]
            else:
                mm = MEM.search(ops)
                if mm and (op.startswith('l') or op.startswith('s')) and not op.startswith('sl') \
                        and not op.startswith('sr') and not op.startswith('sub') and op not in ('lmw.bim', 'smw.adm', 'lmw.adm', 'smw.bim', 'lbi.bi', 'swi.bi', 'lwi.bi', 'sbi.bi'):
                    base = mm.group(1)
                    off = num(mm.group(2)) if mm.group(2) else 0
                    if base in regs:
                        out.append((pc, op, (regs[base] + off) & 0xffffffff))
                # any write to dest register invalidates it
                if parts and parts[0] in regs and not op.startswith('s') and op not in ('beq', 'bne', 'beqz', 'bnez'):
                    regs.pop(parts[0], None)
            if op.startswith(BR) or op in ('ret5', 'jral5', 'jr5', 'j8'):
                regs = {}
        except Exception:
            regs.pop(parts[0] if parts else None, None)
    for pc, op, ea in out:
        print(f'{pc:08x} {op:10s} {ea:08x}')

if __name__ == '__main__':
    main()
