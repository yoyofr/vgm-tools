# vgm-tools

Tools around VGM logging and packaging, written for the ES5505 / ES5506 / ES5510 VGM extension
(see `VGM-ES55xx-proposal.md`) and the ZOOM ZSG-2 / TMS57002 extension of the Taito Zoom sound board
(see `VGM-ZSG2-proposal.md`), but usable on any VGM file.

## vgm_trim

Post-processing for raw MAME VGM captures: cut, split a session into tracks on silence, find loop
points from the note-on sequence (the loop starts where the song becomes periodic: everything
before, intro or build-up, is kept), drop register writes that do not change the chip state, keep only
the sample ROM ranges that are played, upgrade the header to VGM 1.72, read and write `.vgz`.

```sh
c++ -O2 -std=c++17 -o vgm_trim vgm_trim.cpp libvgm/emu/cores/tms57002.cpp -lz   # the DSP core is a copy of libvgm's

vgm_trim capture.vgm --info
vgm_trim capture.vgm --activity
vgm_trim capture.vgm out.vgz --auto --split log --track all --cmp --sro --find-loop   # out_00.vgz, out_01... first free numbers, nothing overwritten
vgm_trim capture.vgm out.vgz --start 18.2s --end 1:05.5 --loop 30.0s --cmp --sro
vgm_trim capture.vgm out.vgz --start 18.2s --end 1:05.5 --volume +6dB
vgm_trim part1.vgz merged.vgz --append part2.vgz
vgm_trim part1.vgz merged.vgz --repeat 2 --append part2.vgz   # intro + 2 loop passes of part 1, then part 2 (its loop is kept)
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
first part, the loop from the last part that has one. `--repeat N` (after the input or after an
`--append FILE`) plays that part's loop body N times instead of looping there (0 = its intro only), so a
two-part song (Ray Crisis BGM 6 then 7) becomes one file: intro + N passes of part 1, then part 2 and its
loop. ZSG-2 junctions are seamless: the voices still sounding get a 0.1 s release and ring into the
next part (its register image is applied voice by voice when it first uses them, its idle key-offs leave
them alone until then), and the DSP runs on, reverb tail included: the next part's state block is left
out when its program is the one already running and only the coefficients that differ are sent through
the host port.

ZSG-2 logs (Taito FX-1B / G-NET, VGM 1.72 with a 0x110-byte header) are handled by a dedicated path: a cut re-creates the
board gains, the DSP (simulated from the start of the capture with libvgm's TMS57002 core, a copy of
which lives in `libvgm/`, and written as a state block 0x98 that loads instantly), the register image and re-keys the
voices still playing at their computed position; `--cmp`, `--sro`, `--find-loop` and `--split log` use
a ZSG-2 voice model; the loop detection also follows the pitch, volume, filter and gain changes
on sustained voices (Ray Crisis plays most of a song that way, with the drums keyed from the
first bar); the loop body is taken from the second repetition when captured (seamless loop).

## vgmstudio.html (formerly gd3edit.html)

Local, single-file tool with two modules: **Tags** (the GD3 editor below) and **Assemblage**, which chains
parts into one file (`vgm_trim --repeat/--append` above): pick the parts among the loaded files, set the
number of loop passes of each one (empty = the part keeps its loop, normally the last one), assemble, then
listen to the result or to a 10 s window around each junction. The result is added to the list (its GD3 =
the first part's, editable in Tags) and saved with the usual button. Assembling and listening need the
local backend, which runs `vgm_trim` and `vgm2wav` (libvgm's own player, so what you hear is what any
player plays):

```sh
python3 vgmstudio.py            # then open http://127.0.0.1:8765/  (--port, --vgm-trim, --vgm2wav)
```

Opened as a plain file, vgmstudio.html still works as the tag editor alone.

The **Tags** module is a GD3 tag editor (open it in Chrome for in-place saving). Japanese tags, batch
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
Ogg Opus / Vorbis files have their comments read and written (TITLE, TITLE_JA, ALBUM, ALBUM_JA, SYSTEM,
SYSTEM_JA, ARTIST, ARTIST_JA, DATE, ENCODED_BY, COMMENT (DESCRIPTION is read too), TRACKNUMBER; other
comments are kept), so the whole GD3 form and the track number apply to them and save in place.

The pack follows the vgmrips submission rules: files `## Track Title.vgz` numbered 01..N in list order
(option, else the files' own numbers), `Game Name.txt/.m3u/.png`, flat zip, description file with block
wrapping, a dash for empty notes, history continuation lines indented by one space, release date as
YYYY-MM-DD / YYYY-MM / YYYY (checked), one screenshot named after the game or several kept by name (one
per region). The `1..N` button renumbers the tracks in list order.

## Related repositories

- MAME VGM logger with the ES55xx extension: github.com/yoyofr/mame, branch `vgmlog0288-es5506`
- libvgm with ES5505/ES5506/ES5510 cores and the extension commands: github.com/yoyofr/libvgm, branch `es55xx`
