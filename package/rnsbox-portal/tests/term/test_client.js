// test_client.js — run the real term.js against the real broker.
//
// The browser is unavailable in this environment, so we provide the small
// slice of the DOM/xterm.js API that term.js touches, inject the real ws
// client, and let the script talk to the broker exactly as a page would.
//
// Usage: node test_client.js <port> <token> [host]
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const WebSocket = require('ws');

if (process.env.CLIENT_DEBUG) {
  const origSend = WebSocket.prototype.send;
  WebSocket.prototype.send = function (data) {
    try {
      console.error('SEND', typeof data, data && data.length);
      return origSend.call(this, data);
    } catch (e) {
      console.error('SEND THREW', e && e.message);
      throw e;
    }
  };
}

const port = process.argv[2];
const token = process.argv[3];
const host = process.argv[4] || '127.0.0.1';
if (!port || !token) {
  console.error('usage: node test_client.js <port> <token> [host]');
  process.exit(2);
}

let term = null;
const handlers = {};
const noopEl = { addEventListener() {}, appendChild() {}, textContent: '' };
const card = {
  getAttribute(name) {
    if (name === 'data-base') return '';
    if (name === 'data-api') return '/terminal';
    return null;
  }
};
const wrap = {
  addEventListener(type, fn) { (handlers[type] || (handlers[type] = [])).push(fn); }
};
const statusEl = { textContent: '', className: '' };
const dotEl = { textContent: '', className: '' };

const document = {
  getElementById(id) {
    if (id === 'term-card') return card;
    if (id === 'term-wrap') return wrap;
    if (id === 'term-status') return statusEl;
    if (id === 'term-dot') return dotEl;
    return noopEl;
  },
  documentElement: { getAttribute() { return 'dark'; } },
  addEventListener(type, fn) {
    (handlers['doc:' + type] || (handlers['doc:' + type] = [])).push(fn);
  }
};

class Terminal {
  constructor(options) {
    this.options = options;
    this.cols = 80;
    this.rows = 24;
    this.buf = '';
    this.decoder = new TextDecoder();
    this.screen = new MiniScreen(80, 24);
    term = this;
  }
  open() {}
  loadAddon() {}
  onData(cb) { this._data = cb; }
  onBinary(cb) { this._bin = cb; }
  onResize(cb) { this._resize = cb; }
  write(data) {
    const s = (typeof data === 'string') ? data : this.decoder.decode(data, { stream: true });
    this.buf += s;
    this.screen.feed(s);
  }
  reset() { this.buf = ''; this.screen.clear(); }
  clear() { this.screen.clear(); }
  focus() {}
}

// Just enough of a terminal to check the rendered labels.  The server only
// emits absolute cursor moves, SGR resets and whole-screen clears; the box
// characters are not inspected by the assertions.
class MiniScreen {
  constructor(cols, rows) {
    this.cols = cols;
    this.rows = rows;
    this.grid = Array.from({ length: rows }, () => Array(cols).fill(' '));
    this.r = 0;
    this.c = 0;
  }
  clear() {
    for (const row of this.grid) row.fill(' ');
    this.r = 0;
    this.c = 0;
  }
  feed(s) {
    let i = 0;
    while (i < s.length) {
      const ch = s[i];
      if (ch === '\x1b') {
        if (s[i + 1] === '[') {
          let j = i + 2;
          while (j < s.length && !/[A-Za-z]/.test(s[j])) j++;
          const final = s[j];
          const body = s.slice(i + 2, j);
          if (final === 'H') {
            const parts = body.split(';');
            this.r = (Number(parts[0]) || 1) - 1;
            this.c = (Number(parts[1]) || 1) - 1;
          } else if (final === 'J' && body === '2') {
            this.clear();
          }
          i = j + 1;
        } else {
          i += 2;
        }
        continue;
      }
      if (ch === '\r') { this.c = 0; i++; continue; }
      if (ch === '\n') { this.r++; i++; continue; }
      if (ch === '\x00' || ch === '\x07') { i++; continue; }
      if (this.r >= 0 && this.r < this.rows && this.c >= 0 && this.c < this.cols)
        this.grid[this.r][this.c] = ch;
      this.c++;
      i++;
    }
  }
  text() {
    return this.grid.map((r) => r.join('').replace(/\s+$/, '')).join('\n');
  }
}

class Fit { fit() {} }
class RO { constructor(cb) {} observe() {} }
class MO { constructor(cb) {} observe() {} }

const fetchStub = async (url) => {
  if (String(url).indexOf('/ws-token') >= 0) {
    return {
      ok: true,
      json: async () => ({ port: Number(port), session: 'nomadnet', token })
    };
  }
  return { ok: false, status: 404, text: async () => 'not found' };
};

const sandbox = {
  document,
  location: { protocol: 'http:', hostname: host },
  Terminal,
  FitAddon: { FitAddon: Fit },
  ResizeObserver: RO,
  MutationObserver: MO,
  requestAnimationFrame: (cb) => setTimeout(cb, 0),
  fetch: fetchStub,
  WebSocket,
  TextEncoder,
  TextDecoder,
  AbortController,
  setTimeout,
  clearTimeout,
  console,
  JSON,
  Math,
  Uint8Array,
  String,
  Number,
  Object,
  Date
};
sandbox.window = sandbox;
sandbox.window.addEventListener = (type, fn) => {
  (handlers['win:' + type] || (handlers['win:' + type] = [])).push(fn);
};
sandbox.window.ResizeObserver = RO;

vm.createContext(sandbox);
const code = fs.readFileSync(
  path.join(__dirname, '..', '..', 'src', 'cgi', 'www', 'term.js'), 'utf8');
vm.runInContext(code, sandbox, { filename: 'term.js' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitFor(fn, timeout, what) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    if (fn()) return;
    await sleep(20);
  }
  throw new Error('timeout waiting for ' + what + '\nbuffer tail:\n' +
                  (term ? term.buf.slice(-1500) : '(no terminal)'));
}

(async () => {
  await waitFor(() => term && term.screen.text().indexOf('RNSBox fast terminal test') >= 0,
                6000, 'snapshot/title');

  // Keyboard byte over the WebSocket (no CGI POST is involved).
  term._data('X');
  await waitFor(() => term.screen.text().indexOf('key: X') >= 0, 3000, 'key X');

  // Modes are delivered in a text frame right after the snapshot.  Wait for
  // the client to have them, then drive its wheel fallback (alt screen, mouse
  // reporting off): it must send an up-arrow and prevent the page scroll.
  await sleep(250);
  const wheel = (handlers.wheel || [])[0];
  if (!wheel) throw new Error('term.js did not register a wheel handler');
  const ev = {
    deltaY: -40,
    prevented: false,
    preventDefault() { this.prevented = true; }
  };
  wheel(ev);
  if (!ev.prevented) throw new Error('wheel fallback did not preventDefault');
  await waitFor(() => term.screen.text().indexOf('arrow: UP') >= 0, 3000,
                'wheel -> UP');

  console.log('client ok');
  process.exit(0);
})().catch((e) => {
  console.error(e && e.message ? e.message : e);
  process.exit(1);
});
