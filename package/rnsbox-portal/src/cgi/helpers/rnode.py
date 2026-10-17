#!/usr/bin/env python3
"""rnode.py — BLE discovery / pairing for RNode modems, printed as JSON.

Standalone helper (no rnsbox package; bleak only when it does BLE work). The
C++ portal shells out to it for the /reticulum/rnode/scan and /pair endpoints.
It never raises: a missing BLE stack or a shy modem yields {"error": ...} /
{"ok": false, ...} instead.

RNS >= 1.5.7 speaks RNode-over-BLE natively (RNodeInterface port = ble://...,
Nordic UART Service) — rnsd scans, connects and bonds peers on its own (the
bundled BLE Agent handles BlueZ JustWorks during the connect). This helper is
the operator's eyes plus a manual pair fallback for a peer whose automatic
bonding keeps failing; it is not part of the normal attach path.

    rnode.py scan        — list advertising RNodes (NUS service UUID present)
    rnode.py scan-any    — list EVERY advertising BLE device (the generic
                           Bluetooth card's discovery: peers and adapters
                           worth pairing, RNode or not)
    rnode.py pair <addr> — bluetoothctl pair + trust <addr>, best-effort
"""
import json
import subprocess
import sys

UART_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
SCAN_TIMEOUT = 6.0     # s; a cheap CoW class CPU wants a few discovery cycles
PAIR_TIMEOUT = 35      # s; bluetoothctl pair waits out the pairing procedure


def _bleak():
    try:
        import bleak  # noqa: F401
        return bleak
    except Exception:
        return None


def _bonded(device) -> bool:
    """BlueZ backend: device.details is a dict with dbus props under 'props';
    RNS's own device_filter() trusts exactly this key."""
    try:
        props = getattr(device, "details", None)
        if isinstance(props, dict):
            return props.get("props", {}).get("Bonded") is True
    except Exception:
        pass
    return False


def scan() -> dict:
    bleak = _bleak()
    if bleak is None:
        return {"error": "bleak is not installed (no BLE stack in this image)", "devices": []}
    import asyncio

    async def _scan():
        found = []

        def keep(device, adv):
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            if UART_SERVICE_UUID not in uuids:
                return False
            name = device.name or adv.local_name or ""
            # RNode firmware names its adverts "RNode ..." (RNS's own default
            # matcher requires that prefix); keep others out of the list.
            return name.startswith("RNode")

        devices = await bleak.BleakScanner.discover(timeout=SCAN_TIMEOUT)
        for d in devices:
            # discovery() already applied no filter; match here so unknown
            # firmware variants advertising NUS still show with their names.
            name = d.name or ""
            uuids = set()
            try:
                for du in (d.metadata or {}).get("uuids", []):
                    uuids.add(str(du).lower())
            except Exception:
                pass
            if not (name.startswith("RNode") or UART_SERVICE_UUID in uuids):
                continue
            found.append({"name": name or "(unnamed)", "address": d.address,
                          "bonded": _bonded(d)})
        # de-dupe by address (a device can appear once per adv payload)
        uniq = {}
        for f in found:
            uniq.setdefault(f["address"], f)
        return sorted(uniq.values(), key=lambda x: x["name"])

    try:
        devices = asyncio.run(_scan())
    except Exception as e:  # noqa: BLE001 - BlueZ down, no adapter, dbus error
        return {"error": "BLE scan failed: %s" % e, "devices": []}
    return {"devices": devices, "error": ""}


def scan_any() -> dict:
    """Every advertising BLE device in range, no name/UUID filter — the
    generic Bluetooth (BLE transport) card's discovery. The RNS BLE
    transport itself keeps its own scan inside rnsd; this list is what the
    operator can see and pair from the portal (a phone, a T-Echo, a dongle
    — anything BlueZ hears)."""
    bleak = _bleak()
    if bleak is None:
        return {"error": "bleak is not installed (no BLE stack in this image)", "devices": []}
    import asyncio

    async def _scan():
        found = []
        devices = await bleak.BleakScanner.discover(timeout=SCAN_TIMEOUT)
        for d in devices:
            name = d.name or "(unnamed)"
            found.append({"name": name, "address": d.address,
                          "bonded": _bonded(d)})
        # de-dupe by address (a device can appear once per adv payload)
        uniq = {}
        for f in found:
            uniq.setdefault(f["address"], f)
        return sorted(uniq.values(), key=lambda x: (x["name"] == "(unnamed)",
                                                    x["name"].lower(),
                                                    x["address"]))

    try:
        devices = asyncio.run(_scan())
    except Exception as e:  # noqa: BLE001 - BlueZ down, no adapter, dbus error
        return {"error": "BLE scan failed: %s" % e, "devices": []}
    return {"devices": devices, "error": ""}


def pair(address: str) -> dict:
    """bluetoothctl one-shot pair + trust. JustWorks pairing (what RNode
    firmware uses) needs no interactive agent; a numeric-compare modem would
    need a UI agent this box does not have."""
    import re
    import shutil

    if not re.fullmatch(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", address or ""):
        return {"ok": False, "message": "bad address"}
    if shutil.which("bluetoothctl") is None:
        return {"ok": False, "message": "bluetoothctl is not installed (no BLE stack in this image)"}
    out = ""
    for args in (["pair", address], ["trust", address]):
        try:
            p = subprocess.run(["bluetoothctl"] + args, capture_output=True,
                               text=True, timeout=PAIR_TIMEOUT)
            out += (p.stdout or "") + (p.stderr or "")
        except subprocess.TimeoutExpired:
            return {"ok": False, "message": "pairing timed out — is the RNode on and in pairing reach?"}
        except OSError as e:
            return {"ok": False, "message": "could not run bluetoothctl: %s" % e}
        if p.returncode != 0 and args[0] == "pair":
            fail = "Failed to pair" in out or "not available" in out
            msg = "pairing failed" if fail else "pairing attempt did not confirm"
            return {"ok": False, "message": "%s (%s)" % (msg, _last_line(out))}
    if "Pairing successful" not in out:
        return {"ok": False, "message": "pairing did not succeed (%s)" % _last_line(out)}
    return {"ok": True, "message": "Paired %s — now Save & apply (restarts rnsd)" % address}


def _last_line(text: str) -> str:
    lines = [ln.strip() for ln in (text or "").splitlines() if ln.strip()]
    return lines[-1][:120] if lines else "no output"


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "scan":
        print(json.dumps(scan()))
    elif cmd == "scan-any":
        print(json.dumps(scan_any()))
    elif cmd == "pair" and len(sys.argv) > 2:
        print(json.dumps(pair(sys.argv[2])))
    else:
        print(json.dumps({"error": "usage: rnode.py scan | scan-any | pair <addr>"}))
        sys.exit(2)
