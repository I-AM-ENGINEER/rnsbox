#!/usr/bin/env python3
"""halow.py — live HaLow (RNode) modem status over SLIP, printed as JSON.

Standalone helper (no rnsbox_portal package, stdlib only). The C++ portal shells
to it for the /reticulum/halow AJAX endpoint (dashboard + Reticulum-tab cards).
It reads /etc/rnsbox/slip.conf directly and never raises — a slow/absent modem
just yields {"reachable": false, ...}. Mirrors the old sysinfo.halow_status().
"""
import json
import os
import socket

SLIP_CONF = "/etc/rnsbox/slip.conf"


def read_slip():
    cfg = {"enabled": "no", "mode": "slip", "device": "/dev/serial0",
           "baud": "2000000", "local_ip": "192.168.7.1",
           "peer_ip": "192.168.7.2", "tcp_port": "4242"}
    try:
        with open(SLIP_CONF) as f:
            for ln in f:
                ln = ln.strip()
                if not ln or ln.startswith("#") or "=" not in ln:
                    continue
                k, v = ln.split("=", 1)
                cfg[k.strip()] = v.strip()
    except OSError:
        pass
    return cfg


def _slurp(p):
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return ""


def _oper_up(dev):
    base = "/sys/class/net/" + dev
    if not os.path.isdir(base):
        return False
    st = _slurp(base + "/operstate").strip()
    if st == "up":
        return True
    if st == "unknown":
        try:
            iff = int((_slurp(base + "/flags").strip() or "0"), 16) & 0x1
        except ValueError:
            iff = 0
        return bool(iff) and _slurp(base + "/carrier").strip() == "1"
    return False


def _modem_api(peer, name, timeout=1.8):
    # A minimal HTTP/1.0 client over a raw socket: importing urllib.request
    # costs several seconds on this CPU (it drags in email/ssl), which made
    # the helper miss the portal's 10 s deadline and the card flip to DOWN.
    sock = None
    try:
        sock = socket.create_connection((peer, 80), timeout=timeout)
        req = ("GET /api/%s HTTP/1.0\r\nHost: %s\r\n"
               "Accept-Encoding: identity\r\nConnection: close\r\n\r\n"
               % (name, peer))
        sock.sendall(req.encode("ascii"))
        chunks = []
        while True:
            b = sock.recv(65536)
            if not b:
                break
            chunks.append(b)
        raw = b"".join(chunks)
        head, _, body = raw.partition(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0]
        if b" 200 " not in status + b" ":
            return None
        if b"content-encoding: gzip" in head.lower():
            import gzip   # only the modem UI gzips; API JSON is plain
            body = gzip.decompress(body)
        return json.loads(body.decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001 - modem down / SLIP not up / bad json
        return None
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass


def halow_status(slip):
    peer = (slip.get("peer_ip") or "192.168.7.2").strip()
    mode = (slip.get("mode") or "slip").strip().lower()
    network = mode == "ip"
    enabled = (slip.get("enabled", "no").strip().lower()
               not in ("no", "0", "false", "off", ""))
    out = {"enabled": enabled, "peer": peer, "mode": mode,
           "local": (slip.get("local_ip") or "").strip(),
           "sl0_up": _oper_up("sl0"), "reachable": False, "neighbours": []}
    if not out["enabled"]:
        return out
    # UART mode needs sl0 before anything can be asked. Network mode has no
    # sl0 at all — the modem's own API is the liveness probe.
    if not network and not out["sl0_up"]:
        return out
    # Sequential keeps the import list tiny (no concurrent.futures); each call
    # has a short timeout, so a dead link costs at most ~5 s, not 10.
    stat = _modem_api(peer, "get_stat")
    allc = _modem_api(peer, "get_all")
    near = _modem_api(peer, "get_nearby_modems")
    if stat is None and allc is None:
        return out
    out["reachable"] = True

    def pick(*vals):
        for v in vals:
            if v is not None and v != "":
                return v
        return None

    # get_stat's radio block is authoritative for counters; the radio
    # configuration lives in get_all, but this firmware also echoes it in
    # get_stat. Fall back to it so a timed-out get_all does not blank the
    # Radio row with "? ? ?".
    h = (allc or {}).get("halow", {}) or {}
    r = (stat or {}).get("radio", {}) or {}
    out["radio"] = {
        "freq": pick(h.get("central_freq"), r.get("central_freq"), r.get("freq")),
        "bw": pick(h.get("bandwidth"), r.get("bandwidth"), r.get("bw")),
        "mcs": pick(h.get("mcs_index"), r.get("mcs_index"), r.get("mcs")),
        "power": pick(h.get("power_dbm"), r.get("power_dbm"), r.get("power")),
    }
    tcp = (allc or {}).get("tcp", {})
    conn = tcp.get("connected")
    out["tcp_port"] = tcp.get("port")
    out["rnsd_up"] = bool(conn and conn != "no connection")
    out["connected"] = conn
    d = (stat or {}).get("device", {}) or {}
    out["device"] = {"temp": d.get("chip_temp"), "fw": d.get("ver"),
                     "uptime": d.get("uptime"), "hostname": d.get("hostname")}
    out["stats"] = {"airtime": r.get("airtime"), "ch_util": r.get("ch_util"),
                    "noise": pick(r.get("bg_pwr_dbm"), r.get("noise")),
                    "rx_speed": r.get("rx_speed"),
                    "tx_speed": r.get("tx_speed"), "rx_bytes": r.get("rx_bytes"),
                    "tx_bytes": r.get("tx_bytes")}
    peers = []
    # NB: rx_last_age is in SECONDS (the modem's own UI formats it with
    # formatNearbyLastSeen(seconds)); the portal pages render it as such.
    for n in ((near or {}).get("d") or []):
        peers.append({"mac": n.get("mac"), "rssi": n.get("rx_rssi"),
                      "snr": n.get("rx_snr"), "rx_mcs": n.get("rx_mcs"),
                      "tx_mcs": n.get("tx_mcs"), "tx_acked": n.get("tx_acked"),
                      "tx_frames": n.get("tx_frames"), "tx_loss": n.get("tx_loss_pct"),
                      "age": n.get("rx_last_age")})
    out["neighbours"] = peers
    if peers:
        out["best"] = max(
            peers,
            key=lambda p: p["rssi"] if isinstance(p["rssi"], (int, float)) else -9999)
    return out


if __name__ == "__main__":
    print(json.dumps(halow_status(read_slip())))
