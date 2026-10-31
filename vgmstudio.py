#!/usr/bin/env python3
"""Local backend for vgmstudio.html: runs vgm_trim (assembling parts) and vgm2wav (listening).

    python3 vgmstudio.py [--port 8765] [--vgm-trim PATH] [--vgm2wav PATH]

then open http://127.0.0.1:8765/ . Without the server, vgmstudio.html still works as a plain tag editor
(open the file directly); the Assemblage module needs the server.
Requests and answers are JSON; file contents travel as base64. Only loopback connections are accepted.
"""
import argparse, base64, json, os, shutil, subprocess, sys, tempfile
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

    # parts: [{name, data (base64), repeat (int or null)}], output: file name (.vgz / .vgm)
    def api_concat(self):
        req = self.read_json()
        parts = req.get("parts") or []
        if len(parts) < 1: self.send_json({"ok": False, "error": "no part"}, 400); return
        tmp = tempfile.mkdtemp(prefix="vgmstudio_")
        try:
            out = os.path.join(tmp, "out.vgz" if str(req.get("output", "out.vgz")).lower().endswith(".vgz") else "out.vgm")
            cmd = [TOOLS["vgm_trim"], write_part(tmp, 0, parts[0]), out]
            if parts[0].get("repeat") is not None: cmd += ["--repeat", str(int(parts[0]["repeat"]))]
            for i, p in enumerate(parts[1:], 1):
                cmd += ["--append", write_part(tmp, i, p)]
                if p.get("repeat") is not None: cmd += ["--repeat", str(int(p["repeat"]))]
            cmd += ["--color", "never"]
            rc, log = run(cmd, tmp)
            if rc != 0 or not os.path.isfile(out):
                self.send_json({"ok": False, "error": "vgm_trim failed", "log": log}); return
            with open(out, "rb") as f: data = f.read()
            self.send_json({"ok": True, "log": log, "data": base64.b64encode(data).decode(), "name": os.path.basename(out)})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    # {name, data (base64), start (s) | null, end (s) | null, loops (default 1), fade (s, default 0 when a window is given, else 8)}
    def api_render(self):
        req = self.read_json()
        tmp = tempfile.mkdtemp(prefix="vgmstudio_")
        try:
            src = write_part(tmp, 0, req)
            start, end = req.get("start"), req.get("end")
            log = ""
            if start is not None or end is not None:
                cut = os.path.join(tmp, "cut.vgm")
                cmd = [TOOLS["vgm_trim"], src, cut, "--color", "never"]
                if start is not None: cmd += ["--start", "%.4fs" % max(0.0, float(start))]
                if end is not None: cmd += ["--end", "%.4fs" % float(end)]
                rc, log = run(cmd, tmp)
                if rc != 0 or not os.path.isfile(cut): self.send_json({"ok": False, "error": "vgm_trim failed", "log": log}); return
                src = cut
            loops = int(req.get("loops") or 1)
            fade = req.get("fade")
            if fade is None: fade = 0 if (start is not None or end is not None) else 8
            wav = os.path.join(tmp, "out.wav")
            rc, log2 = run([TOOLS["vgm2wav"], "--loops", str(loops), "--fade", str(float(fade)), src, wav], tmp)
            if rc != 0 or not os.path.isfile(wav): self.send_json({"ok": False, "error": "vgm2wav failed", "log": log + log2}); return
            with open(wav, "rb") as f: data = f.read()
            self.send_bytes(data, "audio/wav")
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
