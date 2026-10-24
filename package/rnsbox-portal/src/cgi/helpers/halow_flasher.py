#!/usr/bin/env python3
"""halow_flasher.py — factory/HGIC flasher for RNode HaLow modems.

Port of RNode_Halow_OTA_Flasher's raw-Ethernet path without scapy: the HGIC
protocol is plain Ethernet frames (ethertype 0x4847), so a pair of AF_PACKET
sockets is enough. Runs as root from the portal's "HaLow flasher" tab.

  halow_flasher.py scan
  halow_flasher.py ip     <iface> <mac>
  halow_flasher.py reboot <iface> <mac>
  halow_flasher.py format <iface> <mac>
  halow_flasher.py flash  <iface> <mac> <fw.tar|fw.bin> [--preflash] [--tftp]

`flash` runs detached by the portal and records progress in
/run/rnsbox-halow-flasher.json (the page polls it).
"""
from __future__ import annotations

import json
import os
import queue
import socket
import struct
import sys
import tarfile
import threading
import time

ETH_P_OTA = 0x4847
BROADCAST = "ff:ff:ff:ff:ff:ff"

ST_SCAN, ST_SCAN_REPORT, ST_FW_DATA, ST_FW_DATA_RESP = 2, 3, 4, 5
ST_GET_IP, ST_GET_IP_RESP = 0xF0, 0xF1
ST_FORMAT_LFS, ST_FORMAT_LFS_RESP = 0xF2, 0xF3

STATE = "/run/rnsbox-halow-flasher.json"
LOCK = "/run/rnsbox-halow-flasher.lock"
PREFLASH = "/usr/share/rnsbox-portal/preflash.bin"

FW_VERSION = 0x01020503
FW_CHIPID = 0x4002


# ---------------------------------------------------------------- status ---
_log_lines = []


def status(state, stage, done=0, total=0, error=""):
    pct = int(done * 100 / total) if total else 0
    obj = {
        "state": state,
        "stage": stage,
        "done": int(done),
        "total": int(total),
        "pct": pct,
        "error": error,
        "log": _log_lines[-40:],
    }
    try:
        tmp = STATE + ".tmp.%d" % os.getpid()
        with open(tmp, "w") as f:
            json.dump(obj, f)
        os.replace(tmp, STATE)
    except OSError:
        pass


def log(msg):
    line = time.strftime("%H:%M:%S ") + str(msg)
    _log_lines.append(line)
    print(line, flush=True)


# ------------------------------------------------------------ raw ethernet ---
def _mac_bytes(mac):
    return bytes(int(x, 16) for x in mac.split(":"))


def _mac_hex(b):
    return ":".join("%02x" % x for x in b)


class RawEth:
    """One AF_PACKET socket on an interface (send + receive of 0x4847)."""

    def __init__(self, iface):
        self.iface = iface
        self.mac = open("/sys/class/net/%s/address" % iface).read().strip().lower()
        try:
            self.mtu = int(open("/sys/class/net/%s/mtu" % iface).read().strip())
        except OSError:
            self.mtu = 1500
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                  socket.htons(ETH_P_OTA))
        self.sock.bind((iface, 0))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send(self, dst, payload):
        frame = _mac_bytes(dst) + _mac_bytes(self.mac) + struct.pack("!H", ETH_P_OTA) + payload
        self.sock.send(frame)

    def recv(self, timeout):
        end = time.time() + timeout
        while True:
            remaining = end - time.time()
            if remaining <= 0:
                return None
            self.sock.settimeout(max(0.01, remaining))
            try:
                frame = self.sock.recv(65535)
            except socket.timeout:
                return None
            except OSError:
                return None
            if len(frame) < 14 or struct.unpack("!H", frame[12:14])[0] != ETH_P_OTA:
                continue
            return _mac_hex(frame[6:12]), _mac_hex(frame[0:6]), frame[14:]


def iface_up(iface):
    try:
        flags = int(open("/sys/class/net/%s/flags" % iface).read().strip(), 16)
        return bool(flags & 0x1)
    except OSError:
        return False


def iter_ifaces():
    out = []
    try:
        names = os.listdir("/sys/class/net")
    except OSError:
        return out
    for name in sorted(names):
        low = name.lower()
        if low == "lo" or low.startswith(("docker", "veth", "tailscale", "tun", "tap")):
            continue
        if low == "sl0":
            continue
        if not iface_up(name):
            continue
        out.append(name)
    return out


# ---------------------------------------------------------------- protocol ---
def pack_scan_req():
    return struct.pack("BB", ST_SCAN, 0)


def parse_scan_report(b):
    if len(b) < 18 or b[0] != ST_SCAN_REPORT:
        return None
    status_ = b[1]
    version = struct.unpack(">I", b[2:6])[0]
    chipid = struct.unpack(">H", b[6:8])[0]
    mode, rev = b[8], b[9]
    svn = struct.unpack(">I", b[10:14])[0]
    app = struct.unpack(">I", b[14:18])[0]
    return status_, version, chipid, mode, rev, svn, app


def version_str(v):
    return "%d.%d.%d.%d" % ((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def pack_get_ip():
    return struct.pack("BB", ST_GET_IP, 0)


def parse_get_ip(b):
    if len(b) < 14 or b[0] != ST_GET_IP_RESP:
        return None
    status_ = b[1]
    ip, gw, mask = struct.unpack(">III", b[2:14])
    ver = ""
    if len(b) >= 46:
        ver = b[14:46].split(b"\x00", 1)[0].decode("ascii", "replace")
    return status_, socket.inet_ntoa(struct.pack(">I", ip)), \
        socket.inet_ntoa(struct.pack(">I", gw)), \
        socket.inet_ntoa(struct.pack(">I", mask)), ver


def pack_reboot():
    return struct.pack(">BBI", 1, 0, 0)


def pack_format_lfs():
    return struct.pack("BB", ST_FORMAT_LFS, 0)


def inet_checksum_16(data):
    s = 0
    if len(data) & 1:
        data += b"\x00"
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def pack_fw_chunk(chunk, off, total):
    first_word = struct.unpack(">H", chunk[0:2])[0]
    checksum = inet_checksum_16(chunk) & 0xFFFF
    fw_len = len(chunk) & 0xFFFF
    payload = b"".join([
        struct.pack("BB", ST_FW_DATA, 0),
        struct.pack(">I", FW_VERSION),
        struct.pack(">I", off & 0xFFFFFFFF),
        struct.pack(">I", total & 0xFFFFFFFF),
        struct.pack(">H", fw_len),
        struct.pack("<H", checksum),
        struct.pack(">H", FW_CHIPID),
        struct.pack(">H", first_word),
        chunk[2:],
        b"\x00\x00",
    ])
    return payload, {"off": off & 0xFFFFFFFF, "length": fw_len,
                     "checksum": checksum, "first_word": first_word}


def parse_ack(b):
    if len(b) < 22 or b[0] != ST_FW_DATA_RESP:
        return None
    return {
        "status": b[1],
        "off": struct.unpack(">I", b[6:10])[0],
        "length": struct.unpack(">H", b[14:16])[0],
        "checksum": struct.unpack("<H", b[16:18])[0],
        "first_word": struct.unpack(">H", b[20:22])[0],
    }


# ------------------------------------------------------------------- scan ---
def scan_iface(iface, packet_cnt=10, sniff=0.6):
    try:
        raw = RawEth(iface)
    except OSError:
        return []
    found = []
    try:
        req = pack_scan_req()
        for _ in range(packet_cnt):
            try:
                raw.send(BROADCAST, req)
            except OSError:
                break
            time.sleep(0.01)
        end = time.time() + sniff
        seen = set()
        while time.time() < end:
            r = raw.recv(end - time.time())
            if r is None:
                break
            src, dst, payload = r
            if src == raw.mac or src in seen or dst != raw.mac:
                continue
            rep = parse_scan_report(payload)
            if not rep:
                continue
            status_, version, chipid, mode, rev, svn, app = rep
            seen.add(src)
            v = version_str(version)
            found.append({
                "mac": src, "iface": iface, "mtu": raw.mtu,
                "kind": "rnode-halow" if v == "0.0.0.0" else "hgic",
                "version": v, "chipid": chipid, "mode": mode,
                "rev": rev, "svn": svn, "app": app,
            })
    finally:
        raw.close()
    return found


def cmd_scan():
    out, lock = [], threading.Lock()

    def worker(iface):
        try:
            res = scan_iface(iface)
        except Exception:
            res = []
        if res:
            with lock:
                out.extend(res)

    ths = [threading.Thread(target=worker, args=(i,), daemon=True) for i in iter_ifaces()]
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    print(json.dumps({"devices": out, "interfaces": iter_ifaces()}))


# ---------------------------------------------------------- device helpers ---
def _recv_from(raw, mac, timeout, want):
    end = time.time() + timeout
    while True:
        r = raw.recv(end - time.time())
        if r is None:
            return None
        src, dst, payload = r
        if src != mac or dst != raw.mac:
            continue
        val = want(payload)
        if val is not None:
            return val


def get_ip(iface, mac, tries=5, timeout=0.5):
    raw = RawEth(iface)
    try:
        for _ in range(tries):
            raw.send(mac, pack_get_ip())
            info = _recv_from(raw, mac, timeout, parse_get_ip)
            if info:
                return info
    finally:
        raw.close()
    return None


def format_lfs(iface, mac, timeout=20.0):
    raw = RawEth(iface)
    try:
        for _ in range(3):
            raw.send(mac, pack_format_lfs())
            def want(b):
                return b[1] if len(b) >= 2 and b[0] == ST_FORMAT_LFS_RESP else None
            st = _recv_from(raw, mac, timeout, want)
            if st is not None:
                return int(st)
    finally:
        raw.close()
    return None


def reboot(iface, mac):
    raw = RawEth(iface)
    try:
        for _ in range(3):
            raw.send(mac, pack_reboot())
            time.sleep(0.05)
    finally:
        raw.close()


def flash_raw(iface, mac, fw, progress=None):
    raw = RawEth(iface)
    try:
        chunk = max(512, min(1400, raw.mtu - 22)) & ~3
        total = len(fw)
        off = 0
        done = 0
        while off < total:
            part = fw[off:off + chunk]
            payload, expect = pack_fw_chunk(part, off, total)
            acked = False
            for attempt in range(5):
                raw.send(mac, payload)
                end = time.time() + 2.5
                while time.time() < end:
                    r = raw.recv(end - time.time())
                    if r is None:
                        break
                    src, dst, body = r
                    if src != mac or dst != raw.mac:
                        continue
                    ack = parse_ack(body)
                    if not ack or ack["off"] != expect["off"]:
                        continue
                    if ack["status"] != 0:
                        raise RuntimeError("device rejected chunk at %d (status %d)" %
                                           (off, ack["status"]))
                    acked = True
                    break
                if acked:
                    break
                time.sleep(0.08)
            if not acked:
                raise RuntimeError("no ACK for chunk at %d" % off)
            off += len(part)
            done = off
            if progress:
                progress(done, total)
    finally:
        raw.close()


# ------------------------------------------------------------------ TFTP ----
def tftp_put(ip, remote, data, port=69, timeout=1.0, retries=3):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    req = struct.pack("!H", 2) + remote.encode() + b"\x00octet\x00"
    try:
        sock.sendto(req, (ip, port))
        pkt, srv = sock.recvfrom(4096)
        op = struct.unpack("!H", pkt[:2])[0]
        if op == 6:                      # OACK -> ACK 0, then DATA1
            sock.sendto(struct.pack("!HH", 4, 0), srv)
        elif op != 4 or struct.unpack("!H", pkt[2:4])[0] != 0:
            raise RuntimeError("TFTP WRQ refused")
        block, pos = 1, 0
        while True:
            part = data[pos:pos + 512]
            pkt = struct.pack("!HH", 3, block) + part
            ok = False
            for _ in range(retries):
                sock.sendto(pkt, srv)
                try:
                    ack, srv2 = sock.recvfrom(1024)
                except socket.timeout:
                    continue
                if (struct.unpack("!H", ack[:2])[0] == 4 and
                        struct.unpack("!H", ack[2:4])[0] == block):
                    srv = srv2
                    ok = True
                    break
            if not ok:
                raise RuntimeError("TFTP timeout on block %d" % block)
            pos += len(part)
            if len(part) < 512:
                return
            block = (block + 1) & 0xFFFF
    finally:
        sock.close()


# -------------------------------------------------------------- firmware ----
def load_firmware(path):
    """Return (fw_bytes, [ (name, bytes) filesystem entries ])."""
    if tarfile.is_tarfile(path):
        files = []
        fw = None
        try:
            with tarfile.open(path, "r:*") as tf:
                for m in tf:
                    name = m.name
                    while name.startswith("./"):
                        name = name[2:]
                    if not m.isfile():
                        continue
                    if name == "fw.bin":
                        fw = tf.extractfile(m).read()
                    elif name.startswith("www/"):
                        files.append((name, tf.extractfile(m).read()))
        except (tarfile.TarError, OSError) as exc:
            raise RuntimeError("cannot read the tar archive: %s" % exc)
        if not fw:
            raise RuntimeError("the archive has no fw.bin")
        return fw, files
    with open(path, "rb") as f:
        return f.read(), []


def _adopt_target(iface, mac, others, want_kind=None):
    """Return the live MAC for this update.

    A vendor preflash can change the device's MAC. The reference flasher
    blacklists every other device seen before the operation and adopts the
    only remaining candidate when the old MAC disappears.
    """
    found = scan_iface(iface, packet_cnt=6, sniff=0.4)
    cand = [d["mac"] for d in found if d["mac"] not in others]
    if mac in cand:
        return mac
    if want_kind:
        preferred = [d["mac"] for d in cand if d["kind"] == want_kind]
        if len(preferred) == 1:
            return preferred[0]
    if len(cand) == 1:
        return cand[0]
    return None


def wait_hgic(iface, mac, timeout=15.0, others=()):
    end = time.time() + timeout
    while time.time() < end:
        got = wait_hgic_once(iface, mac, others)
        if got:
            return got
        time.sleep(0.2)
    return None


def wait_hgic_candidates(iface, mac, others):
    return [d for d in scan_iface(iface, packet_cnt=3, sniff=0.3)
            if d["mac"] not in others]


def wait_hgic_once(iface, mac, others):
    cand = scan_iface(iface, packet_cnt=6, sniff=0.4)
    live = [d for d in cand if d["mac"] not in others]
    if any(d["mac"] == mac for d in live):
        return mac
    if len(live) == 1:
        return live[0]["mac"]
    return None


def wait_ip(iface, mac, timeout=80.0, others=()):
    end = time.time() + timeout
    while time.time() < end:
        got = _adopt_target(iface, mac, others, want_kind="rnode-halow")
        if got:
            mac = got
        info = get_ip(iface, mac, tries=1, timeout=0.5)
        if info and info[1] not in ("", "0.0.0.0"):
            return mac, info[1]
        time.sleep(0.4)
    return None, None


def cmd_flash(iface, mac, path, preflash, with_tftp):
    try:
        fd = os.open(LOCK, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
    except OSError:
        print("an update is already running", file=sys.stderr)
        return 3
    try:
        status("running", "loading firmware")
        log("target %s on %s" % (mac, iface))
        fw, files = load_firmware(path)
        if not fw:
            raise RuntimeError("no firmware image in the uploaded file")
        # Other devices on this interface must never be mistaken for ours
        # after a reboot (the preflash can change the target's MAC).
        others = {d["mac"] for d in scan_iface(iface, packet_cnt=3, sniff=0.3)}
        others.discard(mac)

        if preflash:
            if not os.path.isfile(PREFLASH):
                raise RuntimeError("bundled preflash image is missing")
            status("running", "preflash (vendor image)")
            log("flashing vendor preflash image")
            with open(PREFLASH, "rb") as f:
                vendor = f.read()
            flash_raw(iface, mac, vendor, lambda d, t: status("running", "preflash", d, t))
            log("vendor image written; rebooting")
            reboot(iface, mac)
            status("running", "waiting for vendor firmware")
            new_mac = wait_hgic(iface, mac, 20.0, others)
            if not new_mac:
                raise RuntimeError("device did not return to HGIC mode after preflash")
            if new_mac != mac:
                log("device MAC changed: %s -> %s" % (mac, new_mac))
                mac = new_mac
            log("device is back in HGIC mode; settling")
            time.sleep(5.0)

        status("running", "flashing firmware", 0, len(fw))
        log("flashing RNode HaLow firmware (%d bytes)" % len(fw))
        flash_raw(iface, mac, fw, lambda d, t: status("running", "flashing firmware", d, t))
        log("firmware written; rebooting")
        reboot(iface, mac)

        if with_tftp and files:
            status("running", "waiting for IP")
            log("waiting for the device to get an IP")
            mac, ip = wait_ip(iface, mac, 80.0, others)
            if not ip:
                raise RuntimeError("device did not get an IP address")
            log("device IP: %s (MAC %s)" % (ip, mac))
            status("running", "formatting LittleFS")
            log("formatting LittleFS")
            format_lfs(iface, mac)
            reboot(iface, mac)
            status("running", "waiting for IP after format")
            mac, ip = wait_ip(iface, mac, 80.0, others)
            if not ip:
                raise RuntimeError("device did not get an IP after the format")
            total = sum(len(d) for _, d in files)
            done = 0
            for name, data in files:
                status("running", "TFTP: " + name, done, total)
                log("TFTP upload: %s" % name)
                tftp_put(ip, name, data)
                done += len(data)
            log("filesystem files uploaded")
        elif with_tftp:
            log("no www/ files in this archive; skipped TFTP")

        status("done", "finished", len(fw), len(fw))
        log("flash done")
    except Exception as exc:  # noqa: BLE001 - report every failure to the page
        status("failed", "failed", error=str(exc))
        log("ERROR: %s" % exc)
        return 1
    finally:
        try:
            os.unlink(LOCK)
        except OSError:
            pass
        try:
            os.unlink(path)
        except OSError:
            pass
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 1
    cmd = argv[1]
    if cmd == "scan":
        cmd_scan()
        return 0
    if cmd in ("ip", "reboot", "format") and len(argv) >= 4:
        iface, mac = argv[2], argv[3].lower()
        if cmd == "ip":
            info = get_ip(iface, mac)
            print(json.dumps({
                "ip": info[1] if info else "",
                "gw": info[2] if info else "",
                "mask": info[3] if info else "",
                "version": info[4] if info else "",
            }))
        elif cmd == "reboot":
            reboot(iface, mac)
            print(json.dumps({"ok": True}))
        else:
            st = format_lfs(iface, mac)
            print(json.dumps({"ok": st is not None, "status": st}))
        return 0
    if cmd == "flash" and len(argv) >= 5:
        return cmd_flash(argv[2], argv[3].lower(), argv[4],
                         "--preflash" in argv, "--tftp" in argv)
    print("usage: halow_flasher.py scan|ip|reboot|format|flash", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
