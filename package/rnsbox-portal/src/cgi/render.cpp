// render.cpp — see render.h.
#include "render.h"
#include "web.h"
#include "session.h"
#include "util.h"
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

std::string pages_dir() {
    const char* d = getenv("RNSBOX_PAGES");
    return d && *d ? std::string(d) : std::string("/usr/share/rnsbox-portal/pages");
}

// Percent-encode everything but the unreserved set — for the flash cookie value.
std::string pct(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        } else {
            o += '%'; o += hex[c >> 4]; o += hex[c & 0xF];
        }
    }
    return o;
}

// Single-pass token fill for the chrome templates. Tokens are matched in the
// template text only — never inside an inserted value — so a value that itself
// contains a token (e.g. the valid hostname "__CONTENT__", or a flash message
// echoing an SSID "__FLASH__") is emitted literally instead of being expanded
// again, as chained replace_all() calls would.
typedef std::vector<std::pair<std::string, std::string>> Tokens;

std::string fill(const std::string& t, const Tokens& kv) {
    std::string o;
    size_t n = t.size();
    for (const auto& e : kv) n += e.second.size();
    o.reserve(n);
    size_t i = 0;
    while (i < t.size()) {
        size_t p = t.find("__", i);
        if (p == std::string::npos) { o.append(t, i, std::string::npos); break; }
        o.append(t, i, p - i);
        const std::pair<std::string, std::string>* hit = nullptr;
        for (const auto& e : kv)
            if (t.compare(p, e.first.size(), e.first) == 0) { hit = &e; break; }
        if (hit) { o += hit->second; i = p + hit->first.size(); }
        else     { o += '_'; i = p + 1; }   // step one char so "___X__" still finds "__X__"
    }
    return o;
}

// Read + clear the one-shot flash cookie; returns the rendered <div> (or "").
std::string take_flash(const http::Request& req, http::Response& res) {
    std::string v = req.cookie("rns_flash");
    if (v.empty()) return "";
    // Clear it so a refresh doesn't show it twice.
    res.set_cookie("rns_flash=; Path=/; Max-Age=0; SameSite=Lax; HttpOnly");
    size_t colon = v.find(':');
    std::string cat = colon == std::string::npos ? std::string("success") : v.substr(0, colon);
    std::string msg = colon == std::string::npos ? std::string() : http::url_decode(v.substr(colon + 1));
    if (cat != "success" && cat != "error" && cat != "warn") cat = "success";
    return "<div class=\"flash " + cat + "\">" + render::esc(msg) + "</div>";
}

}  // namespace

namespace render {

std::string esc(const std::string& s) { return http::html_escape(s); }

std::string page_file(const char* name) {
    return util::read_file(pages_dir() + "/" + name + ".html", 256 * 1024);
}

std::string layout(const http::Request& req, http::Response& res,
                   const std::string& title, const char* active,
                   const std::string& content) {
    std::string host = util::trim(util::read_file("/proc/sys/kernel/hostname", 128));
    session::Info s = session::check(req.cookie(session::COOKIE_NAME));

    // Every value except CONTENT (the route's own, already-escaped HTML) is
    // escaped here. BASE is uhttpd's SCRIPT_NAME (not client-controlled, and
    // unchanged by esc() for a normal path) — escaped anyway, it lands in attrs.
    Tokens kv = {
        {"__BASE__",     esc(web::base())},
        {"__TITLE__",    esc(title)},
        {"__HOSTNAME__", esc(host)},
        {"__USER__",     esc(s.user)},
    };
    // Active-nav: exactly one id gets "active", the rest collapse to "".
    static const char* ids[] = {"dashboard", "network", "wifi", "reticulum", "settings", "donate"};
    for (const char* id : ids)
        kv.push_back({std::string("__ACT_") + id + "__",
                      (active && std::string(active) == id) ? "active" : ""});
    kv.push_back({"__FLASH__", take_flash(req, res)});
    kv.push_back({"__CONTENT__", content});
    return fill(page_file("base"), kv);
}

std::string login_layout(const http::Request& req, http::Response& res,
                         const std::string& content) {
    std::string host = util::trim(util::read_file("/proc/sys/kernel/hostname", 128));
    Tokens kv = {
        {"__BASE__",     esc(web::base())},
        {"__HOSTNAME__", esc(host)},
        {"__FLASH__",    take_flash(req, res)},
        {"__CONTENT__",  content},
    };
    return fill(page_file("login_base"), kv);
}

void redirect_flash(http::Response& res, const std::string& location,
                    const char* category, const std::string& msg) {
    std::string cat = category ? category : "success";
    res.set_cookie("rns_flash=" + cat + ":" + pct(msg) +
                   "; Path=/; Max-Age=60; SameSite=Lax; HttpOnly");
    res.redirect(location);
}

}  // namespace render
