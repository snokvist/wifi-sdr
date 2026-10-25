# SPDX-License-Identifier: GPL-2.0-only
# Marker + ISA probe for the 8822B embedded CPU (MIPS16 + partial MIPS16e2, little-endian).
# Linked at 0x80000230, directly after the vendor IMEM prologue (CP0/stack init, 0x000..0x22f),
# which falls through into _start.
# DMEM block at 0x80201000 (OCP 0x201000):
#   +0  magic 0xCAFEF00D (LUI + ADDIU)          +4  loop counter
#   +8  last REG 0x568 (free-running us timer)   +12 first REG 0x568
#   +16 LUI 0x1234 ; ORI  0x5678   (0x12345678 if MIPS16e2 ORI works)
#   +20 LUI 0x1234 ; XORI 0x00ff   (0x123400ff if MIPS16e2 XORI works)
#   +24 LUI 0x1234 ; ANDI 0xffff   (0x00000000? 0x0000ffff? — records what ANDI does)
# MEASURED (2026-10-25, 8812BU cut 3): LUI works; ORI behaved like LI (upper half zeroed).
# Rules: no MIPS32, no JALX, no PC-relative literal loads; 32-bit constants via LI32 (LUI + ADDIU).
        .set    noreorder
        .set    mips16

        .macro  LI32 reg, val
        lui     \reg, (((\val) + 0x8000) >> 16) & 0xffff
        addiu   \reg, ((\val) & 0xffff) - (((\val) & 0x8000) << 1)
        .endm

        .text
        .globl  _start
_start:
        LI32    $16, 0x80201000         # s0 = DMEM scratch block
        lui     $17, 0xb864             # s1 = 0xB8640000 (MAC registers, uncached)
        LI32    $3, 0xcafef00d
        sw      $3, 0($16)
        lui     $3, 0x1234
        ori     $3, 0x5678
        sw      $3, 16($16)
        lui     $3, 0x1234
        xori    $3, 0x00ff
        sw      $3, 20($16)
        lui     $3, 0x1234
        andi    $3, 0xffff
        sw      $3, 24($16)
        li      $2, 0
        sw      $2, 4($16)
        lw      $4, 0x568($17)
        sw      $4, 12($16)
1:      lw      $2, 4($16)
        addiu   $2, 1
        sw      $2, 4($16)
        lw      $4, 0x568($17)
        sw      $4, 8($16)
        b       1b
        nop
