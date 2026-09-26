#!/usr/bin/env python3
"""halow.py — live HaLow (RNode) modem status over SLIP, printed as JSON.

Standalone helper (no rnsbox_portal package, stdlib only). The C++ portal shells
to it for the /reticulum/halow AJAX endpoint (dashboard + Reticulum-tab cards).
It reads /etc/rnsbox/slip.conf directly and never raises — a slow/absent modem
just yields {"reachable": false, ...}. Mirrors the old sysinfo.halow_status().
"""
import json
import os
import urllib.request

SLIP_CONF = "/etc/rnsbox/slip.conf"


def read_slip():
    cfg = {"enabled": "no", "device": "/dev/serial0", "baud": "2000000",
           "local_ip": "192.168.7.1", "peer_ip": "192.168.7.2"}
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


def _modem_api(peer, name, timeout=2.5):
    try:
        req = urllib.request.Request("http://%s/api/%s" % (peer, name),
                                     headers={"Accept-Encoding": "identity"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001 - modem down / SLIP not up / bad json
        return None


def halow_status(slip):
    peer = (slip.get("peer_ip") or "192.168.7.2").strip()
    enabled = (slip.get("enabled", "no").strip().lower()
               not in ("no", "0", "false", "off", ""))
    out = {"enabled": enabled, "peer": peer,
           "local": (slip.get("local_ip") or "").strip(),
           "sl0_up": _oper_up("sl0"), "reachable": False, "neighbours": []}
    if not out["enabled"] or not out["sl0_up"]:
        return out
    stat = _modem_api(peer, "get_stat")
    allc = _modem_api(peer, "get_all")
    near = _modem_api(peer, "get_nearby_modems")
    if stat is None and allc is None:
        return out
    out["reachable"] = True
    h = (allc or {}).get("halow", {})
    out["radio"] = {"freq": h.get("central_freq"), "bw": h.get("bandwidth"),
                    "mcs": h.get("mcs_index"), "power": h.get("power_dbm")}
    tcp = (allc or {}).get("tcp", {})
    conn = tcp.get("connected")
    out["tcp_port"] = tcp.get("port")
    out["rnsd_up"] = bool(conn and conn != "no connection")
    out["connected"] = conn
    d = (stat or {}).get("device", {})
    r = (stat or {}).get("radio", {})
    out["device"] = {"temp": d.get("chip_temp"), "fw": d.get("ver"),
                     "uptime": d.get("uptime"), "hostname": d.get("hostname")}
    out["stats"] = {"airtime": r.get("airtime"), "ch_util": r.get("ch_util"),
                    "noise": r.get("bg_pwr_dbm"), "rx_speed": r.get("rx_speed"),
                    "tx_speed": r.get("tx_speed"), "rx_bytes": r.get("rx_bytes"),
                    "tx_bytes": r.get("tx_bytes")}
    peers = []
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
