# ES5505 / ES5506 / ES5510 in VGM: proposal v3

yoyofr, 2026-10-25, renumbered 2026-10-31 (v3) to make room for the Namco WSG family of libvgm PR #151;
see `VGM-proposal-v3-allocation.md` for the common chip / header / opcode / block allocation and the
conversion of existing files. Design only, not implemented. Replaces the encoding of the first proposal;
the emulation side (cores ported from MAME, mixer model, cut/loop handling, validation) is unchanged.

The first proposal reused the VGMPlay-era conventions for the ES5506 (per-voice bank through `0xBE 0x40+voice`,
region number and 8-bit flag hidden in the start offset of block `0x90`) and drove the ES5510 through
sub-commands of ES5506 opcodes. This version follows the way the other chips are declared: one header
entry per chip, one fixed-length opcode per function, one data block type per memory, board properties
as their own commands.

## 1. Sampler register writes (ES5505 / ES5506)

Existing commands, kept, with the semantics written down:

| Command | Meaning |
|---|---|
| `0xBE aa dd` | 8-bit write. ES5506: register aa >> 2, byte lane aa & 3, big endian, lane 3 commits the 32-bit latch. ES5505: register aa >> 1, lane aa & 1. aa bit 7 = second chip. |
| `0xD6 aa ddee` | 16-bit write, big endian: dd to lane aa, ee to lane aa + 1. aa bit 7 = second chip. |

Dropped for writers: the VGMPlay convention `0xBE 0x40+voice dd` (external bank), replaced by `0x49`. The
register lanes stop at 0x3F, so players keep decoding it for files made by older loggers.

Header fields of 1.71 unchanged: clock at 0xD0 (bit 31 = ES5506, clear = ES5505; bit 30 = dual), channel
count at 0xD5.

## 2. Board-level memory mapping

The chip addresses 2 (ES5505) or 4 (ES5506) regions of 1M words through the BS bits of the control
register. Boards extend that: the Taito F3 holds a per-voice latch that supplies the upper address bits
and maps both BS values to the same ROM; the Seta SSV (srmp7) moves a 1M-word window over a larger ROM.
Two dedicated opcodes (2 operands, the way the MultiPCM bank command `0xC3` works):

| Command | Meaning |
|---|---|
| `0x49 vv bb` | external bank of voice vv (bits 0-4; bit 7 = second chip): the board adds bb x 1M words to the addresses of that voice. Default 0. |
| `0x4A rr mm` | region map: region rr (bits 0-1; bit 7 = second chip) reads the data of region (mm & 3), starting at word offset (mm >> 2) x 1M. Default: each region reads itself at offset 0. |

`0x4A` covers both the srmp7 window (region 2 at base n) and the F3 alias (region 1 reads region 0).

## 3. Sample memory data blocks

One block type per region, 16-bit little-endian words, byte offsets, partial blocks allowed (a region may
be split into blocks covering only the referenced samples):

| Block | Memory |
|---|---|
| `0x90` | region 0 (unchanged) |
| `0x99` | region 1 |
| `0x9A` | region 2 |
| `0x9B` | region 3 |

The 8-bit flag is dropped: a board with 8-bit ROMs is logged as the 16-bit words the chip reads. The
constant low byte costs nothing once the file is gzipped.

## 4. IRQ acknowledge

Dropped. The IRQV read only matters to the sound CPU; the player acknowledges voice IRQs itself.
Verified output-neutral in the first implementation.

## 5. ES5510 ESP as a chip

New header entry: ES5510 clock (bit 30 = dual) at 0x114, after the Namco WSG fields (0x100-0x10C) and the
ZSG-2 clock (0x110), plus one byte at 0x11C: log2 of the DRAM size in words, 0 = default (1M words, MAME's
value). Chip ID 0x35. Players
only read the field when the header is long enough, as for every earlier addition.

| Command | Meaning |
|---|---|
| `0xCC cc aa dd` | host write: register aa (0x00-0xFF, MAME `host_w`: GPR latch, instruction latch, DOL/DADR, control, read/write selects), data dd. cc bit 7 = second chip, other bits 0. |
| `0x4B cc hh` | ESPHALT line, hh = 1 halted. |

Program, GPRs and DRAM are re-created by host writes, as in a plain log. No state block.

## 6. Board mixer and routing: two generic commands

Every multi-output sampler sits in a board mixer, and an effects DSP has serial inputs wired to some of
the sampler outputs. Rather than ES5506-specific sub-commands, two commands any chip can use:

| Command | Meaning |
|---|---|
| `0xE3 tt cc gggg` | output gain: channel cc of chip tt, gggg big-endian 8.8 (0x0100 = unity, the default for every channel). Even channels go left, odd channels right. The gain applies to the channel wherever it goes (direct mix and DSP input). cc 0xFE / 0xFF: gain of the chip's direct mix bus, left / right (board stage after the channel gains). |
| `0xE2 dd ii ss cc` | serial routing: input ii of DSP chip dd takes output channel cc of chip ss (cc 0xFF = nothing). A channel wired to a DSP input leaves the direct mix. |

tt / dd / ss = VGM chip ID (the numbering of the DAC stream control and the extra header), bit 7 = second
instance.

Taito F3 example: pair 0 direct, pairs 1-3 into the ESP (`0xE2 0x35 ii 0x25 ii+2` for ii = 0-5),
channel gains `0xE3 0x25 cc gg` (cc = 0-7), direct bus `0xE3 0x25 FE gg` / `FF gg`, ESP outputs 6/7 to the
mix (`0xE3 0x35 06 gg`, `0xE3 0x35 07 gg`). The logger writes the product of the board stages (route gain x
MB87078 attenuator), so gains follow the game's volume changes at run time.

This replaces, from the first proposal: the ESP routing preset, the direct-mix flag, the per-input and
per-output routing sub-commands and the ES5506-only gain command.

## 7. Mapping from the first proposal

| v1 | v3 |
|---|---|
| `0xBE 0x40+v bb`, `0xD7 00 v bb` | `0x49 v bb` |
| `0xD7 01 r base` | `0x4A r ((base << 2) \| r)` |
| `0xD7 02 r src` | `0x4A r src` |
| `0xD7 03`, `0xD7 04` | dropped |
| `0xD8 00 aa dd` | `0xCC cc aa dd` |
| `0xD8 01 0 h` | `0x4B cc h` |
| `0xD8 02 i c` | `0xE2 0x35 i 0x25 c` |
| `0xD8 03 o side` | `0xE3 0x35 o gain` |
| `0xD8 04 c flag` | `0xE3 0x25 c gain` |
| `0xD8 05 preset` | expanded into `0xE2` / `0xE3` |
| `0xD8 06 0 n` | header byte 0x11C = n |
| `0xE3 pp cc gggg`, cc 0-11 | `0xE3 0x25 cc gggg` |
| `0xE3 pp 0x10/0x11 gggg` (ESP out) | `0xE3 0x35 06/07 gggg` |
| `0xE3 pp 0x12/0x13 gggg` (direct bus) | `0xE3 0x25 FE/FF gggg` |
| block `0x90` with region bits / 8-bit flag | blocks `0x90` / `0x99` / `0x9A` / `0x9B`, 16-bit data |

All of it is table-driven on our side (logger emission, libvgm command and block tables, vgm_trim
parser). Existing files are converted offline and checked by byte comparison of the rendered audio.

## 8. Open points

- Opcode and block numbers: fixed in `VGM-proposal-v3-allocation.md` around PR #151; any other free
  fixed-length values work as well.
- `0xE2` / `0xE3` as generic board commands, or per-chip opcodes instead.
- Whether MAME driver route gains belong in the file; today they make the VGM match MAME's output level.
