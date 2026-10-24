// routes_console.cpp — real PTY terminals on the portal's own HTTP port.
//
// The Console and NomadNet tabs are xterm.js in the browser. Their output is a
// long-lived chunked HTTP response (uhttpd relays CGI stdout as it arrives);
// keystrokes and window resizes are small POSTs carrying raw bytes. Both
// endpoints are authenticated by the portal session and talk to the local PTY
// broker over a root-only Unix socket — no second port, no separate network
// service. The broker keeps one session per tab (the admin shell, NomadNet's
// TUI), so each tab survives reloads and opens in its own browser tab without
// the portal chrome.
#include "routes.h"
#include "console.h"
#include "render.h"
#include "web.h"
#include "term/token.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace routes {

static int int_arg(const http::Request& req, const char* n, int lo, int hi, int def) {
    const std::string& s = req.a(n);
    if (s.empty()) return def;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return def;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return static_cast<int>(v);
}

static bool write_all_stdout(const char* p, size_t n) {
    while (n) {
        ssize_t w = ::write(STDOUT_FILENO, p, n);
        if (w > 0) { p += w; n -= static_cast<size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// One page body serves every terminal tab and both chrome modes. `api` is the
// route prefix the page's AJAX talks to (/console or /nomadnet); the broker
// session is selected by that prefix, not by the browser.
static void term_page(const http::Request& req, http::Response& res,
                      const char* title, const char* active,
                      const char* api, bool full,
                      bool autostart = true,
                      const std::string& note = std::string()) {
    if (!web::require_auth(req, res, false)) return;
    const std::string base = web::base();
    std::string h = render::page_file("terminal");
    h = web::replace_all(h, "__TERM_TITLE__", render::esc(title));
    h = web::replace_all(h, "__TERM_JSV__", render::asset_version());
    h = web::replace_all(h, "__TERM_BASE__", render::esc(base));
    h = web::replace_all(h, "__TERM_API__", render::esc(base + api));
    h = web::replace_all(h, "__TERM_AUTOSTART__", autostart ? "1" : "0");
    h = web::replace_all(h, "__TERM_NOTE__", note);
    h = web::replace_all(h, "__TERM_HEADING__",
                         full ? std::string()
                              : "<h2>" + render::esc(title) + "</h2>");
    if (full) {
        h = web::replace_all(h, "__TERM_FULL_BTN__", "");
        res.body = render::full_page(title, h);
        return;
    }
    std::string full_url = base + api + "/full";
    h = web::replace_all(h, "__TERM_FULL_BTN__",
        "<a class=\"btn\" href=\"" + render::esc(full_url) +
        "\" target=\"_blank\" rel=\"noopener\">Open in new tab</a>");
    res.body = render::layout(req, res, title, active, h);
}

// GET /<api>/stream — raw VT bytes, chunked, until the browser goes away or
// the session ends. Every attach replays the session's current ring so a
// reload/reconnect reconstructs the visible screen (the page resets xterm
// before reading).
static void term_stream(const http::Request& req, http::Response& res,
                        const std::string& session) {
    if (!web::require_auth(req, res, false)) return;

    int cols = int_arg(req, "cols", 2, 400, 80);
    int rows = int_arg(req, "rows", 2, 200, 24);
    int fd = console::connect_session();
    if (fd < 0) {
        res.status = "503 Service Unavailable";
        res.content_type = "text/plain; charset=utf-8";
        res.body = "Console broker is not running (S11console).\n";
        return;
    }
    if (!console::attach(fd, session, cols, rows)) {
        close(fd);
        res.status = "503 Service Unavailable";
        res.content_type = "text/plain; charset=utf-8";
        res.body = "Console broker refused the session.\n";
        return;
    }

    // Headers first (res.send() flushes), then raw terminal bytes. uhttpd
    // sees no Content-Length and uses Transfer-Encoding: chunked.
    res.streamed = true;
    res.content_type = "application/octet-stream";
    res.headers.push_back("Cache-Control: no-store");
    res.send();

    // Flush the response through uhttpd even when the session has nothing to
    // replay: uhttpd may hold the CGI headers until the first body byte. A NUL
    // is ignored by xterm and just tells the browser the stream is live.
    (void)write_all_stdout("\0", 1);

    char buf[8192];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            if (!write_all_stdout(buf, static_cast<size_t>(n))) break;
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;   // socket timeout/error: let the browser reconnect
    }
    close(fd);
}

// POST /<api>/input — raw bytes from xterm's onData/onBinary. The body is not
// form-encoded: control sequences and UTF-8 must survive untouched.
static void term_input(const http::Request& req, http::Response& res,
                       const std::string& session) {
    if (!web::require_auth(req, res, false)) return;
    if (req.body.size() > 200 * 1024) {
        res.status = "413 Payload Too Large";
        res.content_type = "application/json";
        res.body = "{\"error\":\"input too large\"}";
        return;
    }
    int fd = console::connect_session();
    if (fd < 0) {
        res.status = "503 Service Unavailable";
        res.content_type = "application/json";
        res.body = "{\"error\":\"console broker unavailable\"}";
        return;
    }
    bool ok = console::send_input(fd, session, req.body);
    close(fd);
    if (!ok) {
        res.status = "502 Bad Gateway";
        res.content_type = "application/json";
        res.body = "{\"error\":\"input rejected\"}";
        return;
    }
    res.status = "204 No Content";
    res.content_type = "text/plain";
}

// POST /<api>/resize?cols=N&rows=N — propagate fit() changes to the PTY.
static void term_resize(const http::Request& req, http::Response& res,
                        const std::string& session) {
    if (!web::require_auth(req, res, false)) return;
    int cols = int_arg(req, "cols", 2, 400, 80);
    int rows = int_arg(req, "rows", 2, 200, 24);
    int fd = console::connect_session();
    if (fd < 0) {
        res.status = "503 Service Unavailable";
        res.content_type = "application/json";
        res.body = "{\"error\":\"console broker unavailable\"}";
        return;
    }
    bool ok = console::send_resize(fd, session, cols, rows);
    close(fd);
    if (!ok) {
        res.status = "502 Bad Gateway";
        res.content_type = "application/json";
        res.body = "{\"error\":\"resize rejected\"}";
        return;
    }
    res.status = "204 No Content";
    res.content_type = "text/plain";
}

// ---- Console tab: the admin login shell ----
void console_page(const http::Request& req, http::Response& res) {
    term_page(req, res, "Console", "console", "/console", false);
}
void console_full_page(const http::Request& req, http::Response& res) {
    term_page(req, res, "Console", "console", "/console", true);
}
void console_stream(const http::Request& req, http::Response& res) {
    term_stream(req, res, console::SESSION_SHELL);
}
void console_input(const http::Request& req, http::Response& res) {
    term_input(req, res, console::SESSION_SHELL);
}
void console_resize(const http::Request& req, http::Response& res) {
    term_resize(req, res, console::SESSION_SHELL);
}

// ---- NomadNet tab: the NomadNet TUI client ----
// Deliberately NOT started when the page opens: urwid + LXMF on the single
// C906 is a heavy client, so it starts from the Connect button only.
static const char* NOMADNET_NOTE =
    "<p class=\"muted\" style=\"margin:0 0 10px\">"
    "NomadNet substantially loads the CPU. On single-core SoCs it runs "
    "slowly, and launching it on a 256 MB RAM system is not recommended.</p>";

void nomadnet_page(const http::Request& req, http::Response& res) {
    term_page(req, res, "NomadNet", "nomadnet", "/nomadnet", false, false,
              NOMADNET_NOTE);
}
void nomadnet_full_page(const http::Request& req, http::Response& res) {
    term_page(req, res, "NomadNet", "nomadnet", "/nomadnet", true, false,
              NOMADNET_NOTE);
}
void nomadnet_stream(const http::Request& req, http::Response& res) {
    term_stream(req, res, console::SESSION_NOMADNET);
}
void nomadnet_input(const http::Request& req, http::Response& res) {
    term_input(req, res, console::SESSION_NOMADNET);
}
void nomadnet_resize(const http::Request& req, http::Response& res) {
    term_resize(req, res, console::SESSION_NOMADNET);
}

// GET /<api>/ws-token — mint a short-lived token for the broker's WebSocket.
// The token is bound to the session name; the broker never sees the portal
// cookie.  No token file / no port means the fast path is unavailable and the
// page falls back to the legacy HTTP stream.
static void term_ws_token(const http::Request& req, http::Response& res,
                          const std::string& session) {
    if (!web::require_auth(req, res, true)) return;

    int port = 0;
    FILE* f = fopen(console::port_path().c_str(), "r");
    if (f) {
        if (fscanf(f, "%d", &port) != 1) port = 0;
        fclose(f);
    }
    std::string token = (port > 0)
        ? term::make_token(console::secret_path(), session, 120)
        : std::string();
    if (port <= 0 || token.empty()) {
        res.status = "503 Service Unavailable";
        res.content_type = "application/json";
        res.body = "{\"error\":\"fast terminal unavailable\"}";
        return;
    }
    res.content_type = "application/json";
    res.body = "{\"port\":" + std::to_string(port) +
               ",\"session\":\"" + session + "\",\"token\":\"" + token + "\"}";
}

void console_ws_token(const http::Request& req, http::Response& res) {
    term_ws_token(req, res, console::SESSION_SHELL);
}

void nomadnet_ws_token(const http::Request& req, http::Response& res) {
    term_ws_token(req, res, console::SESSION_NOMADNET);
}

}  // namespace routes
