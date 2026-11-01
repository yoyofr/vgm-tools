#!/usr/bin/env python3
"""Check vgm_trim --cmp / --sro exactness: render original vs trimmed with vgm2wav-v1 (2 loops), compare."""
import sys, os, subprocess, tempfile, concurrent.futures as cf
T = "/Users/yohann/Documents/Dev/vgmrips/vgm-tools"
TRIM = f"{T}/vgm_trim"; V2W_V1 = f"{T}/bin/vgm2wav-v1"; V2W_V3 = "/Users/yohann/Documents/Dev/vgmrips/libvgm/build/bin/vgm2wav"
import gzip, struct
def is_v3(path):
    with open(path, "rb") as f: d = f.read(0x200)
    if d[:2] == b"\x1f\x8b": d = gzip.open(path).read(0x200)
    u = lambda o: struct.unpack_from("<I", d, o)[0]
    ofs = 0x34 + u(0x34) if u(8) >= 0x150 and u(0x34) else 0x40
    return ofs >= 0x120 and (u(0xD0) != 0 or u(0x110) != 0)
KEEP = os.environ.get("KEEP")  # directory: keep trimmed files + wavs there
def render(path, out):
    subprocess.run([V2W_V3 if is_v3(path) else V2W_V1, "--loops", "2", "--fade", "0", path, out], check=True, capture_output=True)
    with open(out, "rb") as f: d = f.read()
    p = 12
    while p + 8 <= len(d):
        tag, n = d[p:p+4], int.from_bytes(d[p+4:p+8], "little")
        if tag == b"data": return d[p+8:p+8+n]
        p += 8 + n + (n & 1)
    raise RuntimeError("no data chunk")
def first_diff(a, b):
    n = min(len(a), len(b))
    if a[:n] == b[:n]: return None
    lo, hi = 0, n
    while lo < hi:
        m = (lo + hi) // 2
        if a[:m] == b[:m]: lo = m + 1
        else: hi = m
    return (lo - 1) // 4
def check(path, mode):
    tmp = tempfile.mkdtemp(prefix="chk_")
    base = os.path.splitext(os.path.basename(path))[0]
    try:
        o = os.path.join(KEEP or tmp, f"{base}{mode.replace('--','_')}.vgm")
        r = subprocess.run([TRIM, path, o, mode, "--color", "never"], capture_output=True, text=True)
        if r.returncode: return (path, mode, "TRIM-FAIL", r.stderr[-200:])
        a = render(path, os.path.join(KEEP or tmp, f"{base}_orig.wav")); b = render(o, os.path.join(KEEP or tmp, f"{base}{mode.replace('--','_')}.wav"))
        fd = first_diff(a, b)
        if a == b: return (path, mode, "OK", "")
        if fd is None: return (path, mode, "LEN", f"same up to common end, {len(a)//4} vs {len(b)//4} samples ({(len(b)-len(a))//4:+d})")
        return (path, mode, "DIFF", f"first diff sample {fd} ({fd/44100:.3f}s), {len(a)//4} vs {len(b)//4} samples")
    finally:
        subprocess.run(["rm", "-rf", tmp])
files = [a for a in sys.argv[1:] if not a.startswith("--")]
modes = [a for a in sys.argv[1:] if a.startswith("--")] or ["--cmp", "--sro"]
with cf.ThreadPoolExecutor(8) as ex:
    for p, m, st, info in ex.map(lambda x: check(*x), [(f, m) for f in files for m in modes]):
        print(f"{st:5} {m:6} {os.path.basename(p)[:50]:50} {info}", flush=True)
