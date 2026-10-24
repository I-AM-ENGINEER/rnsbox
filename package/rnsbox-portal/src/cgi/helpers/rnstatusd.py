#!/usr/bin/python3
"""rnstatusd.py — resident rnstatus provider for the portal.

A cold `rnstatus` run costs a full RNS import before it can ask the shared
instance for its interface table.  On the C906 that is ~4 s of CPU and, under
load, 15-20 s of wall time; the portal polls the table every few seconds, so
starting Python per poll keeps the single core saturated.  This helper pays
the import once, then answers every request from the already-connected shared
instance over a Unix socket.

It starts on demand (the portal's background refresher launches it), idles
out after IDLE_TIMEOUT seconds without a request, and refuses to start on
boards below MIN_MEM_KB — there the portal keeps using the cached CLI path.

Protocol: one request line "stats\\n", one JSON line back.  The JSON is the
same document `rnstatus -a -j` prints, so the portal parses both identically.
"""
import fcntl
import json
import os
import signal
import socket
import sys
import time

SOCK = "/run/rnsbox-rnstatusd.sock"
LOCK = "/run/rnsbox-rnstatusd.lock"
LOG = "/run/rnsbox-rnstatusd.log"
IDLE_TIMEOUT = 300       # seconds without a request before exiting
QUERY_TTL = 1.0          # collapse bursts from several open browser tabs
MIN_MEM_KB = 180 * 1024  # 64 MB boards keep using the CLI fallback
MAX_REQ = 64


def _mem_total_kb():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1])
    except Exception:
        pass
    return 0


def _note(reason):
    """Leave a short breadcrumb; stderr is /dev/null under the launcher."""
    try:
        with open(LOG, "w") as f:
            f.write("%s: %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), reason))
        os.chmod(LOG, 0o600)
    except Exception:
        pass


def _jsonable(stats):
    """Bytes to hex, exactly like rnstatus -j does before json.dumps()."""
    for key in stats:
        if isinstance(stats[key], bytes):
            stats[key] = stats[key].hex()
        if isinstance(stats[key], (dict, list)):
            for item in stats[key]:
                if isinstance(item, dict):
                    for k in item:
                        if isinstance(item[k], bytes):
                            item[k] = item[k].hex()
    return stats


class _Timeout(Exception):
    pass


def _on_alarm(signum, frame):
    raise _Timeout("RNS call timed out")


def main():
    # rnsd may be restarted (or wedged) while we hold a shared-instance
    # socket. A blocking recv then never returns and the helper would keep
    # the socket forever; alarm() turns that into an exception and a clean
    # exit, after which the portal starts a fresh helper.
    signal.signal(signal.SIGALRM, _on_alarm)

    if _mem_total_kb() < MIN_MEM_KB:
        _note("MemTotal below %d kB; using the CLI fallback" % MIN_MEM_KB)
        return 0
    try:
        os.nice(5)
    except Exception:
        pass

    # One helper at a time. The lock is held for the process lifetime; the
    # socket is unlinked only after the lock is ours, so a second start can
    # never remove the socket of a live helper.
    try:
        lock = open(LOCK, "a+")
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except Exception:
        return 0

    started = time.time()
    try:
        signal.alarm(90)   # a cold import plus a wedged shared instance
        import RNS
        reticulum = RNS.Reticulum(require_shared_instance=True, loglevel=3)
    except Exception as e:
        _note("RNS start failed: %s" % e)
        return 1
    finally:
        signal.alarm(0)

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    try:
        srv.bind(SOCK)
    except OSError as e:
        _note("bind %s failed: %s" % (SOCK, e))
        return 1
    os.chmod(SOCK, 0o600)
    srv.listen(8)
    srv.settimeout(5)
    _note("running since %.0f (import took %.1fs)" % (started, time.time() - started))

    last = time.monotonic()
    cached = None
    cached_at = 0.0
    while True:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            if time.monotonic() - last > IDLE_TIMEOUT:
                break
            continue

        last = time.monotonic()
        try:
            conn.settimeout(10)
            req = conn.recv(MAX_REQ)
            if not req:
                continue
            now = time.monotonic()
            if cached is None or now - cached_at > QUERY_TTL:
                signal.alarm(10)
                try:
                    stats = reticulum.get_interface_stats()
                finally:
                    signal.alarm(0)
                cached = json.dumps(_jsonable(stats))
                cached_at = now
            conn.sendall(cached.encode("utf-8") + b"\n")
        except Exception as e:
            # A failed RPC means rnsd restarted (or is gone): exit and let the
            # portal start a fresh helper with a new shared-instance socket.
            _note("query failed: %s" % e)
            break
        finally:
            conn.close()

    try:
        srv.close()
    except Exception:
        pass
    try:
        os.unlink(SOCK)
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
