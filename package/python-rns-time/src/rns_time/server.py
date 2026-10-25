"""rns-time server: time over Reticulum, sourced from ordinary NTP servers.

This is the *minimal* variant: it does not need chrony, a GPS receiver or
root.  It periodically asks a few upstream NTP servers for the time, keeps the
measured offset against the local clock, and serves that corrected time over
Reticulum.  A host whose own clock is wrong therefore still hands out the right
time -- the offset is added to the two timestamps the client compares.

It talks to a *running rnsd* through Reticulum's shared instance (the default),
so it never opens interfaces itself:

    python3 -m rns_time.server --config /etc/reticulum \\
        --ntp pool.ntp.org,time.google.com \\
        --identity /var/lib/rns-time/identity

The destination hash is printed on startup (and written to ``--hash-file``);
that is the trust anchor clients need.  An announce is emitted only while the
upstream sample is healthy; a server in holdover serves stratum 16 instead,
which clients ignore.

Runs with the original rns-time client (protocol v1, app ``rnstime`` aspect
``time``) and with MeshAPI clients (``now`` op).
"""
import argparse
import datetime
import os
import signal
import sys
import threading
import time

import RNS

from . import config, manifest, ntp, protocol

IDENTITY_PATH_DEFAULT = os.path.expanduser("~/.rns_time/identity")
DEFAULT_NTP = "pool.ntp.org"
DEFAULT_INTERVAL = 300.0
DEFAULT_ANNOUNCE = 1800.0
# A sample older than this is not served as healthy even if no clock step is
# detected: upstream drift and rate error accumulate.
DEFAULT_MAX_AGE = 1800.0
# A local clock movement larger than this between two reads means someone
# stepped it (ntpd, the OS, an operator) and our offset no longer applies.
STEP_DETECT = 0.75


def log(message, level=RNS.LOG_NOTICE):
    # Deliberately not RNS.log(): that writes to stdout, which is
    # block-buffered when the service runs under systemd or a redirect, and
    # announcements/samples then sit in the buffer until the next flush.
    if getattr(RNS, "loglevel", RNS.LOG_NOTICE) < level:
        return
    print("%s rns-time: %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), message),
          file=sys.stderr, flush=True)


class TimeSource:
    """The offset between the local clock and upstream NTP, plus its quality.

    ``offset`` is added to local timestamps: ``served = local + offset``.
    """

    def __init__(self, servers, timeout=3.0, max_age=DEFAULT_MAX_AGE):
        self.servers = servers
        self.timeout = timeout
        self.max_age = max_age
        self._lock = threading.Lock()
        self._sample = None
        self._offset = 0.0
        self._sampled_wall = 0.0
        self._sampled_mono = 0.0
        self._valid = False
        self._last_error = "no sample yet"
        self._refresh_wanted = threading.Event()

    def refresh(self):
        sample = ntp.measure(self.servers, timeout=self.timeout)
        with self._lock:
            if sample is None:
                self._valid = False
                self._last_error = "no reply from " + ", ".join(self.servers)
                return None
            self._sample = sample
            self._offset = sample["offset"]
            self._sampled_wall = time.time()
            self._sampled_mono = time.monotonic()
            self._valid = True
            self._last_error = None
        return sample

    def request_refresh(self):
        self._refresh_wanted.set()

    def take_refresh_request(self):
        if self._refresh_wanted.is_set():
            self._refresh_wanted.clear()
            return True
        return False

    def current(self):
        """Return ``(served_unix_time, meta)``; time is None in holdover."""
        with self._lock:
            sample = self._sample
            offset = self._offset
            sampled_wall = self._sampled_wall
            sampled_mono = self._sampled_mono
            valid = self._valid
            last_error = self._last_error

        now_wall = time.time()
        now_mono = time.monotonic()
        age = max(0.0, now_mono - sampled_mono) if sample else 0.0

        meta = {
            "healthy": False,
            "age": round(age, 3),
            "error": last_error,
        }
        if not valid or sample is None:
            return None, meta

        # Detect an external step of the local clock. Rate error over a few
        # minutes is far below this threshold; a real step is not.
        if abs(now_wall - (sampled_wall + age)) > STEP_DETECT:
            with self._lock:
                self._valid = False
                self._last_error = "local clock stepped since the last sample"
            self.request_refresh()
            meta["error"] = "local clock stepped since the last sample"
            return None, meta

        if age > self.max_age:
            meta["error"] = "sample too old (%.0fs)" % age
            return None, meta

        served = now_wall + offset
        # Root dispersion: the upstream bound, half the path delays, and a
        # conservative 50 ppm allowance for the local oscillator since the
        # sample (a 5-minute-old sample adds 15 ms, not 5 minutes).
        dispersion_ms = (
            sample["root_dispersion"] + sample["root_delay"] / 2.0
            + sample["delay"] / 2.0
        ) * 1000.0 + age * 0.05 + 10.0
        stratum = min(int(sample["stratum"]) + 1, 15)
        meta.update({
            "healthy": True,
            "stratum": stratum,
            "root_disp_ms": round(dispersion_ms, 3),
            "leap": sample["leap"],
            "upstream": sample["server"],
            "upstream_stratum": sample["stratum"],
            "upstream_offset": sample["offset"],
            "upstream_delay": sample["delay"],
            "replies": sample.get("replies"),
            "queried": sample.get("queried"),
        })
        return served, meta

    def status(self):
        _served, meta = self.current()
        return meta


class Server:
    def __init__(self, source, identity_path, hash_file=None):
        self.source = source
        self.identity_path = identity_path
        self.hash_file = hash_file
        self.destination = None
        self.dest_hex = None
        self.last_announce = 0.0
        self.announced_healthy = False

    def setup(self):
        self._load_identity()
        self.destination = RNS.Destination(
            self.identity,
            RNS.Destination.IN,
            RNS.Destination.SINGLE,
            protocol.APP_NAME,
            *protocol.ASPECTS,
        )
        self.dest_hex = RNS.hexrep(self.destination.hash, delimit=False)
        self.destination.set_link_established_callback(self._on_link)
        self.destination.register_request_handler(
            protocol.MESHAPI_PATH,
            response_generator=self._on_meshapi_request,
            allow=RNS.Destination.ALLOW_ALL,
        )
        log("serving as " + RNS.prettyhexrep(self.destination.hash))
        print("rns-time destination: " + self.dest_hex, flush=True)
        if self.hash_file:
            self._write_hash_file(self.dest_hex)
        return self.destination

    def _load_identity(self):
        directory = os.path.dirname(os.path.abspath(self.identity_path))
        if directory:
            os.makedirs(directory, mode=0o700, exist_ok=True)
        if os.path.isfile(self.identity_path):
            self.identity = RNS.Identity.from_file(self.identity_path)
            if self.identity is None:
                raise RuntimeError("could not load identity " + self.identity_path)
        else:
            self.identity = RNS.Identity()
            self.identity.to_file(self.identity_path)
            log("created identity " + self.identity_path)

    def _write_hash_file(self, dest_hex):
        try:
            directory = os.path.dirname(os.path.abspath(self.hash_file))
            if directory:
                os.makedirs(directory, exist_ok=True)
            tmp = "%s.%d.tmp" % (self.hash_file, os.getpid())
            with open(tmp, "w", encoding="ascii") as handle:
                handle.write(dest_hex + "\n")
            os.replace(tmp, self.hash_file)
        except OSError as exc:
            log("could not write %s: %s" % (self.hash_file, exc), RNS.LOG_WARNING)

    def announce(self, force=False, announce_interval=DEFAULT_ANNOUNCE):
        served, meta = self.source.current()
        now = time.monotonic()
        if meta.get("healthy"):
            if force or not self.announced_healthy or now - self.last_announce >= announce_interval:
                self.destination.announce()
                self.last_announce = now
                self.announced_healthy = True
                log("announced (upstream %s stratum %s -> served stratum %s, offset %+.3fs)"
                    % (meta.get("upstream"), meta.get("upstream_stratum"),
                       meta.get("stratum"), meta.get("upstream_offset")))
        else:
            log("not announcing: " + str(meta.get("error")), RNS.LOG_WARNING)
            self.announced_healthy = False

    # ---- RNS callbacks -------------------------------------------------
    def _on_link(self, link):
        link.set_packet_callback(self._on_packet)

    def _on_packet(self, data, packet):
        try:
            req = protocol.unpack(data)
        except Exception:
            return
        if not isinstance(req, dict) or req.get("v") != protocol.VERSION or "t1" not in req:
            return
        t2_served, meta = self.source.current()
        if t2_served is None:
            # Holdover: honest stratum 16, uncorrected local clock. Clients
            # with MAX_HEALTHY_STRATUM = 9 refuse to apply it.
            t2 = time.time()
            stratum, disp, leap = 16, None, "Unknown"
        else:
            t2 = t2_served
            stratum = meta["stratum"]
            disp = meta["root_disp_ms"]
            leap = meta["leap"]
        t3_served, meta3 = self.source.current()
        t3 = time.time() if t3_served is None else t3_served
        if meta3.get("healthy"):
            # Use the freshest quality figure for the send timestamp.
            stratum = meta3["stratum"]
            disp = meta3["root_disp_ms"]
            leap = meta3["leap"]
        try:
            RNS.Packet(packet.link,
                       protocol.pack_response(req, stratum, disp, leap,
                                              t2=t2, t3=t3)).send()
        except Exception as exc:
            log("reply failed: %s" % exc, RNS.LOG_DEBUG)

    def _on_meshapi_request(self, path, data, request_id, link_id,
                            remote_identity, requested_at):
        try:
            req = protocol.unpack(data)
        except Exception:
            return protocol.pack({"v": protocol.VERSION, "ok": False,
                                  "err": "bad_encoding"})
        if not isinstance(req, dict):
            return protocol.pack({"v": protocol.VERSION, "ok": False,
                                  "err": "bad_encoding"})
        if req.get("op") == protocol.MANIFEST_OP:
            return protocol.pack({"v": protocol.VERSION, "ok": True,
                                  "manifest": manifest.manifest_for(self.dest_hex)})
        if req.get("op") == "now":
            served, meta = self.source.current()
            if served is None:
                resp = {"v": protocol.VERSION, "ok": True,
                        "res": {"utc": datetime.datetime.now(
                                    datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                                "epoch": time.time(), "stratum": 16,
                                "root_disp_ms": None, "leap": "Unknown"},
                        "degraded": True}
            else:
                resp = {"v": protocol.VERSION, "ok": True,
                        "res": {"utc": datetime.datetime.fromtimestamp(
                                    served, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                                "epoch": served, "stratum": meta["stratum"],
                                "root_disp_ms": meta["root_disp_ms"],
                                "leap": meta["leap"]}}
            return protocol.pack(resp)
        return protocol.pack({"v": protocol.VERSION, "ok": False, "err": "bad_op"})


def _parse_args(argv):
    ap = argparse.ArgumentParser(
        description="minimal rns-time server (upstream NTP -> Reticulum)")
    defaults = {
        "identity": IDENTITY_PATH_DEFAULT,
        "ntp": DEFAULT_NTP,
        "interval": DEFAULT_INTERVAL,
        "announce": DEFAULT_ANNOUNCE,
        "max_age": DEFAULT_MAX_AGE,
        "timeout": 3.0,
    }
    conf_path = None
    # A first pass finds --conf so the file can supply defaults.
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--conf", default=None)
    known, _ = pre.parse_known_args(argv)
    if known.conf:
        conf = config.load(known.conf)
        conf_path = known.conf
        if conf.get("identity"):
            defaults["identity"] = conf["identity"]
        if conf.get("ntp") or conf.get("upstream"):
            defaults["ntp"] = conf.get("ntp") or conf.get("upstream")
        for key in ("interval", "announce", "max_age", "timeout"):
            if conf.get(key):
                try:
                    defaults[key] = float(conf[key])
                except ValueError:
                    pass

    ap.add_argument("--identity", default=defaults["identity"],
                    help="identity file (its hash is the client trust anchor)")
    ap.add_argument("--conf", default=conf_path, help="KEY=VALUE config file")
    ap.add_argument("--ntp", default=defaults["ntp"],
                    help="comma-separated upstream NTP servers")
    ap.add_argument("--interval", type=float, default=defaults["interval"],
                    help="seconds between upstream samples (default 300)")
    ap.add_argument("--announce", type=float, default=defaults["announce"],
                    help="seconds between announces (default 1800)")
    ap.add_argument("--max-age", type=float, default=defaults["max_age"],
                    help="serve holdover once the sample is older than this")
    ap.add_argument("--timeout", type=float, default=defaults["timeout"],
                    help="per-NTP-server timeout")
    ap.add_argument("--config", default=None,
                    help="RNS config dir (default: %s)" % (
                        RNS.Reticulum.configdir if hasattr(RNS.Reticulum, "configdir") else "default"))
    ap.add_argument("--standalone", action="store_true",
                    help="open interfaces from --config instead of requiring rnsd")
    ap.add_argument("--hash-file", default=None,
                    help="write the destination hash here")
    ap.add_argument("--loglevel", type=int, default=None)
    args = ap.parse_args(argv)
    args.servers = config.csv(args.ntp)
    if not args.servers:
        ap.error("--ntp needs at least one server")
    if args.interval < 15:
        args.interval = 15.0
    return args


def main(argv=None):
    args = _parse_args(argv if argv is not None else sys.argv[1:])

    stop = threading.Event()

    def _term(_signum, _frame):
        stop.set()

    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)

    try:
        RNS.Reticulum(configdir=args.config, loglevel=args.loglevel,
                      require_shared_instance=not args.standalone)
    except Exception as exc:
        print("rns-time: cannot start Reticulum: %s" % exc, file=sys.stderr)
        return 1

    instance = RNS.Reticulum.get_instance()
    if not args.standalone and not getattr(instance, "is_connected_to_shared_instance", False):
        print("rns-time: rnsd is not running (server must use its shared instance)",
              file=sys.stderr)
        return 1

    source = TimeSource(args.servers, timeout=args.timeout, max_age=args.max_age)
    first = source.refresh()
    if first is not None:
        log("upstream %s: offset %+.3fs, delay %.3fs, stratum %d"
            % (first["server"], first["offset"], first["delay"], first["stratum"]))
    else:
        log("no upstream NTP reply yet; holding over (stratum 16)",
            RNS.LOG_WARNING)

    server = Server(source, args.identity, hash_file=args.hash_file)
    try:
        server.setup()
    except Exception as exc:
        print("rns-time: destination setup failed: %s" % exc, file=sys.stderr)
        return 1
    server.announce(force=True, announce_interval=args.announce)

    while not stop.is_set():
        if source.take_refresh_request():
            wait = 2.0
        else:
            wait = args.interval
        if stop.wait(wait):
            break
        sample = source.refresh()
        if sample is None:
            log("no upstream reply; serving holdover (stratum 16)", RNS.LOG_WARNING)
        else:
            log("sample %s: offset %+.3fs, delay %.3fs, stratum %d"
                % (sample["server"], sample["offset"], sample["delay"], sample["stratum"]))
        server.announce(announce_interval=args.announce)

    log("stopping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
