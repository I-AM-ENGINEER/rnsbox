// term.js — xterm.js bridge for the Console and NomadNet tabs.
//
// The page markup (pages/terminal.html) carries the route prefix in
// data-base/data-api; the broker session is selected server-side by that
// prefix. Output is one long-lived chunked HTTP response, input and resize are
// small POSTs — see routes_console.cpp and console_broker.cpp.
(function () {
  'use strict';
  var card = document.getElementById('term-card');
  if (!card) return;
  var BASE = card.getAttribute('data-base') || '';
  var API = card.getAttribute('data-api') || '';
  var wrap = document.getElementById('term-wrap');
  var statusEl = document.getElementById('term-status');
  var dotEl = document.getElementById('term-dot');

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

  var lastCols = 0, lastRows = 0;
  var leaving = false;
  function postResize(cols, rows) {
    // Unloading a tab can collapse the layout to 0x0; resizing the PTY to that
    // makes full-screen TUIs (NomadNet) quit. Keep the last real size instead.
    if (leaving || cols < 20 || rows < 5) return;
    if (cols === lastCols && rows === lastRows) return;
    lastCols = cols; lastRows = rows;
    fetch(API + '/resize?cols=' + cols + '&rows=' + rows, {
      method: 'POST', credentials: 'same-origin', cache: 'no-store'
    }).catch(function () {});
  }
  term.onResize(function (size) {
    postResize(size.cols, size.rows);
  });

  if (window.ResizeObserver) {
    new ResizeObserver(scheduleFit).observe(wrap);
  }
  window.addEventListener('resize', scheduleFit);
  wrap.addEventListener('mousedown', function () { term.focus(); });

  new MutationObserver(function () {
    term.options.theme = currentTheme();
  }).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });

  // ---- input: raw key bytes, sent immediately while a request is free ----
  var pending = [];
  var inputBusy = false;
  var sendTimer = null;

  // Take at most 64 KiB per POST: the portal caps a request body, and a big
  // paste must not vanish. The remainder stays queued for the next request.
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
    // A stalled POST must not freeze the keyboard forever: abort after a few
    // seconds and let the next keystrokes through. Losing one small chunk is
    // much better than an input queue that never drains.
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

  function sendBytes(bytes) {
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

  var encoder = new TextEncoder();
  term.onData(function (data) { sendBytes(encoder.encode(data)); });
  term.onBinary(function (data) {
    var out = new Uint8Array(data.length);
    for (var i = 0; i < data.length; i++) out[i] = data.charCodeAt(i) & 0xff;
    sendBytes(out);
  });

  // ---- output: one long-lived chunked HTTP response ----
  var streamAbort = null;
  var connected = false;
  var attempts = 0;
  var reconnectTimer = null;
  var closed = false;
  var streamWatchdog = null;
  var connectWatchdog = null;

  function clearWatchdogs() {
    if (streamWatchdog) { clearTimeout(streamWatchdog); streamWatchdog = null; }
    if (connectWatchdog) { clearTimeout(connectWatchdog); connectWatchdog = null; }
  }

  function scheduleReconnect() {
    if (closed || reconnectTimer) return;
    var delay = Math.min(500 * Math.pow(2, attempts), 5000);
    attempts++;
    setStatus('reconnecting in ' + Math.round(delay / 1000) + 's…', 'warn');
    reconnectTimer = setTimeout(function () {
      reconnectTimer = null;
      connect();
    }, delay);
  }

  function streamFailed() {
    connected = false;
    clearWatchdogs();
    if (streamAbort) { try { streamAbort.abort(); } catch (e) {} }
    if (closed) return;
    scheduleReconnect();
  }

  function connect() {
    if (closed) return;
    clearWatchdogs();
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    if (streamAbort) {
      try { streamAbort.abort(); } catch (e) {}
    }
    var ctl = new AbortController();
    streamAbort = ctl;
    setStatus('connecting…', '');
    // The server flushes a byte right after the headers, so a connect taking
    // longer than this is a stalled HTTP path, not a slow terminal.
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
      setStatus('connected', 'ok');
      term.reset();
      var reader = r.body.getReader();
      function pump() {
        reader.read().then(function (step) {
          if (ctl.signal.aborted) return;
          if (step.done) { streamFailed(); return; }
          term.write(step.value);
          // The broker sends a heartbeat at least every 10 s. Silence beyond
          // 25 s means the HTTP stream itself is stuck; reconnect instead of
          // leaving the tab frozen.
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

  document.getElementById('term-reconnect').addEventListener('click', function () {
    attempts = 0;
    connect();
    term.focus();
  });
  document.getElementById('term-clear').addEventListener('click', function () {
    term.clear();
    term.focus();
  });

  function leave() {
    leaving = true;
    closed = true;
    clearWatchdogs();
    if (sendTimer) { clearTimeout(sendTimer); sendTimer = null; }
    if (streamAbort) { try { streamAbort.abort(); } catch (e) {} }
  }
  window.addEventListener('beforeunload', leave);
  window.addEventListener('pagehide', leave);

  document.addEventListener('visibilitychange', function () {
    if (!document.hidden && !connected && !closed && !reconnectTimer) connect();
  });

  requestAnimationFrame(function () {
    fitNow();
    term.focus();
    connect();
  });
})();
