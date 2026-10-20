// routes_console.cpp — TTY console on the portal's own HTTP port.
//
// A detached GNU screen session runs /bin/login on a real PTY; the CGI route
// snapshots its screen (hardcopy) and stuffs input lines. No second HTTP
// port and no separate network service: the portal's login is the gate.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include <string>
#include <unistd.h>

namespace routes {

static const char* SCREEN = "/usr/bin/screen";
static const char* CAP = "/tmp/rnsbox-console.txt";

static std::string R(std::string s, const std::string& a, const std::string& b) {
    return web::replace_all(std::move(s), a, b);
}

// The helper prints the pid of the single screen session (starting it when
// needed). Address it as <pid>.rnsbox-console so a stale duplicate can never
// make -S ambiguous.
static std::string session_id() {
    util::RunResult r = util::run({"/usr/sbin/rnsbox-console-session"}, "", 10);
    std::string pid = util::trim(r.out);
    return pid.empty() ? std::string() : pid + ".rnsbox-console";
}

void console_page(const http::Request& req, http::Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string h = render::page_file("console");
    h = R(std::move(h), "__BASE__", web::base());
    res.body = render::layout(req, res, "Console", "console", h);
}

void console_screen(const http::Request& req, http::Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string id = session_id();
    if (id.empty()) { res.body = ""; return; }
    ::unlink(CAP);
    util::run({SCREEN, "-S", id, "-X", "hardcopy", CAP}, "", 5);
    res.content_type = "text/plain; charset=utf-8";
    res.body = util::read_file(CAP, 256 * 1024);
}

void console_input(const http::Request& req, http::Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string line = req.f("line");
    if (line.size() > 4096) line.resize(4096);
    std::string id = session_id();
    // No shell: screen receives the line literally, including control bytes
    // (the Ctrl-C button posts U+0003).
    if (!id.empty())
        util::run({SCREEN, "-S", id, "-X", "stuff", line + "\n"}, "", 5);
    res.content_type = "application/json";
    res.body = "{\"ok\":true}";
}

}  // namespace routes
