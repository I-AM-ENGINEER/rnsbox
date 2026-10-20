// routes_console.cpp — LAN-only full zsh terminal (ttyd + xterm.js).
//
// ttyd runs as a separate daemon on 10.42.0.1:7681 (S87ttyd); this route only
// renders the tab and shows the basic-auth password to a logged-in admin.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include <string>

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) {
    return web::replace_all(std::move(s), a, b);
}
static std::string E(const std::string& s) { return render::esc(s); }

void console_page(const http::Request& req, http::Response& res) {
    if (!web::require_auth(req, res, false)) return;

    // S87ttyd generates the password on first boot; until then the file is
    // empty and the tab says so.
    std::string pass;
    std::string conf = util::read_file("/etc/rnsbox/console.conf", 4096);
    size_t p = conf.find("password=");
    if (p != std::string::npos) {
        size_t e = conf.find('\n', p);
        pass = util::trim(conf.substr(p + 9, e == std::string::npos ? std::string::npos : e - (p + 9)));
    }

    std::string h = render::page_file("console");
    h = R(std::move(h), "__BASE__", web::base());
    h = R(std::move(h), "__LAN_IP__", "10.42.0.1");
    h = R(std::move(h), "__TTYD_PORT__", "7681");
    h = R(std::move(h), "__CONSOLE_PASS__", E(pass.empty() ? "(generated at boot — reload)" : pass));
    res.body = render::layout(req, res, "Console", "console", h);
}

}  // namespace routes
