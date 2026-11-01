#!/usr/bin/env python3
"""Register-level dump of a v1 ES5505/ES5506 VGM: t chip page reg value [lane]. usage: regdump.py file from_s to_s"""
import sys; sys.path.insert(0, __import__("os").path.dirname(__file__))
from vgmdump import load, walk
d = load(sys.argv[1]); f = float(sys.argv[2]) * 44100; to = float(sys.argv[3]) * 44100
page = [0, 0]
for p, op, raw, t, isloop in walk(d):
    if t > to: break
    if op in (0xbe, 0xd6, 0xd7, 0xd8, 0xe3):
        ci = raw[1] >> 7; aa = raw[1] & 0x7f
        if op == 0xbe:
            if aa >= 0x40: s = f"bank v{aa&31}={raw[2]:02x}"
            else:
                reg, lane = aa >> 1, aa & 1
                if reg == 15: page[ci] = raw[2]; s = None
                else: s = f"p{page[ci]:02x} r{reg:x}.{'hi' if lane==0 else 'lo'}={raw[2]:02x}"
        elif op == 0xd6:
            reg = aa >> 1; v = raw[2] << 8 | raw[3]
            if aa >= 0x40: s = f"bank v{aa&31}={v:04x}"
            elif reg == 15: page[ci] = v & 0xff; s = None
            else: s = f"p{page[ci]:02x} r{reg:x}={v:04x}"
        elif op == 0xd7: s = f"ext {aa:02x} {raw[2]:02x} {raw[3]:02x}"
        elif op == 0xd8: s = f"esp {aa:02x} {raw[2]:02x} {raw[3]:02x}"
        else: s = f"gain {raw[2]:02x}={raw[3]:02x}{raw[4]:02x}"
        if s and t >= f: print(f"{t:>9} c{ci} {'LOOP ' if isloop else ''}{s}")
    elif isloop and t >= f: print(f"{t:>9} LOOP")
