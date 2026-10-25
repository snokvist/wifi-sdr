# FINDINGS

Register maps, protocol notes, firmware behaviour and quirks, written for someone who has not read the
journal. Each claim cites its evidence (file in `evidence/`, or a source file:line).

## Conventions
- "devourer" = the OpenIPC/devourer tree at 0bf1957 (local clone `~/dev/snokvist/devourer`).
- "ac54" = djObsidian/ac54_sdr at 2b41f98 (`ref/ac54_sdr`). Unlicensed — facts only, no code copied.

## Bench unit: RTL8812BU `0bda:b812`
- Identifies as 8822B cut 3 (UMC), 2T2R, SYS_CFG1 = 0x0c493d37, EFUSE rfe_type = 0x03, EEPROM ID 0x8129.
  Both RF paths exist on the die; the RTL8812BU is the 2T2R AC1200 part
  (`evidence/phase0/doctor-8812bu-ch1.stderr`).
- Enumerates USB 3 SuperSpeed (5000M), bulk wMaxPacketSize 1024, bulk IN EP 0x84.
- Vendor firmware boot through devourer's DLFW path: OK, `0x80 = 0xC078` (FW ready).
