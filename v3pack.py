#!/usr/bin/env python3
"""Convert a vgmrips pack (zip) of ES55xx / ZSG-2 tracks from the v1 encoding to v3.

    python3 v3pack.py "Pack.zip" ["Pack (v3).zip"] [--jobs N] [--no-verify]

Every .vgm / .vgz of the zip goes through vgm_v3conv. Each converted file is checked: the v1 file is
rendered with vgm2wav-v1 (libvgm "es55xx" branch), the v3 file with vgm2wav (libvgm "vgm-v3" branch),
2 loops, no fade; the two WAV files must be identical byte for byte, else the pack is not written.
The other members (.m3u, .txt, .png, .opus...) and the member order, names and dates are kept; the zip
is written stored, like the pack generator of VGM Studio. Used by vgmstudio.py (/api/v3pack).
"""
import argparse, concurrent.futures, hashlib, io, os, shutil, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

def default_tools():
    return {
        "vgm_v3conv": os.path.join(HERE, "vgm_v3conv"),
        "vgm2wav_v1": os.path.join(HERE, "bin", "vgm2wav-v1"),
        "vgm2wav": os.path.join(HERE, "..", "libvgm", "build", "bin", "vgm2wav"),
    }

def _render(tool, src, wav):
    p = subprocess.run([tool, "--loops", "2", "--fade", "0", src, wav], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0 or not os.path.isfile(wav):
        raise RuntimeError(f"{os.path.basename(tool)} failed on {os.path.basename(src)}")
    h = hashlib.sha256()
    with open(wav, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
    size = os.path.getsize(wav)
    os.remove(wav)
    return h.hexdigest(), size

def convert_one(tools, tmp, idx, name, data, verify):
    """-> dict(name, status: converted|copied|error, data (bytes), log, seconds)"""
    ext = ".vgz" if name.lower().endswith(".vgz") else ".vgm"
    src = os.path.join(tmp, f"{idx}_in{ext}")
    dst = os.path.join(tmp, f"{idx}_out{ext}")
    with open(src, "wb") as f: f.write(data)
    p = subprocess.run([tools["vgm_v3conv"], src, dst], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    log = p.stdout.replace(src, name)
    if p.returncode == 2:
        return {"name": name, "status": "copied", "data": data, "log": log.strip()}
    if p.returncode != 0 or not os.path.isfile(dst):
        return {"name": name, "status": "error", "data": None, "log": log.strip()}
    with open(dst, "rb") as f: out = f.read()
    res = {"name": name, "status": "converted", "data": out, "log": log.strip()}
    if verify:
        h1, n1 = _render(tools["vgm2wav_v1"], src, os.path.join(tmp, f"{idx}_1.wav"))
        h3, n3 = _render(tools["vgm2wav"], dst, os.path.join(tmp, f"{idx}_3.wav"))
        res["verified"] = h1 == h3
        res["seconds"] = max(0, (n1 - 68) / 4 / 44100)
        if h1 != h3:
            res["status"] = "error"
            res["log"] += f"\n  render mismatch: v1 {n1} bytes {h1[:12]}, v3 {n3} bytes {h3[:12]}"
    for pth in (src, dst):
        if os.path.exists(pth): os.remove(pth)
    return res

def convert_pack(zip_bytes, tools=None, jobs=None, verify=True, progress=None):
    """zip in -> (zip out or None on failure, report list). progress(done, total, name) is called per track."""
    tools = tools or default_tools()
    for k, v in tools.items():
        if not v or not os.path.isfile(v): raise RuntimeError(f"{k} not found ({v})")
    zin = zipfile.ZipFile(io.BytesIO(zip_bytes))
    infos = [i for i in zin.infolist() if not i.is_dir() and not i.filename.startswith("__MACOSX/") and not os.path.basename(i.filename).startswith("._")]
    tracks = [i for i in infos if i.filename.lower().endswith((".vgm", ".vgz"))]
    tmp = tempfile.mkdtemp(prefix="v3pack_")
    results = {}
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or max(1, (os.cpu_count() or 4) // 2)) as ex:
            futs = {ex.submit(convert_one, tools, tmp, n, i.filename, zin.read(i), verify): i.filename for n, i in enumerate(tracks)}
            for done, fut in enumerate(concurrent.futures.as_completed(futs), 1):
                name = futs[fut]
                try: results[name] = fut.result()
                except Exception as e: results[name] = {"name": name, "status": "error", "data": None, "log": f"{name}: {e}"}
                if progress: progress(done, len(tracks), name)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    report = [{k: v for k, v in results[i.filename].items() if k != "data"} for i in tracks]
    if any(r["status"] == "error" for r in report): return None, report
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as zout:
        for i in infos:
            data = results[i.filename]["data"] if i.filename in results else zin.read(i)
            zi = zipfile.ZipInfo(i.filename, date_time=i.date_time)
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = i.external_attr
            zout.writestr(zi, data)
    return out.getvalue(), report

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--jobs", type=int)
    ap.add_argument("--no-verify", action="store_true", help="skip the render comparison (not recommended)")
    a = ap.parse_args()
    out_path = a.output or os.path.splitext(a.input)[0] + " (v3).zip"
    with open(a.input, "rb") as f: data = f.read()
    prog = lambda d, t, n: print(f"[{d}/{t}] {n}", file=sys.stderr)
    zbytes, report = convert_pack(data, jobs=a.jobs, verify=not a.no_verify, progress=prog)
    for r in report:
        tag = {"converted": "OK  " if r.get("verified", False) else "conv", "copied": "copy", "error": "FAIL"}[r["status"]]
        print(f"{tag} {r['name']}")
        if r["status"] == "error": print("     " + r["log"].replace("\n", "\n     "))
    if zbytes is None:
        print("pack NOT written (errors above)"); sys.exit(1)
    with open(out_path, "wb") as f: f.write(zbytes)
    print(f"written: {out_path}")

if __name__ == "__main__":
    main()
