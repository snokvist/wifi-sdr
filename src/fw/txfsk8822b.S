# SPDX-License-Identifier: GPL-2.0-only
# Two-tone 0x910 FSK player for the 8822B embedded CPU (MIPS16).
# Linked at 0x80000230 behind the vendor prologue.
#
# Host pre-arms 0x914 mode 4 and loads this program. The program writes
# 0x910 (tone frequency; low 12 bits, 0xfc00 preserved), drives the TXAGC word
# at 0x1d00/0x1d80, and dwells on each tone using REG 0x568.
#
# DMEM block at 0x80201000:
#   +0  magic 0x46534B31 ("FSK1")   +4  f1 (0x910 low word)
#   +8  f2                         +12 dwell_us
#   +16 transitions (0 = infinite) +20 reserved
#   +24 done                       +28 completed transitions
#   +32 TXAGC word (idx * 0x01010101)
        .set    noreorder
        .set    mips16

        .macro  LI32 reg, val
        lui     \reg, (((\val) + 0x8000) >> 16) & 0xffff
        addiu   \reg, ((\val) & 0xffff) - (((\val) & 0x8000) << 1)
        .endm

        .macro  SETF reg
        LI32    $2, 0x8666636d
        sw      $2, 0x838($17)
        LI32    $2, 0x9220d010
        sw      $2, 0x800($17)
        LI32    $2, 0x00d047c8
        sw      $2, 0xa00($17)
        LI32    $2, 0xc63c0000
        sw      $2, 0x914($17)
        LI32    $2, 0xf000
        addu    $2, $2, \reg
        sw      $2, 0x910($17)
        LI32    $2, 0xe1000540
        sw      $2, 0xb00($17)
        LI32    $2, 0x00800004
        sw      $2, 0xb04($17)
        .endm

        .text
        .globl  _start
_start:
        LI32    $16, 0x80201000
        LI32    $2, 0x46534b31
        sw      $2, 0($16)
        li      $2, 0
        sw      $2, 24($16)
        sw      $2, 28($16)
        lui     $17, 0xb864

loop:
        lw      $4, 4($16)              # f1
        SETF    $4
        lw      $6, 12($16)             # dwell us
        lw      $5, 0x568($17)
1:      lw      $2, 0x568($17)
        subu    $2, $2, $5
        sltu    $2, $6
        btnez   1b
        nop

        lw      $4, 8($16)              # f2
        SETF    $4
        lw      $6, 12($16)
        lw      $5, 0x568($17)
2:      lw      $2, 0x568($17)
        subu    $2, $2, $5
        sltu    $2, $6
        btnez   2b
        nop

        lw      $2, 28($16)
        addiu   $2, 1
        sw      $2, 28($16)
        lw      $3, 16($16)
        beqz    $3, loop
        sltu    $2, $3
        btnez   loop
        nop
        li      $2, 1
        sw      $2, 24($16)
3:      b       3b
        nop
