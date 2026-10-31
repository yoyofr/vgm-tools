#!/usr/bin/env python3
"""Local backend for vgmstudio.html: runs vgm_trim (assembling parts) and vgm2wav (listening).

    python3 vgmstudio.py [--port 8765] [--vgm-trim PATH] [--vgm2wav PATH]

then open http://127.0.0.1:8765/ . Without the server, vgmstudio.html still works as a plain tag editor
(open the file directly); the Assemblage module needs the server.
Requests and answers are JSON; file contents travel as base64. Only loopback connections are accepted.
"""
import argparse, base64, gzip, json, os, re, shutil, subprocess, sys, tempfile
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

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

def prepare_part(tmp, i, part, log):
    """write the part, unroll its loop (repeat N), cut it (start / end, seconds in the played part).
    Returns the path to feed the merge and whether it still loops."""
    src = write_part(tmp, i, part)
    info = vgm_info(src)
    repeat, start, end = part.get("repeat"), part.get("start"), part.get("end")
    loops = info["loop"] > 0
    if repeat is not None and loops:
        u = os.path.join(tmp, f"u{i}.vgm")
        rc, out = run([TOOLS["vgm_trim"], src, u, "--repeat", str(int(repeat)), "--color", "never"], tmp)
        log.append(out)
        if rc != 0 or not os.path.isfile(u): raise RuntimeError(f"part {i + 1}: unroll failed")
        src, loops = u, False
        info = vgm_info(src)
    total = info["total"]
    s = int(round(float(start) * SR)) if start not in (None, "") else 0
    e = int(round(float(end) * SR)) if end not in (None, "") else total
    s = max(0, min(s, total)); e = max(s + 1, min(e, total))
    if s == 0 and e == total: return src, loops
    c = os.path.join(tmp, f"c{i}.vgm")
    cmd = [TOOLS["vgm_trim"], src, c, "--start", str(s), "--end", str(e), "--color", "never"]
    loop_at = total - info["loop"] if loops else None
    if loops and e == total and s < loop_at: cmd += ["--loop", str(loop_at)]   # the part keeps its loop
    else: loops = False
    rc, out = run(cmd, tmp)
    log.append(out)
    if rc != 0 or not os.path.isfile(c): raise RuntimeError(f"part {i + 1}: cut failed")
    return c, loops

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
        else:
            self.send_error(404)

    def do_POST(self):
        try:
            if self.path == "/api/concat": self.api_concat()
            elif self.path == "/api/render": self.api_render()
            else: self.send_error(404)
        except Exception as e:   # report, never crash the server
            self.send_json({"ok": False, "error": f"{type(e).__name__}: {e}"}, 500)

    # parts: [{name, data (base64), repeat (int|null), start (s|null), end (s|null)}], output: file name
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
            rc, out = run([TOOLS["vgm2wav"], "--loops", str(loops), "--fade", str(float(fade)), src, wav], tmp)
            if rc != 0 or not os.path.isfile(wav): self.send_json({"ok": False, "error": "vgm2wav failed", "log": "\n".join(log) + out}); return
            with open(wav, "rb") as f: data = f.read()
            self.send_bytes(data, "audio/wav")
        except RuntimeError as e:
            self.send_json({"ok": False, "error": str(e), "log": "\n".join(log)})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--vgm-trim", help="path of vgm_trim (default: next to this script, then PATH)")
    ap.add_argument("--vgm2wav", help="path of vgm2wav (default: ../libvgm/build/bin/vgm2wav, then PATH)")
    a = ap.parse_args()
    TOOLS["vgm_trim"] = find_tool("vgm_trim", a.vgm_trim, [os.path.join(HERE, "vgm_trim")])
    TOOLS["vgm2wav"] = find_tool("vgm2wav", a.vgm2wav, [os.path.join(HERE, "..", "libvgm", "build", "bin", "vgm2wav"), os.path.join(HERE, "vgm2wav")])
    for k, v in TOOLS.items():
        if not v: sys.exit(f"{k} not found: pass --{k.replace('_', '-')} PATH")
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"vgmstudio: http://127.0.0.1:{a.port}/  (vgm_trim: {TOOLS['vgm_trim']}, vgm2wav: {TOOLS['vgm2wav']})")
    try: srv.serve_forever()
    except KeyboardInterrupt: pass

if __name__ == "__main__":
    main()
