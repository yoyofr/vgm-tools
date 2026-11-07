#!/usr/bin/env python3
"""Local backend for vgmstudio.html: runs vgm_trim (assembling parts) and vgm2wav (listening).

    python3 vgmstudio.py [--port 8765] [--vgm-trim PATH] [--vgm2wav PATH]

then open http://127.0.0.1:8765/ . Without the server, vgmstudio.html still works as a plain tag editor
(open the file directly); the Assemblage module needs the server.
Requests and answers are JSON; file contents travel as base64. Only loopback connections are accepted.
"""
import argparse, base64, gzip, json, os, re, shutil, subprocess, sys, tempfile, threading, uuid
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import v3pack

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = {}

def find_tool(name, given, candidates):
    if given:
        if not os.path.isfile(given): sys.exit(f"{name}: {given} not found")
        return os.path.abspath(given)
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK): return os.path.abspath(c)
    return shutil.which(name)

def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return p.returncode, p.stdout

def write_part(tmp, i, part):
    name = os.path.basename(part.get("name") or f"part{i}.vgm")
    ext = ".vgz" if name.lower().endswith(".vgz") else ".vgm"
    path = os.path.join(tmp, f"part{i}{ext}")
    with open(path, "wb") as f: f.write(base64.b64decode(part["data"]))
    return path

def vgm_info(path):
    """total and loop (samples) of a .vgm/.vgz file"""
    with open(path, "rb") as f: d = f.read()
    if d[:2] == b"\x1f\x8b": d = gzip.decompress(d)
    le = lambda o: int.from_bytes(d[o:o + 4], "little")
    return {"total": le(0x18), "loop": le(0x20) if le(0x1C) else 0}

SR = 44100

def is_v1_ext(path):
    """True for an ES55xx / ZSG-2 file of the v1 encoding (what the MAME logger writes today; vgm_trim keeps the encoding of its input):
    ES5506 or ZSG-2 clock with a header shorter than 0x120. Those need vgm2wav-v1."""
    with open(path, "rb") as f: d = f.read(0x200)
    if d[:2] == b"\x1f\x8b":
        with gzip.open(path, "rb") as g: d = g.read(0x200)
    le = lambda o: int.from_bytes(d[o:o + 4], "little") if len(d) >= o + 4 else 0
    ver = le(0x08); hdr = 0x34 + le(0x34) if ver >= 0x150 and le(0x34) else 0x40
    return hdr < 0x120 and ((hdr > 0xD3 and le(0xD0)) or (hdr > 0x103 and le(0x100)))

def vgm2wav_for(path):
    return TOOLS["vgm2wav_v1"] if TOOLS.get("vgm2wav_v1") and is_v1_ext(path) else TOOLS["vgm2wav"]

JOBS = {}   # v3 pack conversions: id -> {done, total, current, finished, report, zip, error, name}

def secs_to_samples(v):
    return int(round(float(v) * SR)) if v not in (None, "") and float(v) > 0 else 0

def prepare_part(tmp, i, part, log, fast=False):
    """write the part, unroll its loop (repeat N), cut it (start / end, seconds in the played part), fade it
    (fade_in / fade_out, seconds: mixer gain ramps from the in point / to the out point).
    Returns the path to feed the merge and whether it still loops."""
    src = write_part(tmp, i, part)
    info = vgm_info(src)
    repeat, start, end = part.get("repeat"), part.get("start"), part.get("end")
    fin, fout = secs_to_samples(part.get("fade_in")), secs_to_samples(part.get("fade_out"))
    loops = info["loop"] > 0
    def trim(dst, opts, what):
        rc, out = run([TOOLS["vgm_trim"], src, dst] + opts + ["--color", "never"], tmp)
        log.append(out)
        if rc != 0 or not os.path.isfile(dst):
            err = next((l for l in out.splitlines() if l.startswith("✖")), "")
            raise RuntimeError(f"part {i + 1}: {what} failed" + (": " + err.lstrip("✖ ") if err else ""))
        return dst
    if repeat is not None and loops:
        src, loops = trim(os.path.join(tmp, f"u{i}.vgm"), ["--repeat", str(int(repeat))], "unroll"), False
        info = vgm_info(src)
    total = info["total"]
    s = int(round(float(start) * SR)) if start not in (None, "") else 0
    e = int(round(float(end) * SR)) if end not in (None, "") else total
    s = max(0, min(s, total)); e = max(s + 1, min(e, total))
    loop_at = total - info["loop"] if loops else None
    extra = False
    if loops and e == total and s < loop_at and fin > loop_at - s:
        # the fade-in would reach into the loop body (replayed at each loop): one more pass of the body is
        # written out, the loop moves to the start of that pass (same music, the fade stays in the intro)
        src = trim(os.path.join(tmp, f"u{i}.vgm"), ["--repeat", "2"], "unroll")
        loop_at, total = total, vgm_info(src)["total"]
        e, extra = total, True
    if s == 0 and e == total and not extra:
        out = src
    else:
        cmd = ["--start", str(s), "--end", str(e)] + (["--fast-dsp"] if fast else [])
        if loops and e == total and s < loop_at: cmd += ["--loop", str(loop_at)]   # the part keeps its loop
        else: loops = False
        out = trim(os.path.join(tmp, f"c{i}.vgm"), cmd, "cut")
    if fin or fout:
        if loops and fout: raise RuntimeError(f"part {i + 1}: fondu sortant impossible sur une partie qui garde sa boucle (mets « Boucles » à 0 ou un nombre de passages)")
        src = out
        out = trim(os.path.join(tmp, f"f{i}.vgm"), (["--fade-in", str(fin)] if fin else []) + (["--fade-out", str(fout)] if fout else []), "fade")
    return out, loops

class Handler(BaseHTTPRequestHandler):
    server_version = "vgmstudio/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def send_bytes(self, data, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def read_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n).decode())

    def do_GET(self):
        if self.path.split("?")[0] in ("/", "/index.html", "/vgmstudio.html"):
            try:
                with open(os.path.join(HERE, "vgmstudio.html"), "rb") as f: data = f.read()
            except OSError:
                self.send_error(404, "vgmstudio.html not found next to vgmstudio.py"); return
            self.send_bytes(data, "text/html; charset=utf-8")
        elif self.path == "/favicon.ico":
            self.send_response(204); self.end_headers()
        elif self.path == "/api/ping":
            self.send_json({"ok": True, "tools": TOOLS})
        elif self.path.startswith("/api/v3pack/status"):
            job = JOBS.get(self.path.partition("id=")[2])
            if not job: self.send_json({"ok": False, "error": "unknown job"}, 404); return
            self.send_json({"ok": True, **{k: v for k, v in job.items() if k != "zip"}, "ready": job["zip"] is not None})
        elif self.path.startswith("/api/v3pack/result"):
            job = JOBS.get(self.path.partition("id=")[2])
            if not job or job["zip"] is None: self.send_error(404); return
            self.send_bytes(job["zip"], "application/zip")
        else:
            self.send_error(404)

    def do_POST(self):
        try:
            if self.path == "/api/concat": self.api_concat()
            elif self.path == "/api/junction": self.api_junction()
            elif self.path == "/api/render": self.api_render()
            elif self.path == "/api/optimize": self.api_optimize()
            elif self.path.startswith("/api/v3pack/start"): self.api_v3pack_start()
            else: self.send_error(404)
        except Exception as e:   # report, never crash the server
            self.send_json({"ok": False, "error": f"{type(e).__name__}: {e}"}, 500)

    # parts: [{name, data (base64), repeat (int|null), start (s|null), end (s|null), fade_in / fade_out (s|null)}], output: file name
    # each part is unrolled / cut first, then the parts are merged (seamless junctions), then the used
    # sample ROM ranges of the whole result are recomputed (--sro)
    def api_concat(self):
        req = self.read_json()
        parts = req.get("parts") or []
        if len(parts) < 1: self.send_json({"ok": False, "error": "no part"}, 400); return
        tmp = tempfile.mkdtemp(prefix="vgmstudio_")
        log = []
        try:
            prepared = [prepare_part(tmp, i, p, log) for i, p in enumerate(parts)]
            merged = os.path.join(tmp, "merged.vgm")
            cmd = [TOOLS["vgm_trim"], prepared[0][0], merged]
            for path, _ in prepared[1:]: cmd += ["--append", path]
            cmd += ["--color", "never"]
            rc, out = run(cmd, tmp)
            log.append(out)
            if rc != 0 or not os.path.isfile(merged): self.send_json({"ok": False, "error": "vgm_trim (merge) failed", "log": "\n".join(log)}); return
            # junction times from the merge table ("│    2 │ 3:01.197 (7990778) │")
            junctions = [int(m.group(1)) for m in re.finditer(r"│\s+\d+\s+│\s+[\d:.]+\s+\((\d+)\)", out)][1:]
            out_name = "out.vgz" if str(req.get("output", "out.vgz")).lower().endswith(".vgz") else "out.vgm"
            final = os.path.join(tmp, out_name)
            info = vgm_info(merged)
            cmd = [TOOLS["vgm_trim"], merged, final, "--sro", "--color", "never"]
            if info["loop"]: cmd += ["--loop", str(info["total"] - info["loop"])]
            rc, out = run(cmd, tmp)
            log.append(out)
            if rc != 0 or not os.path.isfile(final): self.send_json({"ok": False, "error": "vgm_trim (sro) failed", "log": "\n".join(log)}); return
            with open(final, "rb") as f: data = f.read()
            self.send_json({"ok": True, "log": "\n".join(log), "data": base64.b64encode(data).decode(), "name": out_name, "junctions": junctions})
        except RuntimeError as e:
            self.send_json({"ok": False, "error": str(e), "log": "\n".join(log)})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # junction preview without assembling everything: the last `window` seconds of part A (before its out
    # point) and the first `window` seconds of part B (after its in point), merged like the full assembly
    # {a: part, b: part, window: s}, parts as in /api/concat -> audio/wav (the junction is at `window` s,
    # or earlier when part A is shorter: header X-Junction gives it in seconds)
    def api_junction(self):
        req = self.read_json()
        w = float(req.get("window") or 5)
        tmp = tempfile.mkdtemp(prefix="vgmstudio_")
        log = []
        try:
            A, B = dict(req["a"]), dict(req["b"])
            # A: unrolled first (its played length is known then), then only its tail is cut
            pa, _ = prepare_part(tmp, 0, {**A, "start": None, "end": None, "fade_in": None, "fade_out": None}, log)
            ta = vgm_info(pa)["total"] / SR
            a1 = ta if A.get("end") in (None, "") else min(float(A["end"]), ta)
            a0 = max(float(A.get("start") or 0), a1 - w)
            pa2 = os.path.join(tmp, "a_tail.vgm")
            fo = secs_to_samples(A.get("fade_out"))   # relative to the out point: a fade longer than the window starts before it
            rc, out = run([TOOLS["vgm_trim"], pa, pa2, "--start", str(int(round(a0 * SR))), "--end", str(int(round(a1 * SR))), "--color", "never", "--fast-dsp"]
                          + (["--fade-out", str(fo)] if fo else []), tmp)
            log.append(out)
            if rc != 0 or not os.path.isfile(pa2): raise RuntimeError("part A: cut failed")
            ja = vgm_info(pa2)["total"] / SR
            b0 = float(B.get("start") or 0)
            b1 = b0 + w if B.get("end") in (None, "") else min(float(B["end"]), b0 + w)
            pb, _ = prepare_part(tmp, 1, {**B, "end": b1, "fade_out": None}, log, fast=True)   # previews: approximate DSP image (--fast-dsp)
            merged = os.path.join(tmp, "j.vgm")
            rc, out = run([TOOLS["vgm_trim"], pa2, merged, "--append", pb, "--color", "never"], tmp)
            log.append(out)
            if rc != 0 or not os.path.isfile(merged): self.send_json({"ok": False, "error": "vgm_trim (merge) failed", "log": "\n".join(log)}); return
            wav = os.path.join(tmp, "j.wav")
            rc, out = run([vgm2wav_for(merged), "--loops", "1", "--fade", "0", merged, wav], tmp)
            if rc != 0 or not os.path.isfile(wav): self.send_json({"ok": False, "error": "vgm2wav failed", "log": "\n".join(log) + out}); return
            with open(wav, "rb") as f: data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Junction", "%.6f" % ja)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)
        except RuntimeError as e:
            self.send_json({"ok": False, "error": str(e), "log": "\n".join(log)})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # {name, data (base64), repeat (int|null), cut_start / cut_end (s|null: the part as assembled),
    #  start / end (s|null: listening window inside that), loops (default 1), fade (s)}
    def api_render(self):
        req = self.read_json()
        tmp = tempfile.mkdtemp(prefix="vgmstudio_")
        log = []
        try:
            src, _ = prepare_part(tmp, 0, {"name": req.get("name"), "data": req["data"], "repeat": req.get("repeat"),
                                           "start": req.get("cut_start"), "end": req.get("cut_end")}, log)
            start, end = req.get("start"), req.get("end")
            if start is not None or end is not None:
                cut = os.path.join(tmp, "win.vgm")
                cmd = [TOOLS["vgm_trim"], src, cut, "--color", "never"]
                if start is not None: cmd += ["--start", "%.4fs" % max(0.0, float(start))]
                if end is not None: cmd += ["--end", "%.4fs" % float(end)]
                rc, out = run(cmd, tmp)
                log.append(out)
                if rc != 0 or not os.path.isfile(cut): self.send_json({"ok": False, "error": "vgm_trim failed", "log": "\n".join(log)}); return
                src = cut
            loops = int(req.get("loops") or 1)
            fade = req.get("fade")
            if fade is None: fade = 0 if (start is not None or end is not None or vgm_info(src)["loop"] == 0) else 8
            wav = os.path.join(tmp, "out.wav")
            rc, out = run([vgm2wav_for(src), "--loops", str(loops), "--fade", str(float(fade)), src, wav], tmp)
            if rc != 0 or not os.path.isfile(wav): self.send_json({"ok": False, "error": "vgm2wav failed", "log": "\n".join(log) + out}); return
            with open(wav, "rb") as f: data = f.read()
            self.send_bytes(data, "audio/wav")
        except RuntimeError as e:
            self.send_json({"ok": False, "error": str(e), "log": "\n".join(log)})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # {name, data (base64), cmp, sro, lead (bools), verify (bool)} -> {ok, data (base64, same compression as the
    # input), before / after sizes, cut (samples removed at the start), exact, diff, sro_dropped, log}
    # Step 1, --cmp / --sro: the result must render byte-identical (2 loops). --sro keeps the sample ranges the
    # log shows played; if the render shows one missing (a loop's second pass can read elsewhere than the
    # first), step 1 is done again without --sro; still different: refused.
    # Step 2, --trim-lead on the step 1 result: compared with it from the cut point. Identical unless an effect
    # DSP ran during the silence (its state at the first note then differs): exact = false and the measured
    # difference, the page decides.
    def api_optimize(self):
        req = self.read_json()
        tmp = tempfile.mkdtemp(prefix="vgmstudio_")
        log = []
        verify = req.get("verify", True)
        try:
            src = write_part(tmp, 0, req)
            ext = ".vgz" if open(src, "rb").read(2) == b"\x1f\x8b" else ".vgm"
            def trim(inp, name, opts):
                o = os.path.join(tmp, name + ext)
                rc, txt = run([TOOLS["vgm_trim"], inp, o, "--color", "never"] + opts, tmp)
                log.append(txt)
                if rc != 0 or not os.path.isfile(o): raise RuntimeError("vgm_trim failed")
                return o
            cur, res, sro_dropped = src, None, False
            o1 = [x for x, k in (("--cmp", "cmp"), ("--sro", "sro")) if req.get(k)]
            if o1:
                out = trim(src, "step1", o1)
                res, refused = self.optimize_check(verify, src, out)
                if refused and "--sro" in o1:
                    log.append("render differs with --sro: step 1 done again without it")
                    o1.remove("--sro"); sro_dropped = True
                    out = trim(src, "step1b", o1) if o1 else src
                    res, refused = self.optimize_check(verify, src, out) if o1 else (None, None)
                if refused: self.send_json({"ok": False, **refused, "log": "\n".join(log)}); return
                cur = out
            if req.get("lead"):
                out = trim(cur, "step2", ["--trim-lead"] + (["--sro"] if "--sro" in o1 else []))
                r2, _ = self.optimize_check(verify, cur, out, allow_cut_diff=True)
                if r2 is not None and r2["cut"] > 0:
                    cur, res = out, r2
            if res is None: res, _ = self.optimize_check(False, src, cur)
            res["before"] = os.path.getsize(src)
            res["after"] = os.path.getsize(cur)
            res["cut"] = vgm_info(src)["total"] - vgm_info(cur)["total"]
            res["sro_dropped"] = sro_dropped
            with open(cur, "rb") as f: res["data"] = base64.b64encode(f.read()).decode()
            res["log"] = "\n".join(log)
            self.send_json(res)
        except RuntimeError as e:
            self.send_json({"ok": False, "error": str(e), "log": "\n".join(log)})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # verify an optimised file against its source: returns (result, None), or (None, refusal) when the render
    # differs without a leading cut (only removing the leading silence may change it: an effect DSP ran then)
    @staticmethod
    def optimize_check(verify, src, out, allow_cut_diff=False):
        import math, wave
        tmp = os.path.dirname(out)
        a, b = vgm_info(src), vgm_info(out)
        res = {"ok": True, "before": os.path.getsize(src), "after": os.path.getsize(out), "cut": a["total"] - b["total"],
               "loop_before": a["loop"], "loop_after": b["loop"], "exact": None, "diff": None}
        if a["loop"] != b["loop"]: return None, {"error": "boucle modifiée"}
        if not verify: return res, None
        def render(p, w):
            rc, o = run([vgm2wav_for(p), "--loops", "2", "--fade", "0", p, w], tmp)
            if rc != 0 or not os.path.isfile(w): raise RuntimeError("vgm2wav failed: " + o[-300:])
            with wave.open(w) as f: return f.readframes(f.getnframes()), f.getnchannels()
        ra, ch = render(src, os.path.join(tmp, "a.wav"))
        rb, _ = render(out, os.path.join(tmp, "b.wav"))
        ra = ra[res["cut"] * 2 * ch:]
        res["exact"] = ra == rb
        if res["exact"]: return res, None
        import array
        n = min(len(ra), len(rb)) // 2
        xa, xb = array.array("h", ra[:n * 2]), array.array("h", rb[:n * 2])
        try:
            import numpy as np
            va, vb = np.frombuffer(xa, np.int16).astype(np.float64), np.frombuffer(xb, np.int16).astype(np.float64)
            d = va - vb
            peak, rms, sig = (float(np.abs(d).max()), float(np.sqrt((d * d).mean())), float(np.sqrt((va * va).mean()))) if n else (0, 0, 0)
        except ImportError:
            peak = max(abs(x - y) for x, y in zip(xa, xb)) if n else 0
            rms = math.sqrt(sum((x - y) ** 2 for x, y in zip(xa, xb)) / n) if n else 0
            sig = math.sqrt(sum(x * x for x in xa) / n) if n else 0
        db = lambda v: round(20 * math.log10(v / 32768), 1) if v > 0 else None
        res["diff"] = {"peak": db(peak), "rms": db(rms), "signal": db(sig), "length_ok": len(ra) == len(rb)}
        if not (allow_cut_diff and res["cut"] > 0): return None, {"error": "rendu différent après optimisation (refusé)", "diff": res["diff"]}
        return res, None

    # body = the pack zip (application/zip), ?name=<file name>&verify=0|1 -> {id}; then poll
    # GET /api/v3pack/status?id= and fetch GET /api/v3pack/result?id= (the converted zip)
    def api_v3pack_start(self):
        from urllib.parse import urlparse, parse_qs
        q = parse_qs(urlparse(self.path).query)
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(n)
        jid = uuid.uuid4().hex
        job = {"done": 0, "total": 0, "current": "", "finished": False, "report": [], "zip": None, "error": None,
               "name": (q.get("name") or ["pack.zip"])[0]}
        JOBS[jid] = job
        verify = (q.get("verify") or ["1"])[0] != "0"
        tools = {"vgm_v3conv": TOOLS["vgm_v3conv"], "vgm2wav_v1": TOOLS["vgm2wav_v1"], "vgm2wav": TOOLS["vgm2wav"]}
        def work():
            def prog(d, t, name): job.update(done=d, total=t, current=name)
            try:
                z, report = v3pack.convert_pack(data, tools=tools, verify=verify, progress=prog)
                job.update(report=report, zip=z)
                if z is None: job["error"] = "conversion refusée : au moins une piste en erreur"
            except Exception as e:
                job["error"] = f"{type(e).__name__}: {e}"
            job["finished"] = True
        threading.Thread(target=work, daemon=True).start()
        self.send_json({"ok": True, "id": jid})

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--vgm-trim", help="path of vgm_trim (default: next to this script, then PATH)")
    ap.add_argument("--vgm2wav", help="path of vgm2wav, v3 player (default: ../libvgm/build/bin/vgm2wav, then PATH)")
    ap.add_argument("--vgm2wav-v1", help="path of the v1 player (default: bin/vgm2wav-v1 next to this script)")
    ap.add_argument("--vgm-v3conv", help="path of vgm_v3conv (default: next to this script)")
    a = ap.parse_args()
    TOOLS["vgm_trim"] = find_tool("vgm_trim", a.vgm_trim, [os.path.join(HERE, "vgm_trim")])
    TOOLS["vgm2wav"] = find_tool("vgm2wav", a.vgm2wav, [os.path.join(HERE, "..", "libvgm", "build", "bin", "vgm2wav"), os.path.join(HERE, "vgm2wav")])
    TOOLS["vgm2wav_v1"] = find_tool("vgm2wav-v1", a.vgm2wav_v1, [os.path.join(HERE, "bin", "vgm2wav-v1")])
    TOOLS["vgm_v3conv"] = find_tool("vgm_v3conv", a.vgm_v3conv, [os.path.join(HERE, "vgm_v3conv")])
    for k, v in TOOLS.items():
        if not v: sys.exit(f"{k} not found: pass --{k.replace('_', '-')} PATH")
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"vgmstudio: http://127.0.0.1:{a.port}/  (vgm_trim: {TOOLS['vgm_trim']}, vgm2wav: {TOOLS['vgm2wav']})")
    try: srv.serve_forever()
    except KeyboardInterrupt: pass

if __name__ == "__main__":
    main()
