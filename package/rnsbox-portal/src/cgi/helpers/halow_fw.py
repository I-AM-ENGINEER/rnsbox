#!/usr/bin/env python3
"""halow_fw.py — list/download RNode HaLow firmware releases from GitHub.

The portal's HaLow card calls this from the box (the browser cannot fetch
GitHub release assets: they redirect to a host without CORS headers). Then
`rnsbox-portal halow-ota` flashes the downloaded archive.

    halow_fw.py list
    halow_fw.py fetch <tag> <output-file>
"""
import json
import os
import ssl
import sys
import urllib.request

API = ("https://api.github.com/repos/I-AM-ENGINEER/RNode_Halow_Firmware/"
       "releases?per_page=50")
CA_FALLBACK = "/usr/share/rnsbox-portal/ca-certificates.crt"
UA = {"User-Agent": "rnsbox-portal", "Accept": "application/vnd.github+json"}


def context():
    try:
        cafile = CA_FALLBACK if os.path.exists(CA_FALLBACK) else None
        return ssl.create_default_context(cafile=cafile)
    except Exception:
        return ssl.create_default_context()


def get(url, timeout=30):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout, context=context()) as r:
        return r.read()


def firmware_asset(rel):
    for a in rel.get("assets") or []:
        name = a.get("name", "")
        if name.endswith(".tar") or name.endswith(".bin"):
            return a
    return None


def releases():
    data = json.loads(get(API).decode("utf-8", "replace"))
    out = []
    for rel in data:
        asset = firmware_asset(rel)
        if not asset:
            continue
        out.append({
            "tag": rel.get("tag_name", ""),
            "name": rel.get("name", ""),
            "prerelease": bool(rel.get("prerelease")),
            "asset": asset.get("name", ""),
            "size": int(asset.get("size") or 0),
            "url": asset.get("browser_download_url", ""),
        })
    return out


def fetch(tag, path):
    for rel in releases():
        if rel["tag"] == tag:
            data = get(rel["url"], timeout=120)
            with open(path, "wb") as f:
                f.write(data)
            print(len(data))
            return 0
    print("release not found: " + tag, file=sys.stderr)
    return 2


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "list":
        print(json.dumps(releases()))
        sys.exit(0)
    if len(sys.argv) >= 4 and sys.argv[1] == "fetch":
        sys.exit(fetch(sys.argv[2], sys.argv[3]))
    print("usage: halow_fw.py list | fetch <tag> <file>", file=sys.stderr)
    sys.exit(1)
