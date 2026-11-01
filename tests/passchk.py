#!/usr/bin/env python3
"""Independent check of --cmp on a looping v1 ES5505 file: replay register writes of the original (pass 1 then pass 2
= body again from the end state) and of the cmp output, compare the register state after every instant where both
have writes; report the first instant where the states differ. usage: passchk.py orig cmp"""
import sys, os; sys.path.insert(0, os.path.dirname(__file__))
from vgmdump import load, walk
import struct
def events(path):
    d = load(path); u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    total = u32(0x18); loop = None
    ev = []  # (t, ci, kind, a, b)
    page = [0, 0]
    for p, op, raw, t, isloop in walk(d):
        if isloop: loop = len(ev)
        if op not in (0xbe, 0xd6): continue
        ci = raw[1] >> 7; aa = raw[1] & 0x7f
        if aa >= 0x40: continue
        if op == 0xbe:
            reg, lane = aa >> 1, aa & 1
            if reg == 15: page[ci] = raw[2]; continue
            ev.append((t, ci, page[ci], reg, lane, raw[2]))
        else:
            reg = aa >> 1; v = raw[2] << 8 | raw[3]
            if reg == 15: page[ci] = v & 0xff; continue
            ev.append((t, ci, page[ci], reg, -1, v))
    return ev, loop, total
def key(ci, page, reg):
    if page < 0x40 and reg == 0: page &= 0x1f
    if page >= 0x40: page = 0x40
    return (ci, page, reg)
def apply(st, e):
    t, ci, page, reg, lane, v = e
    k = key(ci, page, reg)
    old = st.get(k, 0)
    if lane == 0: v = (old & 0xff) | v << 8
    elif lane == 1: v = (old & 0xff00) | v
    st[k] = v
def run(ev, loop, total):
    """yield (time_in_render, state_snapshot_after_instant) for pass1 then pass2"""
    st = {}
    out = []
    # pass 1
    i = 0
    while i < len(ev):
        t = ev[i][0]
        while i < len(ev) and ev[i][0] == t: apply(st, ev[i]); i += 1
        out.append((t, dict(st)))
    # pass 2: body from loop
    i = loop
    while i < len(ev):
        t = ev[i][0]
        while i < len(ev) and ev[i][0] == t: apply(st, ev[i]); i += 1
        out.append((total + t - ev[loop][0] if False else total + (t - ev[loop][0]) + 0, dict(st)))
    return out
ea, la, ta = events(sys.argv[1]); eb, lb, tb = events(sys.argv[2])
print("orig: events", len(ea), "loop idx", la, "total", ta, "| cmp: events", len(eb), "loop idx", lb, "total", tb)
ra = run(ea, la, ta); rb = run(eb, lb, tb)
# compare states at the end of each instant present in orig (cmp may have no write there; use its latest state)
import bisect
tb_list = [t for t, _ in rb]
last_ok = None
for t, sa in ra:
    j = bisect.bisect_right(tb_list, t) - 1
    sb = rb[j][1] if j >= 0 else {}
    bad = [(k, sa[k], sb.get(k)) for k in sa if sb.get(k) != sa[k]]
    if bad:
        print(f"first state mismatch at render sample {t} ({t/44100:.3f}s), pass {'2' if t >= ta else '1'}, song time {(t - ta + ea[la][0]) if t >= ta else t}:")
        for k, x, y in bad[:12]: print(f"   chip{k[0]} page {k[1]:02x} reg {k[2]:x}: orig {x:04x} cmp {y if y is None else f'{y:04x}'}")
        break
    last_ok = t
else: print("states identical throughout; last instant", last_ok)
