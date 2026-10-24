#!/usr/bin/env python3
"""mapnodes.py — nodes with coordinates, read from rnsd's discovery store.

rnsd (with `discover_interfaces = Yes`) validates interface-discovery
announces and persists each one under <storage>/discovery/interfaces.  The
records carry the fields RMAP nodes publish: LATITUDE 0x03 / LONGITUDE 0x04 /
HEIGHT 0x05 plus the interface name and type.  This helper only *reads* that
store — it never touches the network — and prints a JSON array for the Map
tab.  Data therefore lives on flash across reboots and needs no internet.
"""
import glob
import importlib.util
import json
import os
import sys
import time


def _load_umsgpack():
    """Load RNS's vendored umsgpack without importing the RNS package.

    ``import RNS`` pulls in the whole Reticulum stack (seconds on a C906);
    the map helper only needs the msgpack decoder.  Loading the vendor file
    directly keeps this endpoint fast and independent of rnsd.
    """
    for pattern in ("/usr/lib/python3*/site-packages/RNS/vendor/umsgpack.py",
                    "/usr/lib/python3/dist-packages/RNS/vendor/umsgpack.py"):
        for path in glob.glob(pattern):
            try:
                spec = importlib.util.spec_from_file_location("_rns_umsgpack", path)
                mod = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(mod)
                return mod
            except Exception:
                continue
    try:
        import umsgpack as mod
        return mod
    except ImportError:
        from RNS.vendor import umsgpack as mod
        return mod


umsgpack = _load_umsgpack()

CANDIDATES = [
    # rnsd runs with --config /etc/reticulum: RNS puts storage/ next to the
    # config dir, so this is the real location on the box (0700 root, which
    # is fine — the portal CGI runs as root).
    "/etc/reticulum/storage/discovery/interfaces",
    "/root/.reticulum/storage/discovery/interfaces",
    "/var/lib/reticulum/discovery/interfaces",
    "/home/admin/.reticulum/storage/discovery/interfaces",
]


def _store() -> str:
    for p in CANDIDATES:
        if os.path.isdir(p):
            return p
    # RNS puts it next to the identity of whichever user ran rnsd; ask the
    # module for the current user's default as a last resort.
    try:
        return os.path.join(os.path.expanduser("~"), ".reticulum", "storage",
                            "discovery", "interfaces")
    except Exception:
        return CANDIDATES[0]


def _hex(v):
    if isinstance(v, bytes):
        return v.hex()
    return v


def load():
    store = _store()
    if not os.path.isdir(store):
        return {"nodes": [], "total": 0, "store": store,
                "error": "interface discovery has not collected anything yet"}
    nodes = {}
    errors = []
    for path in glob.glob(os.path.join(store, "*")):
        try:
            with open(path, "rb") as f:
                info = umsgpack.unpackb(f.read())
        except Exception as e:  # corrupt or partially written record
            errors.append("%s: %s" % (os.path.basename(path), e))
            continue

        lat = info.get("latitude")
        lon = info.get("longitude")
        tid = _hex(info.get("transport_id")) or _hex(info.get("network_id")) or os.path.basename(path)

        node = {
            "id": tid,
            "name": info.get("name") or "Reticulum node",
            "type": info.get("type") or "",
            "impl": info.get("impl_name") or "",
            "version": info.get("version") or "",
            "lat": lat,
            "lon": lon,
            "height": info.get("height"),
            "hops": info.get("hops"),
            "last_heard": info.get("last_heard") or 0,
            "discovered": info.get("discovered") or 0,
            "heard_count": info.get("heard_count") or 0,
            "status": info.get("status") or "",
            "frequency": info.get("frequency"),
            "bandwidth": info.get("bandwidth"),
            # Interface-discovery records use the short radio keys sf/cr;
            # accept the long spellings too for other publishers.
            "spreading_factor": info.get("spreading_factor") or info.get("sf"),
            "coding_rate": info.get("coding_rate") or info.get("cr"),
            "modulation": info.get("modulation"),
            "channel": info.get("channel"),
            "reachable_on": info.get("reachable_on"),
            "port": info.get("port"),
        }

        # One physical node announces each interface separately under the same
        # transport identity.  Keep the entry with coordinates; among those,
        # the most recently heard one.
        old = nodes.get(tid)
        if old is not None:
            old_has = old.get("lat") is not None and old.get("lon") is not None
            new_has = lat is not None and lon is not None
            if old_has and (not new_has or old["last_heard"] >= node["last_heard"]):
                continue
        nodes[tid] = node

    out = [n for n in nodes.values() if n["lat"] is not None and n["lon"] is not None]
    out.sort(key=lambda n: n["last_heard"], reverse=True)
    return {"nodes": out, "total": len(nodes), "store": store,
            "error": "" if out or not errors else errors[0]}


if __name__ == "__main__":
    print(json.dumps(load()))
