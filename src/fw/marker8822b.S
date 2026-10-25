# SPDX-License-Identifier: GPL-2.0-only
# Marker probe for the 8822B embedded CPU (MIPS16e2, little-endian).
# Linked at 0x80000230, directly after the vendor IMEM prologue (CP0/stack init, 0x000..0x22f),
# which falls through into _start. Proves: code upload, execution, DMEM writes, the free-running
# microsecond counter at REG 0x568 (uncached CPU view 0xB8640568).
# DMEM block at 0x80201000 (OCP 0x201000):
#   +0 magic 0xCAFEF00D, +4 loop counter, +8 last REG 0x568 value, +12 first REG 0x568 value
# Rules learned from ac54_sdr: MIPS16e2 only (no MIPS32, no JALX), no PC-relative literal loads,
# so every 32-bit constant is built with LUI + ADDIU/ORI.
        .set    noreorder
        .set    mips16
        .text
        .globl  _start
_start:
        lui     $16, 0x8020
        addiu   $16, 0x1000             # s0 = 0x80201000
        lui     $17, 0xb864             # s1 = 0xB8640000 (MAC registers, uncached)
        lui     $3, 0xcafe
        ori     $3, 0xf00d
        sw      $3, 0($16)
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
