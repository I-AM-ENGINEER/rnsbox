// http.cpp — see http.h. Defensive CGI parsing for a WAN-facing root service.
#include "http.h"
#include <cstdio>
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

void Response::send() const {
    std::string out;
    out.reserve(body.size() + 256);
    out += "Status: " + status + "\r\n";
    out += "Content-Type: " + content_type + "\r\n";
    for (const auto& h : headers) out += h + "\r\n";
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
