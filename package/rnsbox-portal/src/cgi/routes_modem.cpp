// routes_modem.cpp — reverse-proxy for the external HaLow (RNode) modem's own
// web UI + JSON API, reachable only from this box over the SLIP link (sl0).
// Faithful port of the old Flask /modem + /modem/api/<path> routes: the modem
// serves one self-contained config page at http://<peer_ip>/ plus /api/* JSON;
// we proxy it behind the portal login so the whole UI is usable through the
// portal and gated by the login, rather than DNAT'd unauthenticated onto the LAN.
//
// The old code requested `Accept-Encoding: identity` (uncompressed) so no gzip
// decode is needed — a tiny HTTP/1.0 client (connect, send, read-to-close) does
// the whole job with no libcurl/zlib. The modem page's own fetch('/api/..')
// calls are rewritten to '<base>/modem/api/..' so they stay under the portal.
// Both routes are served only on the separate modem port (main.cpp MODEM_PORT,
// :8081), so the modem's scripts run in their own origin, not the portal's.
#include "routes.h"
#include "web.h"
#include "store.h"
#include "sysinfo.h"
#include "util.h"
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <zlib.h>

using http::Request;
using http::Response;

namespace routes {

namespace {

constexpr int MODEM_TIMEOUT = 8;                 // seconds (matches old Flask)
constexpr size_t MODEM_MAX_RESP = 4 * 1024 * 1024;   // cap a proxied response

std::string modem_peer() {
    store::SlipConfig sl = store::read_slip();
    std::string p = util::trim(sl.peer_ip);
    return p.empty() ? std::string("192.168.7.2") : p;
}

// The RNode modem serves gzip-encoded assets (its index.html) and ignores our
// Accept-Encoding: identity, so decode gzip in-proxy (like the old Flask code).
bool looks_gzip(const std::string& s) {
    return s.size() >= 2 && (unsigned char)s[0] == 0x1f && (unsigned char)s[1] == 0x8b;
}
bool gunzip(const std::string& in, std::string& out) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;   // 16 => gzip wrapper
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    char buf[16384];
    int ret;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return false; }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (out.size() > MODEM_MAX_RESP) { inflateEnd(&zs); return false; }
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return true;
}

// If `body` is gzip, inflate it in place (best-effort; leaves it alone on error).
void maybe_gunzip(std::string& body) {
    if (!looks_gzip(body)) return;
    std::string dec;
    if (gunzip(body, dec)) body.swap(dec);
}

// ---- request validation: nothing from the client reaches the upstream request
// line or headers unless it passed these. uhttpd URL-decodes PATH_INFO, so a
// %0d%0a in the URL arrives here as a real CR/LF (seen live: it injected an
// extra header into the modem request). ----

// /modem/api/<endpoint>: the modem's API names are plain words (get_stat,
// get_all, …). Allow [A-Za-z0-9_./-] only, no leading '/', no "..", bounded.
bool endpoint_ok(const std::string& e) {
    if (e.empty() || e.size() > 128 || e[0] == '/') return false;
    if (e.find("..") != std::string::npos) return false;
    for (unsigned char c : e)
        if (!(isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-')) return false;
    return true;
}

// QUERY_STRING is passed through still percent-encoded (uhttpd doesn't decode
// it), so only URL-safe printable bytes are legal: no space, CR/LF, control
// chars, quotes or '#'. Bounded.
bool query_ok(const std::string& q) {
    if (q.size() > 2048) return false;
    for (unsigned char c : q) {
        if (isalnum(c)) continue;
        if (!strchr("-._~%&=+,;:@/!$'()*?", c) || c == '\0') return false;
    }
    return true;
}

// A header value we forward (Content-Type) or echo back from the modem
// (status reason, Content-Type): printable ASCII only, else "".
std::string header_safe(const std::string& v, size_t max_len = 128) {
    if (v.size() > max_len) return "";
    for (unsigned char c : v) if (c < 0x20 || c > 0x7e) return "";
    return v;
}

// The modem's read endpoints: get_* and the *_cfg config reads (a plain word).
// Everything else (reboot, default_rst, reset_stat, telemetry_send, ota_*) is an
// action, and the modem UI only ever POSTs those (and the *_cfg saves). Only
// non-GET requests are same-origin checked (main.cpp), and SameSite=Lax still
// sends the session cookie on a cross-site top-level GET. So a GET for an action
// would let a plain link fire it with the admin session (finding #1).
bool modem_read_ep(const std::string& e) {
    for (unsigned char c : e) if (!(isalnum(c) || c == '_')) return false;
    if (e.compare(0, 4, "get_") == 0) return true;
    return e.size() > 4 && e.compare(e.size() - 4, 4, "_cfg") == 0;
}

void json_fail(Response& res, const char* status, const char* msg) {
    res.status = status;
    res.content_type = "application/json";
    res.body = std::string("{\"error\":\"") + msg + "\"}";
}

// Minimal HTTP/1.0 client to <peer>:80. Returns false on any connect/IO failure.
// On success: `status`/`reason` from the response line, `resp_ctype` = the
// Content-Type header, `out` = the response body.
bool modem_fetch(const std::string& peer, const std::string& method,
                 const std::string& path_and_query, const std::string& body,
                 const std::string& req_ctype,
                 int& status, std::string& reason, std::string& resp_ctype, std::string& out) {
    status = 0; reason.clear(); resp_ctype.clear(); out.clear();

    // Last line of defence (callers validate first): the request line and
    // headers below are built by concatenation, so refuse anything that could
    // split them — a non-GET/POST method, or a space/CR/LF/control byte in the
    // target. The forwarded Content-Type is dropped unless printable.
    if (method != "GET" && method != "POST") return false;
    for (unsigned char c : path_and_query) if (c <= 0x20 || c >= 0x7f) return false;
    const std::string ctype = header_safe(req_ctype, 256);   // room for a multipart boundary

    struct in_addr addr;
    if (inet_pton(AF_INET, peer.c_str(), &addr) != 1) return false;   // peer is an IPv4 literal

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct Closer { int f; ~Closer() { if (f >= 0) ::close(f); } } closer{fd};

    // Non-blocking connect with a bounded timeout (sl0 down => connect fails fast,
    // but guard against a black-hole route hanging the CGI).
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr = addr;
    int rc = ::connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (rc != 0) {
        if (errno != EINPROGRESS) return false;
        fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
        struct timeval tv{MODEM_TIMEOUT, 0};
        if (::select(fd + 1, nullptr, &wf, nullptr, &tv) <= 0) return false;
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) return false;
    }
    ::fcntl(fd, F_SETFL, flags);   // back to blocking
    struct timeval tv{MODEM_TIMEOUT, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Build + send the request (HTTP/1.0 + Connection: close => read to EOF).
    std::string req = method + " " + path_and_query + " HTTP/1.0\r\n";
    req += "Host: " + peer + "\r\n";
    req += "Accept-Encoding: identity\r\n";
    req += "Connection: close\r\n";
    if (method == "POST") {
        if (!ctype.empty()) req += "Content-Type: " + ctype + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    if (method == "POST") req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = ::write(fd, req.data() + sent, req.size() - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }

    // Read the whole response.
    std::string raw;
    char buf[8192];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) break;
        raw.append(buf, (size_t)n);
        if (raw.size() > MODEM_MAX_RESP) break;
    }
    if (raw.empty()) return false;

    // Parse the status line + headers.
    size_t hdr_end = raw.find("\r\n\r\n");
    size_t body_off = (hdr_end == std::string::npos) ? raw.size() : hdr_end + 4;
    std::string head = raw.substr(0, (hdr_end == std::string::npos) ? raw.size() : hdr_end);
    out = (body_off <= raw.size()) ? raw.substr(body_off) : std::string();

    // "HTTP/1.x <code> <reason>"
    size_t sp1 = head.find(' ');
    if (sp1 != std::string::npos) {
        status = atoi(head.c_str() + sp1 + 1);
        size_t sp2 = head.find(' ', sp1 + 1);
        size_t eol = head.find('\r', sp1 + 1);
        if (sp2 != std::string::npos && eol != std::string::npos && sp2 < eol)
            reason = head.substr(sp2 + 1, eol - sp2 - 1);
    }
    if (status < 100 || status > 599) status = 200;
    // The reason phrase + Content-Type are echoed into OUR response headers:
    // never let a (buggy or hostile) modem smuggle control bytes through.
    reason = header_safe(reason);
    if (reason.empty()) reason = "OK";

    // Content-Type header (case-insensitive scan).
    size_t i = 0;
    while (i < head.size()) {
        size_t nl = head.find('\n', i);
        std::string line = head.substr(i, (nl == std::string::npos ? head.size() : nl) - i);
        i = (nl == std::string::npos) ? head.size() : nl + 1;
        std::string low = line;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (low.rfind("content-type:", 0) == 0) {
            resp_ctype = header_safe(util::trim(line.substr(13)));
            break;
        }
    }
    return true;
}

}  // namespace

// GET /modem  (and /modem/) — proxy the modem's root page, rewriting /api/ links.
void modem_ui(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string peer = modem_peer();
    int status; std::string reason, ct, body;
    // Fast-fail if the SLIP link is down (no route to the modem => an 8 s connect
    // timeout otherwise, since the SYN black-holes out the default route).
    if (!sysinfo::slip_status().up) {
        res.status = "502 Bad Gateway";
        res.content_type = "text/plain; charset=utf-8";
        res.body = "The SLIP link (sl0) is not up, so the HaLow modem at http://" + peer +
                   " can't be reached.\n\nEnable the SLIP link on the Reticulum tab and wire the modem.\n";
        return;
    }
    if (!modem_fetch(peer, "GET", "/", "", "", status, reason, ct, body)) {
        res.status = "502 Bad Gateway";
        res.content_type = "text/plain; charset=utf-8";
        res.body = "HaLow modem not reachable at http://" + peer + ".\n\n"
                   "Enable the SLIP link, wire the modem, and confirm sl0 is up, then retry.\n";
        return;
    }
    maybe_gunzip(body);   // modem serves gzip'd index.html; inflate so we can rewrite it
    std::string low = ct; for (char& c : low) c = (char)tolower((unsigned char)c);
    if (low.find("html") != std::string::npos) {
        // Keep the modem's own fetch('/api/..') calls under the portal.
        body = web::replace_all(std::move(body), "/api/", web::base() + "/modem/api/");
        res.content_type = "text/html; charset=utf-8";
    } else {
        // Never echo the modem's type: e.g. image/svg+xml is script-capable.
        // The UI is one HTML page; anything else is just offered as data.
        res.content_type = "application/octet-stream";
    }
    res.status = std::to_string(status) + " " + reason;
    res.body = body;
}

// GET/POST /modem/api/<endpoint> — proxy the modem's JSON API.
void modem_api(const Request& req, Response& res) {
    // JSON-mode auth: the modem page's XHRs get a 401 JSON they can handle,
    // not a 302 to the HTML login page (which they'd fail to parse).
    if (!web::require_auth(req, res, true)) return;
    // Only the two methods the modem UI uses (GET for reads only, see below);
    // the method string goes verbatim into the upstream request line.
    if (req.method != "GET" && req.method != "POST") {
        json_fail(res, "405 Method Not Allowed", "method not allowed");
        res.headers.push_back("Allow: GET, POST");
        return;
    }
    const char* pfx = "/modem/api/";
    std::string endpoint = (req.path.size() > strlen(pfx)) ? req.path.substr(strlen(pfx)) : std::string();
    if (!endpoint_ok(endpoint) || !query_ok(req.query)) {
        json_fail(res, "400 Bad Request", "invalid modem API path");
        return;
    }
    // GET only for the read endpoints, and without a query (the modem UI never
    // sends one), so no firmware can be made to act on a cross-site link.
    if (req.method == "GET" && (!modem_read_ep(endpoint) || !req.query.empty())) {
        json_fail(res, "405 Method Not Allowed", "method not allowed");
        res.headers.push_back("Allow: POST");
        return;
    }
    std::string peer = modem_peer();
    if (!sysinfo::slip_status().up) {
        res.status = "502 Bad Gateway";
        res.content_type = "application/json";
        res.body = "{\"error\":\"SLIP link (sl0) is not up\",\"rc\":-502}";
        return;
    }
    std::string pq = "/api/" + endpoint;
    if (!req.query.empty()) pq += "?" + req.query;
    std::string body = (req.method == "POST") ? req.body : std::string();
    const char* env_ct = getenv("CONTENT_TYPE");
    std::string req_ct = env_ct ? env_ct : "";
    int status; std::string reason, ct, out;
    if (!modem_fetch(peer, req.method, pq, body, req_ct, status, reason, ct, out)) {
        res.status = "502 Bad Gateway";
        res.content_type = "application/json";
        res.body = "{\"error\":\"modem unreachable over SLIP\",\"rc\":-502}";
        return;
    }
    maybe_gunzip(out);   // decode any gzip'd API response too
    res.status = std::to_string(status) + " " + reason;
    // API answers are data, never a page: an HTML-typed reply would render in
    // the portal's origin, so downgrade it to text/plain (fetch().json() in
    // the modem UI doesn't care about the type).
    std::string low = ct; for (char& c : low) c = (char)tolower((unsigned char)c);
    if (low.find("html") != std::string::npos || low.find("xml") != std::string::npos)
        ct = "text/plain; charset=utf-8";
    res.content_type = ct.empty() ? "application/json" : ct;
    res.body = out;
}

}  // namespace routes
