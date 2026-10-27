# SPDX-License-Identifier: GPL-2.0-only
# BB-register write throughput probe for the 8822B embedded CPU.
# Linked at 0x80000230 behind the vendor prologue.
#
# DMEM block at 0x80201000 (host-seeded):
#   +0  magic 0x54584231 ("TXB1")   +4  BB register offset (0x0000..0xffff)
#   +8  value to write              +12 write count
#   +16 elapsed microseconds        +20 done (1)
#
# The probe writes the same value to base 0xb8640000 + offset N times and
# measures REG 0x568. It touches nothing else. This measures whether the WCPU
# can drive a BB register fast enough for a register-fed waveform experiment.
        .set    noreorder
        .set    mips16

        .macro  LI32 reg, val
        lui     \reg, (((\val) + 0x8000) >> 16) & 0xffff
        addiu   \reg, ((\val) & 0xffff) - (((\val) & 0x8000) << 1)
        .endm

        .text
        .globl  _start
_start:
        LI32    $16, 0x80201000         # s0 = DMEM block
        LI32    $2, 0x54584231
        sw      $2, 0($16)
        lui     $3, 0xb864              # v1 = MAC/BB base (uncached)
        lw      $4, 4($16)              # a0 = register offset
        addu    $17, $3, $4             # s1 = base + offset
        lw      $5, 8($16)              # a1 = value
        lw      $6, 12($16)             # a2 = count
        lw      $7, 0x568($3)           # a3 = t0
1:      sw      $5, 0($17)
        addiu   $6, -1
        bnez    $6, 1b
        nop
        lw      $2, 0x568($3)           # t1
        subu    $2, $2, $7
        sw      $2, 16($16)             # elapsed us
        li      $2, 1
        sw      $2, 20($16)             # done
2:      b       2b
        nop
