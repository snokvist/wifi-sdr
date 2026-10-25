# SPDX-License-Identifier: GPL-2.0-only
# LA ring timing probe for the 8822B embedded CPU (MIPS16, little-endian), linked at 0x80000230.
#
# Arms the free-running LA ring (0x7c0 = 0xcb), waits D microseconds on REG 0x568, then fires the
# manual trigger (0x7c0 = 0xeb). The host reads the finish address from 0x7c0 and compares the ring
# position with D * fs: this measures the sample rate and the arm-to-first-sample latency on the
# chip's own clock, which the streamer's bank schedule depends on. The host configures the LA
# (rate, path, CCK off, MAC RX on) and leaves it stopped before starting the CPU.
#
# DMEM block at 0x80201000:
#   +0   magic 0x1A7E0001        +4  t before the arm write    +8  t after the arm write
#   +12  t after the trigger     +16 done (1)                  +24 D in us (host-seeded)
        .set    noreorder
        .set    mips16

        .macro  LI32 reg, val
        lui     \reg, (((\val) + 0x8000) >> 16) & 0xffff
        addiu   \reg, ((\val) & 0xffff) - (((\val) & 0x8000) << 1)
        .endm

        .text
        .globl  _start
_start:
        LI32    $16, 0x80201000         # s0 = DMEM state block
        lui     $17, 0xb864             # s1 = MAC registers (uncached)
        LI32    $2, 0x1a7e0001
        sw      $2, 0($16)
        li      $2, 0
        sw      $2, 16($16)
        lw      $4, 24($16)             # a0 = D
        li      $3, 0xcb
        lw      $2, 0x568($17)
        sw      $2, 4($16)
        sb      $3, 0x7c0($17)          # arm: ring starts at TXFF 0x20000
        lw      $5, 0x568($17)          # a1 = t_arm
        sw      $5, 8($16)
1:      lw      $2, 0x568($17)
        subu    $2, $2, $5              # elapsed
        sltu    $2, $4                  # T = elapsed < D
        btnez   1b
        li      $3, 0xeb
        sb      $3, 0x7c0($17)          # manual trigger edge
        lw      $2, 0x568($17)
        sw      $2, 12($16)
        li      $2, 1
        sw      $2, 16($16)
2:      b       2b
        nop
