#!/bin/zsh
# Builds "VGM Studio.app" (a launcher: starts vgmstudio.py if needed, then opens the page in Chrome)
# and "VGM Studio Stop.app" (stops the backend)
# usage: macos/make_app.sh [destination dir, default ~/Applications]
set -e
HERE=${0:A:h}
TOOLS=${HERE:h}                       # the vgm-tools checkout (vgmstudio.py, vgm_trim)
DEST=${1:-$HOME/Applications}
APP="$DEST/VGM Studio.app"
PORT=${VGMSTUDIO_PORT:-8765}
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>VGM Studio</string>
  <key>CFBundleDisplayName</key><string>VGM Studio</string>
  <key>CFBundleIdentifier</key><string>fr.yoyofr.vgmstudio</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIconFile</key><string>app</string>
  <key>LSUIElement</key><true/>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
</dict></plist>
PLIST
cat > "$APP/Contents/MacOS/launch" <<LAUNCH
#!/bin/zsh
# VGM Studio launcher: backend (vgmstudio.py) + page in Chrome
TOOLS="$TOOLS"
PORT=$PORT
URL="http://127.0.0.1:\$PORT/"
LOG="\$HOME/Library/Logs/vgmstudio.log"
PY=\$(command -v python3 || echo /usr/bin/python3)
for p in /opt/homebrew/bin/python3 /usr/local/bin/python3; do [ -x "\$p" ] && PY="\$p" && break; done
alive() { curl -s -m 1 "\$URL/api/ping" | grep -q '"ok": true'; }
if ! alive; then
  cd "\$TOOLS" || { osascript -e 'display alert "VGM Studio" message "vgm-tools introuvable : '"\$TOOLS"'"'; exit 1; }
  nohup "\$PY" vgmstudio.py --port "\$PORT" >> "\$LOG" 2>&1 &
  for i in {1..50}; do alive && break; sleep 0.2; done
  alive || { osascript -e 'display alert "VGM Studio" message "Le serveur ne démarre pas, voir ~/Library/Logs/vgmstudio.log"'; exit 1; }
fi
if [ -d "/Applications/Google Chrome.app" ]; then open -a "Google Chrome" "\$URL"; else open "\$URL"; fi
LAUNCH
chmod +x "$APP/Contents/MacOS/launch"
# icon: a simple generated one (sips/iconutil), optional
if command -v iconutil >/dev/null && command -v python3 >/dev/null; then
  TMP=$(mktemp -d); SET="$TMP/app.iconset"; mkdir -p "$SET"
  python3 - "$TMP/base.png" <<'PY'
import struct, zlib, sys, math
W=1024; rows=[]
for y in range(W):
    row=bytearray()
    for x in range(W):
        dx=x-W/2; dy=y-W/2; r=math.hypot(dx,dy)
        if r>W*0.47: row+=bytes((0,0,0,0)); continue
        # rounded dark disc with a waveform-like ring
        ring = abs(r-W*0.30)<W*0.035 and (math.sin(math.atan2(dy,dx)*8)>-0.2)
        if ring: row+=bytes((224,122,74,255))
        else:
            v=int(36+20*(1-r/(W*0.47))); row+=bytes((v,v-2,v-4,255))
    rows.append(bytes(row))
raw=b''.join(b'\x00'+r for r in rows)
def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,W,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b'')
open(sys.argv[1],'wb').write(png)
PY
  for s in 16 32 128 256 512; do
    sips -z $s $s "$TMP/base.png" --out "$SET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$TMP/base.png" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$SET" -o "$APP/Contents/Resources/app.icns" 2>/dev/null || true
  rm -rf "$TMP"
fi
touch "$APP"
echo "built: $APP (backend from $TOOLS, port $PORT)"

# the stop app: kills the backend listening on the port (and any other vgmstudio.py)
STOP="$DEST/VGM Studio Stop.app"
mkdir -p "$STOP/Contents/MacOS" "$STOP/Contents/Resources"
sed -e 's/<string>VGM Studio<\/string>/<string>VGM Studio Stop<\/string>/g' -e 's/fr.yoyofr.vgmstudio/fr.yoyofr.vgmstudio.stop/' "$APP/Contents/Info.plist" > "$STOP/Contents/Info.plist"
cat > "$STOP/Contents/MacOS/launch" <<STOPSH
#!/bin/zsh
# VGM Studio Stop: ends the backend (vgmstudio.py)
PORT=$PORT
PIDS=( \$( { lsof -ti tcp:\$PORT -sTCP:LISTEN; pgrep -f 'vgmstudio[.]py'; } 2>/dev/null | sort -u | grep -vx "\$\$" ) )
if [ \${#PIDS} -eq 0 ]; then
  osascript -e 'display notification "Le serveur ne tournait pas" with title "VGM Studio"'
  exit 0
fi
kill \${PIDS[@]} 2>/dev/null
for i in {1..20}; do pgrep -f 'vgmstudio[.]py' >/dev/null || break; sleep 0.2; done
pgrep -f 'vgmstudio[.]py' >/dev/null && kill -9 \${PIDS[@]} 2>/dev/null
osascript -e 'display notification "Serveur arrêté" with title "VGM Studio"'
STOPSH
chmod +x "$STOP/Contents/MacOS/launch"
if [ -f "$APP/Contents/Resources/app.icns" ] && command -v python3 >/dev/null; then
  # same icon, greyed ring, so the two apps tell apart
  TMP=$(mktemp -d); SET="$TMP/app.iconset"; mkdir -p "$SET"
  python3 - "$TMP/base.png" <<'PY'
import struct, zlib, sys, math
W=1024; rows=[]
for y in range(W):
    row=bytearray()
    for x in range(W):
        dx=x-W/2; dy=y-W/2; r=math.hypot(dx,dy)
        if r>W*0.47: row+=bytes((0,0,0,0)); continue
        ring = abs(r-W*0.30)<W*0.035
        sq = abs(dx)<W*0.11 and abs(dy)<W*0.11
        if sq: row+=bytes((224,122,74,255))
        elif ring: row+=bytes((120,116,110,255))
        else:
            v=int(36+20*(1-r/(W*0.47))); row+=bytes((v,v-2,v-4,255))
    rows.append(bytes(row))
raw=b''.join(b'\x00'+r for r in rows)
def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,W,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b'')
open(sys.argv[1],'wb').write(png)
PY
  for s in 16 32 128 256 512; do
    sips -z $s $s "$TMP/base.png" --out "$SET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$TMP/base.png" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$SET" -o "$STOP/Contents/Resources/app.icns" 2>/dev/null || true
  rm -rf "$TMP"
fi
touch "$STOP"
echo "built: $STOP"
