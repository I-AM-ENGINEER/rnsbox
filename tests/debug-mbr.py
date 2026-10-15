#!/usr/bin/env python3
"""Trace the apply path step by step to find the hang."""
import json
import os
import sys
import threading
import faulthandler
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_sysupdate as T

faulthandler.dump_traceback_later(25, exit=True)

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
print("step: prep_apply", flush=True)
img_xz = t.prep_apply()
print("step: parsed mbr via worker module", flush=True)
mod = T.load_worker_module()
dl = os.path.join(t.statedir, "download.img.xz")
print("step: parse_mbr", flush=True)
parts = mod.parse_mbr(dl)
print("parts:", parts, flush=True)
print("step: read_range_from_xz", flush=True)
d = mod.read_range_from_xz(dl, parts[0][0], min(parts[0][1], 4096))
print("fat head:", d[:16].hex(), flush=True)
print("step: write_idle_slot", flush=True)
st = mod.State().load()
mod.write_idle_slot(dl, t.slot_b, parts[2][0], parts[2][1], st)
print("slot written OK", flush=True)
t.tearDown()
srv.shutdown()
