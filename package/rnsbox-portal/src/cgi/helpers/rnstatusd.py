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
PATH_TTL = 60.0          # known-destinations count is refreshed this often
DEST_TABLE = "/etc/reticulum/storage/destination_table"
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


def _dest_count():
    """Entries in rnsd's destination table: the portal's 'known addresses'."""
    from RNS.vendor import umsgpack
    with open(DEST_TABLE, "rb") as f:
        dest = umsgpack.unpackb(f.read())
    if isinstance(dest, (list, dict)):
        return len(dest)
    return 0


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
    # Destination count: rnsd rewrites the table on announces, so a first read
    # can catch a partial file. Retry a few times before giving up.
    path_count = None
    count_err = ""
    for _ in range(4):
        try:
            signal.alarm(20)
            path_count = _dest_count()
            break
        except Exception as e:
            count_err = str(e)
            time.sleep(0.5)
        finally:
            signal.alarm(0)
    path_at = time.monotonic()
    note = "running since %.0f (import took %.1fs)" % (started, time.time() - started)
    if path_count is None and count_err:
        note += " (dest count: %s)" % count_err
    _note(note)

    last = time.monotonic()
    cached = None
    cached_at = 0.0
    fail_count = 0
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
                # The RPC can take >10 s on a loaded C906; the portal waits
                # 25 s (45 s for the prewarm), so allow most of that here.
                signal.alarm(20)
                try:
                    stats = reticulum.get_interface_stats()
                    fail_count = 0
                finally:
                    signal.alarm(0)
                doc = _jsonable(stats)
                if path_count is not None:
                    doc["path_table_count"] = path_count
                cached = json.dumps(doc)
                cached_at = now
            # The dashboard's "known destinations" counter. The shared-
            # instance path-table RPC serialises ~1000 entries and takes tens
            # of seconds, so read rnsd's own destination table instead: it is
            # the same set of known addresses and decodes in ~2 s. Refresh it
            # on a slow clock and bolt the count onto the cached table.
            if path_count is None or now - path_at > PATH_TTL:
                ok = False
                try:
                    signal.alarm(20)
                    path_count = _dest_count()
                    ok = True
                except Exception:
                    # A partial rewrite: retry in 10 s rather than the full TTL.
                    pass
                finally:
                    signal.alarm(0)
                path_at = now if ok else now - PATH_TTL + 10
                if path_count is not None:
                    doc = json.loads(cached)
                    doc["path_table_count"] = path_count
                    cached = json.dumps(doc)
            elif path_count is not None and "path_table_count" not in cached:
                doc = json.loads(cached)
                doc["path_table_count"] = path_count
                cached = json.dumps(doc)
            conn.sendall(cached.encode("utf-8") + b"\n")
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            # The caller gave up (page closed, uhttpd client timeout). The
            # helper is still healthy and the next request must not pay a
            # fresh RNS import, so keep serving.
            continue
        except Exception as e:
            # A transient RPC timeout must not throw away a 30 s import: keep
            # serving the last snapshot and retry on the next request. Three
            # consecutive failures mean rnsd restarted (or died) and the
            # shared-instance socket is dead, so a fresh helper is needed.
            _note("query failed (%d): %s" % (fail_count + 1, e))
            fail_count += 1
            try:
                if cached is not None:
                    conn.sendall(cached.encode("utf-8") + b"\n")
            except Exception:
                pass
            if fail_count >= 3:
                break
            continue
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
