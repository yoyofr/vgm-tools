# mame-scripts

Helpers used to investigate the Taito Zoom (ZSG-2) sound driver of Ray Crisis with MAME (2026-10-31).

- `run6.sh <tag> <mame-binary> [song]`: boots raycrisj, enters the test mode, opens the sound settings,
  selects BGM code `song` and plays it with `-vgmwrite 1`; capture in `run_<tag>/` (`RUNDIR` to change
  where, `WAIT` = seconds of song to capture, `EXTRA` = extra MAME options, `DIAG` / `POSTCMDS` see
  `ctl.lua`). Edit `M=` (MAME checkout with roms/ and nvram/raycrisj) at the top.
- `ctl.lua`: autoboot script, file-driven remote control (`cmd.txt` in the run folder, written atomically:
  `tap <port|field>`, `set` / `clear`, `snap`, `peek <hex addr>`, `dump <hex addr> <hex len>`, `time`,
  `quit`), logs the PSX -> sound CPU mailbox writes (`tap_ctl.log`), optional diagnostics (`DIAG=5`: Lua
  read tap on the song data, `TAPLO` / `TAPHI` hex range).
- `dec.py [song.bin] [track]`: decodes a Zoom sequence (CPU byte order): tracks, channels, note-ons,
  commands, end times. Song table / format notes: see the vgmrips memory of the session.
- `ini/ui.ini`: `skip_warnings 1` (the missing-ROM popup otherwise blocks the scripted inputs).

Never run two MAME instances on the same run folder (they share `cmd.txt`).
