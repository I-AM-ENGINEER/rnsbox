#!/usr/bin/env python3
"""rnode.py — BLE discovery / pairing for RNode modems, printed as JSON.

Standalone helper (no rnsbox package). Scanning uses bluetoothctl, not bleak. The
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
import os
import re
import subprocess
import sys

UART_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
SCAN_TIMEOUT = 15.0    # s; the RNode advertises slowly, phones scan longer
PAIR_TIMEOUT = 35      # s; bluetoothctl pair waits out the pairing procedure


def _bt_scan(timeout):
    """Scan through bluetoothctl (BlueZ's own C client).

    Bleak is a large Python/D-Bus import and its discover() can sit for tens
    of seconds on this CPU; the helper is called from a web request, so keep
    it to one short-lived CLI process. Returns {address: name} or (None, err).
    """
    env = dict(os.environ)
    env["NO_COLOR"] = "1"
    env["TERM"] = "dumb"
    try:
        p = subprocess.run(
            ["bluetoothctl", "--timeout", str(max(2, int(timeout))), "scan", "on"],
            capture_output=True, text=True, timeout=max(2, int(timeout)) + 10,
            env=env)
    except Exception as exc:  # noqa: BLE001 - no adapter / bluetoothd down
        return None, str(exc)
    text = re.sub(r"\x1b\[[0-9;]*m", "",
                  (p.stdout or "") + "\n" + (p.stderr or ""))
    devs = {}
    for line in text.splitlines():
        m = re.search(r"Device\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})(?:\s+(.*))?$",
                      line)
        if not m:
            continue
        addr, name = m.group(1).upper(), (m.group(2) or "").strip()
        # [CHG] lines carry "RSSI: ..." / "TxPower: ..." after the address —
        # those are properties, not the advertised name.
        if name.startswith(("RSSI:", "TxPower:", "ManufacturerData",
                            "ServiceData", "UUIDs:", "Class:")):
            continue
        if name.replace("-", ":").upper() == addr:
            name = ""
        if name or addr not in devs:
            devs[addr] = name
    return devs, ""


def _bt_has_nus(addr):
    """True when BlueZ reports the Nordic UART service for this device:
    the same fallback the old bleak path used for unnamed RNode adverts."""
    try:
        out = subprocess.run(["bluetoothctl", "info", addr],
                             capture_output=True, text=True, timeout=6).stdout.lower()
        return UART_SERVICE_UUID in out or "nordic uart" in out
    except Exception:
        return False


def _bt_bonded(addr):
    try:
        out = subprocess.run(["bluetoothctl", "info", addr],
                             capture_output=True, text=True, timeout=5).stdout
        return "Paired: yes" in out
    except Exception:
        return False


def scan() -> dict:
    """Advertising RNodes (name starts with "RNode", the prefix RNS uses)."""
    devs, err = _bt_scan(SCAN_TIMEOUT)
    if devs is None:
        return {"error": "BLE scan failed: %s" % err, "devices": []}
    found = []
    for addr, name in devs.items():
        if name.lower().startswith("rnode"):
            found.append({"name": name, "address": addr, "bonded": False})
            continue
        # NUS fallback only for unnamed adverts: `bluetoothctl info` on an
        # address BlueZ does not cache can block for seconds, and the named
        # RNode is the normal case anyway.
        if not name and _bt_has_nus(addr):
            found.append({"name": "(unnamed)", "address": addr, "bonded": False})
    found.sort(key=lambda x: (x["name"].lower(), x["address"]))
    return {"devices": found, "error": ""}


def scan_any() -> dict:
    """Every advertising BLE device in range, no name filter."""
    devs, err = _bt_scan(SCAN_TIMEOUT)
    if devs is None:
        return {"error": "BLE scan failed: %s" % err, "devices": []}
    # No per-device `info` calls here: the generic list can hold dozens of
    # phones and beacons, and bonded state is not what that card is for.
    found = [{"name": name or "(unnamed)", "address": addr, "bonded": False}
             for addr, name in list(devs.items())[:40]]
    found.sort(key=lambda x: (x["name"] == "(unnamed)", x["name"].lower(),
                              x["address"]))
    return {"devices": found, "error": ""}


def scan_watch(path: str) -> None:
    """Progressive scan for the portal: write found RNodes as they appear.

    bluetoothctl prints [NEW] lines while discovery runs, so the page can
    render the first RNode immediately instead of waiting for the whole
    window. The file is replaced atomically on every change and ends with
    {"done": true}.
    """
    found = {}

    def flush(done, error=""):
        rows = [{"name": found[a], "address": a, "bonded": False}
                for a in sorted(found)]
        tmp = path + ".tmp.%d" % os.getpid()
        try:
            with open(tmp, "w") as f:
                json.dump({"devices": rows, "done": done, "error": error}, f)
            os.replace(tmp, path)
        except OSError:
            pass

    flush(False)
    env = dict(os.environ)
    env["NO_COLOR"] = "1"
    env["TERM"] = "dumb"
    try:
        p = subprocess.Popen(
            ["bluetoothctl", "--timeout", str(int(SCAN_TIMEOUT)), "scan", "on"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            bufsize=1, env=env)
    except Exception as exc:  # noqa: BLE001
        flush(True, "BLE scan failed: %s" % exc)
        return
    try:
        for line in p.stdout:
            line = re.sub(r"\x1b\[[0-9;]*m", "", line)
            m = re.search(r"Device\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})(?:\s+(.*))?$",
                          line)
            if not m:
                continue
            addr, name = m.group(1).upper(), (m.group(2) or "").strip()
            if name.startswith(("RSSI:", "TxPower:", "ManufacturerData",
                                "ServiceData", "UUIDs:", "Class:")):
                continue
            if not name.lower().startswith("rnode"):
                continue
            if found.get(addr) != name:
                found[addr] = name
                flush(False)
    finally:
        try:
            p.wait(timeout=5)
        except Exception:
            p.kill()
    flush(True)


def _bt_pair_session(address: str, code: str, wait_s: int):
    """Run bluetoothctl on a PTY so its pairing agent can prompt.

    RNode firmware can show a 6-digit passkey on its display; bluetoothctl's
    KeyboardDisplay agent prints "Enter passkey..." and reads the answer from
    the terminal. A plain pipe cannot answer that prompt. Returns
    (output, need_code): need_code is true when the modem asked for a code and
    none was supplied.
    """
    import os
    import pty
    import re
    import select
    import time

    master, slave = pty.openpty()
    p = subprocess.Popen(["bluetoothctl"], stdin=slave, stdout=slave,
                         stderr=slave, close_fds=True)
    os.close(slave)
    chunks = []

    def pump(secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.2)
            if not r:
                continue
            try:
                data = os.read(master, 4096)
            except OSError:
                return
            if not data:
                return
            chunks.append(data.decode("utf-8", "replace"))

    def output():
        return "".join(chunks)

    def send(line):
        try:
            os.write(master, (line + "\n").encode())
        except OSError:
            pass
        time.sleep(0.2)

    need_code = False
    try:
        send("agent KeyboardDisplay")
        pump(0.6)
        send("pair %s" % address)
        deadline = time.time() + wait_s
        answered = False
        while time.time() < deadline:
            pump(0.3)
            text = output()
            low = text.lower()
            if "confirm passkey" in low and not answered:
                m = re.search(r"confirm passkey\s+(\d+)", low)
                if m and (not code or code == m.group(1)):
                    send("yes")
                else:
                    send("no")
                answered = True
                continue
            if re.search(r"enter (the )?(passkey|pin)", low) and not answered:
                if code:
                    send(code)
                    answered = True
                    continue
                need_code = True
                break
            if re.search(r"pairing successful|failed to pair|authentication ?failed|"
                         r"authentication cancel|not available|device not available",
                         low):
                break
        text = output()
        if re.search(r"pairing successful|already paired|alreadyexists", text, re.I):
            send("trust %s" % address)
            pump(0.6)
        send("quit")
        pump(0.3)
    finally:
        try:
            p.terminate()
        except Exception:
            pass
        try:
            p.wait(timeout=3)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass
        try:
            os.close(master)
        except Exception:
            pass
    return output(), need_code


def pair(address: str, code: str = "") -> dict:
    """Pair + trust an RNode. `code` is the pairing code shown on the modem
    (empty for JustWorks)."""
    import re
    import shutil

    if not re.fullmatch(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", address or ""):
        return {"ok": False, "message": "bad address"}
    if code and not re.fullmatch(r"[0-9]{1,6}", code):
        return {"ok": False, "message": "bad pairing code (expected up to 6 digits)"}
    if shutil.which("bluetoothctl") is None:
        return {"ok": False, "message": "bluetoothctl is not installed (no BLE stack in this image)"}

    text, need_code = _bt_pair_session(address, code, PAIR_TIMEOUT)
    if need_code:
        return {"ok": False,
                "message": "The RNode is asking for a pairing code. Enter the code shown on its display and press Pair again."}

    # A device that is no longer in BlueZ's cache says "not available"; a short
    # discovery puts it back, then retry once.
    if re.search(r"not available|device not available", text, re.I):
        try:
            subprocess.run(["bluetoothctl", "--timeout", "8", "scan", "on"],
                           capture_output=True, text=True, timeout=12)
        except Exception:
            pass
        text, need_code = _bt_pair_session(address, code, PAIR_TIMEOUT)
        if need_code:
            return {"ok": False,
                    "message": "The RNode is asking for a pairing code. Enter the code shown on its display and press Pair again."}

    if re.search(r"pairing successful|already paired|alreadyexists", text, re.I):
        # Pairing leaves the LE link open. A connected device stops
        # advertising, so rnsd's scanner cannot find it and the interface
        # retries forever. Drop the link; rnsd connects (and bonds) anew.
        try:
            subprocess.run(["bluetoothctl", "disconnect", address],
                           capture_output=True, text=True, timeout=10)
        except Exception:
            pass
        return {"ok": True, "message": "Paired %s" % address}
    return {"ok": False, "message": "pairing failed (%s)" % _last_line(text)}


def _last_line(text: str) -> str:
    import re
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text or "")
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
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
                           text=True, errors="replace", timeout=timeout)
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
    elif cmd == "scan-watch" and len(sys.argv) > 2:
        scan_watch(sys.argv[2])
    elif cmd == "pair" and len(sys.argv) > 2:
        print(json.dumps(pair(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "")))
    elif cmd == "serial":
        print(json.dumps(serial_devices()))
    elif cmd == "action" and len(sys.argv) > 3:
        print(json.dumps(action(sys.argv[2], sys.argv[3])))
    else:
        print(json.dumps({"error": "usage: rnode.py scan | scan-any | pair <addr> | "
                                   "serial | action <port> <info|config|stats|display|"
                                   "bt-on|bt-off|bt-pair>"}))
        sys.exit(2)
