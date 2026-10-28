#!/usr/bin/env python3
"""Serve one local MP4 and run the HTTP playback check against it."""

import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

root = Path(__file__).resolve().parents[1]
media = root / "testdata" / "media" / "portrait-360p-h264-0.mp4"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        payload = self.server.payload
        self.send_response(200)
        if self.server.allow_range:
            self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        return


def main(argv):
    if len(argv) != 2 or argv[1] not in ("range", "plain"):
        print("usage: http_playback.py <legato_http_playback> range|plain", file=sys.stderr)
        return 2
    if not media.is_file():
        print("FAIL http_playback: missing %s" % media, file=sys.stderr)
        return 1
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.payload = media.read_bytes()
    server.allow_range = argv[1] == "range"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = "http://localhost:%d/clip.mp4" % server.server_address[1]
    try:
        return subprocess.call([argv[0], url, argv[1]])
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
