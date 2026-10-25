# SPDX-License-Identifier: GPL-2.0-only
# Marker + ISA probe for the 8822C embedded CPU (MIPS16, little-endian). Linked at 0x8003003c, directly
# after the vendor IMEM entry stub (sp = 0x80210000 + CP0 setup), replacing its warm/cold-boot branch.
# DMEM block at 0x80208000 (OCP 0x208000), above the 0x4708-byte vendor DMEM image and below the stack:
#   +0  magic 0xCAFE0C22 (LUI + ADDIU)          +4  loop counter
#   +8  last REG 0x568 (free-running us timer)   +12 first REG 0x568
#   +16 LUI 0x1234 ; ORI  0x5678   (0x12345678 if MIPS16e2 ORI works)
#   +20 LUI 0x1234 ; XORI 0x00ff   (0x123400ff if MIPS16e2 XORI works)
#   +24 LUI 0x1234 ; ANDI 0xffff   (records what ANDI does)
#   +28 SLL/ADDU/SLTU/BTNEZ probe: 0x5 if they behave as on the 8822B
#   +32 REG 0x0080 via the 0xb8600000 alias the stub uses (MCUFW_CTRL)
# Rules carried over from the 8822B as hypotheses: no MIPS32, no JALX, 32-bit constants via LUI + ADDIU.
        .set    noreorder
        .set    mips16

        .macro  LI32 reg, val
        lui     \reg, (((\val) + 0x8000) >> 16) & 0xffff
        addiu   \reg, ((\val) & 0xffff) - (((\val) & 0x8000) << 1)
        .endm

        .text
        .globl  _start
_start:
        LI32    $16, 0x80208000         # s0 = DMEM scratch block
        lui     $17, 0xb864             # s1 = 0xB8640000 (MAC registers, uncached)
        LI32    $3, 0xcafe0c22
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
        # probe: v0 = (3 << 1) + 1 = 7; sltu 7 < 9 -> T = 1; result 5 if btnez is taken
        li      $2, 3
        sll     $2, $2, 1
        addiu   $2, 1
        li      $3, 9
        sltu    $2, $3
        btnez   1f
        li      $4, 0xbad
        b       2f
        nop
1:      li      $4, 5
2:      sw      $4, 28($16)
        lui     $3, 0xb860
        lw      $3, 0x80($3)
        sw      $3, 32($16)
        li      $2, 0
        sw      $2, 4($16)
        lw      $4, 0x568($17)
        sw      $4, 12($16)
3:      lw      $2, 4($16)
        addiu   $2, 1
        sw      $2, 4($16)
        lw      $4, 0x568($17)
        sw      $4, 8($16)
        b       3b
        nop
