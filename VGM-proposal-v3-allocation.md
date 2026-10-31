# VGM proposal v3: common allocation for Namco WSG, ES55xx and ZSG-2 / TMS57002

yoyofr, 2026-10-31. Design only. Supersedes the numbering of the v2 proposals
(`VGM-ES55xx-proposal-v2.md`, `VGM-ZSG2-proposal-v2.md`); their command semantics are unchanged.

## Why a v3

libvgm PR #151 (cam900, "Add Namco WSG family support", open since 2026-04) allocates chip IDs 0x30-0x33,
header clocks 0x100-0x10C, commands 0x45-0x48 and 0xCA/0xCB. The v2 proposals used the same values
(ZSG-2 = 0x30 with its clock at 0x100, ES5510 = 0x31, TMS57002 = 0x32, opcodes 0x45-0x4A, 0xCA, 0xCB).
PR #151 was published first and cam900's MAME branch (`mao_vgm`) already writes files with it, so v3
keeps the WSG allocation exactly as in the PR and moves the ES55xx / Zoom additions after it.

Detailed semantics: `VGM-ES55xx-proposal-v3.md`, `VGM-ZSG2-proposal-v3.md`. Namco WSG: PR #151.

## 1. Chip IDs (VGM chip index = libvgm DEVID)

| ID | Chip | Source |
|---|---|---|
| 0x30 | Namco WSG (3-voice discrete WSG, Pac-Man / Rally-X / Galaga...) | PR #151 |
| 0x31 | Namco C15 (15XX, 8 voices mono) | PR #151 |
| 0x32 | Namco C30 (CUS30, 8 voices, noise, RAM; clock bit 31 = stereo) | PR #151 |
| 0x33 | Namco Pole Position WSG (8 voices, quadraphonic) | PR #151 |
| 0x34 | ZOOM ZSG-2 | this proposal (v2: 0x30) |
| 0x35 | Ensoniq ES5510 ESP | this proposal (v2: 0x31) |
| 0x36 | TI TMS57002 DSP | this proposal (v2: 0x32) |

These IDs are also the chip type of the DAC stream control (`0x90`) and the `tt / dd / ss` operands of
the generic board commands `0xE2` / `0xE3`.

## 2. Header

| Offset | Field |
|---|---|
| 0x100 | Namco WSG clock (PR #151) |
| 0x104 | Namco C15 clock (PR #151) |
| 0x108 | Namco C30 clock, bit 31 = stereo (PR #151) |
| 0x10C | Namco Pole Position WSG clock (PR #151) |
| 0x110 | ZSG-2 clock, bit 30 = dual |
| 0x114 | ES5510 clock, bit 30 = dual |
| 0x118 | TMS57002 clock, bit 30 = dual |
| 0x11C | ES5510 DRAM size: log2 of the size in words, 0 = default (1M words, MAME's value) |
| 0x11D-0x11F | reserved, 0 |

Header length 0x120 for a file using the ES55xx / Zoom fields (the data offset at 0x34 says how long the
header is; a player reads a field only when the header covers it, as for every earlier addition).
PR #151 sets `_HDR_BUF_SIZE` to 0x200 as a placeholder; 0x120 is enough for everything listed here.

## 3. Commands

Free in libvgm master today: 0x45-0x4E (2 operands), 0xCA-0xCF and 0xD7-0xDF (3 operands), 0xE2-0xFF
(4 operands).

| Command | Meaning | v2 |
|---|---|---|
| `0x45 aa dd` | Namco WSG register write (PR #151) | (ES5506 bank) |
| `0x46 aa dd` | Namco C15 register write (PR #151) | (ES5506 region map) |
| `0x47 aa dd` | Namco C30 register write (PR #151) | (ESPHALT) |
| `0x48 aa dd` | Namco Pole Position WSG register write (PR #151) | (ZSG-2 control) |
| `0x49 vv bb` | ES5506 external bank of a voice | `0x45` |
| `0x4A rr mm` | ES5506 region map | `0x46` |
| `0x4B cc hh` | ES5510 ESPHALT line | `0x47` |
| `0x4C cc rr` | ZSG-2 control (reset) | `0x48` |
| `0x4D cc dd` | TMS57002 host data write | `0x49` |
| `0x4E cc ll` | TMS57002 control lines (PLOAD / CLOAD / reset) | `0x4A` |
| `0xCA aaaa dd` | Namco WSG memory write (PR #151) | (ES5510 host write) |
| `0xCB aaaa dd` | Namco C30 memory write (PR #151) | (TMS57002 pre-run) |
| `0xCC cc aa dd` | ES5510 host write | `0xCA` |
| `0xCD cc nnnn` | TMS57002 pre-run (nnnn silent sample periods) | `0xCB` |
| `0xE2 dd ii ss cc` | generic serial routing | unchanged |
| `0xE3 tt cc gggg` | generic output gain (8.8); cc 0xFE / 0xFF = the chip's direct mix bus, left / right | unchanged + bus channels |
| `0xE4 oooo dddd` | ZSG-2 register write | unchanged |

The 2-operand range 0x41-0x4E is full after this. If that is a problem, `0x4B`, `0x4C` and `0x4E`
(control lines, rare) can move to 3-operand opcodes in 0xD7-0xD9 with a zero padding byte.

## 4. Data blocks

| Type | Content | Source |
|---|---|---|
| `0x09` | Namco C30 stream data | PR #151 |
| `0xC4`-`0xC7` | WSG / C15 / C30 / PPW RAM writes (16-bit addressing) | PR #151 |
| `0x90` | ES5506 region 0 (unchanged, 16-bit words) | 1.71 |
| `0x97` | ZSG-2 sample ROM / flash | v2 |
| `0x99`-`0x9B` | ES5506 regions 1-3 | v2 |
| `0xE2` | TMS57002 memory image (32-bit addressing RAM write) | v2 (block 0x98, number open) |

Block `0x98` is left free.

## 5. Generic board commands and the WSG

`0xE2` / `0xE3` are chip-independent. The WSG family does not need them (the PR's cores mix their outputs
themselves), but a C30 in stereo or the quadraphonic Pole Position chip can use `0xE3 0x32 cc gggg` /
`0xE3 0x33 cc gggg` for board-level balance, with no new opcode.

## 6. Conversion of existing files (`vgm_v3conv`, `v3pack.py`, VGM Studio)

The released ES55xx and ZSG-2 packs use the **v1** encoding (what our MAME logger and libvgm `es55xx`
branch implement today): ES5506 `0xBE 0x40+v`, `0xD7`, `0xD8`, `0xE3 pp`; ZSG-2 `0xE4 ss aa dddd`, clock
at 0x100, header 0x110, state block `0x98`. Nothing was written in v2. The tool goes straight from v1 to
v3.

### Detection (v1)

- ES55xx: ES5506 clock at 0xD0 non-zero and any `0xD7` / `0xD8` / `0xBE 0x40+v` command, or block `0x90`
  with region / 8-bit bits in the start offset.
- ZSG-2: header length 0x110 with a non-zero 32-bit value at 0x100 and `0xE4` commands. (A v3 or PR #151
  file has a header of at least 0x120, so 0x110 is unambiguous.) Captures made before 2026-10-23 say
  version 1.73; they are written back as 1.72.
- Anything else (already v3, other chips only): copied unchanged, reported as such.

### Rewrite

- Header: grown to 0x120; ZSG-2 clock moved 0x100 -> 0x110 and 0x100 cleared; ES5510 clock set at 0x114
  when the file contains ESP commands (v1 has no ESP header field: 10 MHz, the Taito F3 / Gun Buster
  value from MAME's `taito_en.cpp`); TMS57002 clock 12500000 at 0x118 when the file has a DSP; DRAM flags
  from `0xD8 06`; loop offset, GD3 offset, EOF offset recomputed; version stays 1.72.
- Commands: per the mapping tables of section 7 of the ES55xx v3 doc and section 6 of the ZSG-2 v3 doc
  (one table-driven pass; ESP presets `0xD8 05` expanded into `0xE2` / `0xE3`).
- Blocks: `0x90` with region bits split into `0x90` / `0x99`-`0x9B`; 8-bit ROM blocks widened to 16-bit
  words; `0x98` DSP state turned into block `0xE2` (or into pre-runs, depending on the open point of the
  ZSG-2 doc).
- Loop point: the byte offset of the loop target is tracked through the rewrite (a loop never lands in
  the middle of a converted command); wait commands and sample counts are untouched.
- GD3 tag copied byte for byte.

### Packs

- One file: `vgm_v3conv in.vgz out.vgz` (exit 0 = converted, 2 = nothing to convert, copied; 1 = error,
  with the reason: an unrepresentable v1 command is refused, never approximated).
- A pack: `python3 v3pack.py "Game.zip" ["Game (v3).zip"]`, or the **Conversion v3** tab of VGM Studio
  (drop the zip, convert, save). Every `.vgm` / `.vgz` in the zip is converted (gzip kept);
  `.m3u`, `.txt`, `.png`, `.opus` and file names, order and dates are copied unchanged; the zip is
  rebuilt stored like the VGM Studio pack generator.

### Verification (mandatory, per file)

The old file is rendered with the v1 player (libvgm `es55xx` branch build, kept as `vgm2wav-v1`), the new
one with the v3 player (`vgm2wav`), both at 44100 Hz, 2 loops. The PCM must be identical byte for byte;
any difference fails the file and the pack is not written. This is the same rule as every other output
of the tool (100 % faithful, no lossy step).

### Implementation order

1. libvgm, branch `vgm-v3` (= `es55xx` + PR #151 merged): command and block tables, header fields at
   0x110-0x11C, ZSG-2 = DEVID 0x34. The v3 commands drive the same core entry points as v1, which is
   what makes the byte comparison meaningful. `vgm2wav-v1` = a build of the `es55xx` branch, kept in
   `vgm-tools/bin/`. Done 2026-10-31.
2. `vgm_v3conv` + `v3pack.py` + VGM Studio tab. Done 2026-10-31. vgm_trim reads both encodings: a v3
   input is converted to v1 in memory (`vgm_v3.h`, the exact inverse mapping; `vgm_v3conv --to-v1` does
   the same on a file), processed by the unchanged v1 logic, and written back as v3. Done 2026-10-31
   (v3 -> v1 -> v3 gives back every converted pack file byte for byte; trims of the v1 and v3 versions
   of a file render identically). VGM Studio renders v1 files with `vgm2wav-v1`, v3 files with `vgm2wav`.
3. MAME logger: branch `vgmlog0289-es5506` (= `vgmlog0288-es5506` + MAME 0.289 merged) writes v3. The
   devices still log the logical commands; `vgmwrite.cpp` translates them through a copy of `vgm_v3.h`
   (`src/lib/util/vgmv3.h`), so a capture is exactly what `vgm_v3conv` would make of the v1 capture.
   Header 0x120 for every ES5505/6 or ZSG-2 file. cam900's WSG logging (`mao_vgm` 52ec2dc7c62) is ported
   to the 0.289 `namco.cpp` on the same branch (header 0x120 too), so WSG games are captured from the
   same build. Done 2026-10-31.
4. Convert and verify every released pack (Darius Gaiden, Gunlock, Elevator Action Returns, Ray Storm,
   G-Darius, Ray Crisis...), then republish.
