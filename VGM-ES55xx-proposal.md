# VGM extension for ES5505 / ES5506 / ES5510

Author: yoyofr, 2026-10-21. Live document: https://claude.ai/artifact/9kq7iMTkEqvss3wMNVdnNU

## Summary

A small, backward-compatible extension of the VGM format so that Ensoniq ES5505 / ES5506 boards
can be logged and replayed faithfully, including the ES5510 effects DSP (ESP) behind the sampler
on Taito F3 and Ensoniq synthesizers. No new header field; files using it are written as VGM 1.72
(the draft version that already covers the 0x100-byte header) so that players can tell them apart from 1.71 logs. Five opcodes: 0xBE and 0xD6 (already
assigned to the ES5506 in VGM 1.71, meaning pinned down) plus three fixed-length reserved opcodes
0xD7, 0xD8 and 0xE3. Implemented on both sides (MAME 0.288 logger on top of vgmlog0288, libvgm
with C ports of MAME's current ES5506 and ES5510 cores). Replay of Darius Gaiden, Macross Plus,
Vasara and Super Real Mahjong P7 matches MAME's WAV output: waveform correlation 0.996-1.000,
levels within 1%.

## What VGM 1.71 cannot express

| Missing | Consequence | Boards |
|---|---|---|
| ES5510 state (program, registers, DRAM, halt) | no effects, Taito F3 silent (its direct output is silence) | Taito F3/JC, Ensoniq VFX/SD/ASR/KT |
| Board mixer gains (MB87078, driver routes) | wrong dry/wet balance and level | all |
| More than one stereo pair | pair routing lost | Taito (4), Ensoniq (6) |
| Per-voice external bank (ES5505) | VGMPlay convention 0xBE 0x40+voice, undocumented | Taito F3 |
| Run-time bank window on regions 2/3 | wrong samples after a switch | Seta SSV srmp7 |
| Region alias (two regions, one ROM) | ROM dumped twice | Taito F3 |
| IRQ acknowledge (host read of IRQV) | voices stuck in IRQ state | all |
| Byte order of 0xD6 data | ambiguous | all |
| libvgm ES5506 core | 38-line stub | all |

## Design principles

1. A VGM is a log of host bus writes plus fixed board properties (gains, routing, ROM layout).
2. No new header field: the ES5510 is a sub-device of the ES5506 instance, created by the first 0xD8.
   The version is set to 1.72 (header length 0x100) whenever an ES5505/ES5506 is logged.
3. Only fixed-length reserved opcodes (0xD7/0xD8: 3 operand bytes, 0xE3: 4). Old players skip them.
4. Bit-exact replay: libvgm cores are straight ports of MAME 0.288, quirks included. Any log-side
   reduction is proven output-neutral by byte comparison of rendered audio.
5. Board knowledge stays on the logging side (MAME drivers emit presets, gains, aliases).

## Header

| Offset | Size | Content |
|---|---|---|
| 0xD0 | 32 | ES5506 clock; bit 31 = 1 for ES5506, 0 for ES5505; bit 30 = dual chip |
| 0xD5 | 8 | stereo output pairs: 1-4 (ES5505), 1-6 (ES5506); 0 = 1 |

libvgm flags = (byte 0xD5 & 0x0F) | (clock bit 31 >> 24); the old player lost bit 31.

Version (0x08): 0x00000172. 1.71 files with the same opcodes are upgraded in place by the trim tool
(version word, header zero-padded to 0x100, data offset updated).

## Commands

First operand byte: bit 7 = chip select. `aa` = byte offset into the register window as on the host bus.

| Opcode | Operands | Meaning |
|---|---|---|
| 0xBE | aa dd | 8-bit write. ES5506: register aa/4, lane aa&3 (big endian, lane 3 commits the 32-bit latch). ES5505: register aa/2, lane aa&1 (other lane masked). aa 0x40-0x5F: legacy VGMPlay per-voice bank (voice aa&0x1F, bank dd in 1M-word units). |
| 0xD6 | aa ddee | 16-bit write, big endian: dd to lane aa\|0, ee to lane aa\|1. ES5506 register = two 0xD6 at 4N and 4N+2; ES5505 register = one 0xD6 at 2N. |
| 0xD7 | pp aa dd | ES5506 extension, sub-command pp & 0x7F |
| 0xD8 | pp aa dd | ES5510 command, sub-command pp & 0x7F |
| 0xE3 | pp cc gggg | mixer gain of channel cc, gggg big endian 8.8 (0x0100 = unity), pp & 0x7F = 0 |

0xD7 sub-commands:

| pp | aa | dd | Meaning |
|---|---|---|---|
| 00 | voice 0-31 | bank | external bank of the voice, 1M-word units (Taito F3 ES5505 bank latch) |
| 01 | region 0-3 | base | bank window: region reads from offset base x 1M words of its ROM (srmp7) |
| 02 | region | source | alias: region aa reads the data of region dd (Taito F3 bank1 = bank0) |
| 03 | 0 | 0 | host read of IRQV: acknowledges the pending voice IRQ |
| 04 | 0 | 0/1 | 1 = player acknowledges IRQs itself (default); 0 = only via 03 |

0xD8 sub-commands:

| pp | aa | dd | Meaning |
|---|---|---|---|
| 00 | host reg 0x00-0xFF | data | host write (`host_w` of MAME es5510.cpp): GPR latch 0x00-0x02, instruction latch 0x03-0x08, DOL/DADR, control 0x12/0x14/0x18/0x1F, read select 0x80, write selects 0xA0/0xC0/0xE0 |
| 01 | 0 | 0/1 | ESPHALT line, 1 = halted |
| 02 | serial in 0-5 | channel / 0xFF | sampler output channel dd into ESP serial input aa |
| 03 | serial out 6-7 | 0/1 | ESP serial output aa to left (0) / right (1) |
| 04 | channel 0-11 | 0/1 | sampler channel mixed directly (1) or only through the ESP (0) |
| 05 | preset | 0 | 0 = Taito board (pair 0 direct, pairs 1-3 into inputs 0-5, outputs 6/7 to L/R); 1 = Ensoniq synth (pairs 0-2 into the ESP, nothing direct); 0xFF = all direct |
| 06 | 0 | n | DRAM size 2^n words before first access; 0 = 1M (MAME default) |

0xE3 channel IDs: 0x00-0x0B sampler outputs (pair p: L = 2p, R = 2p+1), 0x10/0x11 ESP out L/R,
0x12/0x13 direct mix L/R. The logger emits the product of all gain stages: Taito F3 = route 0.18
x MB87078 for sampler channels, 0.5 x MB87078 for ESP and direct mix; plain boards = driver
add_route gain per output (Macross Plus 1.6, SSV 1.2); unrouted outputs = 0.

## Sample ROM data block 0x90

`rom_size, start_offset, data` as usual. start_offset bit 31 = 8-bit samples (stored in the high
byte of each word), bits 28-29 = region 0-3, bits 0-27 = byte offset. Words are 16-bit little
endian. A region may be split into several blocks covering only referenced samples.

## ES5510 model

- State fully reachable through the host interface: program (160 x 48 bits), 192 GPRs (24 bits),
  DRAM (1M x 16 in MAME, unverified, overridable with 0xD8 06), halt line.
- One program pass per sample: routed channels into serial inputs 0-5, run 160 instructions, read
  outputs 6/7. Halted: no output, state kept.
- Trimmed files start from the program and host-written register image; DRAM and program-modified
  GPRs cannot be reproduced, the effect tail converges in a few seconds (only non-exact spot, also
  at the loop point).
- Host reads are not logged (no audio effect). Cost ~7 M instructions/s at 44.1 kHz.

## Mixer model

Sampler (12 channels, 20-bit scaled to 16) -> per-channel gain 0xE3 cc -> direct mix (0xD8 04,
gain 0xE3 0x12/13) or ESP serial inputs (0xD8 02) -> ESP outputs 6/7 (0xD8 03, gain 0xE3
0x10/11) -> L/R. Boards without ESP: everything direct, only 0xE3 matters.

## Player implications (libvgm)

- emu/cores/es5506.c: C port of MAME 0.288 es5506.cpp (21-bit addresses, LP3/LP4 filters, u-law,
  interpolation, ramps, 4 regions, 6 pairs). emu/cores/es5510.c: port of cpu/es5510/es5510.cpp,
  owned by the ES5506 instance (no new device ID).
- rwFuncs: A8D8 (0xBE), A8D16 (0xD6, big endian), A16D8 user 0/1 (0xD7/0xD8), A16D16 user 2 (0xE3),
  BLOCK (region in offset bits), MEMSIZE per region, CHN_MUTE (32 voices). DEVRW_A8D32 (0x14) was
  added and is unused; can be dropped.
- _CHIP_VOLUME[0x25] = 0x100, _PB_VOL_AMNT[0x25] = 0x200.
- Taito drivers rewrite most registers every frame: log everything, de-duplicate afterwards with
  effective-bit masks, never dropping registers the chip modifies or with side effects (CR, ACC,
  filter history, ES5506 ramped volumes/filters, ECOUNT). Darius Gaiden: 233 MB raw, 14.3 MB cut,
  bit-exact.
- Loops: register image first (globals, then voices with CR last), ROM blocks, ESP program; page
  register re-selected explicitly at the loop point.
- vgm_cmp / vgm2txt / vgmlpfnd: only the opcode lengths (0xD7, 0xD8: 3; 0xE3: 4).

## Validation against MAME

| Game (driver) | Chips | Waveform correlation | RMS VGM/MAME |
|---|---|---|---|
| Darius Gaiden (taito_f3) | ES5505 + ES5510 + MB87078 | 0.999 | 0.989 |
| Macross Plus (macrossp) | ES5506 | 1.000 | 1.001 |
| Vasara (ssv) | ES5506 | 0.998 | 0.993 |
| Super Real Mahjong P7 (ssv) | ES5506 | 0.996 | 0.998 |

60 s attract-mode runs, MAME -wavwrite vs libvgm vgm2wav, aligned for MAME's constant 3-5 ms
stream latency. Not yet exercised: srmp7 bank window, iTech32, Ensoniq synthesizers (preset 1),
dual-chip files.

## Alternatives considered, open questions

- Atomic 32-bit write 0xE2 (6 bytes): rejected, breaks the fixed length of 0xE2-0xFF.
- Separate ES5510 chip with header clock: possible but meaningless without its sampler.
- Voice-volume macro opcode (page+LVOL+RVOL in 5 bytes): raw 14.35 -> 12.21 MB, .vgz unchanged. Not worth it.
- Time quantisation / envelope decimation: 88% of Taito volume writes are +-1 steps every 1-5 ms; rejected.
- Open: opcode numbers (any free fixed-length values work, table-driven).
- Open: driver route gains are a MAME mixing choice; logged today so the VGM matches MAME's output.
- Open: 8-bit flag and region bits of block 0x90 should be written into the spec.

## Reference implementation

- MAME: fork branch `vgmlog0288-es5506` (on vgmlog0288, 0.288): vgmwrite.cpp/.hpp, es5506.cpp/.h,
  es5510.cpp/.h, taito_en.cpp/.h, ssv.cpp. 9 files, ~240 lines.
- libvgm: fork branch `es55xx`, commit 99cfea3 on a6eed94: emu/cores/es5506.c/.h, es5510.c/.h,
  EmuStructs.h, player/vgmplayer.cpp/.hpp, vgmplayer_cmdhandler.cpp. ~3100 lines. Source: the libvgm fork,
  branch es55xx.
- Tool: vgm-tools/vgm_trim.cpp (cut, split on silence, loop finding, de-duplication, ROM pruning,
  ESP dead command elimination, .vgz in/out, 1.72 header upgrade; all bit-exact).
- Test captures and MAME WAV references available on request.
