#!/usr/bin/env python3
"""Update the bundled Reticulum manual (check + download).

The manual is plain Markdown rendered by the browser (marked.js), so this
helper only fetches newer sources; it never renders anything.

  check     -> {ok, message, upstream, local}
  download  -> {ok, message, upstream, local}

Update sources, in order:
  1. rngit: /etc/rnsbox/docs.conf may set `rngit_artifact=<glob>`; the fetch
     mirrors updatecheck.py (`rngit release --signer ... <anchor> fetch
     latest:<glob>`), so the docs can come over Reticulum from a release node.
  2. GitHub over HTTPS (api.github.com + codeload), used when rngit is not
     configured or does not carry the artifact.

Only docs/markdown/*.md plus the bundled pictures are installed; the pictures
in /www/manual/img stay as shipped unless the update carries new ones.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

DOCS = "/www/manual/docs"
VERSION = os.path.join(DOCS, "VERSION")
CONF = "/etc/rnsbox/docs.conf"
STATE = "/run/rnsbox-docs.json"
REPO = "markqvist/Reticulum"
SUBDIR = "docs/markdown"
RNGIT_BIN = "/usr/bin/rngit"
RNGIT_SIGNER = "bc7291552be7a58f361522990465165c"
RNGIT_ANCHORS = ("/var/lib/rnsbox/rns-release.rsm",
                 "/usr/share/rnsbox-portal/rns-release.rsm")


def conf():
    out = {}
    try:
        for line in open(CONF, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"')
    except OSError:
        pass
    return out


def version():
    try:
        return open(VERSION, encoding="utf-8").read().strip() or "unknown"
    except OSError:
        return "unknown"


def save_state(obj):
    try:
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        tmp = STATE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(obj, f)
        os.replace(tmp, STATE)
    except OSError:
        pass


def github_head():
    url = ("https://api.github.com/repos/%s/commits?path=%s&per_page=1"
           % (REPO, SUBDIR))
    req = urllib.request.Request(url, headers={"User-Agent": "rnsbox-docs"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)[0]["sha"]


def rngit_artifact(pattern):
    anchor = None
    for p in RNGIT_ANCHORS:
        if os.path.isfile(p) and os.path.getsize(p):
            anchor = p
            break
    if not anchor or not os.path.isfile(RNGIT_BIN):
        return None, "rngit is not configured/installed"
    tmp = tempfile.mkdtemp(prefix="rnsbox-docs-")
    try:
        cmd = [RNGIT_BIN, "release", "--signer", RNGIT_SIGNER,
               anchor, "fetch", "latest:" + pattern]
        env = dict(os.environ)
        env.setdefault("HOME", "/root")
        cp = subprocess.run(cmd, cwd=tmp, capture_output=True, text=True,
                            timeout=120, env=env)
        files = sorted(glob.glob(os.path.join(tmp, pattern)))
        if not files:
            out = ((cp.stdout or "") + (cp.stderr or "")).strip()
            return None, (out[-300:] or "rngit returned no docs artifact")
        return files[0], ""
    except Exception as e:
        return None, str(e)


def install_archive(path):
    """Replace DOCS/*.md from an archive or a plain .md file."""
    os.makedirs(DOCS, exist_ok=True)
    staging = tempfile.mkdtemp(prefix="rnsbox-docs-new-")
    if path.endswith((".tar.gz", ".tgz", ".tar")):
        with tarfile.open(path) as t:
            members = [m for m in t.getmembers()
                       if "/docs/markdown/" in m.name and m.name.endswith(".md")]
            if not members:
                raise RuntimeError("archive has no docs/markdown/*.md")
            for m in members:
                src = t.extractfile(m)
                if src:
                    with open(os.path.join(staging, os.path.basename(m.name)), "wb") as o:
                        shutil.copyfileobj(src, o)
    elif path.endswith(".md"):
        shutil.copy(path, os.path.join(staging, os.path.basename(path)))
    else:
        raise RuntimeError("unsupported docs artifact")
    files = [f for f in os.listdir(staging) if f.endswith(".md")]
    if not files:
        raise RuntimeError("no markdown files in the artifact")
    for f in glob.glob(os.path.join(DOCS, "*.md")):
        os.unlink(f)
    for f in files:
        shutil.move(os.path.join(staging, f), os.path.join(DOCS, f))
    shutil.rmtree(staging, ignore_errors=True)


def cmd_check():
    local = version()
    cfg = conf()
    upstream, err = None, ""
    art = cfg.get("rngit_artifact", "")
    if art:
        path, err = rngit_artifact(art)
        if path:
            upstream = "rngit"
    if upstream is None:
        try:
            upstream = github_head()
        except Exception as e:
            err = str(e)
    ok = upstream is not None
    if ok and upstream != local:
        result = "Update available: %s → %s" % (local[:12], upstream[:12])
    elif ok:
        result = "Documentation is up to date (%s)" % local[:12]
    else:
        result = "Could not check for updates: %s" % (err or "unknown error")
    save_state({"checked_at": int(time.time()), "upstream": upstream,
                "local": local, "result": result})
    print(json.dumps({"ok": ok, "message": result, "upstream": upstream,
                      "local": local}))


def cmd_download():
    local = version()
    cfg = conf()
    art = cfg.get("rngit_artifact", "")
    err = ""
    if art:
        path, err = rngit_artifact(art)
        if path:
            try:
                install_archive(path)
                open(VERSION, "w").write("rngit\n")
                print(json.dumps({"ok": True,
                                  "message": "Documentation updated over Reticulum.",
                                  "upstream": "rngit", "local": local}))
                return
            except Exception as e:
                err = str(e)
    try:
        # Fetch the Markdown files one by one: the repository tarball carries
        # ~50 MB of graphics and takes minutes on this box.
        names = ["index", "whatis", "gettingstartedfast", "zen", "software",
                 "using", "understanding", "hardware", "interfaces",
                 "networks", "distributed", "git", "support", "examples",
                 "license", "brandolinis", "reference"]
        base = ("https://raw.githubusercontent.com/%s/master/%s/" % (REPO, SUBDIR))
        staging = tempfile.mkdtemp(prefix="rnsbox-docs-new-")
        for n in names:
            req = urllib.request.Request(base + n + ".md",
                                         headers={"User-Agent": "rnsbox-docs"})
            with urllib.request.urlopen(req, timeout=20) as r:
                data = r.read()
            open(os.path.join(staging, n + ".md"), "wb").write(data)
        os.makedirs(DOCS, exist_ok=True)
        for f in glob.glob(os.path.join(DOCS, "*.md")):
            os.unlink(f)
        for f in os.listdir(staging):
            shutil.move(os.path.join(staging, f), os.path.join(DOCS, f))
        shutil.rmtree(staging, ignore_errors=True)
        try:
            up = github_head()
        except Exception:
            up = "github"
        open(VERSION, "w").write(up)
        print(json.dumps({"ok": True,
                          "message": "Documentation downloaded from the upstream repository.",
                          "upstream": up, "local": local}))
    except Exception as e:
        print(json.dumps({"ok": False,
                          "message": "Download failed: %s" % (e or err or "unknown error"),
                          "upstream": None, "local": local}))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        cmd_check()
    elif cmd == "download":
        cmd_download()
    else:
        print(json.dumps({"ok": False, "message": "unknown command"}))
        sys.exit(2)


if __name__ == "__main__":
    main()
