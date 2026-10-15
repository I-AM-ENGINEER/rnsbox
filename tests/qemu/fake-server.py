#!/usr/bin/env python3
"""Fake GitHub Releases + image server for the QEMU acceptance run.

Serves on 0.0.0.0:<port> so the QEMU guest reaches it via user-net
(10.0.2.2). The payload is the same make_ab_image fixture the unit tests
use (MBR + FAT p1 with a CIMG-wrapped boot.sd + p2/p3/p4).
"""
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
import test_sysupdate as T   # reuse make_ab_image/BOOT_FILES/IMG fixtures

TAG = "r2026.10.15-deadbe7"
PAYLOAD = {}


def build_payload():
    img_xz = T.make_ab_image(T.SysupdateTest.BOOT_FILES)
    PAYLOAD["img"] = img_xz
    PAYLOAD["sha"] = T.sha(img_xz)
    PAYLOAD["releases"] = [{
        "tag_name": TAG, "prerelease": False, "draft": False,
        "body": ("| Board | Image | sha256 |\n|---|---|---|\n"
                 "| licheerv-nano | `rnsbox-licheerv-nano.img.xz` | `%s` |"
                 % PAYLOAD["sha"]),
        "assets": [{"name": "rnsbox-licheerv-nano.img.xz",
                    "browser_download_url":
                        "http://10.0.2.2:%s/dl/rnsbox-licheerv-nano.img.xz"
                        % PORT}],
    }]


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/repos/"):
            return self._send(200, json.dumps(PAYLOAD["releases"]).encode(),
                              "application/json")
        if self.path.startswith("/dl/"):
            return self._send(200, PAYLOAD["img"], "application/x-xz")
        self._send(404, b"nope", "text/plain")


srv = ThreadingHTTPServer(("0.0.0.0", int(sys.argv[1])), H)
PORT = str(srv.server_address[1])
build_payload()
print("FAKE SERVER READY port=%s tag=%s sha=%s"
      % (PORT, TAG, PAYLOAD["sha"]), flush=True)
threading.Thread(target=srv.serve_forever, daemon=True).start()
try:
    threading.Event().wait()
except KeyboardInterrupt:
    pass
