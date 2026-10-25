# Wider LA taps on the 8822B (8812BU) — 2026-10-25

Question: is there an LA setting that gives more than the ~20 MHz band we saw at every channel width, and is
there a genuine 80 MS/s tap?

**Yes, both.** Two independent ways, both measured on our 8812BU (USB3, fixed gain `--igi 0x30 --nocca`):

1. **dma 0 + the vendor RX DFIR for the width.** The ~20 MHz limit was the RX DFIR, not the LA tap. devourer
   writes the BW20 DFIR set at every width; the vendor's `phydm_rxdfirpar_by_bw_8822b` uses other values at
   40 and 80 MHz. With the vendor values:
   - BW80, rate_sel 0: **genuine 80 MS/s** (adjacent duplicates 0.0001, was 0.50), floor −6 dB at ±37 MHz,
     −10 dB at ±39 MHz, 12-bit, same word layout as today.
   - BW40, rate_sel 1: 40 MS/s with the full ±20 MHz (was ±10.9 at −6 dB).
2. **dma_type 11 = raw ADC, 160 MS/s, both RX paths, int8.** It does not go through the DFIR. Its band is the
   analog filter: −6 dB at ±40 MHz (BW80), ±21 (BW40), ±11 (BW20), then −15 dB at ±50 and about −24 dB from
   ±60 MHz. It is snapshot-only: 16384 words is 204.8 µs.

Evidence: `evidence/phase3-widetap-dfir.txt`, `-ltf.txt`, `-dma.txt`, `-floor.png`.

## Important correction: `--channel` is the primary channel

devourer's `SelectedChannel.Channel` is the **primary 20 MHz channel**. `Session` passes `ChannelOffset = 1`,
so the centre is **ch+2 at BW40 and ch+6 at BW80** (`HalJaguar2::central_ch`). `--channel 42 --bw 80` is centred
on ch48 (5240 MHz), not 5210. `--channel 38 --bw 40` is centred on ch40. That is why the earlier off-centre
checks put the 5180 AP in the wrong place. Use `--channel 36 --bw 80` for a 5210 MHz centre and
`--channel 36 --bw 40` for 5190. A BW40/80 caller (Soapy, `wsdr_tcp`) must convert centre to primary.

## What limits the band (measured)

- **0xc20[31] / 0xe20[31] = 1** is a BW20 stage: with it set, the tap is always ±10 MHz and doubled at
  rate_sel 0, whatever 0x948 and 0x94c hold. That is all 16 `c1` rows in the matrix.
- With 0xc20[31] = 0, the pair (0x948[29:28], 0x94c[29:28]) picks the filter:
  - (1|2|3, 0): ±20.5 MHz brick-wall, genuine 80 MS/s, about −40 dB outside.
  - (2, 1) is the vendor BW80 set; (3, 1), (3, 2) and (3, 3) are similar: ±37–39 MHz, genuine.
  - 0x948 = 0 gives a doubled output, and so does (1|2, 2).
  - Full table: `phase3-widetap-dfir.txt` §B.
- In every DFIR mode wide enough to matter, the floor has about 5 dB of ripple. The raw ADC (dma 11) is flat
  to ±1 dB, so the ripple comes from the DFIR.
- At BW20, no DFIR setting widens the tap. dma 11 shows the analog filter is ±11 MHz there.
- **The 0x8fc selector does not pick the sampled data.** 512 selectors (0x800–0x8ff, 0xa00–0xaff) all gave
  the same 24-bit IQ bus with dma 0. Only the upper word bits change. The 0x0 / 0x880 equivalence is the
  same fact.
- dma 10 and 11 did not change with the selector either. We tried 0x000, 0x880 and the selectors that are
  live at the register level.
- A register-level survey (0x8fc = 0x000–0xfff, 8 reads of 0xfa0 each) found 35 live ports, listed in
  `-dma.txt`. Nothing we found routes them into the LA ring.

## Verification

- **Live:** every candidate showed ≤ 0.0005 identical words between two snapshots.
- **Genuine rate:** at rate_sel 0, adjacent duplicates are 0.0001 with the BW80 DFIR, against 0.50 with the
  devourer DFIR.
- **Word rate:** `latime --dma 11` measured 80.0 Mword/s at rate_sel 0 and 40.0 Mword/s at rate_sel 1.
- **Real signals in the right place:** L-LTF pairs were found only at 20 MHz channel centres, never at the
  control offsets, and never in conj(x) (`-ltf.txt`).
  - BW80 at 5210 MHz, dma 0 with the BW80 DFIR, 399 snapshots: 17 / 14 / 18 / 11 hits at −30 / −10 / +10 / +30
    MHz, 0 at −20 / 0 / +20. With the devourer DFIR: 1 / 0 / 1 / 0.
  - dma 11 found the same channels on path A (13 / 10 / 9 / 3) and on path B (12 / 7 / 6 / 8), and nothing at
    ±40 or ±50.
  - BW40 at 5190 MHz with the BW40 DFIR: 36 hits at −10 and 20 at +10. With the devourer DFIR: 4 and 0.
  - CFO is +80…+100 kHz throughout, which is our crystal offset as in Phase 1.

## dma_type survey (BW80, BW80 DFIR, rate_sel 0, dbg 0x880)

| dma | live bits | decode | status |
|---|---|---|---|
| 0 | 24 | path A I[11:0] Q[23:12], post-DFIR | measured |
| 1 | 24 | 12+12 IQ, post-DFIR (devourer: path B with 0xa80) | path not checked |
| 2, 3, 12–15 | none | static | measured |
| 4, 5 | 48 | inferred: A 12+12 in [23:0], B 12+12 in [47:24], both ±39 MHz | inferred |
| 6, 7 | 40 | inferred: A and B as 10+10 | inferred |
| 8, 9 | 20 | inferred: one path, 10+10 | inferred |
| 10 | 32 | inferred: A and B int8 I,Q at 80 MS/s, pre-DFIR, flat to Nyquist | inferred |
| 11 | 64 | raw ADC: per path 4 bytes (I1,Q1,I0,Q0); time order bytes [2,3] then [0,1]; path A = bytes 0–3, B = 4–7; 160 MS/s | measured |

These match ac54's unverified guesses for the widths. With dma 11, rate_sel 1 or 2 only skips words; the two
samples inside a word stay 160 MS/s apart. Use rate_sel 0.

## How to reproduce

```sh
tools/coord.sh lock 8812bu claude "..."
# genuine 80 MS/s, ~76 MHz band (snapshot / ring):
sudo -n env DEVOURER_LOG_LEVEL=silent build-bw/wsdr_probe ring --pid 0xb812 --no-thermal \
     --channel 36 --bw 80 --rate 0 --dfir 80 --igi 0x30 --nocca     # adj_dup 0.0001 (0.46 with --dfir 0)
# 40 MS/s, full 40 MHz:            ... --channel 36 --bw 40 --rate 1 --dfir 40
# raw ADC 160 MS/s x 2 paths:      ... snap/tapscan --bw 80 --dma 11 --rate 0   (decode: tools/tapscan_psd.py --layout a8x2|b8x2)
# many configs in one session:     wsdr_probe tapscan --plan FILE --dump PREFIX
#   plan line: label [dbg=N] [dma=N] [rate=N] [count=N] [w=ADDR/MASK/VAL ...]
#   vendor BW80 DFIR as w=: w=0x948/0x30000000/0x20000000 w=0x94c/0x30000000/0x10000000 w=0xc20/0x80000000/0 w=0xe20/0x80000000/0
python3 tools/tapscan_psd.py --json run.jsonl PREFIX-*.u64 --plot floor.png
python3 tools/tapscan_ltf.py PREFIX-label.u64 --fs 80e6 --offsets=-30,-20,-10,0,10,20,30 [--layout a8x2]
tools/coord.sh unlock 8812bu claude
```

## Code added (uncommitted)

- `src/la88xx.{h,cpp}`: `LaRingCfg::dfir` (20/40/80, vendor values). 0x948, 0x94c, 0xc20 and 0xe20 are now in
  the snapshot/restore set. The default is 0, which leaves the DFIR alone, so existing users see no behaviour
  change.
- `src/wsdr_probe.cpp`: new flags `--dma` and `--dfir`, and two new commands:
  - `dbgscan`: 0x8fc / 0xfa0 survey, which restores 0x8fc, 0x8f8 and 0x198c.
  - `tapscan`: plan file. Each entry gets its own LA config plus masked writes. The writes are restored after
    every entry, and the La8822b restore runs at the end.
- `tools/tapscan_psd.py`: floor width, duplicates and burst PSD. Layouts iq12, a8x2/b8x2 (dma 11), a8/b8 (dma 10).
- `tools/tapscan_ltf.py`: L-LTF search at frequency offsets with controls.

## Uncertain / not done

- The decodes of dma 1 and 4–10 are inferred from bit statistics and floor shape. A two-path L-LTF check on
  dma 4 would confirm a coherent 2-path 12-bit 80 MS/s tap at the same 8 B per word. That would be the
  cheapest way to get path B into the streamer.
- What 0x948 and 0x94c encode internally (stage order, coefficient sets) is not known. Only the measured
  input → output table exists.
- The effect on normal 40/80 MHz frame reception of devourer's BW20-only DFIR is not tested. The vendor and the
  devourer comment both say the DFIR must match the width, so this is probably a devourer bug at BW40/80
  (inference).
- `LCK TIMEOUT (LO not locked!)` was logged at some bring-ups: --channel 42/bw80, 38/bw40 and 36/bw20 once. It
  was not logged at 36/bw80 or 36/bw40. The data was live and the L-LTFs landed correctly in every capture
  that was checked. Not investigated.
- A strong narrow spur sits at about 5198 MHz: −12 MHz at the 5210 centre and +8 MHz at 5190. Source not
  investigated.
- 8822C (8812CU) is untested. Its JGR3 LA block has a different register map.

## Next steps

1. Streaming at 40 MS/s with a 40 MHz band: `--bw 40 --dfir 40 --rate 1` drops straight into the existing USB3
   40 MS/s streamer. Wire `dfir` through `StreamCfg` and Soapy, setting the bandwidth to 40 MHz at BW40.
2. Soapy and `wsdr_tcp`: when the requested width is 40 or 80 MHz, map the centre frequency to the primary
   channel (centre − 10 or centre − 30 MHz).
3. 80 MS/s (640 MB/s) and dma 11 (160 MS/s × 2 paths) exceed the C2H ceiling of about 391 MB/s, so they are
   snapshot-only. Offer them as a "wideband snapshot" mode (204.8 µs at 80 MS/s or 160 MS/s).
4. Report the BW40/80 DFIR gap to devourer, so `set_channel_bw` calls the vendor `rxdfirpar_by_bw`.
