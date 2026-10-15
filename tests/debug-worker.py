#!/usr/bin/env python3
"""Run the real worker apply with a hang dump (env prepared like tests)."""
import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer

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
t.prep_apply()
print("prepared; running worker apply with 20s hang dump", flush=True)
r = subprocess.run(
    [sys.executable, "-c",
     "import faulthandler, runpy, sys; "
     "faulthandler.dump_traceback_later(20, exit=True); "
     "sys.argv=['rnsbox-sysupdate', 'apply']; "
     "runpy.run_path(sys.argv[0] if False else %r, run_name='__main__')"
     % T.WORKER],
    capture_output=True, text=True, env=t.env())
print("rc", r.returncode, flush=True)
print("stderr tail:", r.stderr[-2500:], flush=True)
print("state:", open(t.runst).read()[:400], flush=True)
t.tearDown()
srv.shutdown()
