#!/usr/bin/env python3
"""Benchmark the fast terminal path against the raw PTY stream.

  TERMD_BIN=build/termd FAKE_TUI=build/fake_tui python3 bench.py [seconds]

The fake TUI repaints the whole 80x24 screen every 50 ms (FAKE_TUI_FULL=1),
which is the worst case for the old raw-VT transport. The broker coalesces
those repaints into cell diffs at ~80 fps.
"""
import asyncio
import fcntl
import hashlib
import hmac
import os
import pty
import select
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time
from pathlib import Path

import pyte
import websockets

HERE = os.path.dirname(os.path.abspath(__file__))
TERMD = os.environ.get("TERMD_BIN", os.path.join(HERE, "build", "termd"))
FAKE = os.environ.get("FAKE_TUI", os.path.join(HERE, "build", "fake_tui"))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def mint(run, session, ttl=60):
    key = (Path(run) / "rnsbox-term.secret").read_bytes()
    exp = int(time.time()) + ttl
    nonce = os.urandom(8).hex()
    msg = "{}|{}|{}".format(session, exp, nonce).encode()
    return "{}.{}.{}".format(exp, nonce,
                             hmac.new(key, msg, hashlib.sha256).hexdigest())


def start_broker():
    run = tempfile.mkdtemp(prefix="rnsbox-bench-")
    port = free_port()
    env = os.environ.copy()
    env["RNSBOX_TERM_RUN_DIR"] = run
    env["RNSBOX_TERM_NOMADNET_EXE"] = FAKE
    env["RNSBOX_TERM_FRAME_MS"] = "12"
    env["FAKE_TUI_FULL"] = "1"
    log = open(os.path.join(run, "broker.log"), "wb")
    proc = subprocess.Popen([TERMD, str(port), "127.0.0.1"], env=env,
                            stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if os.path.exists(os.path.join(run, "rnsbox-term.port")):
            return proc, port, run, log
        if proc.poll() is not None:
            raise RuntimeError(open(os.path.join(run, "broker.log")).read())
        time.sleep(0.02)
    raise RuntimeError("broker did not start")


def raw_capture(seconds):
    """Bytes the old carrier would have forwarded: the PTY stream itself."""
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    env = os.environ.copy()
    env["FAKE_TUI_FULL"] = "1"
    p = subprocess.Popen([FAKE], stdin=slave, stdout=slave, stderr=slave,
                         env=env, close_fds=True)
    os.close(slave)
    os.set_blocking(master, False)
    total = 0
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        r, _, _ = select.select([master], [], [], 0.05)
        if not r:
            continue
        try:
            data = os.read(master, 65536)
        except BlockingIOError:
            continue
        if not data:
            break
        total += len(data)
    p.terminate()
    try:
        p.wait(2)
    except subprocess.TimeoutExpired:
        p.kill()
    os.close(master)
    return total


class Term:
    def __init__(self, cols=80, rows=24):
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.Stream(self.screen)

    def feed(self, data):
        if not data:
            return
        if data[0] == 2:
            self.screen.reset()
        self.stream.feed(data[1:].decode("utf-8", "replace"))

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)


async def wait_text(ws, term, needle, timeout=3.0):
    end = time.monotonic() + timeout
    while needle not in term.text():
        left = end - time.monotonic()
        if left <= 0:
            raise AssertionError("never saw %r" % needle)
        msg = await asyncio.wait_for(ws.recv(), left)
        if isinstance(msg, bytes):
            term.feed(msg)


async def fast_capture(port, run, seconds):
    token = mint(run, "nomadnet")
    uri = ("ws://127.0.0.1:{}/term?session=nomadnet&token={}&cols=80&rows=24"
           .format(port, token))
    ws = await websockets.connect(uri)
    term = Term()
    first = await asyncio.wait_for(ws.recv(), 5)
    assert isinstance(first, bytes) and first[0] == 2
    term.feed(first)
    snapshot = len(first)

    total = len(first)
    diffs = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        left = end - time.monotonic()
        if left <= 0:
            break
        try:
            msg = await asyncio.wait_for(ws.recv(), left)
        except asyncio.TimeoutError:
            break
        total += len(msg)
        if isinstance(msg, bytes):
            if msg[0] == 1:
                diffs.append(len(msg) - 1)
            term.feed(msg)

    latency = []
    for ch in "ABCDE":
        t0 = time.monotonic()
        await ws.send(b"\x01" + ch.encode())
        await wait_text(ws, term, "key: " + ch)
        latency.append(time.monotonic() - t0)

    await ws.close()
    return total, snapshot, diffs, latency


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 1.5
    proc, port, run, log = start_broker()
    try:
        raw = raw_capture(seconds)
        fast, snapshot, diffs, latency = asyncio.run(
            fast_capture(port, run, seconds))
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()

    avg_diff = sum(diffs) / len(diffs) if diffs else 0
    reduction = (1 - fast / raw) * 100 if raw else 0
    lat_ms = sorted(x * 1000 for x in latency)
    print("window: {:.1f}s, 80x24, full-screen repaint every 50ms".format(seconds))
    print("raw PTY bytes:   {:7d}  ({:.0f} B/s)".format(raw, raw / seconds))
    print("fast-path bytes: {:7d}  ({:.0f} B/s)".format(fast, fast / seconds))
    print("  snapshot:      {:7d} B".format(snapshot))
    print("  diffs:         {:5d} frames, avg {:.1f} B".format(len(diffs), avg_diff))
    print("reduction:       {:.1f}%".format(reduction))
    print("input latency:   min {:.1f} ms, median {:.1f} ms, max {:.1f} ms".format(
        lat_ms[0], lat_ms[len(lat_ms) // 2], lat_ms[-1]))


if __name__ == "__main__":
    main()
