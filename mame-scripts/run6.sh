#!/bin/zsh
# usage: run6.sh <tag> <mame-binary> [song]
Z=${0:A:h}                                  # this folder (ctl.lua, ini/)
OUT=${RUNDIR:-$PWD}                        # where run_<tag>/ is created
M=/Users/yohann/Documents/Dev/vgmrips/mame
TAG=$1; BIN=$2; SONG=${3:-6}
R=$OUT/run_$TAG; rm -rf $R; mkdir -p $R/nvram $R/cfg; cp -r $M/nvram/raycrisj $R/nvram/; cp $M/cfg/*.cfg $R/cfg/ 2>/dev/null
cd $R
ZDIR=$R $BIN raycrisj -rompath $M/roms -bgfx_path $M/bgfx -nvram_directory $R/nvram -cfg_directory $R/cfg -inipath $Z/ini -homepath $M -skip_gameinfo -snapshot_directory $R/snap -autoboot_script $Z/ctl.lua -window -sound none -vgmwrite 1 ${=EXTRA} > $R/mame.log 2>&1 &
PID=$!
cmd(){ printf '%s\n' "$1" > $R/cmd.tmp; mv $R/cmd.tmp $R/cmd.txt; sleep 0.7; }
tap(){ cmd "tap $1"; }
while true; do n=$(grep -c IRQ $R/tap_ctl.log 2>/dev/null); [ -n "$n" ] && [ "$n" -ge 3 ] && break; sleep 1; done; sleep 4
cmd 'set :SERVICE|Service_Mode 1'; sleep 4; cmd snap
tap ':P1|P1_Down'; tap ':P1|P1_Down'; cmd snap; tap ':P1|P1_Button_1'; sleep 2
tap ':P1|P1_Down'; for i in $(seq 1 $((SONG-1))); do tap ':P1|P1_Right'; done; sleep 0.5
cmd snap
tap ':P1|P1_Button_1'
cmd time
T0=$(tail -1 $R/ctl_out.log | awk '{print $1}'); echo "play at emulated $T0" | tee $R/t0.txt
if [ -n "$POSTCMDS" ]; then sleep ${POSTDELAY:-8}; while IFS= read -r line; do cmd "$line"; done < $POSTCMDS; fi
while true; do cmd time; sleep 5; T=$(tail -1 $R/ctl_out.log | awk '{print $1}'); if [ $(echo "$T - $T0 >= ${WAIT:-175}" | bc) -eq 1 ]; then break; fi; done
cmd quit; wait $PID; echo "done; files:"; ls -la $R/*.vgm; grep -B3 "IRQ" $R/tap_ctl.log | tail -8
