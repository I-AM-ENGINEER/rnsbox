// routes_console.cpp — authenticated one-shot root shell for field debugging.
//
// This is deliberately not a PTY/WebSocket terminal: uhttpd runs CGI, and an
// interactive TTY would need a streaming server. Each POST runs one command
// with /bin/sh -c under a 20 s timeout and shows the captured output.
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

    std::string cmd = util::trim(req.f("cmd"));
    if (cmd.size() > 4096) cmd.resize(4096);

    std::string out;
    if (!cmd.empty()) {
        util::RunResult r = util::run({"/bin/sh", "-c", cmd}, "", 20, 256 * 1024);
        out = r.out;
        if (r.timed_out) out += "\n[command timed out after 20 s]\n";
        else if (r.exit_code > 0) out += "\n[exit status " + std::to_string(r.exit_code) + "]\n";
        else if (r.exit_code < 0) out += "\n[command could not be started]\n";
    }

    std::string h = render::page_file("console");
    h = R(std::move(h), "__BASE__", web::base());
    h = R(std::move(h), "__CMD__", E(cmd));
    std::string body = cmd.empty()
        ? std::string("<p class=\"muted\">No command run yet.</p>")
        : "<pre class=\"console-out\">" + E(out) + "</pre>";
    h = R(std::move(h), "__OUTPUT__", body);
    res.body = render::layout(req, res, "Console", "console", h);
}

}  // namespace routes
