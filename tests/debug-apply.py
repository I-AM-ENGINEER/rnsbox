#!/usr/bin/env python3
"""Debug: full apply transaction against the fake server; print state+log."""
import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_sysupdate as T

T.Handler.repo = T.FakeRepo()
T.IMG_SHA = T.sha(T.IMG_BYTES)
srv = ThreadingHTTPServer(("127.0.0.1", 0), T.Handler)
T.PORT = srv.server_address[1]
T.URL = "http://127.0.0.1:%d" % T.PORT
globals()["PORT"] = T.PORT
globals()["URL"] = T.URL
threading.Thread(target=srv.serve_forever, daemon=True).start()


class Tx(T.SysupdateTest):
    def runTest(self):
        pass


t = Tx()
t.setUp()
with open(t.ver, "w") as f:
    f.write(T.TAG_OLD + "\n")
t.boot_state("a", "0")
T.Handler.repo.files["rnsbox-licheerv-nano.img.xz"] = T.make_ab_image(
    Tx.BOOT_FILES)
t.write_conf(dict(T.DEFAULT_CONF, provider="github-releases",
                  allow_insecure_http="yes"))
t.worker("check", may_fail=True)
print("after check:", t.state()["phase"], t.state().get("error"))
r = t.worker("apply", may_fail=True)
print("apply rc", r.returncode, r.stderr[-300:])
print("after apply:", json.dumps(t.state())[:500])
log = os.path.join(t.statedir, "update.log")
if os.path.exists(log):
    print("log tail:", open(log).read()[-800:])
t.tearDown()
srv.shutdown()
