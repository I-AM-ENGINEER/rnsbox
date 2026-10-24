# Fast terminal transport — tests and benchmark

The Console and NomadNet tabs used to receive the PTY's raw VT stream over
chunked HTTP and POSTed every keystroke as a new CGI process. On the small
boards that is the wrong shape: the TUI repaints the screen, the CGI tier
forwards every byte, and the browser replays a backlog when the link is slow.

This directory tests the replacement:

* the broker speaks WebSocket on a token-authenticated TCP port (default
  **7681**, LAN only — the firewall drops WAN);
* one server-side VT model per session turns the PTY stream into a compact
  snapshot plus cell diffs;
* frames are coalesced to ~80 fps and a client never has more than one frame in
  flight, so a slow client converges on the latest screen instead of replaying
  history;
* input, resize and mouse/focus/paste modes travel on the same socket. The
  shell keeps the raw-VT transport so xterm.js retains its scrollback;
* the old HTTP path stays as a fallback when `ws://` is unavailable.

## Run

```sh
cd package/rnsbox-portal/tests/term
make test          # C++ unit tests + Python end-to-end + the Node client test
make strict        # broker with the production flags (-Os -fno-exceptions)
make bench         # bytes/latency comparison, full-screen repaint every 50 ms
```

The end-to-end suite starts a real broker, runs `fake_tui` on a PTY, speaks
WebSocket to it and applies the frames to `pyte`. `make test` copies the
binaries to `/tmp` first: `exec()` from `/mnt/c` (9p) can take seconds and makes
the timing tests meaningless. The Node test (`test_client.js`) loads the real
`term.js` with a stub DOM and the real `ws` client; it skips when Node or
`node-ws` is not installed.

## Layout

| file | purpose |
| --- | --- |
| `../../src/cgi/term/screen.*` | VT parser, screen grid, snapshot/diff encoder, Unicode widths |
| `../../src/cgi/term/websocket.*` | RFC 6455 server handshake and framing |
| `../../src/cgi/term/token.*` | short-lived HMAC tokens for the WS attach |
| `test_screen.cpp` | parser, modes, alt screen, scroll regions, diff round-trips |
| `test_ws.cpp` | handshake, masking, fragmentation, ping/pong, denial |
| `test_token.cpp` | mint/validate/expire/tamper |
| `fake_tui.cpp` | urwid-shaped stand-in: alt screen, mouse, arrows, colours, burst mode |
| `test_e2e.py` | snapshot, diffs, input latency, resize, two clients, auth, raw shell |
| `bench.py` | raw PTY bytes vs fast-path bytes, input latency |

## Protocol

Server → client binary frames are `[type][payload]`:

* `1` — ANSI diff from the screen the client was last sent;
* `2` — full snapshot (the client calls `term.reset()` before applying it);
* `3` — raw VT bytes (shell session);
* `4` — raw replay ring on attach (reset first; shell session).

Client → server:

* binary `[1][bytes]` — raw input;
* text `R <cols> <rows>` — resize;
* text `H` is ignored (server heartbeat is the other direction).

The server also sends a small JSON `{"t":"m",...}` with the modes the page needs
for the wheel fallback (alt screen, mouse reporting, application cursor keys).
