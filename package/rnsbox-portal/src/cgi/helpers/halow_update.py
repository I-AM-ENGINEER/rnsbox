#!/usr/bin/env python3
"""halow_update.py — scheduled automatic updates for RNode HaLow modems.

Policy (deliberately conservative):
  * only STABLE releases (a prerelease is never selected);
  * every modem is scheduled on its own: published_at + 3 days + a random
    0..7 days, so a fleet never updates in lockstep;
  * nothing is downloaded before the scheduled moment;
  * at that moment one device at a time is updated over HTTP, the next one
    waits for the following tick.

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

sys.path.insert(0, "/usr/lib/rnsbox")
import halow_fw  # noqa: E402

CONF = "/etc/rnsbox/halow_update.conf"
STATE = "/var/lib/rnsbox/halow-update.json"
FLASHER = "/usr/lib/rnsbox/halow_flasher.py"
OTA_LOCK = "/run/rnsbox-halow-ota.lock"
OTA_IMAGE = "/run/rnsbox-halow-ota.fw"
PORTAL = "/usr/sbin/rnsbox-portal"

SOAK = 3 * 86400
JITTER = 7 * 86400
RETRY = 6 * 3600


def now():
    return int(time.time())


def auto_on(conf):
    return conf.get("auto", "no").lower() in ("yes", "1", "true", "on")


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
            st = json.load(f)
    except (OSError, ValueError):
        return {}
    # The first version kept one global schedule; start clean rather than
    # interpreting those keys as a device.
    if not isinstance(st.get("devices"), dict):
        return {}
    return st


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


def iso(ts):
    if not ts:
        return ""
    try:
        return datetime.datetime.fromtimestamp(
            int(ts), datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    except (ValueError, OSError):
        return ""


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


def discover():
    """Every RNode HaLow modem the box can reach over HTTP."""
    try:
        out = subprocess.check_output(
            ["/usr/bin/python3", FLASHER, "scan"],
            timeout=90, stderr=subprocess.DEVNULL)
        data = json.loads(out.decode("utf-8", "replace"))
    except Exception as exc:  # noqa: BLE001
        return [], str(exc)
    devs = []
    for d in data.get("devices") or []:
        # Auto-updates only apply to modems running our firmware; a factory
        # device has no HTTP OTA endpoint.
        if d.get("kind") != "rnode-halow":
            continue
        ip = (d.get("ip") or "").strip()
        if not ip:
            continue
        devs.append({
            "mac": (d.get("mac") or "").lower(),
            "iface": d.get("iface") or "",
            "ip": ip,
            "transport": d.get("transport") or "raw",
            "hostname": d.get("hostname") or "",
            "installed": d.get("version") or "",
        })
    return devs, ""


def plan(st, devs, latest):
    """Refresh the per-device schedules. Never touches a device itself."""
    keyed = st.setdefault("devices", {})
    seen = set()
    new_ver = ver(latest["tag"])
    for d in devs:
        key = d["mac"] or d["ip"]
        seen.add(key)
        e = dict(keyed.get(key) or {})
        e.update({
            "mac": d["mac"], "iface": d["iface"], "ip": d["ip"],
            "transport": d["transport"], "hostname": d["hostname"],
            "installed": d["installed"], "online": True, "last_seen": now(),
        })

        # A tick that died mid-update must not pin the device forever.
        if e.get("state") == "updating" and now() - int(e.get("attempted") or 0) > 3600:
            e["state"] = "scheduled"
            e["scheduled"] = now()

        cur = ver(d["installed"])
        if cur and new_ver and cur >= new_ver:
            if e.get("state") != "updating":
                e.update({"state": "current", "tag": latest["tag"],
                          "scheduled": 0, "eligible": 0, "error": ""})
        elif e.get("tag") != latest["tag"]:
            s = schedule_for(latest, e)
            s.update({k: e[k] for k in
                      ("mac", "iface", "ip", "transport", "hostname",
                       "installed", "online", "last_seen")})
            e = s
        elif e.get("state") not in ("scheduled", "updating", "failed"):
            e["state"] = "scheduled"
            if not e.get("scheduled"):
                s = schedule_for(latest, e)
                s.update({k: e[k] for k in
                          ("mac", "iface", "ip", "transport", "hostname",
                           "installed", "online", "last_seen")})
                e = s
        keyed[key] = e

    for key, e in keyed.items():
        if key not in seen:
            e["online"] = False

    st["latest"] = latest["tag"]
    st["checked"] = now()
    return st


def latest_stable():
    rels = stable_releases()
    return rels[0][1] if rels else None


def cmd_status():
    conf = read_conf()
    st = read_state()
    latest, error = None, ""
    try:
        latest = latest_stable()
    except Exception as exc:  # noqa: BLE001
        error = str(exc)

    if latest:
        devs, derr = discover()
        if derr and not devs:
            error = derr
        plan(st, devs, latest)
        write_state(st)

    out = {
        "auto": auto_on(conf),
        "latest": latest["tag"] if latest else "",
        "published_iso": iso(latest.get("published")) if latest else "",
        "error": error,
        "devices": [],
    }
    entries = sorted(
        st.get("devices", {}).values(),
        key=lambda e: (0 if e.get("state") == "updating" else
                       1 if e.get("state") in ("scheduled", "failed") else 2,
                       int(e.get("scheduled") or 0)))
    for e in entries:
        out["devices"].append({
            "mac": e.get("mac", ""), "iface": e.get("iface", ""),
            "ip": e.get("ip", ""), "transport": e.get("transport", "raw"),
            "hostname": e.get("hostname", ""),
            "installed": e.get("installed", ""),
            "state": e.get("state", ""), "tag": e.get("tag", ""),
            "scheduled_iso": iso(e.get("scheduled")),
            "eligible_iso": iso(e.get("eligible")),
            "error": e.get("error", ""),
            "online": bool(e.get("online", False)),
        })
    print(json.dumps(out))


def cmd_tick():
    conf = read_conf()
    if not auto_on(conf):
        return 0

    st = read_state()
    try:
        latest = latest_stable()
    except Exception as exc:  # noqa: BLE001
        st["error"] = "GitHub: %s" % exc
        write_state(st)
        return 1
    if not latest:
        return 0

    devs, _ = discover()
    plan(st, devs, latest)
    write_state(st)
    if not devs:
        return 0
    if os.path.exists(OTA_LOCK):
        print("another update is running; retry next tick")
        return 0

    due = [e for e in st["devices"].values()
           if e.get("online") and e.get("state") == "scheduled"
           and int(e.get("scheduled") or 0) <= now()]
    if not due:
        return 0
    due.sort(key=lambda e: int(e.get("scheduled") or 0))
    dev = due[0]                     # one device per tick, never the whole fleet
    key = dev.get("mac") or dev.get("ip")

    print("updating %s (%s) to %s" % (key, dev.get("ip"), st["tag"]))
    dev["state"] = "updating"
    dev["attempted"] = now()
    dev["error"] = ""
    write_state(st)

    rc, err = 1, ""
    try:
        halow_fw.fetch(st["tag"], OTA_IMAGE)
        rc = subprocess.call(
            [PORTAL, "halow-ota", OTA_IMAGE, dev["ip"]], timeout=900)
    except Exception as exc:  # noqa: BLE001
        err = str(exc)

    dev = st["devices"].get(key, dev)
    if rc == 0:
        dev.update({"state": "current", "installed": st["tag"],
                    "tag": st["tag"], "scheduled": 0, "eligible": 0,
                    "error": "", "last_update": now()})
        print("updated %s to %s" % (key, st["tag"]))
    else:
        dev.update({"state": "failed", "scheduled": now() + RETRY,
                    "error": err or "firmware update failed"})
        print("update of %s failed; retry at %s" %
              (key, iso(dev["scheduled"])), file=sys.stderr)
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
