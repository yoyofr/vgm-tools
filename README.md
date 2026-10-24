# vgm-tools

Tools around VGM logging and packaging, written for the ES5505 / ES5506 / ES5510 VGM extension
(see `VGM-ES55xx-proposal.md`) and the ZOOM ZSG-2 / TMS57002 extension of the Taito Zoom sound board
(see `VGM-ZSG2-proposal.md`), but usable on any VGM file.

## vgm_trim

Post-processing for raw MAME VGM captures: cut, split a session into tracks on silence, find loop
points from the note-on sequence, drop register writes that do not change the chip state, keep only
the sample ROM ranges that are played, upgrade the header to VGM 1.72, read and write `.vgz`.

```sh
c++ -O2 -std=c++17 -o vgm_trim vgm_trim.cpp libvgm/emu/cores/tms57002.cpp -lz   # the DSP core is a copy of libvgm's

vgm_trim capture.vgm --info
vgm_trim capture.vgm --activity
vgm_trim capture.vgm out.vgz --auto --split log --track all --cmp --sro --find-loop   # out_00.vgz, out_01... first free numbers, nothing overwritten
vgm_trim capture.vgm out.vgz --start 18.2s --end 1:05.5 --loop 30.0s --cmp --sro
vgm_trim capture.vgm out.vgz --start 18.2s --end 1:05.5 --volume +6dB
vgm_trim part1.vgz merged.vgz --append part2.vgz
vgm_trim capture.vgm capture.vgz            # no option: rewrite as is (loop kept); 1.73 files become 1.72
vgm_trim capture.wav tracks.wav --auto --track all --gap 3s   # a .wav input is split / cut the same way (16-bit PCM out)
```

All reductions are output-neutral: rendered audio is byte-identical before and after.

A `.wav` input (for example the `<capture>.wav` that MAME's `-vgmwav` option records alongside each
capture: same start and stop, same numbering) goes through the same silence split, margins, `--start`
`--end` and numbering, and comes out as 16-bit PCM wav files; the VGM-only options are ignored.

The console output uses sections, status lines and tables; colours when stdout is a terminal
(`--color auto|always|never`, `NO_COLOR` honoured).

`--volume` sets the VGM header volume modifier (offset 0x7C, VGM 1.60+): the player applies
2^(n/32) to its output, n = round(dB / 6.02 × 32), range -12 dB..+36 dB. The chip data is untouched,
so it stays lossless and reversible; `--info` prints the current value.

`--append` (repeatable) concatenates captures: data blocks of every part first, then the command
streams in order. The initial register image of an appended part re-creates its state at the
junction and the ESP keeps the tail of the previous part. Header, GD3 and volume come from the
first part, the loop from the last part that has one.

ZSG-2 logs (Taito FX-1B / G-NET, VGM 1.72 with a 0x110-byte header) are handled by a dedicated path: a cut re-creates the
board gains, the DSP (simulated from the start of the capture with libvgm's TMS57002 core, a copy of
which lives in `libvgm/`, and written as a state block 0x98 that loads instantly), the register image and re-keys the
voices still playing at their computed position; `--cmp`, `--sro`, `--find-loop` and `--split log` use
a ZSG-2 voice model; the loop body is taken from the second repetition when captured (seamless loop).

## gd3edit.html

Local, single-file GD3 tag editor (open it in Chrome for in-place saving). Japanese tags, batch
copy of a field to every loaded file, title from the file name (track number and extension
stripped, shift-click for every file), `.vgm` / `.vgz`, the header volume modifier (in dB, see
`--volume` above), the loop modifier and loop base (0x7F / 0x7E: the player plays
`default × modifier − base` loops), and a vgmrips pack generator (zip with the tracks, `.m3u`, `.txt` in the vgmrips
layout and the pack `.png`).

Saving in place uses the File System Access API (Chrome/Edge). Open the files with **Dossier…** or drop
their folder: Chrome then asks the write permission once for the whole folder, and **Tout enregistrer**
runs without any dialog. Files opened one by one each need their own permission prompt.

Each file has a track number (not stored in the VGM: taken from the file name, "03-title.vgz" or
"game_track_02.vgz", editable). It orders the m3u and names the pack entries, and **Renommer…** renames
the files on disk from a format (default `{nn} {title}`; fields `{n}` `{nn}` `{nnn}` `{title}` `{titlej}`
`{game}` `{gamej}` `{system}` `{author}` `{date}` `{name}`), with a preview and duplicate check.

Audio files (.ogg, .opus, .oga, .mp3, .flac, .wav) can be added to the list next to the VGMs: title and
number from the file name, duration from the browser's decoder, nothing is written into them. They are
packed as they are, listed in the m3u and the txt in track order, and renamed with the same format.

## Related repositories

- MAME VGM logger with the ES55xx extension: github.com/yoyofr/mame, branch `vgmlog0288-es5506`
- libvgm with ES5505/ES5506/ES5510 cores and the extension commands: github.com/yoyofr/libvgm, branch `es55xx`
