"""End-to-end tests for the fast terminal path (real PTY + WebSocket + pyte).

Run through `make test` from this directory.  The broker is started on a
loopback port for each test; the NomadNet session runs ./build/fake_tui, a
small stand-in for urwid that uses the same screen/mouse/colour features.
"""
import asyncio
import hashlib
import hmac
import json
import os
import shutil
import socket
import subprocess
import time

import pytest
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


def mint(run_dir, session, ttl=60):
    key = (run_dir / "rnsbox-term.secret").read_bytes()
    exp = int(time.time()) + ttl
    nonce = os.urandom(8).hex()
    msg = "{}|{}|{}".format(session, exp, nonce).encode()
    mac = hmac.new(key, msg, hashlib.sha256).hexdigest()
    return "{}.{}.{}".format(exp, nonce, mac)


def uri(port, session, token, cols=80, rows=24):
    return ("ws://127.0.0.1:{}/term?session={}&token={}&cols={}&rows={}"
            .format(port, session, token, cols, rows))


def _start_broker(tmp_path, extra_env=None):
    port = free_port()
    log = open(tmp_path / "broker.log", "wb")
    env = os.environ.copy()
    env["RNSBOX_TERM_RUN_DIR"] = str(tmp_path)
    env["RNSBOX_TERM_NOMADNET_EXE"] = FAKE
    env["RNSBOX_TERM_FRAME_MS"] = "12"
    if extra_env:
        env.update(extra_env)
    proc = subprocess.Popen(
        [TERMD, str(port), "127.0.0.1"],
        env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if (tmp_path / "rnsbox-term.port").exists() and \
           (tmp_path / "rnsbox-term.secret").exists():
            return proc, port, log
        if proc.poll() is not None:
            log.close()
            raise RuntimeError("broker exited: " + (tmp_path / "broker.log").read_text())
        time.sleep(0.02)
    proc.terminate()
    log.close()
    raise RuntimeError("broker did not start")


def _stop_broker(proc, log):
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()


def _fixture(tmp_path, extra_env=None):
    proc, port, log = _start_broker(tmp_path, extra_env)
    return proc, port, log


@pytest.fixture
def termd(tmp_path):
    proc, port, log = _fixture(tmp_path)
    yield port, tmp_path
    _stop_broker(proc, log)


@pytest.fixture
def termd_nomouse(tmp_path):
    proc, port, log = _fixture(tmp_path, {"FAKE_TUI_NO_MOUSE": "1"})
    yield port, tmp_path
    _stop_broker(proc, log)


class Term:
    """Apply the broker's frames to a pyte screen."""

    def __init__(self, cols=80, rows=24):
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.Stream(self.screen)
        self.diff_bytes = 0
        self.diff_frames = 0
        self.snapshots = 0

    def feed(self, data):
        if not data:
            return
        t = data[0]
        body = data[1:]
        if t == 2:
            self.snapshots += 1
            self.screen.reset()
        elif t == 1:
            self.diff_frames += 1
            self.diff_bytes += len(body)
        self.stream.feed(body.decode("utf-8", "replace"))

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)


async def open_term(port, run, session="nomadnet", cols=80, rows=24, **kw):
    ws = await websockets.connect(
        uri(port, session, mint(run, session), cols, rows), **kw)
    term = Term(cols, rows)
    msg = await asyncio.wait_for(ws.recv(), 5)
    assert isinstance(msg, bytes) and msg[0] == 2, repr(msg)[:80]
    term.feed(msg)
    # The session is spawned on this first attach, so the first snapshot can
    # legitimately be empty.  Wait for the TUI's initial frame.
    await wait_text(ws, term, "RNSBox fast terminal test", timeout=5)
    return ws, term


async def wait_text(ws, term, needle, timeout=3.0):
    end = time.monotonic() + timeout
    while True:
        if needle in term.text():
            return time.monotonic()
        left = end - time.monotonic()
        if left <= 0:
            raise AssertionError("never saw {!r}; screen:\n{}".format(needle, term.text()))
        try:
            msg = await asyncio.wait_for(ws.recv(), left)
        except asyncio.TimeoutError:
            raise AssertionError("never saw {!r}; screen:\n{}".format(needle, term.text()))
        if isinstance(msg, bytes):
            term.feed(msg)


async def drain(ws, term, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            msg = await asyncio.wait_for(ws.recv(), max(0.01, end - time.monotonic()))
        except asyncio.TimeoutError:
            break
        if isinstance(msg, bytes):
            term.feed(msg)


def test_snapshot(termd):
    port, run = termd

    async def go():
        ws, term = await open_term(port, run)
        assert "RNSBox fast terminal test" in term.text()
        assert "tick " in term.text()
        await ws.close()

    asyncio.run(go())


def test_modes_message(termd):
    """The page needs the current modes for the wheel fallback."""
    port, run = termd

    async def go():
        ws = await websockets.connect(uri(port, "nomadnet", mint(run, "nomadnet")))
        end = time.monotonic() + 5
        while time.monotonic() < end:
            msg = await asyncio.wait_for(ws.recv(), end - time.monotonic())
            if isinstance(msg, str) and msg.startswith("{"):
                o = json.loads(msg)
                if o.get("t") == "m" and o.get("alt") and o.get("mouse") == 1000:
                    assert o.get("enc") == 1006
                    await ws.close()
                    return
        raise AssertionError("no modes message with alt+mouse")

    asyncio.run(go())


def test_animation_uses_small_diffs(termd):
    port, run = termd

    async def go():
        ws, term = await open_term(port, run)
        before = term.text()
        await drain(ws, term, 0.8)
        assert term.diff_frames >= 5, term.diff_frames
        avg = term.diff_bytes / term.diff_frames
        assert avg < 200, "average diff {} bytes".format(avg)
        # The tick counter really advanced.
        assert term.text() != before
        await ws.close()

    asyncio.run(go())


def test_input_roundtrip(termd):
    port, run = termd

    async def go():
        ws, term = await open_term(port, run)
        t0 = time.monotonic()
        await ws.send(b"\x01X")
        t1 = await wait_text(ws, term, "key: X")
        assert t1 - t0 < 0.5, "input latency {}".format(t1 - t0)

        await ws.send(b"\x01\x1b[A")
        await wait_text(ws, term, "arrow: UP")

        await ws.send(b"\x01\x1b[<0;10;5M")
        await wait_text(ws, term, "mouse: yes")
        await ws.close()

    asyncio.run(go())


def test_resize_sends_snapshot(termd):
    port, run = termd

    async def go():
        ws, term = await open_term(port, run)
        await ws.send("R 100 30")
        end = time.monotonic() + 3
        while time.monotonic() < end:
            msg = await asyncio.wait_for(ws.recv(), end - time.monotonic())
            if isinstance(msg, bytes) and msg[0] == 2:
                big = Term(100, 30)
                big.feed(msg)
                assert big.screen.columns == 100
                assert big.screen.lines == 30
                assert "RNSBox fast terminal test" in big.text()
                await ws.close()
                return
        raise AssertionError("no snapshot after resize")

    asyncio.run(go())


def test_exit_message(termd):
    port, run = termd

    async def go():
        ws, term = await open_term(port, run)
        await ws.send(b"\x01q")
        await wait_text(ws, term, "NomadNet exited", timeout=4)
        await ws.close()

    asyncio.run(go())


def test_burst_is_coalesced(termd):
    """100 immediate full repaints must not become 100 queued frames."""
    port, run = termd

    async def go():
        ws, term = await open_term(port, run)
        frames0 = term.diff_frames
        bytes0 = term.diff_bytes
        await ws.send(b"\x01b")
        await wait_text(ws, term, "BURST done", timeout=6)
        await drain(ws, term, 0.3)
        # The point is that a 100-repaint burst does not become a 100-frame
        # backlog, and the bytes stay small (the final screen converges).
        burst_frames = term.diff_frames - frames0
        burst_bytes = term.diff_bytes - bytes0
        assert burst_frames < 90, "{} frames for a 100-frame burst".format(
            burst_frames)
        assert burst_bytes < 20000, "{} diff bytes".format(burst_bytes)
        await ws.close()

    asyncio.run(go())


def test_two_clients(termd):
    port, run = termd

    async def go():
        ws1, t1 = await open_term(port, run)
        ws2, t2 = await open_term(port, run)
        await ws1.send(b"\x01Z")
        await wait_text(ws1, t1, "key: Z")
        await wait_text(ws2, t2, "key: Z")
        await ws1.close()
        await ws2.close()

    asyncio.run(go())


def _expect_status(port, run, session, token, status, **kw):
    async def go():
        await websockets.connect(uri(port, session, token), **kw)

    with pytest.raises(websockets.InvalidStatusCode) as ei:
        asyncio.run(go())
    assert ei.value.status_code == status


def test_bad_token_is_401(termd):
    port, run = termd
    _expect_status(port, run, "nomadnet", "1.abc.def", 401)


def test_expired_token_is_401(termd):
    port, run = termd
    _expect_status(port, run, "nomadnet", mint(run, "nomadnet", ttl=-10000), 401)


def test_unknown_session_is_404(termd):
    port, run = termd
    _expect_status(port, run, "nope", mint(run, "nope"), 404)


def test_origin_mismatch_is_403(termd):
    port, run = termd
    _expect_status(port, run, "nomadnet", mint(run, "nomadnet"), 403,
                   origin="http://evil.example")


def test_node_client(termd_nomouse):
    """Load the real term.js in a DOM stub and drive it over WebSocket."""
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    probe = subprocess.run([node, "-e", "require('ws')"], capture_output=True)
    if probe.returncode != 0:
        pytest.skip("node-ws is not installed")

    port, run = termd_nomouse
    token = mint(run, "nomadnet")
    p = subprocess.run(
        [node, os.path.join(HERE, "test_client.js"), str(port), token, "127.0.0.1"],
        capture_output=True, text=True, timeout=25)
    assert p.returncode == 0, p.stdout + p.stderr


def test_shell_session_is_raw(termd):
    """The shell keeps the raw-VT transport (scrollback stays in xterm.js)."""
    port, run = termd

    async def go():
        ws = await websockets.connect(uri(port, "shell", mint(run, "shell")))
        # Absolute positioning and a marker, so this is valid raw VT either way.
        await ws.send(b"\x01echo FASTPATH_$((6*7))\n")
        end = time.monotonic() + 4
        seen = b""
        while time.monotonic() < end:
            try:
                msg = await asyncio.wait_for(ws.recv(), end - time.monotonic())
            except asyncio.TimeoutError:
                break
            if isinstance(msg, bytes):
                # 4 = replay ring on attach (resets xterm), 3 = live raw VT.
                assert msg[0] in (3, 4), "shell frames must be raw VT, got {}".format(msg[0])
                seen += msg[1:]
                if b"FASTPATH_42" in seen:
                    await ws.close()
                    return
        raise AssertionError("shell output missing; got {!r}".format(seen[-200:]))

    asyncio.run(go())
