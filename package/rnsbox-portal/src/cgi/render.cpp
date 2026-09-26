// render.cpp — see render.h.
#include "render.h"
#include "web.h"
#include "session.h"
#include "util.h"
#include <cstdlib>

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

std::string all(std::string s, const std::string& from, const std::string& to) {
    return web::replace_all(std::move(s), from, to);
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

    std::string h = page_file("base");
    h = all(std::move(h), "__BASE__", web::base());
    h = all(std::move(h), "__TITLE__", esc(title));
    h = all(std::move(h), "__HOSTNAME__", esc(host));
    h = all(std::move(h), "__USER__", esc(s.user));
    // Active-nav: exactly one id gets "active", the rest collapse to "".
    static const char* ids[] = {"dashboard", "network", "wifi", "reticulum", "settings", "donate"};
    for (const char* id : ids) {
        std::string tok = std::string("__ACT_") + id + "__";
        h = all(std::move(h), tok, (active && std::string(active) == id) ? "active" : "");
    }
    h = all(std::move(h), "__FLASH__", take_flash(req, res));
    h = all(std::move(h), "__CONTENT__", content);
    return h;
}

std::string login_layout(const http::Request& req, http::Response& res,
                         const std::string& content) {
    std::string host = util::trim(util::read_file("/proc/sys/kernel/hostname", 128));
    std::string h = page_file("login_base");
    h = all(std::move(h), "__BASE__", web::base());
    h = all(std::move(h), "__HOSTNAME__", esc(host));
    h = all(std::move(h), "__FLASH__", take_flash(req, res));
    h = all(std::move(h), "__CONTENT__", content);
    return h;
}

void redirect_flash(http::Response& res, const std::string& location,
                    const char* category, const std::string& msg) {
    std::string cat = category ? category : "success";
    res.set_cookie("rns_flash=" + cat + ":" + pct(msg) +
                   "; Path=/; Max-Age=60; SameSite=Lax; HttpOnly");
    res.redirect(location);
}

}  // namespace render
