#!/usr/bin/env python3
"""
serve.py — PCem-web development/deployment server (stdlib only).

* Serves ./web with correct MIME types (.wasm => application/wasm).
* Sends COOP/COEP headers on every response — the pthreads (SharedArrayBuffer)
  build requires cross-origin isolation.
* /roms/** is the machine ROM directory, laid out exactly like a desktop
  PCem roms/ folder. By default that's ./web/roms; use --roms to serve an
  existing roms directory from anywhere on disk (e.g. a copy of your desktop
  install's roms folder) without moving it.

Usage:  python3 serve.py [--port 8000] [--bind 127.0.0.1] [--roms /path/to/roms]
"""
import argparse
import os
import posixpath
import urllib.parse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
ROMS_DIR = os.path.join(ROOT, "roms")


class PCemHandler(SimpleHTTPRequestHandler):
    extensions_map = {
        **SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".json": "application/json",
        ".rom": "application/octet-stream",
        ".bin": "application/octet-stream",
    }

    def end_headers(self):
        # Cross-origin isolation for SharedArrayBuffer / pthreads.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Accept-Ranges", "bytes")
        if self.path.startswith("/roms/"):
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def do_GET(self):
        # Single-range support (hosted disk/CD images are read lazily with
        # Range requests; production servers do this natively).
        rng = self.headers.get("Range")
        if not rng or not rng.startswith("bytes="):
            return super().do_GET()
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().do_GET()
        size = os.path.getsize(path)
        spec = rng[len("bytes="):].split(",")[0].strip()
        try:
            if spec.startswith("-"):           # suffix: last N bytes
                n = int(spec[1:])
                start, end = max(0, size - n), size - 1
            else:
                a, _, b = spec.partition("-")
                start = int(a)
                end = int(b) if b else size - 1
        except ValueError:
            return super().do_GET()
        if start >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return
        end = min(end, size - 1)
        length = end - start + 1
        ctype = self.guess_type(path)
        self.send_response(206)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(length))
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(1024 * 256, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    break
                remaining -= len(chunk)

    def translate_path(self, path):
        # Map /roms/** onto the configured roms directory (which may live
        # outside the web root).
        clean = urllib.parse.urlparse(path).path
        clean = urllib.parse.unquote(clean)
        if clean == "/roms" or clean.startswith("/roms/"):
            rel = posixpath.normpath(clean[len("/roms"):]).lstrip("/")
            if rel.startswith(".."):
                return ROMS_DIR  # traversal attempt -> just the dir (404s)
            return os.path.join(ROMS_DIR, *rel.split("/")) if rel else ROMS_DIR
        return super().translate_path(path)

    def log_message(self, fmt, *args):
        msg = fmt % args
        # the on-demand ROM prober generates plenty of expected 404s; keep the
        # log readable by skipping them unless VERBOSE is set
        if " 404 " in msg and "/roms/" in msg and not os.environ.get("VERBOSE"):
            return
        print(f"[serve] {self.address_string()} {msg}")


def main():
    global ROMS_DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--roms", default=ROMS_DIR,
                    help="directory served as /roms (desktop-PCem roms/ layout)")
    args = ap.parse_args()

    ROMS_DIR = os.path.abspath(args.roms)
    os.makedirs(ROMS_DIR, exist_ok=True)

    handler = partial(PCemHandler, directory=ROOT)
    httpd = ThreadingHTTPServer((args.bind, args.port), handler)
    print(f"PCem-web serving {ROOT}")
    print(f"  -> http://{args.bind}:{args.port}/  (COOP/COEP enabled)")
    print(f"  -> /roms maps to {ROMS_DIR}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
