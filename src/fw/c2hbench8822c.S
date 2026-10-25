# SPDX-License-Identifier: GPL-2.0-only
# C2H transport latency benchmark for the 8822C embedded CPU (MIPS16, little-endian).
# Linked at 0x8003003c, directly after the vendor 8822C IMEM entry stub (see cpu8822c.h).
#
# The program issues N C2H requests for the packet staged in RX SRAM 0x18701000 (24-byte RX
# descriptor + payload; request word 0x00011000) and records each request's start/end from the
# free-running microsecond counter REG 0x568. The host seeds N and reads the block back over DDMA.
#
# DMEM block at 0x80208000 (OCP 0x208000):
#   +0   magic 0x0C2B0001 (set by the program)
#   +4   issued    (requests whose completion bit was observed)
#   +8   done      (1 after the last request)
#   +12  error     (reserved; 0)
#   +16  current t0 (us at the last request's issue, for hang diagnosis)
#   +24  N          (host-seeded before start)
#   +0x40 durations[N] (u32, t1 - t0 in us)
#
# Measured ISA constraints on this cut: only LUI of the MIPS16e2 extended-LI group exists
# (ORI/XORI behave as LI, ANDI 0xffff as LUI). All 32-bit constants go through LI32.
# No MIPS32, no JALX, no PC-relative literal loads.
#
# gas accepts exactly the MIPS16 GPR subset $2-$7 + $16-$17 (v0,v1,a0-a3,s0,s1); every other
# register number is rejected. The register budget therefore keeps the busy-poll hot path
# register-only and reloads the request constant (LI32) once per request.
        .set    noreorder
        .set    mips16

        .macro  LI32 reg, val
        lui     \reg, (((\val) + 0x8000) >> 16) & 0xffff
        addiu   \reg, ((\val) & 0xffff) - (((\val) & 0x8000) << 1)
        .endm

        .text
        .globl  _start
_start:
        LI32    $16, 0x80208000         # s0 = DMEM state block
        LI32    $17, 0xb8640568         # s1 = REG 0x568 (free-running us counter)
        LI32    $4,  0x80208040         # a0 = durations[]
        LI32    $6,  0xb8640294         # a2 = REG 0x294 (C2H request)
        LI32    $7,  0x00010000         # a3 = request busy bit
        lw      $5, 24($16)             # a1 = N (host-seeded)

        LI32    $2, 0x0c2b0001
        sw      $2, 0($16)              # magic
        li      $2, 0
        sw      $2, 4($16)              # issued = 0
        sw      $2, 8($16)              # done = 0
        sw      $2, 12($16)             # error = 0

1:      lw      $3, 0($17)              # v1 = t0
        sw      $3, 16($16)             # current t0
        LI32    $2, 0x00011000
        sw      $2, 0($6)               # issue C2H request
2:      lw      $2, 0($6)
        and     $2, $7                  # busy?
        bnez    $2, 2b                  # delay slot below reloads t1 either way
        lw      $2, 0($17)              # t1
        subu    $2, $3                  # t1 - t0
        sw      $2, 0($4)               # durations[i]
        addiu   $4, 4
        lw      $2, 4($16)
        addiu   $2, 1
        sw      $2, 4($16)              # issued++
        addiu   $5, -1
        bnez    $5, 1b
        li      $2, 1
        sw      $2, 8($16)              # done = 1
3:      b       3b
        nop
