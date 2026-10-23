# VGM extension for the ZOOM ZSG-2 + TMS57002 (Taito Zoom sound board)

Author: yoyofr, 2026-10-23. Companion of [VGM-ES55xx-proposal.md](VGM-ES55xx-proposal.md).

## Summary

The Taito Zoom sound board (Taito FX-1B: Ray Storm, G-Darius; Taito G-NET: Ray Crisis, ...) is a
MN10200 sound CPU driving a ZOOM ZSG-2 48-voice wavetable chip whose four outputs (reverb send, chorus
send, direct left, direct right) go through a Texas Instruments TMS57002 effects DSP programmed by the
sound CPU. Neither chip exists in VGM 1.71. This extension adds one header field, one fixed-length
reserved opcode (0xE4, 5 bytes) and one ROM data block type (0x97). Files using it stay VGM 1.72 (the
draft version of the ES55xx extension, nothing validated by Valley Bell yet) with the header grown to
0x110 bytes. Implemented on both sides (MAME 0.288 logger on top of vgmlog0288, libvgm with a C
port of MAME's ZSG-2 core and a C++ port of MAME's TMS57002 core). Replay of Ray Storm matches MAME's
own output of the sound board: level 1.00, waveform correlation 0.98-1.00 at the native 32552 Hz rate
(brief dips at note attacks come from the 44100 Hz quantisation of the write times).

The PSX SPU of the FX-1B / G-NET main board is not part of this extension: in Ray Storm it plays its own
material (uncorrelated with the Zoom output, comparable level during the attract demo), which a VGM
of the sound board does not contain.

## Design

1. The TMS57002 is a sub-device of the ZSG-2 instance, like the ES5510 of the ES5506 extension: its host
   writes, control lines and the board mixer gains are sub-commands of the ZSG-2 opcode. The DSP runs at
   half the ZSG-2 clock, one program pass per ZSG-2 sample (clock / 768 = 32552 Hz at 25 MHz).
2. A VGM is a log of host bus writes. The DSP program and coefficients are not stored as data: the sound
   CPU uploads them through the host interface and those writes are logged as they happen. A cut point
   re-creates the DSP state by replaying the host writes with the elapsed time replaced by *pre-runs*
   (sub-command 8), which is exact because the DSP is deterministic given its program and silent input.
3. Bit-exact replay: the libvgm cores are straight ports of MAME 0.288 (ZSG-2 ramps, emphasis filter,
   DSP instruction cache and host update FIFO quirks included), the board wiring (ZSG-2 -> DSP gains,
   DSP outputs 2/3 -> left/right) is the libvgm default and is logged as gains anyway.

## Header

| Offset | Size | Content |
|---|---|---|
| 0x100 | 32 | ZSG-2 clock (25000000 on the Taito Zoom board); bit 30 = dual chip |
| 0x104 | 12 | reserved |

Version (0x08): 0x00000172, data offset >= 0x110 (players read the field only when the header reaches it).

## Command 0xE4 ss aa dddd

`ss` bit 7 = chip select, bits 0-3 = sub-command, bits 4-6 reserved (0). `dddd` is big endian.

| ss & 0x0F | aa | dddd | Meaning |
|---|---|---|---|
| 0x0-0x3 | offset bits 0-7 | data | ZSG-2 16-bit register write; word offset = ((ss & 3) << 8) \| aa (0x000-0x2FF voice registers, 16 per voice, 48 voices; 0x300-0x31F control registers: 0x300-0x302 key on, 0x304-0x306 key off, 0x31C/0x31D ROM readback address) |
| 0x4 | 0 | 0x00dd | TMS57002 host data write (byte dd) |
| 0x5 | 0 | 0x00dd | TMS57002 control lines: dd bit 0 = PLOAD level, bit 1 = CLOAD level (active low). The lines are set CLOAD first, PLOAD second, as on the board |
| 0x6 | channel | gain | mixer gain, 8.8 fixed point (see below) |
| 0x7 | 0 | 0 | reset of the ZSG-2 and the DSP (the program and coefficients are kept) |
| 0x8 | ticks bits 16-23 | ticks bits 0-15 | DSP pre-run: the player runs the DSP program for that many sample periods with silent inputs and discards the output. Used at a cut point and at a restarted capture to re-create the program-internal state (coefficient updates land where the running program reads them, LFO phases) |

Gain channels (defaults are the Taito Zoom board wiring, nothing else is needed for Taito boards):

| Channel | Meaning | Default |
|---|---|---|
| 0x00-0x03 | ZSG-2 output n -> DSP input n | 0.5, 0.5, 1.0, 1.0 |
| 0x10-0x13 | DSP output n -> left | output 2: 1.0 |
| 0x14-0x17 | DSP output n -> right | output 3: 1.0 |
| 0x20-0x23 | ZSG-2 output n -> left, bypassing the DSP | 0 |
| 0x24-0x27 | ZSG-2 output n -> right, bypassing the DSP | 0 |

The board registers 4/5 (global volume, written by the main CPU) scale the DSP outputs 2/3: the logger
emits them as channels 0x12 / 0x17 with (value & 0x3F) / 63.

## ROM data block 0x97

`67 66 97 size romsize start data`: the ZSG-2 sample ROM (32-bit little-endian blocks of four 7-bit
compressed samples), plain byte offsets, one region.

## Player model (libvgm)

Per ZSG-2 sample: the 48 voices are mixed into the four outputs (clamped to 16 bits), scaled by the
input gains and latched as the DSP serial inputs (24-bit, x256 when the DSP is in 16-bit input mode);
the DSP outputs of the previous pass, scaled by the output gains, plus the bypass mix give the stereo
sample; then the DSP is synchronised and runs up to clock / 2 / sample rate = 384 instruction cycles.
The DSP external data memory is 256 KB (the board RAM), accesses beyond it are unmapped as in MAME.
Device ID 0x30, cores `emu/cores/zsg2.c` and `emu/cores/tms57002.cpp` (+ generated
`tms57002_ops.hxx`), playback volume weight 0x200 like the ES5506 (MAME output level).

## Logger (MAME)

`zsg2.cpp` opens the VGM device, dumps its ROM region and logs every 16-bit write; `taito_zm.cpp`
wraps the DSP host port to log the data writes, logs the control lines and the gains, and the reset.
A capture restarted at run time (`-vgmpause`, hotkey) re-dumps the ROM, the ZSG-2 register image,
re-keys the voices that are playing at their current block (key on with the start address set to the
next block to fetch, then the real start address, current volume and filter value put back), and
replays the DSP host write history kept since reset with the sample periods the DSP ran in between as
pre-runs (exact state: coefficient updates land where the running program reads them, LFO phases),
then the gains. What is not restored: the DSP delay lines (reverb tail of the music before the restart)
and the per-voice filter memories. Ray Storm restarted in the middle of the music: correlation 0.90
with the uncut render in the first second, 0.98-0.99 after 6 s, level 1.00.

## Trim tool (vgm_trim)

ZSG-2 logs are recognised from the header. At a cut point the tool re-creates the gains, the DSP
(host write history with pre-runs), the control registers and every known voice register, and
re-keys the voices still playing at their computed position (position model from the key-on time,
pitch, loop and end registers; current volume and filter ramps). `--cmp` drops the writes that do not
change the chip state (never key on/off, current volume/filter values, DSP writes, or a ramp speed
write that overrides the key-on ramp), tracked over both loop passes for looped files; `--sro` keeps the
played ROM ranges; `--find-loop` matches the key-on sequence (sample address, end, loop, pitch);
`--split log` uses the voice model. The driver never frees a voice (idle voices keep ringing at a
small volume and get re-allocated), so a loop whose body is the first repetition replaces the tails of
the end of the song by the tails of the intro at every re-entry, which is audible. The loop body is
therefore taken from the second repetition whenever the capture holds it (`--loop-pass` auto, as for
the ES55xx logs): the first repetition is left out, a splice image re-creates the state at its end
(registers, voices re-keyed, DSP host writes of the skipped span), and the full voice image at the loop
point makes the second pass start from the same "end of a repetition" state. Capture intro + 2
repetitions + a few seconds.

Validation on Ray Storm (90 s capture): cut 1 s before the first note, no `--cmp`: correlation 1.00
with the uncut render; automatic track with `--cmp --sro` (intro + second repetition, 26 s): first pass
0.97-1.00, loop re-entry vs the game's own second repetition 0.88 in the first 0.5 s then 0.96-0.99,
6.7 MB -> 0.5 MB vgz.
