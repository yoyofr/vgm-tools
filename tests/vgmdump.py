#!/usr/bin/env python3
"""Dump VGM (v1 ES55xx / ZSG-2) commands with sample times. usage: vgmdump.py file [from_s] [to_s] [--waits]"""
import sys, gzip, struct
def is_v3(d):
    u = lambda o: struct.unpack_from("<I", d, o)[0]
    ofs = 0x34 + u(0x34) if u(8) >= 0x150 and u(0x34) else 0x40
    return ofs >= 0x120 and len(d) >= 0x120 and (u(0xD0) != 0 or u(0x110) != 0)
def load(p):
    """raw bytes of a VGM, inflated; a v3-encoded file is converted to v1 with vgm_v3conv --to-v1 first"""
    d = open(p, "rb").read()
    d = gzip.decompress(d) if d[:2] == b"\x1f\x8b" else d
    if is_v3(d):
        import subprocess, tempfile, os
        tmp = tempfile.mkdtemp(prefix="v3dump_"); out = os.path.join(tmp, "v1.vgm")
        r = subprocess.run(["/Users/yohann/Documents/Dev/vgmrips/vgm-tools/vgm_v3conv", "--to-v1", p, out], capture_output=True, text=True)
        if r.returncode: raise RuntimeError("vgm_v3conv failed: " + r.stderr)
        d = open(out, "rb").read(); subprocess.run(["rm", "-rf", tmp])
    return d
def cmd_len(op):
    if op in (0x30, 0x3f, 0x4f, 0x50) or 0x31 <= op <= 0x3e: return 2
    if 0x40 <= op <= 0x4e or 0x51 <= op <= 0x5f or op == 0x61: return 3
    if op in (0x62, 0x63, 0x66): return 1
    if op == 0x64: return 4
    if 0x70 <= op <= 0x8f: return 1
    if 0x90 <= op <= 0x95: return (5, 5, 6, 11, 2, 5)[op - 0x90]
    if 0xa0 <= op <= 0xbf: return 3
    if 0xc0 <= op <= 0xdf: return 4
    if op >= 0xe0: return 5
    return 1
def walk(d):
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    ver = u32(8); gd3 = u32(0x14) + 0x14 if u32(0x14) else 0; eof = u32(4) + 4
    data = u32(0x34) + 0x34 if ver >= 0x150 else 0x40
    loop = u32(0x1c) + 0x1c if u32(0x1c) else 0
    end = gd3 or eof; p = data; t = 0
    while p < end:
        op = d[p]
        if op == 0x67: n = 7 + (u32(p + 3) & 0x7fffffff)
        elif op == 0x68: n = 12
        else: n = cmd_len(op)
        yield p, op, d[p:p + n], t, p == loop
        if op == 0x61: t += d[p + 1] | d[p + 2] << 8
        elif op == 0x62: t += 735
        elif op == 0x63: t += 882
        elif 0x70 <= op <= 0x7f: t += (op & 15) + 1
        elif 0x80 <= op <= 0x8f: t += op & 15
        if op == 0x66: return
        p += n
if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    d = load(args[0]); f = float(args[1]) * 44100 if len(args) > 1 else 0; to = float(args[2]) * 44100 if len(args) > 2 else 1e18
    waits = "--waits" in sys.argv
    for p, op, raw, t, isloop in walk(d):
        if t < f: continue
        if t > to: break
        if op == 0x67: s = f"BLOCK type {raw[2]:02x} size {len(raw)-7}"
        elif 0x61 <= op <= 0x63 or 0x70 <= op <= 0x8f:
            if not waits: continue
            s = "wait"
        else: s = raw.hex(" ")
        print(f"{t:>9} {t/44100:9.4f} {p:#08x} {'LOOP ' if isloop else ''}{s}")
