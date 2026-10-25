"""rns-time client: fetch authenticated time from a rns-time server over Reticulum.

    python3 -m rns_time.client <server_hash> [<server_hash> ...] [options]

Measurement only:
    python3 -m rns_time.client e210fb... --rounds 5

Step the clock (root; ``--apply`` also records the clock source and writes an
RTC, which is what the rnsbox init script uses):
    python3 -m rns_time.client e210fb... --apply --config /etc/reticulum --shared \\
        --source-file /run/clock-source --synced-file /run/clock-synced \\
        --lock /run/ntpsync.step.lock --rtc /dev/rtc0 --respect-source

Exit codes: 0 ok/applied/skipped, 1 no path or link, 2 no samples,
3 server unhealthy, 4 could not set the clock, 5 clock-step lock busy.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time

import RNS

from . import config, protocol

CLOCK_FLOOR = 1577836800   # 2020-01-01
CLOCK_CEIL = 4102444800    # 2100-01-01
DEFAULT_TIMEOUT = 90.0


class Exchange:
    def __init__(self):
        self.event = threading.Event()
        self.response = None
        self.t4 = None

    def on_packet(self, data, packet):
        self.t4 = time.time()  # stamp arrival first
        self.response = data
        self.event.set()


def _read_source(path):
    if not path:
        return ""
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return handle.read().strip()
    except OSError:
        return ""


def _write_atomic(path, data, mode=0o644):
    tmp = "%s.%d.tmp" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as handle:
        handle.write(data)
    try:
        os.chmod(tmp, mode)
    except OSError:
        pass
    os.replace(tmp, path)


def _clock_plausible(now=None):
    now = time.time() if now is None else now
    return CLOCK_FLOOR <= now < CLOCK_CEIL


def _step_clock(target):
    """Set CLOCK_REALTIME to ``target`` (Unix seconds). Returns (ok, error)."""
    try:
        import ctypes

        class Timespec(ctypes.Structure):
            _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]

        libc = ctypes.CDLL(None, use_errno=True)
        seconds = int(target)
        nanoseconds = int(round((target - seconds) * 1e9))
        if nanoseconds >= 1000000000:
            seconds += 1
            nanoseconds -= 1000000000
        ts = Timespec(seconds, nanoseconds)
        CLOCK_REALTIME = 0
        if libc.clock_settime(CLOCK_REALTIME, ctypes.byref(ts)) == 0:
            return True, None
        error = ctypes.get_errno()
        ctypes_error = os.strerror(error)
    except Exception as exc:  # Windows, or a libc without clock_settime
        ctypes_error = str(exc)

    # Portable fallback: whole seconds (busybox date, Windows date).
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(int(target)))
    try:
        result = subprocess.run(["date", "-u", "-s", stamp],
                                capture_output=True, text=True)
        if result.returncode == 0:
            return True, None
        return False, (result.stderr or result.stdout or ctypes_error).strip()
    except Exception as exc:
        return False, str(exc)


class StepLock:
    """fcntl lock around the clock step, shared with S45ntpsync and the portal."""

    def __init__(self, path, timeout=60.0):
        self.path = path
        self.timeout = timeout
        self.fd = None

    def __enter__(self):
        if not self.path:
            return self
        import fcntl

        self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        deadline = time.time() + self.timeout
        while True:
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except OSError:
                if time.time() >= deadline:
                    os.close(self.fd)
                    self.fd = None
                    raise TimeoutError("clock-step lock is busy")
                time.sleep(0.25)

    def __exit__(self, _exc_type, _exc, _tb):
        if self.fd is not None:
            import fcntl

            fcntl.flock(self.fd, fcntl.LOCK_UN)
            os.close(self.fd)
            self.fd = None


def _write_rtc(device):
    """Persist the (just verified) clock in an RTC. Returns a status string."""
    if not device or not os.path.exists(device):
        return None
    command = "/sbin/hwclock" if os.path.exists("/sbin/hwclock") else "hwclock"
    try:
        result = subprocess.run([command, "-w", "-u", "-f", device],
                                capture_output=True, text=True)
    except Exception as exc:
        return "rtc write failed: %s" % exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "exit %d" % result.returncode).strip()
        return "rtc write failed: %s" % detail
    return "rtc written"


def _split_targets(values):
    out = []
    for value in values:
        out.extend(config.csv(value))
    return out


def measure(rns_destination_hex, timeout, rounds, quiet=False):
    """Open a Link and run ``rounds`` exchanges.

    Returns (samples, error) where samples is [(offset, delay, meta), ...].
    """
    try:
        dest_hash = bytes.fromhex(rns_destination_hex)
    except ValueError:
        return [], "invalid destination hash"
    if len(dest_hash) != 16:
        return [], "destination hash must be 32 hex characters"

    if not quiet:
        print("requesting path to %s..." % rns_destination_hex, file=sys.stderr)
    if not RNS.Transport.has_path(dest_hash):
        RNS.Transport.request_path(dest_hash)
        deadline = time.time() + timeout
        next_request = time.time() + 2.0
        while not RNS.Transport.has_path(dest_hash):
            if time.time() > deadline:
                return [], "no path to server"
            if time.time() >= next_request:
                RNS.Transport.request_path(dest_hash)
                next_request = time.time() + 3.0
            time.sleep(0.25)

    identity = RNS.Identity.recall(dest_hash)
    if identity is None:
        return [], "path known but no identity for that destination"
    dest = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
                           protocol.APP_NAME, *protocol.ASPECTS)

    established = threading.Event()
    closed = threading.Event()

    link = RNS.Link(dest,
                    established_callback=lambda _link: established.set(),
                    closed_callback=lambda _link: closed.set())
    if not established.wait(timeout):
        try:
            link.teardown()
        except Exception:
            pass
        return [], "link establishment timed out"

    samples = []
    try:
        for index in range(rounds):
            exchange = Exchange()
            link.set_packet_callback(exchange.on_packet)
            payload, t1 = protocol.pack_request()
            RNS.Packet(link, payload).send()
            if not exchange.event.wait(timeout):
                if not quiet:
                    print("round %d: timeout" % (index + 1), file=sys.stderr)
                if closed.is_set():
                    return samples, "link closed by the server"
                continue
            try:
                reply = protocol.unpack(exchange.response)
                if reply.get("t1") != t1:
                    # A late reply from the previous round.
                    continue
                offset, delay = protocol.compute(reply["t1"], reply["t2"],
                                                 reply["t3"], exchange.t4)
                if delay < 0:
                    continue
                samples.append((offset, delay, reply))
                if not quiet:
                    print("round %d: offset %+.4fs  delay %.4fs  "
                          "(stratum %s, root_disp %sms)"
                          % (index + 1, offset, delay, reply.get("stratum"),
                             reply.get("root_disp_ms")), file=sys.stderr)
            except Exception as exc:
                if not quiet:
                    print("round %d: bad response (%s)" % (index + 1, exc),
                          file=sys.stderr)
            if not quiet:
                # No point in waiting between samples on a fast link.
                time.sleep(0.2)
    finally:
        try:
            link.teardown()
        except Exception:
            pass

    return samples, None


def apply_clock(offset, best_meta, args, result):
    """Apply a healthy measurement: step, record the source, write the RTC."""
    result.update({"applied": False, "skipped": None, "rtc": None})

    source_before = _read_source(args.source_file)
    if args.respect_source and source_before == "ntp":
        result["skipped"] = "standard NTP has already synced the clock"
        return 0

    try:
        with StepLock(args.lock, args.lock_timeout):
            # Re-read under the lock: an NTP one-shot may have stepped just now.
            source_now = _read_source(args.source_file)
            if args.respect_source and source_now == "ntp":
                result["skipped"] = "standard NTP has already synced the clock"
                return 0

            now = time.time()
            target = now + offset
            needs_step = (not _clock_plausible(now)) or abs(offset) >= args.min_offset

            if needs_step:
                ok, error = _step_clock(target)
                if not ok:
                    result["error"] = "could not set the clock: %s" % error
                    return 4
                result["applied"] = True
                result["clock_set"] = True
                result["target_epoch"] = target
                result["target_utc"] = time.strftime("%Y-%m-%d %H:%M:%S",
                                                     time.gmtime(target))
            else:
                result["applied"] = True
                result["clock_set"] = False

            if args.source_file:
                try:
                    _write_atomic(args.source_file, "rns\n")
                    if args.synced_file:
                        _write_atomic(args.synced_file, "")
                except OSError as exc:
                    result["error"] = "could not record the clock source: %s" % exc
                    return 4

            result["rtc"] = _write_rtc(args.rtc)
            return 0
    except TimeoutError as exc:
        result["error"] = str(exc)
        return 5
    except OSError as exc:
        result["error"] = str(exc)
        return 4


def _parse_args(argv):
    ap = argparse.ArgumentParser(
        description="fetch authenticated time over Reticulum")
    ap.add_argument("servers", nargs="*", help="server destination hash(es), hex")
    ap.add_argument("--server", action="append", default=[],
                    help="add a destination hash (repeatable)")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                    help="per-step timeout in seconds (LoRa needs patience)")
    ap.add_argument("--config", default=None, help="RNS config dir")
    ap.add_argument("--rns-loglevel", type=int, default=0,
                    help="RNS internal log level (0 = silent, 4 = info)")
    ap.add_argument("--shared", action="store_true",
                    help="require the running rnsd shared instance")
    ap.add_argument("--conf", default=None, help="KEY=VALUE config file")
    ap.add_argument("--json", action="store_true", help="machine-readable stdout")
    ap.add_argument("--quiet", action="store_true", help="no progress on stderr")
    ap.add_argument("--set", action="store_true", help="step the local clock (root)")
    ap.add_argument("--apply", action="store_true",
                    help="step + record the clock source + write the RTC")
    ap.add_argument("--min-offset", type=float, default=0.0,
                    help="do not step when |offset| is below this (seconds)")
    ap.add_argument("--respect-source", action="store_true",
                    help="leave the clock alone when /run/clock-source says ntp")
    ap.add_argument("--source-file", default=None,
                    help="clock-source file to read/write (rnsbox: /run/clock-source)")
    ap.add_argument("--synced-file", default=None,
                    help="marker created after a successful apply")
    ap.add_argument("--lock", default=None,
                    help="lock file serialising clock steps")
    ap.add_argument("--lock-timeout", type=float, default=60.0)
    ap.add_argument("--rtc", default=None,
                    help="RTC device to write with hwclock (e.g. /dev/rtc0)")
    args = ap.parse_args(argv)

    conf = config.load(args.conf)
    if not args.servers and not args.server and conf.get("servers"):
        args.server = [conf["servers"]]
    args.servers = _split_targets(list(args.servers) + args.server)
    if not args.servers:
        ap.error("at least one server destination hash is required")
    if args.rounds < 1:
        args.rounds = 1
    if args.apply:
        args.set = True
    return args


def main(argv=None):
    args = _parse_args(argv if argv is not None else sys.argv[1:])
    result = {
        "servers": args.servers,
        "healthy": False,
        "applied": False,
        "offset": None,
        "delay": None,
        "stratum": None,
        "root_disp_ms": None,
        "leap": None,
        "server": None,
    }

    try:
        RNS.Reticulum(configdir=args.config, loglevel=args.rns_loglevel,
                      require_shared_instance=args.shared)
    except Exception as exc:
        print("rns-time: cannot start Reticulum: %s" % exc, file=sys.stderr)
        return 1
    if args.shared:
        instance = RNS.Reticulum.get_instance()
        if not getattr(instance, "is_connected_to_shared_instance", False):
            print("rns-time: rnsd is not running (shared instance required)",
                  file=sys.stderr)
            return 1

    samples = []
    errors = []
    used_server = None
    for server in args.servers:
        got, error = measure(server, args.timeout, args.rounds, quiet=args.quiet)
        if got:
            samples = got
            used_server = server
            break
        errors.append("%s: %s" % (server, error or "no samples"))
        if not args.quiet:
            print("rns-time: %s failed (%s)" % (server, error or "no samples"),
                  file=sys.stderr)

    if not samples:
        result["error"] = "; ".join(errors) if errors else "no samples"
        if args.json:
            print(json.dumps(result), flush=True)
        else:
            print("no valid samples from any server")
        return 2

    offset, delay, meta = protocol.best_sample(samples)
    stratum = int(meta.get("stratum", 16))
    healthy = stratum <= protocol.MAX_HEALTHY_STRATUM
    result.update({
        "server": used_server,
        "healthy": healthy,
        "offset": offset,
        "delay": delay,
        "stratum": stratum,
        "root_disp_ms": meta.get("root_disp_ms"),
        "leap": meta.get("leap"),
        "rounds": len(samples),
        "server_utc": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(time.time() + offset)),
    })

    if not args.quiet and not args.json:
        print("\nBEST: offset %+.4fs (local clock is %s server)  delay %.4fs"
              % (offset, "behind" if offset > 0 else "ahead of", delay))
        print("      server %s, stratum %d, root_disp %sms, leap %s"
              % (used_server, stratum, meta.get("root_disp_ms"), meta.get("leap")))

    if not healthy:
        result["error"] = "server is in holdover (stratum %d)" % stratum
        if args.json:
            print(json.dumps(result), flush=True)
        else:
            print("server is UNHEALTHY (holdover), not applying")
        return 3

    if args.set:
        code = apply_clock(offset, meta, args, result)
        if code != 0:
            if args.json:
                print(json.dumps(result), flush=True)
            else:
                print("apply failed: %s" % result.get("error"))
            return code
        if not args.quiet and not args.json:
            if result.get("skipped"):
                print("clock left alone: %s" % result["skipped"])
            elif result.get("clock_set"):
                print("clock stepped to %s UTC (source=rns)"
                      % result.get("target_utc"))
            else:
                print("offset %.3fs is below the step threshold; clock left as is"
                      % offset)
            if result.get("rtc"):
                print(result["rtc"])

    if args.json:
        print(json.dumps(result), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
