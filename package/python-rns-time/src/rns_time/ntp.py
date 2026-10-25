"""Minimal SNTP client (RFC 4330 subset) with no third-party dependencies.

Only what rns-time needs: one packet, mode 3 (client) -> mode 4 (server),
the four timestamps, stratum and root dispersion.  No leap-second handling,
no authentication, no clock discipline -- the caller decides what to do with
the measured offset.
"""
import socket
import struct
import time

# 1900-01-01 (NTP epoch) -> 1970-01-01 (Unix epoch), in seconds.
NTP_EPOCH_DELTA = 2208988800

PACKET_LEN = 48


def _to_unix(seconds, fraction):
    return (seconds + fraction / 4294967296.0) - NTP_EPOCH_DELTA


def _to_ntp(ts):
    ntp = ts + NTP_EPOCH_DELTA
    seconds = int(ntp)
    fraction = int((ntp - seconds) * 4294967296.0) & 0xFFFFFFFF
    return seconds & 0xFFFFFFFF, fraction


def query(server, timeout=3.0, port=123):
    """Query one server.  Returns a sample dict, or None.

    The sample carries ``offset`` (server - local, seconds), ``delay``
    (round trip minus server processing), and the server's ``stratum`` and
    ``root_dispersion`` / ``root_delay`` (seconds) for quality reporting.
    """
    try:
        infos = socket.getaddrinfo(server, port, socket.AF_UNSPEC, socket.SOCK_DGRAM)
    except OSError:
        return None

    last_error = None
    for family, socktype, proto, _canon, address in infos:
        sock = None
        try:
            sock = socket.socket(family, socktype, proto)
            sock.settimeout(timeout)

            tx_sec, tx_frac = _to_ntp(time.time())
            packet = bytearray(PACKET_LEN)
            packet[0] = 0x1B  # LI = 0, version = 3, mode = 3 (client)
            struct.pack_into("!II", packet, 40, tx_sec, tx_frac)  # transmit
            t1 = _to_unix(tx_sec, tx_frac)

            sock.sendto(packet, address)
            data, _peer = sock.recvfrom(PACKET_LEN)
            t4 = time.time()

            if len(data) < PACKET_LEN:
                continue
            mode = data[0] & 0x07
            stratum = data[1]
            if mode != 4 or not 1 <= stratum <= 15:
                continue
            orig_sec, orig_frac = struct.unpack_from("!II", data, 24)
            if (orig_sec, orig_frac) != (tx_sec, tx_frac):
                continue  # not the reply to our request
            recv_sec, recv_frac = struct.unpack_from("!II", data, 32)
            send_sec, send_frac = struct.unpack_from("!II", data, 40)
            if (recv_sec, recv_frac) == (0, 0) or (send_sec, send_frac) == (0, 0):
                continue
            t2 = _to_unix(recv_sec, recv_frac)
            t3 = _to_unix(send_sec, send_frac)

            offset = ((t2 - t1) + (t3 - t4)) / 2.0
            delay = (t4 - t1) - (t3 - t2)
            if delay < 0:
                continue

            root_delay = struct.unpack_from("!I", data, 4)[0] / 65536.0
            root_disp = struct.unpack_from("!I", data, 8)[0] / 65536.0
            leap_bits = (data[0] >> 6) & 0x03
            leap = {0: "Normal", 1: "AddSecond", 2: "DelSecond", 3: "Unknown"}[leap_bits]
            return {
                "server": server,
                "offset": offset,
                "delay": delay,
                "stratum": stratum,
                "root_delay": root_delay,
                "root_dispersion": root_disp,
                "leap": leap,
                "unix_time": t3,
            }
        except OSError as exc:
            last_error = exc
        finally:
            if sock is not None:
                sock.close()

    if last_error is not None:
        return None
    return None


def measure(servers, timeout=3.0):
    """Query every server and keep the lowest-delay reply (least queuing)."""
    samples = []
    for server in servers:
        sample = query(server, timeout=timeout)
        if sample is not None:
            samples.append(sample)
    if not samples:
        return None
    best = min(samples, key=lambda s: s["delay"])
    best["replies"] = len(samples)
    best["queried"] = len(servers)
    return best


if __name__ == "__main__":
    import argparse
    import pprint

    ap = argparse.ArgumentParser(description="query NTP servers (measurement only)")
    ap.add_argument("servers", nargs="+")
    ap.add_argument("--timeout", type=float, default=3.0)
    args = ap.parse_args()
    pprint.pprint(measure(args.servers, args.timeout))
