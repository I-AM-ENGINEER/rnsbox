// render.cpp — see render.h.
#include "render.h"
#include "web.h"
#include "session.h"
#include "sysinfo.h"
#include "util.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

std::string pages_dir() {
    const char* d = getenv("RNSBOX_PAGES");
    return d && *d ? std::string(d) : std::string("/usr/share/rnsbox-portal/pages");
}

// NomadNet tab only when the client is installed AND enabled in
// /etc/rnsbox/nomadnet.conf. On the core image (no python-nomadnet) it would
// be a dead link, so it is left out of the nav entirely.
bool nomadnet_on() {
    if (::access("/usr/bin/nomadnet", X_OK) != 0) return false;
    std::string c = util::read_file("/etc/rnsbox/nomadnet.conf", 4096);
    size_t p = 0;
    while (p < c.size()) {
        size_t e = c.find('\n', p);
        if (e == std::string::npos) e = c.size();
        std::string line = c.substr(p, e - p);
        size_t s = line.find_first_not_of(" \t");
        if (s != std::string::npos) line = line.substr(s);
        if (line.compare(0, 8, "enabled=") == 0) {
            std::string v = util::trim(line.substr(8));
            return v == "yes" || v == "1" || v == "true" || v == "on";
        }
        p = e + 1;
    }
    return false;
}

// Map tab only when rnsd is configured to collect interface-discovery
// announces; without that store the page would always be empty.
bool map_on() {
    std::string c = util::read_file("/etc/reticulum/config", 64 * 1024);
    size_t p = 0;
    while (p < c.size()) {
        size_t e = c.find('\n', p);
        if (e == std::string::npos) e = c.size();
        std::string line = util::trim(c.substr(p, e - p));
        if (line.compare(0, 21, "discover_interfaces =") == 0) {
            std::string v = util::trim(line.substr(21));
            return v == "yes" || v == "Yes" || v == "1" || v == "true";
        }
        p = e + 1;
    }
    return false;
}

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

// Cache-buster for /style.css and term.js: derived from this binary's build
// stamp, so every rebuild gives the URL a new query and browsers pick up
// new assets without a hard refresh.
const std::string& asset_version() {
    static const std::string v = [] {
        std::string s = __DATE__ " " __TIME__, o;
        for (char c : s)
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                o += c;
        return o;
    }();
    return v;
}

std::string layout(const http::Request& req, http::Response& res,
                   const std::string& title, const char* active,
                   const std::string& content) {
    // Pages are rendered server-side and carry the asset version; never let a
    // browser reuse an old HTML page (it would pin an old term.js/style.css).
    res.headers.push_back("Cache-Control: no-store");
    std::string host = sysinfo::hostname();
    session::Info s = session::check(req.cookie(session::COOKIE_NAME));

    // Every value except CONTENT (the route's own, already-escaped HTML) is
    // escaped here. BASE is uhttpd's SCRIPT_NAME (not client-controlled, and
    // unchanged by esc() for a normal path) — escaped anyway, it lands in attrs.
    Tokens kv = {
        {"__BASE__",     esc(web::base())},
        {"__TITLE__",    esc(title)},
        {"__HOSTNAME__", esc(host)},
        {"__USER__",     esc(s.user)},
        {"__CSSV__",     asset_version()},
    };
    // Active-nav: exactly one id gets "active", the rest collapse to "".
    static const char* ids[] = {"dashboard", "network", "wifi", "reticulum", "rnode", "map", "console", "nomadnet", "manual", "updates", "echobot", "settings", "donate", "access"};
    for (const char* id : ids)
        kv.push_back({std::string("__ACT_") + id + "__",
                      (active && std::string(active) == id) ? "active" : ""});
    std::string nav_nomadnet;
    if (nomadnet_on()) {
        std::string cls = (active && std::string(active) == "nomadnet") ? "active" : "";
        nav_nomadnet = "<a href=\"" + esc(web::base()) + "/nomadnet\" class=\"" +
                       cls + "\">NomadNet</a>";
    }
    kv.push_back({"__NAV_NOMADNET__", nav_nomadnet});
    kv.push_back({"__NAV_MANUAL__",
                  "<a href=\"" + esc(web::base()) + "/manual\" class=\"" +
                  std::string((active && std::string(active) == "manual") ? "active" : "") +
                  "\">Manual</a>"});
    // WiFi and the browser console are router features: on Windows there is
    // no radio and no PTY broker, so their tabs are left out of the nav (the
    // routes answer 503 if something still links to them).
    std::string nav_wifi, nav_console, nav_rnode, nav_map;
#ifndef _WIN32
    nav_wifi = "<a href=\"" + esc(web::base()) + "/wifi\" class=\"" +
               std::string((active && std::string(active) == "wifi") ? "active" : "") + "\">WiFi</a>";
    nav_console = "<a href=\"" + esc(web::base()) + "/console\" class=\"" +
                  std::string((active && std::string(active) == "console") ? "active" : "") + "\">Console</a>";
    // RNode and Map need the Linux helpers (rnodeconf/serial, rnsd storage).
    nav_rnode = "<a href=\"" + esc(web::base()) + "/rnode\" class=\"" +
                std::string((active && std::string(active) == "rnode") ? "active" : "") + "\">RNode</a>";
    if (map_on())
        nav_map = "<a href=\"" + esc(web::base()) + "/map\" class=\"" +
                  std::string((active && std::string(active) == "map") ? "active" : "") + "\">Map</a>";
#endif
    kv.push_back({"__NAV_WIFI__", nav_wifi});
    kv.push_back({"__NAV_CONSOLE__", nav_console});
    kv.push_back({"__NAV_RNODE__", nav_rnode});
    kv.push_back({"__NAV_MAP__", nav_map});
    kv.push_back({"__FLASH__", take_flash(req, res)});
    kv.push_back({"__CONTENT__", content});
    return fill(page_file("base"), kv);
}

std::string login_layout(const http::Request& req, http::Response& res,
                         const std::string& content) {
    res.headers.push_back("Cache-Control: no-store");
    std::string host = sysinfo::hostname();
    Tokens kv = {
        {"__BASE__",     esc(web::base())},
        {"__HOSTNAME__", esc(host)},
        {"__FLASH__",    take_flash(req, res)},
        {"__CSSV__",     asset_version()},
        {"__CONTENT__",  content},
    };
    return fill(page_file("login_base"), kv);
}

std::string full_page(const std::string& title, const std::string& content) {
    std::string host = sysinfo::hostname();
    std::string o;
    o.reserve(content.size() + 1024);
    o += "<!doctype html>\n<html lang=\"en\">\n<head>\n";
    o += "  <meta charset=\"utf-8\">\n";
    o += "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    o += "  <title>" + esc(title) + " — " + esc(host) + "</title>\n";
    o += "  <link rel=\"stylesheet\" href=\"/style.css?v=" + asset_version() + "\">\n";
    o += "  <script>\n"
         "  // Same theme bootstrap as the chrome's base.html: set before first\n"
         "  // paint so the full-screen terminal never flashes the wrong palette.\n"
         "  (function(){\n"
         "    var t = 'dark';\n"
         "    try { t = localStorage.getItem('rnsbox-theme') || 'dark'; } catch(e){}\n"
         "    if (t !== 'light' && t !== 'dark') t = 'dark';\n"
         "    document.documentElement.setAttribute('data-theme', t);\n"
         "  })();\n"
         "  </script>\n";
    o += "</head>\n<body class=\"term-full\">\n";
    o += content;
    o += "\n</body>\n</html>\n";
    return o;
}

void redirect_flash(http::Response& res, const std::string& location,
                    const char* category, const std::string& msg) {
    std::string cat = category ? category : "success";
    res.set_cookie("rns_flash=" + cat + ":" + pct(msg) +
                   "; Path=/; Max-Age=60; SameSite=Lax; HttpOnly");
    res.redirect(location);
}

}  // namespace render
