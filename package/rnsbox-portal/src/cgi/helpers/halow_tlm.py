#!/usr/bin/env python3
"""halow_tlm.py — collect MQTT telemetry from HaLow nodes into /run/halow-tlm.

Standalone helper (stdlib only). MQTT itself is delegated to mosquitto_sub:
this script keeps it running against the local broker (S50mosquitto) and
writes each message as /run/halow-tlm/<node-key>.json (atomic, tmpfs). The
portal's halow.py reads the directory for the HaLow card. The node's payload
is the firmware's telemetry JSON (see RNode_Halow_Firmware src/telemetry.c):
id, name, fw, loc, freq, bw, [direction], uptime, mcs, lxmf [, tx_b rx_b tx_p rx_p].

Never exits: the broker may be down or restarting, so mosquitto_sub is
restarted in a loop with a short sleep. Run under S87halowtlm (FIFO -> logger).
"""
import json
import os
import subprocess
import time

BROKER = "127.0.0.1"
TOPIC = "rnode-halow/telemetry"
TLM_DIR = "/run/halow-tlm"


def key_for(d):
    n = (d.get("name") or "").strip()
    k = "".join(c for c in n if c.isalnum() or c in "-_")[:32]
    if not k:
        try:
            k = "id-%08x" % int(d.get("id") or 0)
        except (TypeError, ValueError):
            k = "node"
    return k


def store(payload):
    try:
        d = json.loads(payload)
    except Exception:
        return
    if not isinstance(d, dict) or "id" not in d:
        return
    d["_seen"] = int(time.time())
    os.makedirs(TLM_DIR, exist_ok=True)
    tmp = os.path.join(TLM_DIR, ".tmp")
    try:
        with open(tmp, "w") as f:
            json.dump(d, f)
        os.replace(tmp, os.path.join(TLM_DIR, key_for(d) + ".json"))
    except OSError:
        pass


def main():
    while True:
        p = None
        try:
            p = subprocess.Popen(["/usr/bin/mosquitto_sub", "-h", BROKER,
                                  "-t", TOPIC, "-v"],
                                 stdout=subprocess.PIPE)
            for line in p.stdout:
                parts = line.decode("utf-8", "replace").split(" ", 1)
                if len(parts) == 2:
                    store(parts[1].rstrip("\n"))
        except Exception:
            pass
        finally:
            if p is not None:
                try:
                    p.kill()
                except OSError:
                    pass
        time.sleep(5)


if __name__ == "__main__":
    main()
