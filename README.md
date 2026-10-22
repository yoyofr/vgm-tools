# vgm-tools

Tools around VGM logging and packaging, written for the ES5505 / ES5506 / ES5510 VGM extension
(see `VGM-ES55xx-proposal.md`) but usable on any VGM file.

## vgm_trim

Post-processing for raw MAME VGM captures: cut, split a session into tracks on silence, find loop
points from the note-on sequence, drop register writes that do not change the chip state, keep only
the sample ROM ranges that are played, upgrade the header to VGM 1.72, read and write `.vgz`.

```sh
c++ -O2 -std=c++17 -o vgm_trim vgm_trim.cpp -lz

vgm_trim capture.vgm --info
vgm_trim capture.vgm --activity
vgm_trim capture.vgm out.vgz --auto --split log --track all --cmp --sro --find-loop
vgm_trim capture.vgm out.vgz --start 18.2s --end 1:05.5 --loop 30.0s --cmp --sro
```

All reductions are output-neutral: rendered audio is byte-identical before and after.

## gd3edit.html

Local, single-file GD3 tag editor (open it in Chrome for in-place saving). Japanese tags, batch
copy of a field to every loaded file, `.vgm` / `.vgz`, and a vgmrips pack generator (zip with the
tracks, `.m3u`, `.txt` in the vgmrips layout and the pack `.png`).

## Related repositories

- MAME VGM logger with the ES55xx extension: github.com/yoyofr/mame, branch `vgmlog0288-es5506`
- libvgm with ES5505/ES5506/ES5510 cores and the extension commands: github.com/yoyofr/libvgm, branch `es55xx`
