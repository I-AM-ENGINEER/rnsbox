// http.cpp — see http.h. Defensive CGI parsing for a WAN-facing root service.
#include "http.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace http {

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size()) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);  // malformed %XX -> literal
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            o += (char)c;
        } else {
            o += '%'; o += hex[c >> 4]; o += hex[c & 0xF];
        }
    }
    return o;
}

void parse_urlencoded(const std::string& s, std::map<std::string, std::string>& out) {
    size_t i = 0;
    while (i < s.size()) {
        size_t amp = s.find('&', i);
        if (amp == std::string::npos) amp = s.size();
        std::string pair = s.substr(i, amp - i);
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            std::string k = url_decode(pair);
            if (!k.empty()) out[k] = "";
        } else {
            std::string k = url_decode(pair.substr(0, eq));
            std::string v = url_decode(pair.substr(eq + 1));
            if (!k.empty()) out[k] = v;
        }
        i = amp + 1;
    }
}

void parse_cookies(const std::string& s, std::map<std::string, std::string>& out) {
    size_t i = 0;
    while (i < s.size()) {
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) semi = s.size();
        std::string pair = s.substr(i, semi - i);
        // trim leading spaces
        size_t start = pair.find_first_not_of(' ');
        if (start != std::string::npos) {
            pair = pair.substr(start);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string k = pair.substr(0, eq);
                std::string v = pair.substr(eq + 1);
                if (!k.empty()) out[k] = v;
            }
        }
        i = semi + 1;
    }
}

static std::string env_str(const char* name) {
    const char* v = getenv(name);
    return v ? std::string(v) : std::string();
}

Request read_request() {
    Request r;
    r.method = env_str("REQUEST_METHOD");
    r.path = env_str("PATH_INFO");
    if (r.path.empty()) r.path = "/";
    r.query = env_str("QUERY_STRING");
    r.remote_addr = env_str("REMOTE_ADDR");
    r.host = env_str("HTTP_HOST");
    r.origin = env_str("HTTP_ORIGIN");
    r.referer = env_str("HTTP_REFERER");

    parse_urlencoded(r.query, r.args);
    parse_cookies(env_str("HTTP_COOKIE"), r.cookies);

    // POST body: bounded read. Trust CONTENT_LENGTH only as a hint; cap hard.
    if (r.method == "POST" || r.method == "PUT") {
        std::string cl = env_str("CONTENT_LENGTH");
        long want = -1;
        if (!cl.empty()) {
            char* end = nullptr;
            long v = strtol(cl.c_str(), &end, 10);
            if (end && *end == '\0' && v >= 0) want = v;
        }
        if (want > MAX_BODY) { r.too_large = true; return r; }
        // Read up to MAX_BODY regardless (defends against a lying CONTENT_LENGTH).
        long limit = (want >= 0 && want < MAX_BODY) ? want : MAX_BODY;
        r.body.reserve(static_cast<size_t>(limit < 4096 ? limit : 4096));
        char buf[4096];
        long total = 0;
        while (total < limit) {
            long chunk = limit - total;
            if (chunk > (long)sizeof(buf)) chunk = sizeof(buf);
            ssize_t n = ::read(STDIN_FILENO, buf, static_cast<size_t>(chunk));
            if (n <= 0) break;
            r.body.append(buf, static_cast<size_t>(n));
            total += n;
            if (total > MAX_BODY) { r.too_large = true; break; }
        }
        std::string ct = env_str("CONTENT_TYPE");
        if (ct.find("application/x-www-form-urlencoded") != std::string::npos)
            parse_urlencoded(r.body, r.form);
    }
    return r;
}

// ---- same-origin check ----
static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Drop a trailing default port (":80" for http, ":443" for https) so
// "10.42.0.1:80" and "10.42.0.1" compare equal.
static std::string strip_default_port(const std::string& auth, const std::string& scheme) {
    const char* dp = scheme == "https" ? ":443" : ":80";
    size_t n = strlen(dp);
    if (auth.size() > n && auth.compare(auth.size() - n, n, dp) == 0)
        return auth.substr(0, auth.size() - n);
    return auth;
}

// True if `url` ("scheme://authority[/path...]", an Origin or a Referer) names
// this box: the request's own scheme (http unless uhttpd set HTTPS), authority == Host.
static bool url_matches_host(const std::string& url, const std::string& host) {
    if (host.empty()) return false;
    std::string u = lower(url);
    // Only the scheme this request arrived on: uhttpd exports HTTPS=on for a TLS
    // client and leaves it unset otherwise (this build has no TLS listener), so
    // today only http:// matches. An https:// page at the same host is a
    // different origin (some other server, e.g. a WAN 443 port forward).
    const std::string scheme = getenv("HTTPS") ? "https" : "http";
    const std::string prefix = scheme + "://";
    if (u.compare(0, prefix.size(), prefix) != 0) return false;  // other scheme, "null", garbage
    size_t a = scheme.size() + 3;
    size_t e = u.find_first_of("/?#", a);
    std::string auth = u.substr(a, e == std::string::npos ? std::string::npos : e - a);
    if (auth.empty()) return false;
    return strip_default_port(auth, scheme) == strip_default_port(lower(host), scheme);
}

bool same_origin(const Request& r) {
    if (!r.origin.empty()) return url_matches_host(r.origin, r.host);
    if (!r.referer.empty()) return url_matches_host(r.referer, r.host);
    return true;   // no Origin, no Referer: non-browser client (see http.h)
}

// Header-line hygiene: CR/LF would split a value into a new header (uhttpd's
// relay splits CGI output on '\n' and re-emits each line verbatim), NUL would
// truncate it. Normal values never contain any of them, so this is a no-op for
// every legitimate response.
static std::string header_safe(const std::string& s) {
    if (s.find_first_of(std::string("\r\n\0", 3)) == std::string::npos) return s;
    std::string o;
    o.reserve(s.size());
    for (char c : s) if (c != '\r' && c != '\n' && c != '\0') o.push_back(c);
    return o;
}

void Response::send() const {
    std::string out;
    out.reserve(body.size() + 256);
    out += "Status: " + header_safe(status) + "\r\n";
    out += "Content-Type: " + header_safe(content_type) + "\r\n";
    for (const auto& h : headers) out += header_safe(h) + "\r\n";
    // Conservative security headers (WAN-facing).
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "X-Frame-Options: DENY\r\n";
    out += "\r\n";
    out += body;
    ::fwrite(out.data(), 1, out.size(), stdout);
    ::fflush(stdout);
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o.push_back(static_cast<char>(c));
                }
        }
    }
    return o;
}

std::string html_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

}  // namespace http
