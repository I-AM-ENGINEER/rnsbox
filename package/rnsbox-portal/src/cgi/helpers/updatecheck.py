"""Check, download and (optionally) install a newer rnsd (the 'rns' package).

Runs periodically from cron (and once, best-effort, at boot). The primary
source is Git over Reticulum: `rngit release` fetches the latest signed
manifest and artifact from the official Reticulum release node. If that node
is unreachable, it falls back to PyPI (and a reachable PEP 503 mirror). It
compares the latest release to the installed version, downloads and verifies
the wheel, and writes a small JSON state file the portal reads.

Check and download never touch the installed package. install_downloaded()
replaces site-packages from the downloaded wheel, restarts rnsd, waits up to
RNSD_UP_TIMEOUT for the new version to appear and stay up, and restores the
rollback tar if it does not. Automatic installation is opt-in and off by
default (`update_install=no` in cron.conf). Network failures are non-fatal:
check()/download_update() never raise and preserve the last known result.
"""
import hashlib
import json
import os
import re
import ssl
import subprocess
import sys
import time
import urllib.parse
import urllib.request

# glob / shutil / tarfile / zipfile are imported lazily inside the
# update-apply path (_pkg_paths / install_downloaded) so the portal's steady state —
# which only ever calls installed_version() + display_state() — never pays
# their import RAM.

PKG = "rns"                       # rnsd is provided by the Reticulum 'rns' pkg
STATE_DIR = "/var/lib/rnsbox"
# The state JSON is rewritten on every cron check — keep it off the SD card
# (/run is tmpfs). Only the rollback tar, written once per applied update,
# lives on disk: a rollback must survive a reboot.
STATE_FILE = "/run/rnsbox-update.json"
ROLLBACK_TAR = STATE_DIR + "/rns-rollback.tar"
RNGIT_ANCHOR_STATE = STATE_DIR + "/rns-release.rsm"
CRON_CONF = "/etc/rnsbox/cron.conf"
PYPI_URL = "https://pypi.org/pypi/{pkg}/json"
# Some ISPs black-hole pypi.org's TLS (the handshake times out even though
# TCP connects and DNS resolves). These PEP 503 indexes are current and
# reachable there; the first one that answers is remembered in the state file
# and tried first next time. A plain index has no dependency metadata, so the
# apply path reads Requires-Dist out of the downloaded wheel instead.
SIMPLE_FALLBACKS = (
    ("pypi.tuna.tsinghua.edu.cn", "https://pypi.tuna.tsinghua.edu.cn/simple"),
    ("pypi.mirrors.ustc.edu.cn", "https://pypi.mirrors.ustc.edu.cn/simple"),
)
# Shipped with the portal for images built without the ca-certificates
# package (several rnsbox boards): /etc/ssl/certs exists but is empty, so
# every HTTPS request failed with CERTIFICATE_VERIFY_FAILED.
CA_BUNDLES = (
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
    "/usr/share/rnsbox-portal/ca-certificates.crt",
)
# Git over Reticulum is the primary update source: rngit release fetches the
# latest signed manifest and artifacts from the official Reticulum release
# node. The shipped manifest is the trust anchor; a freshly fetched manifest
# replaces it in /var/lib so origin changes are followed. Passing --signer
# pins the release signing identity (the same one that signed the anchor).
RNGIT_BIN = "/usr/bin/rngit"
RNGIT_ANCHOR_SHIPPED = "/usr/share/rnsbox-portal/rns-release.rsm"
# Release signing identity from the official rns_1.5.7.rsm manifest.
RNGIT_SIGNER = "bc7291552be7a58f361522990465165c"
# Matches nothing on purpose: rngit still fetches and validates the latest
# manifest (which is what a check needs), then reports no matching artifacts.
RNGIT_PROBE_ARTIFACT = "latest:__rnsbox_probe__"
RNGIT_WHEEL_ARTIFACT = "latest:rns-*-py3-none-any.whl"
RNGIT_CHECK_TIMEOUT = 60
RNGIT_DOWNLOAD_TIMEOUT = 180
# If the rngit download burned this long, do not start an HTTP fallback: the
# caller's request budget (uhttpd 300 s) would not fit both.
RNGIT_FALLBACK_BUDGET = 120
RNSD_INIT = "/etc/init.d/S82rnsd"
RNSD_PIDFILE = "/var/run/rnsd.pid"
# S82rnsd restart takes up to ~21 s (18 s SIGTERM drain + SIGKILL fallback +
# 1 s + start) and first waits at most 25 s for /run/rnsd.lock (one queued
# S82rnsd operation: S46wanwatch/S84cron kick-restart, a portal restart), then
# gives up: 25 + 21 = 46 s.
RESTART_TIMEOUT = 50
TIMEOUT = 8
DOWNLOAD_TIMEOUT = 90      # one ~1 MB pure-python wheel, even on a slow WAN

_ssl_ctx = None


def _clock_ok():
    """True when a real date is available for TLS (HTTPS/PyPI). Git over
    Reticulum (rngit) verifies signatures, not dates, so it works without."""
    try:
        if os.path.exists("/run/clock-synced"):
            return True
    except OSError:
        pass
    return time.time() >= 1600000000     # 2020-09-13


def _ssl_context():
    """Default TLS context, using the shipped bundle when the image has no
    system CA store (see CA_BUNDLES)."""
    global _ssl_ctx
    if _ssl_ctx is None:
        bundle = next((p for p in CA_BUNDLES
                       if os.path.isfile(p) and os.path.getsize(p) > 0), None)
        try:
            _ssl_ctx = ssl.create_default_context(cafile=bundle)
        except Exception:
            _ssl_ctx = ssl.create_default_context()
    return _ssl_ctx


def _urlopen(req, timeout=TIMEOUT):
    return urllib.request.urlopen(req, timeout=timeout, context=_ssl_context())


def _errstr(exc):
    """One-line failure reason, unwrapping URLError's reason."""
    reason = getattr(exc, "reason", None)
    if isinstance(reason, BaseException):
        exc = reason
    s = "%s: %s" % (type(exc).__name__, exc)
    s = s.replace("\n", " ").strip()
    return s[:200]


def _conf_bool(key, default=False):
    """Boolean key from /etc/rnsbox/cron.conf (last occurrence wins)."""
    val = None
    try:
        with open(CRON_CONF) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line.startswith(key + "="):
                    val = line.split("=", 1)[1].strip().lower()
    except OSError:
        return default
    if val is None:
        return default
    return val not in ("no", "0", "false", "off", "")


def _wheel_path(ver):
    return os.path.join(STATE_DIR, "%s-%s.whl" % (PKG, ver))


def _rngit_anchor():
    """Newest signed manifest we hold: fetched in /var/lib, else shipped."""
    for p in (RNGIT_ANCHOR_STATE, RNGIT_ANCHOR_SHIPPED):
        try:
            if os.path.isfile(p) and os.path.getsize(p) > 0:
                return p
        except OSError:
            pass
    return None


def _rngit_fetch(target, timeout):
    """Run `rngit release ... fetch target` in a temp dir.

    Returns (version, wheel_path, manifests, output, error). A requested wheel
    is copied to /var/lib/rnsbox/rns-<ver>.whl after rngit verified it against
    the signed manifest. The freshly fetched manifest replaces the persistent
    anchor so future fetches follow the release origin.
    """
    import glob
    import shutil
    import subprocess
    import tempfile
    anchor = _rngit_anchor()
    if not anchor:
        return None, None, [], "", "no signed release manifest installed"
    if not os.path.isfile(RNGIT_BIN):
        return None, None, [], "", "rngit is not installed"
    tmp = tempfile.mkdtemp(prefix="rnsbox-rngit-")
    try:
        cmd = [RNGIT_BIN, "release", "--signer", RNGIT_SIGNER,
               anchor, "fetch", target]
        env = dict(os.environ)
        env.setdefault("HOME", "/root")   # CGI may run without HOME
        cp = subprocess.run(cmd, cwd=tmp, capture_output=True, text=True,
                            timeout=timeout, env=env)
        out = (cp.stdout or "") + (cp.stderr or "")
        ver = None
        manifests = sorted(glob.glob(os.path.join(tmp, "rns_*.rsm")))
        for p in manifests:
            m = re.match(r"^rns_(.+)\.rsm$", os.path.basename(p))
            if m and ver is None:
                ver = m.group(1)
        wheel = None
        for p in glob.glob(os.path.join(tmp, "rns-*-py3-none-any.whl")):
            m = re.match(r"^rns-(.+)-py3-none-any\.whl$", os.path.basename(p))
            if m:
                if ver is None:
                    ver = m.group(1)
                wheel = p
                break
        if ver is None:
            return None, None, [], out, (out.strip()[-300:] or "rngit returned no release")
        os.makedirs(STATE_DIR, exist_ok=True)
        if manifests:
            try:
                dst = RNGIT_ANCHOR_STATE + ".tmp"
                with open(dst, "wb") as f:
                    with open(manifests[0], "rb") as src:
                        shutil.copyfileobj(src, f)
                os.chmod(dst, 0o644)
                os.replace(dst, RNGIT_ANCHOR_STATE)
            except OSError:
                pass
        dest = None
        if wheel:
            dest = _wheel_path(ver)
            try:
                tmpdest = dest + ".tmp"
                shutil.copyfile(wheel, tmpdest)
                os.chmod(tmpdest, 0o600)
                os.replace(tmpdest, dest)
            except OSError as e:
                return ver, None, manifests, out, _errstr(e)
        return ver, dest, manifests, out, ""
    except subprocess.TimeoutExpired:
        return None, None, [], "", "rngit timed out"
    except Exception as e:
        return None, None, [], "", _errstr(e)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _rngit_release():
    """Probe the release node for the latest signed version (manifest only)."""
    ver, _wheel, _manifests, _out, err = _rngit_fetch(
        RNGIT_PROBE_ARTIFACT, RNGIT_CHECK_TIMEOUT)
    if ver is None:
        raise RuntimeError(err or "rngit release probe failed")
    return {"version": ver, "requires_dist": None, "url": None,
            "sha256": None, "index_url": None, "source": "rngit"}


# Time budget for `install`, in seconds. The callers kill from the outside in,
# and a kill after the old install is cleared but before the new wheel is in
# place leaves RNS uninstalled with no rollback. So each layer must finish
# before the one around it gives up:
#   uhttpd script timeout  S81router -t/-T                    300
#   portal util::run       routes_reticulum.cpp installupdate  285
#   this helper            APPLY_BUDGET, monotonic from the    270
#                          start of install_downloaded(), plus
#                          ~5 s of python start-up/imports
# Worst case once the old install is cleared: extract, restart, wait for rnsd
# (up to two minutes), then restore + restart:
#   10 + 50 + 120 + 10 + 50 = 240 s = POST_CLEAR_WORST.
# install_downloaded() clears the old install only while at least that much of
# APPLY_BUDGET is left (wheel verification, dep check and rollback tarball get
# the first 30 s); otherwise it stops with nothing changed.
RNSD_UP_TIMEOUT = 120    # "a couple of minutes" for the new version to come up
RNSD_STABLE = 10         # ...and stay up for this long before we call it good
SITE_IO = 10             # clear or restore the package tree (a few MB on the SD card)
POST_CLEAR_WORST = (SITE_IO + RESTART_TIMEOUT + RNSD_UP_TIMEOUT + RNSD_STABLE
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


def _pypi_release(pkg=PKG, timeout=TIMEOUT):
    """Release metadata from PyPI's JSON API (includes Requires-Dist)."""
    req = urllib.request.Request(
        PYPI_URL.format(pkg=pkg),
        headers={"User-Agent": "rnsbox-update-check",
                 "Accept": "application/json"})
    with _urlopen(req, timeout) as r:
        data = json.load(r)
    wheel = None
    for u in data.get("urls") or []:
        if u.get("packagetype") != "bdist_wheel":
            continue
        wheel = u
        if u.get("filename", "").endswith("-py3-none-any.whl"):
            break
    return {
        "version": data["info"]["version"],
        "requires_dist": data["info"].get("requires_dist") or [],
        "url": wheel.get("url") if wheel else None,
        "sha256": ((wheel or {}).get("digests") or {}).get("sha256"),
        "index_url": None,
        "source": "pypi.org",
    }


def _simple_release(host, index, pkg=PKG, timeout=TIMEOUT):
    """Latest wheel from a PEP 503 index (no dependency metadata)."""
    url = index.rstrip("/") + "/" + pkg + "/"
    req = urllib.request.Request(url, headers={"User-Agent": "rnsbox-update-check"})
    with _urlopen(req, timeout) as r:
        html = r.read().decode("utf-8", "replace")

    best = None
    for href in re.findall(r"""href=["']([^"']+)["']""", html, re.I):
        raw, frag = (href.split("#", 1) + [""])[:2]
        full = urllib.parse.urljoin(url, raw)
        name = urllib.parse.unquote(full.rsplit("/", 1)[-1])
        if not name.startswith(pkg + "-") or not name.endswith(".whl"):
            continue
        m = re.match(r"^[0-9][^-]*-", name[len(pkg) + 1:])
        if not m:
            continue
        ver = m.group(0)[:-1]
        if best is not None and _vtuple(ver) <= _vtuple(best["version"]):
            continue
        sha = frag[7:] if frag.startswith("sha256=") else None
        best = {"version": ver, "requires_dist": None,
                "url": full.split("#", 1)[0],
                "sha256": sha, "index_url": index, "source": host}
    if best is None:
        raise RuntimeError("no wheel for %s at %s" % (pkg, host))
    return best


def _sources(prev):
    """Previous winner first, then PyPI, then the reachable mirrors."""
    order = []
    for host, index in SIMPLE_FALLBACKS:
        if host == prev:
            order.append((host, index))
    order.append(("pypi.org", None))
    order += [s for s in SIMPLE_FALLBACKS if s not in order]
    return order


def _resolve_release(pkg=PKG, use_rngit=True):
    """Git over Reticulum first, then PyPI, then the reachable mirrors."""
    errors = []
    if use_rngit:
        try:
            return _rngit_release()
        except Exception as e:
            errors.append("rngit: %s" % _errstr(e))
    # HTTPS needs a valid certificate date; rngit does not. With no trusted
    # clock, stop after the rngit attempt instead of recording a TLS failure.
    if not _clock_ok():
        raise RuntimeError("; ".join(errors + [
            "clock not set - HTTPS fallback skipped (set the clock under "
            "Settings -> Time & Clock); rngit needs no clock"])[-400:])
    prev = read_state().get("source") or ""
    for host, index in _sources(prev):
        try:
            rel = _pypi_release(pkg) if index is None else _simple_release(host, index, pkg)
            rel["source"] = host
            return rel
        except Exception as e:
            errors.append("%s: %s" % (host, _errstr(e)))
    raise RuntimeError("; ".join(errors)[-400:])


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
    a manual wheel install, or install_downloaded itself — still shows a
    stale 'update available' until the next periodic check rewrites the file."""
    state = read_state()
    installed = installed_version(pkg)
    state["installed"] = installed
    latest = state.get("latest")
    state["update_available"] = bool(latest) and _newer(latest, installed)
    return state


def _write_state(state):
    try:
        tmp = STATE_FILE + ".tmp"
        with open(tmp, "w") as f:
            json.dump(state, f)
        os.rename(tmp, STATE_FILE)
    except Exception:
        pass                       # best-effort; never fatal


def check(pkg=PKG):
    """Query PyPI (or a fallback index), compare, persist. Never raises."""
    state = read_state()           # keep prior 'latest' if this attempt fails
    state.update({"package": pkg,
                  "installed": installed_version(pkg),
                  # 0 = "not checked yet": a 1970 stamp is worse than none.
                  "checked_at": int(time.time()) if _clock_ok() else 0})
    try:
        rel = _resolve_release(pkg)
        ver = rel["version"]
        state["latest"] = ver
        state["update_available"] = _newer(ver, state["installed"])
        state["source"] = rel["source"]
        state["error"] = ""
        # A wheel downloaded for another version (or for a version that is now
        # installed) must not be offered as installable.
        if state.get("downloaded_version") != ver or not state["update_available"]:
            for k in ("downloaded_version", "downloaded_at",
                      "downloaded_sha256", "downloaded_source"):
                state.pop(k, None)
    except Exception as e:
        state["error"] = _errstr(e)
    _write_state(state)
    return state


# --------------------------------------------------------------------------
# In-place update (rootfs is writeable). rns is a pure-Python wheel: a wheel
# is a zip, so we extract it straight into site-packages -- no pip, which
# several rnsbox images (the C906/SG2000 boards) do not ship. The compiled
# deps (cryptography/pyserial) are never touched, the old buildroot install is
# cleared first, a rollback tarball is kept, and rnsd is restarted+verified --
# rolling back if it fails. Only site-packages is touched; /etc and user
# config are never modified.
# --------------------------------------------------------------------------

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


def _rnsd_stable(pid, seconds):
    """True if `pid` is still alive after `seconds` (catches an instant crash
    loop that a single pidfile sample would miss)."""
    if not pid:
        return False
    for _ in range(seconds):
        if not os.path.exists("/proc/" + pid):
            return False
        time.sleep(1)
    return os.path.exists("/proc/" + pid)


def _restart_rnsd():
    """`S82rnsd restart`. A timeout is not an install failure by itself --
    the caller decides from _rnsd_up() whether rnsd came back (if the killed
    restart left it down, S46wanwatch kicks it on its next 10 s tick)."""
    try:
        subprocess.run([RNSD_INIT, "restart"], check=False,
                       timeout=RESTART_TIMEOUT)
    except subprocess.TimeoutExpired:
        pass


def _file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _download_wheel(rel, path):
    """Fetch the wheel and verify the index's sha256 when it provides one."""
    req = urllib.request.Request(rel["url"],
                                 headers={"User-Agent": "rnsbox-update-check"})
    h = hashlib.sha256()
    with _urlopen(req, DOWNLOAD_TIMEOUT) as r, open(path, "wb") as f:
        while True:
            chunk = r.read(65536)
            if not chunk:
                break
            h.update(chunk)
            f.write(chunk)
    want = (rel.get("sha256") or "").lower()
    if want and h.hexdigest().lower() != want:
        raise RuntimeError("wheel checksum mismatch")
    return path


def _wheel_requires(path):
    """Requires-Dist lines from a wheel's METADATA (simple indexes omit them)."""
    import zipfile
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            if name.endswith(".dist-info/METADATA"):
                text = z.read(name).decode("utf-8", "replace")
                return re.findall(r"^Requires-Dist:\s*(.+)$", text, re.M)
    return []


def _install_wheel(path, site):
    """Extract a wheel over site-packages; no pip/compile step needed."""
    import shutil
    import zipfile
    root = os.path.abspath(site)
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            dest = os.path.abspath(os.path.join(root, name))
            if dest != root and not dest.startswith(root + os.sep):
                continue                      # reject ../ or absolute members
            if name.endswith("/"):
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(name) as src, open(dest, "wb") as out:
                shutil.copyfileobj(src, out)


def _remove_other_wheels(pkg, keep):
    import glob
    for old in glob.glob(os.path.join(STATE_DIR, pkg + "-*.whl")):
        if old != keep:
            try: os.remove(old)
            except OSError: pass


def _record_up_to_date(pkg, installed, ver, source):
    state = read_state()
    state.update({"package": pkg, "installed": installed, "latest": ver,
                  "update_available": False, "error": "",
                  "source": source, "checked_at": int(time.time())})
    for k in ("downloaded_version", "downloaded_at",
              "downloaded_sha256", "downloaded_source"):
        state.pop(k, None)
    _write_state(state)
    try:
        os.remove(_wheel_path(ver))
    except OSError:
        pass


def _record_downloaded(pkg, installed, ver, source, sha):
    now = int(time.time())
    state = read_state()
    state.update({
        "package": pkg, "installed": installed, "latest": ver,
        "update_available": True, "source": source, "error": "",
        "checked_at": now, "downloaded_version": ver, "downloaded_at": now,
        "downloaded_sha256": (sha or "").lower(),
        "downloaded_source": source,
    })
    _write_state(state)


def download_update(pkg=PKG):
    """Resolve and download+verify the latest wheel. Never raises.

    Primary source: Git over Reticulum (signed rngit release). If that node is
    unreachable, fall back to PyPI/mirrors. The wheel is persisted for a later
    install and its sha256 is recorded."""
    res = {"ok": False, "version": None, "source": "", "error": ""}
    installed = installed_version(pkg)

    # Reuse a wheel that is already downloaded and verified for the latest
    # version (the daily cron runs even when auto-install is off).
    st0 = read_state()
    dlv = st0.get("downloaded_version") or ""
    if (dlv and _newer(dlv, installed) and st0.get("update_available")
            and dlv == (st0.get("latest") or "")):
        p = _wheel_path(dlv)
        if os.path.isfile(p):
            want = (st0.get("downloaded_sha256") or "").lower()
            if not want or _file_sha256(p) == want:
                res.update({"ok": True, "version": dlv,
                            "source": st0.get("downloaded_source") or
                                      st0.get("source", "")})
                return res

    # 1) Git over Reticulum: rngit verifies both manifest and artifact.
    # If the last check already fell back to PyPI/mirror because rngit was
    # unreachable, do not burn another long timeout here.
    prev_source = read_state().get("source") or ""
    use_rngit = prev_source in ("", "rngit")
    t0 = time.monotonic()
    ver = wheel = None
    rngit_err = ""
    if use_rngit:
        ver, wheel, _manifests, _out, rngit_err = _rngit_fetch(
            RNGIT_WHEEL_ARTIFACT, RNGIT_DOWNLOAD_TIMEOUT)
        if ver and wheel:
            res["version"] = ver
            res["source"] = "rngit"
            if not _newer(ver, installed):
                _record_up_to_date(pkg, installed, ver, "rngit")
                res["error"] = "already up to date"
                return res
            _remove_other_wheels(pkg, wheel)
            _record_downloaded(pkg, installed, ver, "rngit", _file_sha256(wheel))
            res["ok"] = True
            return res
        if ver and not wheel:
            rngit_err = rngit_err or "rngit release has no wheel artifact"

    # A long rngit attempt leaves no room for the HTTP fallback in the same
    # request; report the rngit failure instead of running past the timeout.
    if time.monotonic() - t0 > RNGIT_FALLBACK_BUDGET:
        res["error"] = ("rngit: %s" % (rngit_err or "timed out"))[:400]
        try:
            st = read_state()
            st["error"] = res["error"]
            _write_state(st)
        except Exception:
            pass
        return res

    # 2) PyPI / reachable mirror fallback.
    try:
        rel = _resolve_release(pkg, use_rngit=False)
        ver = rel["version"]
        res["version"] = ver
        res["source"] = rel["source"]
        if not _newer(ver, installed):
            _record_up_to_date(pkg, installed, ver, rel["source"])
            res["error"] = "already up to date"
            return res
        if not rel.get("url"):
            raise RuntimeError("no wheel URL from %s" % rel["source"])
        os.makedirs(STATE_DIR, exist_ok=True)
        path = _wheel_path(ver)
        want = (rel.get("sha256") or "").lower()
        have = os.path.isfile(path)
        if have and want:
            try:
                have = _file_sha256(path) == want
            except OSError:
                have = False
        if not have:                       # reuse a verified download
            _download_wheel(rel, path)
        _remove_other_wheels(pkg, path)
        _record_downloaded(pkg, installed, ver, rel["source"],
                           want or _file_sha256(path))
        res["ok"] = True
        return res
    except Exception as e:
        msg = _errstr(e)
        if rngit_err:
            msg = "rngit: %s; %s" % (rngit_err, msg)
        res["error"] = msg[:400]
        try:
            st = read_state()
            st["error"] = res["error"]
            _write_state(st)
        except Exception:
            pass
        return res


def install_downloaded(pkg=PKG):
    """Install the pre-downloaded wheel, restart+verify rnsd, and roll back on
    any failure. Returns a result dict; never raises."""
    import shutil
    import tarfile
    t0 = time.monotonic()              # APPLY_BUDGET clock (immune to date -s / NTP)
    res = {"ok": False, "from": installed_version(pkg), "to": None,
           "error": "", "rolled_back": False}
    site = None
    touched = False
    try:
        state = read_state()
        ver = state.get("downloaded_version") or ""
        latest = state.get("latest") or ""
        path = _wheel_path(ver) if ver else ""
        res["to"] = ver or None
        if not ver or not path or not os.path.isfile(path):
            res["error"] = "no downloaded update — download it first"
            return res
        if latest and ver != latest:
            res["error"] = ("rns %s was downloaded, but %s is available — "
                            "download the newer version first" % (ver, latest))
            return res
        if not _newer(ver, res["from"]):
            res["error"] = "already up to date"
            return res
        want = (state.get("downloaded_sha256") or "").lower()
        if want and _file_sha256(path).lower() != want:
            raise RuntimeError("downloaded wheel checksum mismatch")
        requires = _wheel_requires(path)
        unmet = _unmet_deps(requires)
        if unmet:
            res["error"] = ("%s %s needs a newer %s than is installed — reflash "
                            "the image to update" % (pkg, ver, ", ".join(unmet)))
            return res
        site = _site_base(pkg)
        remove = _pkg_paths(site, pkg)
        with tarfile.open(ROLLBACK_TAR, "w") as tf:
            for p in remove:
                tf.add(p, arcname=os.path.relpath(p, site))
        # Last exit before the tree is touched: past this point the rest must
        # fit in APPLY_BUDGET, rollback included (see POST_CLEAR_WORST).
        if time.monotonic() - t0 > APPLY_BUDGET - POST_CLEAR_WORST:
            res["error"] = ("The wheel check or the SD card was too slow to "
                            "finish inside the time limit — nothing changed, "
                            "try again")
            return res
        touched = True
        for p in remove:                   # clear the old buildroot install
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        _install_wheel(path, site)
        _ver_cache.pop(pkg, None)          # re-read the package after extraction
        old_pid = _rnsd_pid() or None
        _restart_rnsd()
        if not _rnsd_up(RNSD_UP_TIMEOUT, old_pid):
            raise RuntimeError("rnsd did not come back after the update")
        loaded = installed_version(pkg)
        if loaded != ver:
            raise RuntimeError("rnsd started but reports %s instead of %s"
                               % (loaded or "unknown", ver))
        new_pid = _rnsd_pid()
        if not _rnsd_stable(new_pid, RNSD_STABLE):
            raise RuntimeError("rnsd exited within %d s of the update"
                               % RNSD_STABLE)
        res["ok"] = True
        _ver_cache[pkg] = ver          # keep the memoised version in step
        state.update({"package": pkg, "installed": ver, "latest": ver,
                      "update_available": False, "error": "",
                      "source": state.get("source", ""),
                      "checked_at": int(time.time())})
        for k in ("downloaded_version", "downloaded_at",
                  "downloaded_sha256", "downloaded_source"):
            state.pop(k, None)
        _write_state(state)
        try:
            os.remove(path)                # installed; free the rootfs
        except OSError:
            pass
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
        # Keep the downloaded wheel for a retry, and say why install failed.
        try:
            st = read_state()
            st["installed"] = res["from"]
            latest = st.get("latest") or ""
            st["update_available"] = bool(latest) and _newer(latest, res["from"])
            st["error"] = "install failed: " + res["error"]
            _write_state(st)
        except Exception:
            pass
        return res


def apply_update(pkg=PKG):
    """Compatibility wrapper: download then install. never raises."""
    dl = download_update(pkg)
    if not dl.get("ok"):
        return {"ok": False, "from": installed_version(pkg),
                "to": dl.get("version"), "error": dl.get("error", "download failed"),
                "rolled_back": False}
    return install_downloaded(pkg)


def cron(pkg=PKG):
    """Daily/boot job: check, download, and (if enabled) install."""
    st = check(pkg)
    if st.get("error"):
        print("update-check: failed (%s); installed=%s"
              % (st["error"], st.get("installed")))
        return st
    if not st.get("update_available"):
        print("update-check: %s up to date (%s)"
              % (st.get("package"), st.get("installed")))
        return st
    dl = download_update(pkg)
    if not dl.get("ok"):
        print("update-check: %s %s available, download failed (%s)"
              % (st.get("package"), st.get("latest"), dl.get("error")))
        return st
    print("update-check: %s %s downloaded via %s (installed %s)"
          % (st.get("package"), st.get("latest"),
             dl.get("source") or "?", st.get("installed")))
    if _conf_bool("update_install", False):
        res = install_downloaded(pkg)
        if res.get("ok"):
            print("update-install: rns %s installed, rnsd restarted"
                  % res.get("to"))
        elif res.get("rolled_back"):
            print("update-install: failed (%s), rolled back to %s"
                  % (res.get("error"), res.get("from")))
        else:
            print("update-install: failed (%s)" % res.get("error"))
    else:
        print("update-install: automatic installation disabled; waiting for the portal")
    return st


if __name__ == "__main__":
    # CLI for the C++ portal (and cron). Prints ONE line of JSON so the caller
    # parses it trivially:
    #   display  -> {installed, latest, update_available, error, source,
    #                downloaded_version, checked_at}
    #   check    -> queries PyPI/mirror + persists, same shape
    #   download -> {ok, version, source, error}
    #   install  -> {ok, from, to, error, rolled_back}
    #   apply    -> download+install compatibility wrapper
    #   cron     -> the daily/boot job (human summary)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "cron"
    if cmd == "display":
        print(json.dumps(display_state()))
    elif cmd == "check":
        print(json.dumps(check()))
    elif cmd == "download":
        print(json.dumps(download_update()))
    elif cmd == "install":
        print(json.dumps(install_downloaded()))
    elif cmd == "apply":
        print(json.dumps(apply_update()))
    else:
        cron()
