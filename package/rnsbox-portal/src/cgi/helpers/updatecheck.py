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
# S82rnsd restart takes up to ~21 s (18 s SIGTERM drain + SIGKILL fallback +
# 1 s + start) and first waits at most 25 s for /run/rnsd.lock (one queued
# S82rnsd operation: S46wanwatch/S84cron kick-restart, a portal restart), then
# gives up: 25 + 21 = 46 s.
RESTART_TIMEOUT = 50
TIMEOUT = 8

# Time budget for `apply`, in seconds. The callers kill from the outside in,
# and a kill after the old install is cleared but before pip has finished
# leaves RNS uninstalled with no rollback. So each layer must finish before the
# one around it gives up:
#   uhttpd script timeout  S81router -t/-T                    300
#   portal util::run       routes_reticulum.cpp applyupdate    285
#   this helper            APPLY_BUDGET, monotonic from the    270
#                          start of apply_update(), plus ~5 s
#                          of python start-up and imports
# Worst case once the old install is cleared: clear, pip succeeds just inside
# PIP_TIMEOUT, restart, rnsd never shows up, then restore + restart:
#   10 + 90 + 50 + 30 + 10 + 50 = 240 s = POST_CLEAR_WORST.
# apply_update() clears the old install only while at least that much of
# APPLY_BUDGET is left (so the PyPI fetch, dep check and rollback tarball get
# the first 30 s); otherwise it stops with nothing changed.
PIP_TIMEOUT = 90         # one pure-python wheel: download, unpack, byte-compile
RNSD_UP_TIMEOUT = 30     # covers a S46wanwatch kick (10 s tick) after a killed restart
SITE_IO = 10             # clear or restore the package tree (a few MB on the SD card)
POST_CLEAR_WORST = (SITE_IO + PIP_TIMEOUT + RESTART_TIMEOUT + RNSD_UP_TIMEOUT
                    + SITE_IO + RESTART_TIMEOUT)
APPLY_BUDGET = 270


_ver_cache = {}

# Packages that carry their own version file: {dist name: (top-level pkg, file)}.
# Read as TEXT — `import RNS` would pull in the whole Reticulum stack.
_VERSION_FILES = {"rns": ("RNS", "_version.py")}


def _version_from_package(pkg):
    """`__version__` from the package's own version file, or None."""
    loc = _VERSION_FILES.get(pkg)
    if not loc:
        return None
    try:
        import importlib.util
        import re
        spec = importlib.util.find_spec(loc[0])       # locates, doesn't import
        dirs = list(spec.submodule_search_locations or []) if spec else []
        for d in dirs:
            try:
                with open(os.path.join(d, loc[1])) as f:
                    m = re.search(r"""__version__\s*=\s*['"]([^'"]+)['"]""", f.read())
            except OSError:
                continue
            if m:
                return m.group(1)
    except Exception:
        pass
    return None


def _metadata_version(pkg):
    """HIGHEST version among ALL installed metadata records for `pkg`, or None.

    importlib.metadata.version() returns whichever record it meets first, and
    an incremental buildroot build leaves the old one behind: a shipped image
    had rns-1.2.6-py3.11.egg-info next to rns-1.5.2.dist-info (and a stale
    cryptography-3.4.8 egg-info), so the check reported "installed 1.2.6" and
    the dep check would have seen cryptography 3.4.8."""
    try:
        import importlib.metadata as md
        vers = [d.version for d in md.distributions(name=pkg) if d.version]
    except Exception:
        return None
    return max(vers, key=_vtuple) if vers else None


def installed_version(pkg=PKG):
    """Installed version string, or None. Memoised per process.

    Source of truth is the package itself (RNS/_version.py for rns), then the
    highest metadata record (see _metadata_version). The metadata scan costs
    ~0.1-0.5 s on the C906 and the answer only changes when a package is
    (re)installed — apply_update() refreshes this cache, and a reflash restarts
    the process — so it is computed once per process."""
    if pkg in _ver_cache:
        return _ver_cache[pkg]
    v = _version_from_package(pkg) or _metadata_version(pkg)
    if v is None and pkg == PKG:
        try:                          # last resort: the (heavy) import
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


def _rnsd_pid():
    try:
        with open(RNSD_PIDFILE) as f:
            return f.read().strip()
    except Exception:
        return ""


def _rnsd_up(timeout=15, old_pid=None):
    """True once the pidfile names a live process other than `old_pid` (the
    rnsd from before the restart: a restart killed inside stop() leaves the
    pidfile naming the exiting old rnsd)."""
    for _ in range(timeout):
        pid = _rnsd_pid()
        if pid and pid != old_pid and os.path.exists("/proc/" + pid):
            return True
        time.sleep(1)
    return False


def _restart_rnsd():
    """`S82rnsd restart`. A timeout is not an install failure by itself --
    the caller decides from _rnsd_up() whether rnsd came back (if the killed
    restart left it down, S46wanwatch kicks it on its next 10 s tick)."""
    try:
        subprocess.run([RNSD_INIT, "restart"], check=False,
                       timeout=RESTART_TIMEOUT)
    except subprocess.TimeoutExpired:
        pass


def apply_update(pkg=PKG):
    """Update `pkg` in place from its PyPI wheel. Returns a result dict; never
    raises. On any failure after the tree is touched, restores the rollback
    tarball and restarts rnsd."""
    import shutil
    import tarfile
    t0 = time.monotonic()              # APPLY_BUDGET clock (immune to date -s / NTP)
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
        # Last exit before the tree is touched: past this point the rest must
        # fit in APPLY_BUDGET, rollback included (see POST_CLEAR_WORST).
        if time.monotonic() - t0 > APPLY_BUDGET - POST_CLEAR_WORST:
            res["error"] = ("PyPI or the SD card was too slow to finish inside "
                            "the time limit — nothing changed, try again")
            return res
        touched = True
        for p in remove:                       # clear the old distutils install
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        # --no-deps + wheel-only so the compiled deps (cryptography/pyserial) are
        # never rebuilt; buildroot's rootfs is not PEP-668 externally-managed, so
        # pip installs into the system site-packages as root without extra flags.
        # pip's temp/download dir defaults to /tmp, a small RAM tmpfs on the
        # box (16 MB, shared with syslog and the other runtime files) — a
        # wheel download plus unpack can fill it (ENOSPC) even with tens of GB
        # free on the rootfs. Point TMPDIR at the rootfs (and skip the HTTP
        # cache) so the download has room.
        piptmp = os.path.join(STATE_DIR, "piptmp")
        os.makedirs(piptmp, exist_ok=True)
        env = dict(os.environ, TMPDIR=piptmp)
        try:
            cp = subprocess.run(
                [sys.executable, "-m", "pip", "install", "--no-deps",
                 "--only-binary=:all:", "--no-cache-dir", "--no-input",
                 "--disable-pip-version-check", "%s==%s" % (pkg, ver)],
                capture_output=True, text=True, timeout=PIP_TIMEOUT, env=env)
        finally:
            # remove piptmp on EVERY exit path -- a pip timeout (or any raise)
            # would otherwise leave partial downloads on the persistent rootfs.
            shutil.rmtree(piptmp, ignore_errors=True)
        if cp.returncode != 0:
            raise RuntimeError("pip install failed: "
                               + (cp.stderr or cp.stdout or "")[-300:].strip())
        old_pid = _rnsd_pid() or None
        _restart_rnsd()
        if not _rnsd_up(RNSD_UP_TIMEOUT, old_pid):  # covers a S46wanwatch kick after a killed restart
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
                _restart_rnsd()
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
