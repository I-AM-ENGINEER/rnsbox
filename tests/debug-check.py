#!/usr/bin/env python3
"""Debug: run one check against the fake server and print the state error."""
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
srv = ThreadingHTTPServer(("127.0.0.1", 0), T.Handler)
T.PORT = srv.server_address[1]
T.URL = "http://127.0.0.1:%d" % T.PORT
globals()["PORT"] = T.PORT
globals()["URL"] = T.URL
threading.Thread(target=srv.serve_forever, daemon=True).start()

tmp = tempfile.mkdtemp(prefix="dbg.")
conf = os.path.join(tmp, "update.conf")
with open(conf, "w") as f:
    f.write("provider=github-releases\nrepo=example/rnsbox\nboard=licheerv-nano\n"
            "allow_insecure_http=yes\n")
devmap = os.path.join(tmp, "devmap.json")
sa = os.path.join(tmp, "sa"); sb = os.path.join(tmp, "sb")
for p in (sa, sb):
    open(p, "wb").write(b"\x00" * (512 * 1024))
with open(devmap, "w") as f:
    json.dump({"boot_dir": tmp, "boot_dev": "/dev/mmcblk0p1",
               "slot_a": sa, "slot_b": sb}, f)

env = dict(os.environ)
env.update({
    "RNSBOX_SYSUPDATE_CONF": conf,
    "RNSBOX_SYSUPDATE_RUN": os.path.join(tmp, "run.json"),
    "RNSBOX_SYSUPDATE_STATEDIR": os.path.join(tmp, "state"),
    "RNSBOX_SYSUPDATE_VER": os.path.join(tmp, "ver"),
    "RNSBOX_SYSUPDATE_LOCK": os.path.join(tmp, "lock"),
    "RNSBOX_SYSUPDATE_BOOT": tmp,
    "RNSBOX_UPDATE_DEVMAP": devmap,
    "RNSBOX_SYSUPDATE_FOREGROUND": "1",
    "RNSBOX_SYSUPDATE_NO_REBOOT": "1",
    "RNSBOX_UPDATE_GHAPI": "http://127.0.0.1:%d/api" % T.PORT,
})
r = subprocess.run([sys.executable, T.WORKER, "check"],
                   capture_output=True, text=True, env=env)
print("rc", r.returncode, r.stderr[-300:])
print(open(os.path.join(tmp, "run.json")).read())
srv.shutdown()
