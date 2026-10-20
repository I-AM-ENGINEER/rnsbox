// routes_console.cpp — LAN-only full zsh terminal (ttyd + xterm.js).
//
// ttyd runs as a separate daemon on 10.42.0.1:7681 (S87ttyd); the portal
// login is the only gate, so the iframe needs no second basic-auth prompt.
#include "routes.h"
#include "render.h"
#include "web.h"
#include <string>

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) {
    return web::replace_all(std::move(s), a, b);
}

void console_page(const http::Request& req, http::Response& res) {
    if (!web::require_auth(req, res, false)) return;

    std::string h = render::page_file("console");
    h = R(std::move(h), "__BASE__", web::base());
    h = R(std::move(h), "__LAN_IP__", "10.42.0.1");
    h = R(std::move(h), "__TTYD_PORT__", "7681");
    res.body = render::layout(req, res, "Console", "console", h);
}

}  // namespace routes
