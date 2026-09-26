// http.h — CGI request/response core for the RNSBox portal.
//
// WAN-facing root CGI: every parser here is written defensively — bounded
// reads into std::string (never fixed char[] sized from attacker input),
// explicit length caps, and no use of the untrusted bytes in any shell.
// Compiled with -fno-exceptions, so we never rely on throwing; callers check
// return values / emptiness instead.
#ifndef RNSBOX_HTTP_H
#define RNSBOX_HTTP_H

#include <string>
#include <map>
#include <vector>
#include <cstdlib>

namespace http {

// Hard cap on a request body we will read (matches the Flask portal's 256 KiB
// MAX_CONTENT_LENGTH). A larger CONTENT_LENGTH is refused with 413.
constexpr long MAX_BODY = 256 * 1024;

struct Request {
    std::string method;        // REQUEST_METHOD
    std::string path;          // PATH_INFO (the app route, e.g. "/login")
    std::string query;         // QUERY_STRING (raw)
    std::string body;          // request body (POST), capped at MAX_BODY
    std::string remote_addr;   // REMOTE_ADDR
    bool too_large = false;    // CONTENT_LENGTH exceeded MAX_BODY
    std::map<std::string, std::string> cookies;  // parsed Cookie header
    std::map<std::string, std::string> form;     // parsed application/x-www-form-urlencoded body
    std::map<std::string, std::string> args;     // parsed QUERY_STRING

    // Convenience: form field, else empty string.
    std::string f(const char* k) const { auto it = form.find(k); return it == form.end() ? std::string() : it->second; }
    std::string a(const char* k) const { auto it = args.find(k); return it == args.end() ? std::string() : it->second; }
    std::string cookie(const char* k) const { auto it = cookies.find(k); return it == cookies.end() ? std::string() : it->second; }
};

// Percent-decode + '+'-to-space. Bounded by the input length; malformed %XX is
// passed through literally rather than read past the end.
std::string url_decode(const std::string& s);

// Parse "a=1&b=two%20words" into a map (last value wins). Keys/values decoded.
void parse_urlencoded(const std::string& s, std::map<std::string, std::string>& out);

// Parse a Cookie header ("k=v; k2=v2") into a map. Values are NOT url-decoded
// (cookies we set are already token-safe base64url).
void parse_cookies(const std::string& s, std::map<std::string, std::string>& out);

// Read the CGI environment + (for POST) stdin into a Request. Enforces MAX_BODY.
Request read_request();

// --- response helpers (write to stdout, CGI style) ---

// A response accumulates headers then a body, emitted once via send().
struct Response {
    std::string status = "200 OK";
    std::string content_type = "text/html; charset=utf-8";
    std::vector<std::string> headers;   // extra raw headers ("Set-Cookie: ...")
    std::string body;

    void set_cookie(const std::string& raw) { headers.push_back("Set-Cookie: " + raw); }
    void redirect(const std::string& location) { status = "302 Found"; headers.push_back("Location: " + location); body.clear(); }
    void send() const;
};

// JSON string escaper (for our tiny hand-rolled JSON emitter). Escapes the
// control set + quote/backslash; emits \uXXXX for < 0x20.
std::string json_escape(const std::string& s);

// HTML-escape for the rare case we echo a value into a shell page.
std::string html_escape(const std::string& s);

}  // namespace http
#endif
