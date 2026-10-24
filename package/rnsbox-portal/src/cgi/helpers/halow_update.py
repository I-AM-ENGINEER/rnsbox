#!/usr/bin/env python3
"""halow_update.py — scheduled automatic updates for the HaLow modem.

Policy (deliberately conservative):
  * only STABLE releases (a prerelease is never selected);
  * as soon as a newer stable release is known, the update is scheduled at
    published_at + 3 days + a random 0..7 days;
  * nothing is downloaded before the scheduled moment;
  * at that moment the box downloads the asset and runs the normal HTTP OTA
    worker (over SLIP).

The schedule is stored in /var/lib/rnsbox/halow-update.json (flash), so a
reboot does not lose it. The web UI reads `status`; `tick` runs from cron.
"""
from __future__ import annotations

import datetime
import json
import os
import random
import re
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, "/usr/lib/rnsbox")
import halow_fw  # noqa: E402

CONF = "/etc/rnsbox/halow_update.conf"
STATE = "/var/lib/rnsbox/halow-update.json"
SLIP = "/etc/rnsbox/slip.conf"
OTA_LOCK = "/run/rnsbox-halow-ota.lock"
OTA_IMAGE = "/run/rnsbox-halow-ota.fw"

SOAK = 3 * 86400
JITTER = 7 * 86400


def now():
    return int(time.time())


def read_conf():
    conf = {"auto": "no"}
    try:
        for line in open(CONF):
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def read_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_state(st):
    try:
        os.makedirs(os.path.dirname(STATE), mode=0o700, exist_ok=True)
        tmp = STATE + ".tmp.%d" % os.getpid()
        with open(tmp, "w") as f:
            json.dump(st, f)
        os.replace(tmp, STATE)
    except OSError as exc:
        print("cannot write state: %s" % exc, file=sys.stderr)


def ver(text):
    m = re.search(r"(?<![0-9.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![0-9.])", text or "")
    return tuple(int(x) for x in m.groups()) if m else None


def slip_peer():
    peer = "192.168.7.2"
    enabled = False
    try:
        for line in open(SLIP):
            line = line.strip()
            if line.startswith("enabled="):
                enabled = line.split("=", 1)[1].strip() in ("yes", "1", "true", "on")
            elif line.startswith("peer_ip="):
                peer = line.split("=", 1)[1].strip() or peer
    except OSError:
        pass
    return peer if enabled else None


def installed_version(peer):
    try:
        req = urllib.request.Request("http://%s/api/get_stat" % peer,
                                     headers={"Accept-Encoding": "identity"})
        with urllib.request.urlopen(req, timeout=3) as r:
            data = json.loads(r.read().decode("utf-8", "replace"))
        return (data.get("device") or {}).get("ver", "")
    except Exception:
        return ""


def stable_releases():
    out = []
    for rel in halow_fw.releases():
        if rel.get("prerelease"):
            continue
        if not re.match(r"^v?\d+\.\d+\.\d+$", rel.get("tag", "")):
            continue
        v = ver(rel["tag"])
        if v:
            out.append((v, rel))
    out.sort(key=lambda x: x[0], reverse=True)
    return out


def iso(epoch):
    if not epoch:
        return ""
    return datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc) \
        .strftime("%Y-%m-%d %H:%M UTC")


def schedule_for(rel, previous=None):
    published = int(rel.get("published") or 0) or now()
    base = published + SOAK
    when = base + random.uniform(0, JITTER)
    if when < now():                 # an old release enabled later
        when = now() + random.uniform(0, 86400)
    return {
        "tag": rel["tag"],
        "asset": rel.get("asset", ""),
        "url": rel.get("url", ""),
        "published": published,
        "eligible": base,
        "scheduled": int(when),
        "state": "scheduled",
        "error": "",
        "installed": (previous or {}).get("installed", ""),
    }


def cmd_status():
    conf = read_conf()
    st = read_state()
    peer = slip_peer()
    inst = installed_version(peer) if peer else ""
    latest = None
    error = ""
    try:
        rels = stable_releases()
        latest = rels[0][1] if rels else None
    except Exception as exc:  # noqa: BLE001
        error = str(exc)

    if latest:
        cur = ver(inst)
        new = ver(latest["tag"])
        if cur and new and new <= cur:
            st["state"] = "current"
            st["tag"] = latest["tag"]
            st["installed"] = inst
        elif st.get("tag") != latest["tag"]:
            st = schedule_for(latest, st)
            st["installed"] = inst
    st["auto"] = (conf.get("auto", "no").lower() in ("yes", "1", "true", "on"))
    st["installed"] = inst
    st["latest"] = latest["tag"] if latest else ""
    st["published_iso"] = iso((latest or {}).get("published"))
    st["scheduled_iso"] = iso(st.get("scheduled"))
    st["eligible_iso"] = iso(st.get("eligible"))
    st["error"] = error
    print(json.dumps(st))


def cmd_tick():
    conf = read_conf()
    if conf.get("auto", "no").lower() not in ("yes", "1", "true", "on"):
        return 0
    peer = slip_peer()
    if not peer:
        return 0
    if os.path.exists(OTA_LOCK):
        print("another update is running; retry next tick")
        return 0

    st = read_state()
    inst = installed_version(peer)
    try:
        rels = stable_releases()
    except Exception as exc:  # noqa: BLE001
        st["state"] = "error"
        st["error"] = "GitHub: %s" % exc
        write_state(st)
        return 1
    if not rels:
        return 0

    latest = rels[0][1]
    cur = ver(inst)
    new = ver(latest["tag"])
    if cur and new and new <= cur:
        st.update({"state": "current", "tag": latest["tag"], "installed": inst,
                   "latest": latest["tag"], "error": ""})
        write_state(st)
        return 0

    # New release (or a newer one): plan immediately, 3 days + 0..7 days.
    if st.get("tag") != latest["tag"] or not st.get("scheduled"):
        st = schedule_for(latest, st)
        st["installed"] = inst
        write_state(st)
        print("scheduled %s for %s" % (st["tag"], iso(st["scheduled"])))
        return 0

    st["installed"] = inst
    if now() < int(st.get("scheduled") or 0):
        write_state(st)
        return 0

    # Due: re-check that this is still the newest stable release.
    if latest["tag"] != st["tag"]:
        st = schedule_for(latest, st)
        st["installed"] = inst
        write_state(st)
        return 0

    print("downloading %s" % st["tag"])
    st["state"] = "updating"
    st["error"] = ""
    write_state(st)
    try:
        halow_fw.fetch(st["tag"], OTA_IMAGE)
        rc = subprocess.call(["/usr/sbin/rnsbox-portal", "halow-ota", OTA_IMAGE],
                             timeout=900)
    except Exception as exc:  # noqa: BLE001
        rc = 1
        st["error"] = str(exc)

    if rc == 0:
        st.update({"state": "current", "installed": st["tag"],
                   "latest": st["tag"], "error": "", "last_update": now()})
        print("updated to %s" % st["tag"])
    else:
        st["state"] = "failed"
        st["scheduled"] = now() + 6 * 3600     # retry in six hours
        if not st.get("error"):
            st["error"] = "firmware update failed"
        print("update failed; retry at %s" % iso(st["scheduled"]), file=sys.stderr)
    write_state(st)
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "status":
        cmd_status()
        sys.exit(0)
    if cmd == "tick":
        sys.exit(cmd_tick())
    print("usage: halow_update.py status|tick", file=sys.stderr)
    sys.exit(1)
