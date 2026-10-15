#!/usr/bin/env python3
"""Full apply trace with hang dump."""
import json
import os
import sys
import threading
import faulthandler
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
print("prep...", flush=True)
t.prep_apply()
faulthandler.dump_traceback_later(30, exit=True)
print("apply...", flush=True)
r = t.worker("apply", may_fail=True)
print("rc", r.returncode, flush=True)
print("state:", json.dumps(t.state())[:300], flush=True)
t.tearDown()
srv.shutdown()
