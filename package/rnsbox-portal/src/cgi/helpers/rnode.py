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


# ---------------------------------------------------------------------------
# Serial RNode tools (rnodeconf + the RNode protocol itself)
#
# rnodeconf ships with python-rns; these actions drive it and the serial link
# directly, so the portal can show what a modem is doing: its config, live
# counters, and its OLED framebuffer.  rnsd may already hold the port open for
# a configured [[RNode ...]] interface; in that case opening fails and the
# error is reported instead of disturbing the running link.
# ---------------------------------------------------------------------------

import glob
import re
import struct
import time
import zlib

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
CMD_STAT_RX, CMD_STAT_TX = 0x21, 0x22
CMD_STAT_RSSI, CMD_STAT_SNR = 0x23, 0x24
CMD_FB_READ, CMD_DISP_READ = 0x42, 0x66


def _valid_port(port: str) -> bool:
    return bool(re.fullmatch(r"/dev/tty(?:USB|ACM)[0-9]+", port or ""))


def serial_devices() -> dict:
    ports = [p for p in sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
             if _valid_port(p)]
    return {"ports": ports, "ok": True}


def _rnodeconf(args, timeout=45):
    import shutil
    if shutil.which("rnodeconf") is None:
        return {"ok": False, "output": "rnodeconf is not installed (python-rns)"}
    try:
        p = subprocess.run(["rnodeconf"] + args, capture_output=True,
                           text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"ok": False, "output": "rnodeconf timed out after %d s" % timeout}
    except OSError as e:
        return {"ok": False, "output": "could not run rnodeconf: %s" % e}
    out = (p.stdout or "") + (p.stderr or "")
    return {"ok": p.returncode == 0, "output": out.strip()[-8000:]}


def info(port: str) -> dict:
    if not _valid_port(port):
        return {"ok": False, "output": "invalid serial port"}
    return _rnodeconf(["--info", "--nocheck", port])


def config(port: str) -> dict:
    if not _valid_port(port):
        return {"ok": False, "output": "invalid serial port"}
    return _rnodeconf(["--config", "--nocheck", port])


def bluetooth(port: str, mode: str) -> dict:
    if not _valid_port(port):
        return {"ok": False, "output": "invalid serial port"}
    flags = {"on": "-b", "off": "-B", "pair": "-p"}
    if mode not in flags:
        return {"ok": False, "output": "invalid bluetooth mode"}
    return _rnodeconf([flags[mode], "--nocheck", port], timeout=20)


def _escape(payload: bytes) -> bytes:
    out = bytearray()
    for b in payload:
        if b == FEND:
            out += bytes([FESC, TFEND])
        elif b == FESC:
            out += bytes([FESC, TFESC])
        else:
            out.append(b)
    return bytes(out)


def _frame(cmd: int, payload: bytes = b"") -> bytes:
    return bytes([FEND, cmd]) + _escape(payload) + bytes([FEND])


def _read_frames(ser, deadline: float):
    """Decode HDLC frames until the deadline; returns a list of byte strings."""
    frames = []
    buf = bytearray()
    esc = False
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            continue
        for b in chunk:
            if b == FEND:
                if buf:
                    frames.append(bytes(buf))
                    buf.clear()
                esc = False
            elif b == FESC:
                esc = True
            else:
                if esc:
                    if b == TFEND:
                        b = FEND
                    elif b == TFESC:
                        b = FESC
                    esc = False
                buf.append(b)
    return frames


def _open(port: str):
    import serial
    return serial.Serial(port, 115200, timeout=0.2, write_timeout=2)


def stats(port: str) -> dict:
    if not _valid_port(port):
        return {"ok": False, "output": "invalid serial port"}
    try:
        ser = _open(port)
    except Exception as e:  # noqa: BLE001 - busy port, permissions, unplugged
        return {"ok": False, "output": "cannot open %s: %s" % (port, e)}
    try:
        ser.reset_input_buffer()
        for cmd in (CMD_STAT_RX, CMD_STAT_TX, CMD_STAT_RSSI, CMD_STAT_SNR):
            ser.write(_frame(cmd))
        ser.flush()
        frames = _read_frames(ser, time.time() + 3.0)
    finally:
        ser.close()

    out = {}
    for f in frames:
        if not f:
            continue
        cmd, data = f[0], f[1:]
        if cmd in (CMD_STAT_RX, CMD_STAT_TX) and len(data) >= 4:
            out["rx" if cmd == CMD_STAT_RX else "tx"] = struct.unpack("!I", data[:4])[0]
        elif cmd == CMD_STAT_RSSI and data:
            out["rssi"] = data[0] - 157
        elif cmd == CMD_STAT_SNR and data:
            snr = data[0] - 256 if data[0] > 127 else data[0]
            out["snr"] = snr * 0.25
    if not out:
        return {"ok": False, "output": "no answer from %s (is rnsd using this port?)" % port}
    return {"ok": True, "stats": out}


def _png_gray(width: int, height: int, rows) -> bytes:
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(t, d):
        return (struct.pack("!I", len(d)) + t + d +
                struct.pack("!I", zlib.crc32(t + d) & 0xffffffff))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def _render_1bpp(buf: bytes, width: int = 64):
    """SSD1306 page layout: 8 vertical pixels per byte, LSB at the top."""
    height = len(buf) * 8 // width
    rows = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            page = (y // 8) * (width // 8) + (x // 8)
            bit = (buf[page] >> (y % 8)) & 1 if page < len(buf) else 0
            line.append(255 if bit else 0)
        rows.append(line)
    return _png_gray(width, height, rows)


def display(port: str) -> dict:
    if not _valid_port(port):
        return {"ok": False, "output": "invalid serial port"}
    try:
        ser = _open(port)
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "output": "cannot open %s: %s" % (port, e)}
    try:
        ser.reset_input_buffer()
        ser.write(_frame(CMD_DISP_READ, b"\x01"))
        ser.flush()
        body = None
        for f in _read_frames(ser, time.time() + 3.0):
            if f and f[0] == CMD_DISP_READ:
                body = f[1:]
                break
        if body is None:
            ser.write(_frame(CMD_FB_READ, b"\x01"))
            ser.flush()
            for f in _read_frames(ser, time.time() + 2.0):
                if f and f[0] == CMD_FB_READ:
                    body = f[1:]
                    break
    finally:
        ser.close()

    if body is None:
        return {"ok": False, "output": "no display answer from %s (is rnsd using this port?)" % port}
    if len(body) == 1 and body[0] == 0xFF:
        return {"ok": False, "output": "this RNode reports no display"}
    if len(body) not in (512, 1024):
        return {"ok": False, "output": "unexpected framebuffer size %d" % len(body)}

    import base64
    # CMD_DISP_READ returns the content area then the status area, 512 bytes
    # each; render both stacked as 64x128.  CMD_FB_READ is a single 64x64.
    img = _render_1bpp(body, 64)
    return {"ok": True, "image": "data:image/png;base64," +
            base64.b64encode(img).decode("ascii")}


def action(port: str, what: str) -> dict:
    if what == "info":
        return info(port)
    if what == "config":
        return config(port)
    if what == "stats":
        return stats(port)
    if what == "display":
        return display(port)
    if what in ("bt-on", "bt-off", "bt-pair"):
        return bluetooth(port, what[3:])
    return {"ok": False, "output": "unknown action"}


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "scan":
        print(json.dumps(scan()))
    elif cmd == "scan-any":
        print(json.dumps(scan_any()))
    elif cmd == "pair" and len(sys.argv) > 2:
        print(json.dumps(pair(sys.argv[2])))
    elif cmd == "serial":
        print(json.dumps(serial_devices()))
    elif cmd == "action" and len(sys.argv) > 3:
        print(json.dumps(action(sys.argv[2], sys.argv[3])))
    else:
        print(json.dumps({"error": "usage: rnode.py scan | scan-any | pair <addr> | "
                                   "serial | action <port> <info|config|stats|display|"
                                   "bt-on|bt-off|bt-pair>"}))
        sys.exit(2)
