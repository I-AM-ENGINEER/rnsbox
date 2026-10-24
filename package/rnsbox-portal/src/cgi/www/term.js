// term.js — xterm.js bridge for the Console and NomadNet tabs.
//
// Fast path: fetch a short-lived token, open a WebSocket to the broker, and
// receive [type][payload] binary frames (1 = cell diff, 2 = full snapshot,
// 3 = raw VT, 4 = raw replay on attach).  Input goes back over the same socket
// as [1][bytes], so a keystroke no longer spawns a CGI process.  The legacy
// chunked-HTTP stream is kept as a fallback for https pages, blocked ports or
// an old broker.
(function () {
  'use strict';
  var card = document.getElementById('term-card');
  if (!card) return;
  var BASE = card.getAttribute('data-base') || '';
  var API = card.getAttribute('data-api') || '';
  var wrap = document.getElementById('term-wrap');
  var statusEl = document.getElementById('term-status');
  var dotEl = document.getElementById('term-dot');
  // NomadNet is started from its Connect button only; the Console keeps
  // connecting as soon as the page opens.
  var autostart = card.getAttribute('data-autostart') !== '0';
  var connectBtn = document.getElementById('term-connect');
  var disconnectBtn = document.getElementById('term-disconnect');
  var started = autostart;

  var darkTheme = {
    background: '#0b141a', foreground: '#d9e7f2', cursor: '#7ee787',
    cursorAccent: '#0b141a', selectionBackground: 'rgba(126,231,135,0.28)',
    black: '#1b2229', red: '#f07178', green: '#7ee787', yellow: '#e3b341',
    blue: '#6cb6ff', magenta: '#d2a8ff', cyan: '#56d4dd', white: '#d9e7f2',
    brightBlack: '#6e7f8d', brightRed: '#ff8b92', brightGreen: '#9af0a1',
    brightYellow: '#f2cc60', brightBlue: '#8cc8ff', brightMagenta: '#e2c5ff',
    brightCyan: '#76e4eb', brightWhite: '#ffffff'
  };
  var lightTheme = {
    background: '#fbfcfd', foreground: '#1f2933', cursor: '#0b6bcb',
    cursorAccent: '#fbfcfd', selectionBackground: 'rgba(11,107,203,0.20)',
    black: '#1f2933', red: '#b42318', green: '#1a7f37', yellow: '#8a6100',
    blue: '#0b6bcb', magenta: '#8250df', cyan: '#0e7490', white: '#e8edf2',
    brightBlack: '#5b6770', brightRed: '#d1242f', brightGreen: '#2da44e',
    brightYellow: '#9a6700', brightBlue: '#218bff', brightMagenta: '#a475f9',
    brightCyan: '#1b9aaa', brightWhite: '#ffffff'
  };
  function currentTheme() {
    return document.documentElement.getAttribute('data-theme') === 'light'
      ? lightTheme : darkTheme;
  }

  var term = new Terminal({
    cursorBlink: true,
    cursorStyle: 'block',
    scrollback: 5000,
    fontSize: 13,
    lineHeight: 1.2,
    // NomadNet/urwid draw UI ornaments from the whole Unicode BMP: rounded
    // box corners (U+256D..U+2570), ❬❭, ⧖, ∿ and emoji.  Consolas has no
    // glyphs for several of them, so keep monospace for the cell grid and let
    // the symbol/emoji faces fill in the gaps.
    fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, "Cascadia Mono", ' +
                '"DejaVu Sans Mono", "Noto Sans Mono", "Segoe UI Symbol", ' +
                '"Segoe UI Emoji", "Apple Color Emoji", monospace',
    theme: currentTheme(),
    allowTransparency: false
  });
  var fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(document.getElementById('term'));

  function setStatus(text, cls) {
    statusEl.textContent = text;
    dotEl.className = 'term-dot' + (cls ? ' ' + cls : '');
  }

  function fitNow() {
    try { fit.fit(); } catch (e) { /* container not laid out yet */ }
  }

  var resizeTimer = null;
  function scheduleFit() {
    if (resizeTimer) clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
      resizeTimer = null;
      fitNow();
    }, 120);
  }

  // ---- shared state ----
  var lastCols = 0, lastRows = 0;
  var leaving = false, closed = false, connected = false;
  var attempts = 0;
  var reconnectTimer = null;
  var streamAbort = null, streamWatchdog = null, connectWatchdog = null;
  var encoder = new TextEncoder();

  var fast = {
    attempting: false,
    disabled: false,
    ws: null,
    open: false,
    failed: 0,
    pending: [],
    watchdog: null,
    modes: null
  };
  var fastTimer = null;

  function postResize(cols, rows) {
    // Unloading a tab can collapse the layout to 0x0; resizing the PTY to that
    // makes full-screen TUIs (NomadNet) quit. Keep the last real size instead.
    if (leaving || cols < 20 || rows < 5) return;
    if (cols === lastCols && rows === lastRows) return;
    lastCols = cols; lastRows = rows;
    if (fast.open && fast.ws) {
      try { fast.ws.send('R ' + cols + ' ' + rows); } catch (e) {}
      return;
    }
    fetch(API + '/resize?cols=' + cols + '&rows=' + rows, {
      method: 'POST', credentials: 'same-origin', cache: 'no-store'
    }).catch(function () {});
  }
  term.onResize(function (size) { postResize(size.cols, size.rows); });

  if (window.ResizeObserver) {
    new ResizeObserver(scheduleFit).observe(wrap);
  }
  window.addEventListener('resize', scheduleFit);
  wrap.addEventListener('mousedown', function () { term.focus(); });

  new MutationObserver(function () {
    term.options.theme = currentTheme();
  }).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });

  // ---- input ----
  // Legacy: raw key bytes are POSTed, coalesced into ~15 ms batches.
  var pending = [];
  var inputBusy = false;
  var sendTimer = null;
  var INPUT_CHUNK = 64 * 1024;

  function takeInput() {
    var chunks = [], total = 0;
    while (pending.length && total < INPUT_CHUNK) {
      var chunk = pending[0], room = INPUT_CHUNK - total;
      if (chunk.length <= room) {
        chunks.push(chunk);
        total += chunk.length;
        pending.shift();
      } else {
        chunks.push(chunk.subarray(0, room));
        pending[0] = chunk.subarray(room);
        total = INPUT_CHUNK;
      }
    }
    var buf = new Uint8Array(total), off = 0;
    for (var i = 0; i < chunks.length; i++) { buf.set(chunks[i], off); off += chunks[i].length; }
    return buf;
  }

  function flushInput() {
    if (inputBusy || pending.length === 0) return;
    if (sendTimer) { clearTimeout(sendTimer); sendTimer = null; }
    var buf = takeInput();
    inputBusy = true;
    var ctl = new AbortController();
    var timer = setTimeout(function () {
      try { ctl.abort(); } catch (e) {}
    }, 5000);
    fetch(API + '/input', {
      method: 'POST',
      credentials: 'same-origin',
      cache: 'no-store',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: buf,
      signal: ctl.signal
    }).catch(function () {}).then(function () {
      clearTimeout(timer);
      inputBusy = false;
      if (pending.length) flushInput();
    });
  }

  function fastFrame(bytes) {
    var f = new Uint8Array(bytes.length + 1);
    f[0] = 1;                 // 1 = raw input bytes
    f.set(bytes, 1);
    return f;
  }

  function sendFastOrQueue(bytes) {
    // Keep each WebSocket frame small: a large paste must not hit the broker's
    // per-frame cap, and ordering is preserved by the socket itself.
    var CHUNK = 64 * 1024;
    if (fast.open && fast.ws) {
      try {
        for (var off = 0; off < bytes.length; off += CHUNK) {
          fast.ws.send(fastFrame(bytes.subarray(off, Math.min(bytes.length, off + CHUNK))));
        }
        return;
      } catch (e) { /* fall through */ }
    }
    for (var off2 = 0; off2 < bytes.length; off2 += CHUNK) {
      fast.pending.push(fastFrame(bytes.subarray(off2, Math.min(bytes.length, off2 + CHUNK))));
    }
    while (fast.pending.length > 32) fast.pending.shift();
  }

  function sendBytes(bytes) {
    if (fast.ws || fast.attempting) {
      sendFastOrQueue(bytes);
      return;
    }
    pending.push(bytes);
    // A tiny debounce coalesces fast typing/paste into fewer CGI requests
    // while staying below the threshold of perception for a single key.
    if (inputBusy) return;
    if (sendTimer) clearTimeout(sendTimer);
    sendTimer = setTimeout(function () {
      sendTimer = null;
      flushInput();
    }, 15);
  }

  term.onData(function (data) { sendBytes(encoder.encode(data)); });
  term.onBinary(function (data) {
    var out = new Uint8Array(data.length);
    for (var i = 0; i < data.length; i++) out[i] = data.charCodeAt(i) & 0xff;
    sendBytes(out);
  });

  function flushFastPending() {
    if (!fast.open || !fast.ws) return;
    var q = fast.pending;
    fast.pending = [];
    for (var i = 0; i < q.length; i++) {
      try { fast.ws.send(q[i]); } catch (e) {
        fast.pending = q.slice(i);
        return;
      }
    }
  }

  function drainFastToLegacy() {
    if (!fast.pending.length) return;
    var q = fast.pending;
    fast.pending = [];
    for (var i = 0; i < q.length; i++) pending.push(q[i].subarray(1));
    flushInput();
  }

  function touchFastWatchdog() {
    if (fast.watchdog) clearTimeout(fast.watchdog);
    fast.watchdog = setTimeout(function () {
      fast.watchdog = null;
      if (fast.ws) { try { fast.ws.close(); } catch (e) {} }
    }, 25000);
  }

  function clearFastWatchdog() {
    if (fast.watchdog) { clearTimeout(fast.watchdog); fast.watchdog = null; }
  }

  function handleFastText(s) {
    if (s === 'H') return;            // heartbeat, already touched
    if (s.charAt(0) !== '{') return;
    try {
      var o = JSON.parse(s);
      if (o && o.t === 'm') fast.modes = o;
    } catch (e) {}
  }

  function scheduleFastReconnect() {
    if (!started || closed || fastTimer) return;
    var delay = Math.min(500 * Math.pow(2, fast.failed), 5000);
    setStatus('reconnecting in ' + Math.round(delay / 1000) + 's…', 'warn');
    fastTimer = setTimeout(function () {
      fastTimer = null;
      connect();
    }, delay);
  }

  // ---- fast path: WebSocket ----
  function connectFast() {
    if (!started || closed || fast.disabled) return false;
    if (!window.WebSocket || location.protocol !== 'http:') return false;
    fast.attempting = true;
    setStatus('connecting…', '');
    fetch(API + '/ws-token', { credentials: 'same-origin', cache: 'no-store' })
      .then(function (r) {
        if (!r.ok) throw new Error('fast path unavailable');
        return r.json();
      })
      .then(function (info) {
        if (closed) return null;
        if (!info || !info.port || !info.token) throw new Error('fast path unavailable');
        var url = 'ws://' + location.hostname + ':' + info.port +
          '/term?session=' + encodeURIComponent(info.session) +
          '&token=' + encodeURIComponent(info.token) +
          '&cols=' + (term.cols || 80) + '&rows=' + (term.rows || 24);
        var ws;
        try { ws = new WebSocket(url); } catch (e) {
          throw new Error('ws failed');
        }
        ws.binaryType = 'arraybuffer';
        fast.ws = ws;
        fast.open = false;
        var opened = false;

        ws.onopen = function () {
          opened = true;
          fast.open = true;
          fast.attempting = false;
          fast.failed = 0;
          connected = true;
          attempts = 0;
          setStatus('connected', 'ok');
          try { ws.send('R ' + (term.cols || 80) + ' ' + (term.rows || 24)); } catch (e) {}
          flushFastPending();
          touchFastWatchdog();
        };

        ws.onmessage = function (ev) {
          touchFastWatchdog();
          if (typeof ev.data === 'string') { handleFastText(ev.data); return; }
          var u = new Uint8Array(ev.data);
          if (!u.length) return;
          if (u[0] === 2 || u[0] === 4) {
            // 2 = screen snapshot, 4 = raw replay after a (re)attach: both are
            // a fresh terminal state, so reset before painting.
            term.reset();
            term.write(u.subarray(1));
          } else {
            term.write(u.subarray(1));
          }
        };

        ws.onerror = function () { /* onclose follows */ };

        ws.onclose = function () {
          fast.open = false;
          fast.ws = null;
          fast.attempting = false;
          clearFastWatchdog();
          connected = false;
          if (closed) return;
          if (!opened) {
            fast.failed++;
            if (fast.failed >= 2) {
              fast.disabled = true;
              drainFastToLegacy();
              connectLegacy();
              return;
            }
          }
          scheduleFastReconnect();
        };
        return null;
      })
      .catch(function () {
        fast.attempting = false;
        fast.failed++;
        if (fast.failed >= 2) {
          fast.disabled = true;
          drainFastToLegacy();
          connectLegacy();
        } else {
          scheduleFastReconnect();
        }
      });
    return true;
  }

  // ---- legacy path: chunked HTTP ----
  function clearWatchdogs() {
    if (streamWatchdog) { clearTimeout(streamWatchdog); streamWatchdog = null; }
    if (connectWatchdog) { clearTimeout(connectWatchdog); connectWatchdog = null; }
  }

  function scheduleReconnect() {
    if (!started || closed || reconnectTimer) return;
    var delay = Math.min(500 * Math.pow(2, attempts), 5000);
    attempts++;
    setStatus('reconnecting in ' + Math.round(delay / 1000) + 's…', 'warn');
    reconnectTimer = setTimeout(function () {
      reconnectTimer = null;
      connectLegacy();
    }, delay);
  }

  function streamFailed() {
    connected = false;
    clearWatchdogs();
    if (streamAbort) { try { streamAbort.abort(); } catch (e) {} }
    if (!started || closed) return;
    scheduleReconnect();
  }

  function connectLegacy() {
    if (!started || closed) return;
    clearWatchdogs();
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    if (streamAbort) {
      try { streamAbort.abort(); } catch (e) {}
    }
    var ctl = new AbortController();
    streamAbort = ctl;
    setStatus('connecting…', '');
    connectWatchdog = setTimeout(function () {
      connectWatchdog = null;
      streamFailed();
    }, 10000);
    var cols = term.cols || 80, rows = term.rows || 24;

    fetch(API + '/stream?cols=' + cols + '&rows=' + rows, {
      cache: 'no-store',
      credentials: 'same-origin',
      signal: ctl.signal
    }).then(function (r) {
      if (ctl.signal.aborted) return null;
      if (connectWatchdog) { clearTimeout(connectWatchdog); connectWatchdog = null; }
      if (r.redirected) { window.location.href = r.url; return null; }
      if (r.status !== 200) {
        return r.text().then(function (t) {
          throw new Error(t || ('HTTP ' + r.status));
        });
      }
      var ct = r.headers.get('content-type') || '';
      if (ct.indexOf('application/octet-stream') < 0) {
        window.location.href = BASE + '/login';
        return null;
      }
      if (!r.body || !r.body.getReader) {
        throw new Error('This browser cannot stream the terminal');
      }
      connected = true;
      attempts = 0;
      setStatus('connected (fallback)', 'ok');
      term.reset();
      var reader = r.body.getReader();
      function pump() {
        reader.read().then(function (step) {
          if (ctl.signal.aborted) return;
          if (step.done) { streamFailed(); return; }
          term.write(step.value);
          if (streamWatchdog) clearTimeout(streamWatchdog);
          streamWatchdog = setTimeout(function () {
            streamWatchdog = null;
            streamFailed();
          }, 25000);
          pump();
        }).catch(function () {
          if (!ctl.signal.aborted) streamFailed();
        });
      }
      pump();
      return null;
    }).catch(function (err) {
      if (ctl.signal.aborted || closed) return;
      clearWatchdogs();
      connected = false;
      setStatus(err && err.message ? String(err.message) : 'console unavailable', 'err');
      scheduleReconnect();
    });
  }

  function connect() {
    if (!started || closed) return;
    clearWatchdogs();
    clearFastWatchdog();
    if (fastTimer) { clearTimeout(fastTimer); fastTimer = null; }
    if (!fast.disabled && window.WebSocket && location.protocol === 'http:') {
      if (connectFast()) return;
    }
    connectLegacy();
  }

  // Wheel fallback for full-screen apps that do not enable mouse reporting:
  // their alternate screen has no scrollback for xterm to move, so send arrow
  // keys (the classic "alternate scroll" behaviour).  When the app does ask
  // for mouse events (1000/1002/1003), xterm.js encodes the wheel itself.
  wrap.addEventListener('wheel', function (ev) {
    if (!fast.open || !fast.modes) return;
    if (!fast.modes.alt || fast.modes.mouse) return;
    if (!ev.deltaY) return;
    ev.preventDefault();
    var seq = fast.modes.app ? (ev.deltaY < 0 ? '\x1bOA' : '\x1bOB')
                             : (ev.deltaY < 0 ? '\x1b[A' : '\x1b[B');
    var n = Math.max(1, Math.min(5, Math.round(Math.abs(ev.deltaY) / 40) || 1));
    var s = '';
    for (var i = 0; i < n; i++) s += seq;
    sendBytes(encoder.encode(s));
  }, { passive: false });

  function startConnect() {
    started = true;
    attempts = 0;
    fast.failed = 0;
    fast.disabled = false;
    if (fastTimer) { clearTimeout(fastTimer); fastTimer = null; }
    if (fast.ws) {
      try { fast.ws.onclose = null; fast.ws.close(); } catch (e) {}
      fast.ws = null;
      fast.open = false;
    }
    if (streamAbort) { try { streamAbort.abort(); } catch (e) {} }
    connect();
    term.focus();
    if (connectBtn) connectBtn.style.display = 'none';
    if (disconnectBtn && !autostart) disconnectBtn.style.display = '';
  }
  document.getElementById('term-reconnect').addEventListener('click', startConnect);
  if (connectBtn) {
    connectBtn.addEventListener('click', startConnect);
    if (!autostart) {
      connectBtn.style.display = '';
      if (disconnectBtn) disconnectBtn.style.display = 'none';
      var rcBtn = document.getElementById('term-reconnect');
      var clBtn = document.getElementById('term-clear');
      if (rcBtn) rcBtn.style.display = 'none';
      if (clBtn) clBtn.style.display = 'none';
      setStatus('disconnected — press Connect', 'warn');
    }
  }

  // Detach from the session without reloading the page. The broker freezes
  // the NomadNet TUI after 30 s with no client, so this also stops the CPU
  // load. A later Connect re-attaches to the same session.
  function disconnect() {
    started = false;
    connected = false;
    fast.attempting = false;
    if (fastTimer) { clearTimeout(fastTimer); fastTimer = null; }
    if (fast.ws) {
      try { fast.ws.onclose = null; fast.ws.close(); } catch (e) {}
      fast.ws = null;
      fast.open = false;
    }
    if (streamAbort) { try { streamAbort.abort(); } catch (e) {} }
    clearWatchdogs();
    clearFastWatchdog();
    setStatus('disconnected — press Connect', 'warn');
    dotEl.className = 'term-dot';
    if (connectBtn) connectBtn.style.display = '';
    if (disconnectBtn) disconnectBtn.style.display = 'none';
  }
  if (disconnectBtn) disconnectBtn.addEventListener('click', disconnect);
  document.getElementById('term-clear').addEventListener('click', function () {
    term.clear();
    term.focus();
  });

  function leave() {
    leaving = true;
    closed = true;
    clearWatchdogs();
    clearFastWatchdog();
    if (sendTimer) { clearTimeout(sendTimer); sendTimer = null; }
    if (fastTimer) { clearTimeout(fastTimer); fastTimer = null; }
    if (fast.ws) { try { fast.ws.close(); } catch (e) {} fast.ws = null; fast.open = false; }
    if (streamAbort) { try { streamAbort.abort(); } catch (e) {} }
  }
  window.addEventListener('beforeunload', leave);
  window.addEventListener('pagehide', leave);

  document.addEventListener('visibilitychange', function () {
    if (!document.hidden && started && !connected && !closed && !reconnectTimer && !fastTimer) connect();
  });

  requestAnimationFrame(function () {
    fitNow();
    term.focus();
    if (autostart) connect();
  });
})();
