# SPDX-License-Identifier: GPL-2.0-only
# 0x914 low-word sweep on the 8822B embedded CPU (MIPS16, little-endian).
# Linked at 0x80000230 behind the vendor prologue.
#
# The host first arms the 0x914 single-tone test mode (mode 4, PMAC/TX-OFDM).
# This program then writes 0x914 = 0xc63c0000 | low for low = 0..0xffff in a
# tight loop (~5 M writes/s measured), repeating the sweep until the host halts
# the CPU. It is a probe for a register-fed phase/frequency control, not a
# finished waveform player.
#
# DMEM block at 0x80201000:
#   +0 magic 0x54393134 ("T914")   +4 state (1)   +8 completed sweeps
        .set    noreorder
        .set    mips16

        .macro  LI32 reg, val
        lui     \reg, (((\val) + 0x8000) >> 16) & 0xffff
        addiu   \reg, ((\val) & 0xffff) - (((\val) & 0x8000) << 1)
        .endm

        .text
        .globl  _start
_start:
        LI32    $16, 0x80201000
        LI32    $2, 0x54393134
        sw      $2, 0($16)
        li      $2, 1
        sw      $2, 4($16)
        li      $2, 0
        sw      $2, 8($16)
        lui     $3, 0xb864              # MAC/BB MMIO base
        lui     $5, 0xc63c              # 0x914: mode 4, low word swept
        li      $6, 0
        LI32    $7, 0x10000
sweep:
        addu    $4, $5, $6
        sw      $4, 0x914($3)
        addiu   $6, 1
        bne     $6, $7, sweep
        nop
        lw      $2, 8($16)
        addiu   $2, 1
        sw      $2, 8($16)
        li      $6, 0
        b       sweep
        nop
