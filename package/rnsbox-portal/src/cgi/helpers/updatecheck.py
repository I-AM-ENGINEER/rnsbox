"""Check whether a newer rnsd (the Reticulum 'rns' package) is available.

Runs periodically from cron (and once, best-effort, at boot); it queries PyPI
for the latest 'rns' release, compares it to the installed version, and writes
a small JSON state file the portal reads to show an "update available" notice.

Deliberately a *notifier only*:
  * it never touches the installed package or any user config -- the only file
    it writes is the machine-state JSON under /var/lib/rnsbox (never /etc);
  * network failures are non-fatal (the box may be offline) -- check() never
    raises and preserves the last known result.
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

# glob / shutil / tarfile are imported lazily inside the update-apply path
# (_pkg_paths / apply_update) so the portal's steady state — which only ever
# calls installed_version() + display_state() — never pays their import RAM.

PKG = "rns"                       # rnsd is provided by the Reticulum 'rns' pkg
STATE_DIR = "/var/lib/rnsbox"
STATE_FILE = STATE_DIR + "/update.json"
ROLLBACK_TAR = STATE_DIR + "/rns-rollback.tar"
PYPI_URL = "https://pypi.org/pypi/{pkg}/json"
RNSD_INIT = "/etc/init.d/S82rnsd"
RNSD_PIDFILE = "/var/run/rnsd.pid"
TIMEOUT = 8


_ver_cache = {}


def installed_version(pkg=PKG):
    """Installed version string, or None. Memoised per process.

    importlib.metadata.version() scans site-packages (~0.1-0.5 s on the C906)
    and the answer only changes when a package is (re)installed — apply_update()
    refreshes this cache, and a reflash restarts the process — so we compute it
    once and serve it from a dict thereafter. This is the single biggest win for
    the dashboard + Reticulum page load, which used to pay the scan every render
    (twice, on /reticulum). No heavy `import RNS`; RNS._version is the fallback."""
    if pkg in _ver_cache:
        return _ver_cache[pkg]
    v = None
    try:
        import importlib.metadata as md
        v = md.version(pkg)
    except Exception:
        try:                          # fallback: RNS ships _version.py
            import RNS
            v = getattr(RNS, "__version__", None)
        except Exception:
            v = None
    _ver_cache[pkg] = v
    return v


def _vtuple(v):
    """Leading numeric components of a version as an int tuple ('1.5.2' ->
    (1,5,2)); trailing non-numeric (rc/beta) parts are ignored per component."""
    out = []
    for part in str(v).split("."):
        num = ""
        for ch in part:
            if ch.isdigit():
                num += ch
            else:
                break
        if num == "":
            break
        out.append(int(num))
    return tuple(out)


def _newer(latest, installed):
    if not latest or not installed:
        return False
    return _vtuple(latest) > _vtuple(installed)


def latest_version(pkg=PKG, timeout=TIMEOUT):
    req = urllib.request.Request(
        PYPI_URL.format(pkg=pkg),
        headers={"User-Agent": "rnsbox-update-check",
                 "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = json.load(r)
    return data["info"]["version"]


def read_state():
    """Last recorded state dict (empty if never checked / unreadable)."""
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except Exception:
        return {}


def display_state(pkg=PKG):
    """State for the web UI. Keeps the last check's `latest`/`checked_at`/
    `error`, but recomputes `installed` and `update_available` against the
    version installed RIGHT NOW. Otherwise an update applied out of band —
    a manual `pip install -U rns`, or apply_update itself — still shows a
    stale 'update available' until the next periodic check rewrites the file."""
    state = read_state()
    installed = installed_version(pkg)
    state["installed"] = installed
    latest = state.get("latest")
    state["update_available"] = bool(latest) and _newer(latest, installed)
    return state


def _write_state(state):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        tmp = STATE_FILE + ".tmp"
        with open(tmp, "w") as f:
            json.dump(state, f)
        os.rename(tmp, STATE_FILE)
    except Exception:
        pass                       # best-effort; never fatal


def check(pkg=PKG):
    """Query PyPI, compare, persist. Never raises; returns the state dict."""
    state = read_state()           # keep prior 'latest' if this attempt fails
    state.update({"package": pkg,
                  "installed": installed_version(pkg),
                  "checked_at": int(time.time())})
    try:
        latest = latest_version(pkg)
        state["latest"] = latest
        state["update_available"] = _newer(latest, state["installed"])
        state["error"] = ""
    except Exception as e:
        state["error"] = type(e).__name__
    _write_state(state)
    return state


# --------------------------------------------------------------------------
# In-place update (rootfs is writeable). rns is a pure-Python wheel, so we can
# update it without a compiler. We use pip with --no-deps + wheel-only so the
# compiled deps (cryptography/pyserial) are never rebuilt, clear the old
# buildroot (distutils) install first (pip cannot uninstall it), keep a
# rollback tarball, and restart+verify rnsd -- rolling back if it fails.
# Only site-packages is touched; /etc and user config are never modified.
# --------------------------------------------------------------------------

def _pypi_json(pkg=PKG, timeout=TIMEOUT):
    req = urllib.request.Request(
        PYPI_URL.format(pkg=pkg),
        headers={"User-Agent": "rnsbox-update-check", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def _dep_floor(req):
    """'cryptography>=3.4.7' -> ('cryptography', '3.4.7'); skip optional extras."""
    parts = req.split(";")
    if len(parts) > 1 and "extra" in parts[1]:
        return None, None
    spec = parts[0].strip()
    name = ""
    for ch in spec:
        if ch.isalnum() or ch in "_.-":
            name += ch
        else:
            break
    floor = ""
    i = spec.find(">=")
    if i >= 0:
        for ch in spec[i + 2:].strip():
            if ch.isdigit() or ch == ".":
                floor += ch
            else:
                break
    return (name or None), (floor or None)


def _unmet_deps(requires_dist):
    """Runtime deps whose installed version is below the new release's floor."""
    unmet = []
    for req in (requires_dist or []):
        name, floor = _dep_floor(req)
        if not name:
            continue
        inst = installed_version(name)
        if inst is None:
            unmet.append("%s (missing)" % name)
        elif floor and _vtuple(inst) < _vtuple(floor):
            unmet.append("%s %s < %s" % (name, inst, floor))
    return unmet


def _site_base(pkg=PKG):
    import importlib.metadata as md
    return str(md.distribution(pkg).locate_file(""))


def _pkg_paths(site, pkg):
    """Package dirs + metadata for `pkg` under site-packages (best-effort)."""
    import glob
    import importlib.metadata as md
    tops = set()
    try:
        tl = md.distribution(pkg).read_text("top_level.txt") or ""
        tops = {t.strip() for t in tl.splitlines() if t.strip()}
    except Exception:
        pass
    if not tops:
        tops = {"RNS", "CRNS"}                 # known top-levels for rns
    paths = [os.path.join(site, t) for t in tops
             if os.path.exists(os.path.join(site, t))]
    paths += glob.glob(os.path.join(site, pkg + "-*.egg-info"))
    paths += glob.glob(os.path.join(site, pkg + "-*.dist-info"))
    return paths


def _rnsd_up(timeout=15):
    for _ in range(timeout):
        try:
            with open(RNSD_PIDFILE) as f:
                pid = f.read().strip()
            if pid and os.path.exists("/proc/" + pid):
                return True
        except Exception:
            pass
        time.sleep(1)
    return False


def apply_update(pkg=PKG):
    """Update `pkg` in place from its PyPI wheel. Returns a result dict; never
    raises. On any failure after the tree is touched, restores the rollback
    tarball and restarts rnsd."""
    import shutil
    import tarfile
    res = {"ok": False, "from": installed_version(pkg), "to": None,
           "error": "", "rolled_back": False}
    site = None
    touched = False
    try:
        data = _pypi_json(pkg)
        ver = data["info"]["version"]
        res["to"] = ver
        if not _newer(ver, res["from"]):
            res["error"] = "already up to date"
            return res
        unmet = _unmet_deps(data["info"].get("requires_dist"))
        if unmet:
            res["error"] = ("%s %s needs a newer %s than is installed — reflash "
                            "the image to update" % (pkg, ver, ", ".join(unmet)))
            return res
        site = _site_base(pkg)
        remove = _pkg_paths(site, pkg)
        os.makedirs(STATE_DIR, exist_ok=True)
        with tarfile.open(ROLLBACK_TAR, "w") as tf:
            for p in remove:
                tf.add(p, arcname=os.path.relpath(p, site))
        touched = True
        for p in remove:                       # clear the old distutils install
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        # --no-deps + wheel-only so the compiled deps (cryptography/pyserial) are
        # never rebuilt; buildroot's rootfs is not PEP-668 externally-managed, so
        # pip installs into the system site-packages as root without extra flags.
        # pip's temp/download dir defaults to /tmp, which is a tiny (512 KB)
        # tmpfs on the box — a wheel download overflows it (ENOSPC) even with
        # tens of GB free on the rootfs. Point TMPDIR at the rootfs (and skip
        # the HTTP cache) so the download has room.
        piptmp = os.path.join(STATE_DIR, "piptmp")
        os.makedirs(piptmp, exist_ok=True)
        env = dict(os.environ, TMPDIR=piptmp)
        try:
            cp = subprocess.run(
                [sys.executable, "-m", "pip", "install", "--no-deps",
                 "--only-binary=:all:", "--no-cache-dir", "--no-input",
                 "--disable-pip-version-check", "%s==%s" % (pkg, ver)],
                capture_output=True, text=True, timeout=180, env=env)
        finally:
            # remove piptmp on EVERY exit path -- a pip timeout (or any raise)
            # would otherwise leave partial downloads on the persistent rootfs.
            shutil.rmtree(piptmp, ignore_errors=True)
        if cp.returncode != 0:
            raise RuntimeError("pip install failed: "
                               + (cp.stderr or cp.stdout or "")[-300:].strip())
        subprocess.run([RNSD_INIT, "restart"], check=False, timeout=25)
        if not _rnsd_up(15):
            raise RuntimeError("rnsd did not come back after the update")
        res["ok"] = True
        _ver_cache[pkg] = ver          # keep the memoised version in step
        _write_state({"package": pkg, "installed": ver, "latest": ver,
                      "update_available": False, "error": "",
                      "checked_at": int(time.time())})
        return res
    except Exception as e:
        res["error"] = str(e) if isinstance(e, RuntimeError) \
            else "%s: %s" % (type(e).__name__, e)
        if touched and site:                   # roll back to the saved package
            try:
                for p in _pkg_paths(site, pkg):
                    shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
                with tarfile.open(ROLLBACK_TAR) as tf:
                    tf.extractall(site)
                subprocess.run([RNSD_INIT, "restart"], check=False, timeout=25)
                res["rolled_back"] = True
                _ver_cache[pkg] = res["from"]   # restored the old version
            except Exception:
                pass
        return res


if __name__ == "__main__":
    # CLI for the C++ portal (and cron). Prints ONE line of JSON so the caller
    # parses it trivially:
    #   updatecheck.py display  -> {installed, latest, update_available, error, checked_at}
    #   updatecheck.py check    -> queries PyPI + persists, same shape
    #   updatecheck.py apply     -> {ok, from, to, error, rolled_back}
    #   updatecheck.py cron      -> check(), human line (the boot/cron notifier)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "cron"
    if cmd == "display":
        print(json.dumps(display_state()))
    elif cmd == "check":
        print(json.dumps(check()))
    elif cmd == "apply":
        print(json.dumps(apply_update()))
    else:  # cron: human summary (unchanged from the old __main__)
        st = check()
        if st.get("error"):
            print("update-check: failed (%s); installed=%s"
                  % (st["error"], st.get("installed")))
        elif st.get("update_available"):
            print("update-check: %s %s available (installed %s)"
                  % (st.get("package"), st.get("latest"), st.get("installed")))
        else:
            print("update-check: %s up to date (%s)"
                  % (st.get("package"), st.get("installed")))
