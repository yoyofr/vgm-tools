# ZOOM ZSG-2 + TMS57002 (Taito Zoom board) in VGM: proposal v2

yoyofr, 2026-10-25. Design only, not implemented. Replaces the encoding of the first proposal; the
emulation side (C port of MAME's ZSG-2, C++ port of MAME's TMS57002, mixer model, cut/loop handling,
validation against MAME) is unchanged.

The Taito Zoom board (Taito FX-1B: Ray Storm, G-Darius; Taito G-NET: Ray Crisis, Super Puzzle Bobble...)
is a MN10200 sound CPU driving a ZSG-2 48-voice wavetable chip whose four outputs (reverb send, chorus
send, direct left, direct right) go through a TMS57002 effects DSP programmed by the sound CPU.

The first proposal made the TMS57002 a sub-device of the ZSG-2 and put register writes, DSP host writes,
control lines, mixer gains, reset and pre-run behind sub-command bits of one opcode. This version
declares two chips, one opcode per function, and uses the generic board commands shared with the
ES55xx proposal.

## 1. Header

Two new 32-bit clock fields in the first free slots after the 1.72 fields (bit 30 = dual chip):

| Field | Content |
|---|---|
| ZSG-2 clock | 25000000 on the Taito Zoom board (sample rate = clock / 768) |
| TMS57002 clock | 12500000 (one program pass per ZSG-2 sample, 384 instruction cycles) |

Players read a field only when the header is long enough, as for every earlier addition.
Chip IDs: ZSG-2 = 0x30 (already allocated), TMS57002 = 0x32.

## 2. ZSG-2

| Command | Meaning |
|---|---|
| `0xE4 oooo dddd` | register write: 16-bit word offset, big endian (0x000-0x2FF voice registers, 16 per voice; 0x300-0x31F control registers: key on 0x300-0x302, key off 0x304-0x306, ROM readback address 0x31C/0x31D); bit 15 = second chip. 16-bit data, big endian. |
| `0x48 cc rr` | control: bit 0 = reset pulse (the sound CPU resets the chip at boot and on a game reset). |

Sample memory: data block `0x97`, 32-bit little-endian words of four 7-bit compressed samples, plain byte
offsets, partial blocks allowed. On the Taito G-NET the samples sit in flash memories programmed by the
game: the block is logged at the first key-on and again (changed range only) when the game reprograms them.

## 3. TMS57002

| Command | Meaning |
|---|---|
| `0x49 cc dd` | host data write (program upload under PLOAD, coefficient upload and updates under CLOAD). |
| `0x4A cc ll` | control lines: bit 0 = PLOAD, bit 1 = CLOAD (levels, active low as on the pins), bit 2 = reset pulse. Lines are set CLOAD first, PLOAD second, as the board does. |
| `0xCB cc nnnn` | run nnnn sample periods with silent inputs, output discarded. Re-creates the program-internal state at a cut point or at a restarted capture (coefficient updates landing where the running program reads them, LFO phases). Repeated for longer spans. |

cc bit 7 = second chip, other bits 0.

## 4. Board wiring: generic commands

Shared with the ES55xx proposal:

| Command | Meaning |
|---|---|
| `0xE3 tt cc gggg` | output gain: channel cc of chip tt into the final mix, big-endian 8.8 (0x0100 = unity, default). Even channels left, odd channels right. Channels 0x80+ii are the serial inputs of a DSP (input attenuation). |
| `0xE2 dd ii ss cc` | serial routing: input ii of DSP dd takes output channel cc of chip ss. |

Taito Zoom wiring:

```
0xE2 0x32 00 0x30 00      ZSG-2 output 0 (reverb send) -> DSP input 0
0xE2 0x32 01 0x30 01      ZSG-2 output 1 (chorus send) -> DSP input 1
0xE2 0x32 02 0x30 02      ZSG-2 output 2 (direct L)    -> DSP input 2
0xE2 0x32 03 0x30 03      ZSG-2 output 3 (direct R)    -> DSP input 3
0xE3 0x32 80 0080         DSP input 0 at 0.5
0xE3 0x32 81 0080         DSP input 1 at 0.5
0xE3 0x32 02 gggg         DSP output 2 -> left   (global volume register of the board)
0xE3 0x32 03 gggg         DSP output 3 -> right
```

ZSG-2 channels 0-3 have no direct gain (they only reach the mix through the DSP). The two global volume
registers written by the main CPU are logged as the output gains, (value & 0x3F) / 63.

## 5. DSP state at a cut point

A track cut late in a long session would otherwise replay minutes of DSP host history with pre-runs
before its first note (4-16 s on a phone). The first implementation added a versioned "complete state"
block, which is a savestate; VGM never had one. Two regular alternatives, to be chosen:

1. **Memory images as a 32-bit RAM-write block** (the SCSP `0xE0` precedent): one block type for the
   TMS57002 whose address space maps its memories (external RAM 256 KB, dmem0, dmem1, coefficient RAM,
   program RAM). Registers are not in the image: the program counter is re-synchronised every sample,
   the accumulators live one pass, and the circular-buffer pointers are normalised by the cut tool
   (the delay RAM image is rotated so the pointers equal their reset values). Pure "memory + bus
   writes"; the pointer normalisation has to be proven bit-exact.
2. **Keep the state block**, documented as a 32-bit RAM-write block with the register image at a fixed
   address range of the same space. Already bit-exact, simpler, still a savestate.

`0xCB` stays for the no-block case (short histories, restarted captures).

## 6. Mapping from the first proposal

| v1 (`0xE4 ss aa dddd`) | v2 |
|---|---|
| ss & 0x0F = 0-3: register write | `0xE4 oooo dddd` |
| ss = 4: DSP host byte | `0x49 cc dd` |
| ss = 5: DSP control lines | `0x4A cc ll` |
| ss = 6: mixer gain channel | `0xE2` + `0xE3` |
| ss = 7: reset | `0x48 cc 01` + `0x4A cc 04` |
| ss = 8: DSP pre-run | `0xCB cc nnnn` |
| block `0x97` | unchanged |
| block `0x98` (DSP state) | option 1 or 2 of section 5 |
| ZSG-2 clock at 0x100 | ZSG-2 and TMS57002 clock fields |

Table-driven on our side (logger emission, libvgm command and block tables, vgm_trim parser); the cores
and the cut/loop logic are untouched. Existing files are converted offline and checked by byte
comparison of the rendered audio.

## 7. Open points

- Opcode and block numbers: any free fixed-length values work.
- DSP state at a cut point: option 1, option 2, or no block at all (pre-run only).
- The TMS57002 is a generic DSP (also on Konami GX/Mystic Warriors boards with the K054539): declaring it
  as its own chip with generic routing makes those boards possible later without a new encoding.
