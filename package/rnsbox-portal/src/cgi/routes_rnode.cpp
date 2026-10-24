// routes_rnode.cpp — RNode tab: rnodeconf, live counters, display, pairing.
//
// The heavy lifting is /usr/lib/rnsbox/rnode.py: it drives rnodeconf for
// device info/config and speaks the RNode serial protocol itself for the
// counters and the OLED framebuffer. This file only validates the inputs,
// runs the helper with a bounded timeout and returns its JSON.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include <cctype>
#include <string>

using http::Request;
using http::Response;

namespace routes {

static bool valid_port(const std::string& p) {
    static const char* prefixes[] = {"/dev/ttyUSB", "/dev/ttyACM"};
    for (const char* pre : prefixes) {
        std::string pre_s(pre);
        if (p.compare(0, pre_s.size(), pre_s) != 0) continue;
        if (p.size() == pre_s.size()) return false;
        for (size_t i = pre_s.size(); i < p.size(); ++i)
            if (!isdigit((unsigned char)p[i])) return false;
        return true;
    }
    return false;
}

static bool valid_action(const std::string& a) {
    static const char* allowed[] = {"info", "config", "stats", "display",
                                    "bt-on", "bt-off", "bt-pair"};
    for (const char* a0 : allowed)
        if (a == a0) return true;
    return false;
}

void rnode_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string h = render::page_file("rnode");
    h = web::replace_all(std::move(h), "__BASE__", render::esc(web::base()));
    res.body = render::layout(req, res, "RNode", "rnode", h);
}

void rnode_serial(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    util::RunResult rr = util::run(
        {"/usr/bin/python3", "/usr/lib/rnsbox/rnode.py", "serial"}, "", 15);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') {
        res.body = out;
        return;
    }
    res.body = "{\"ok\":false,\"ports\":[],\"error\":\"serial scan failed\"}";
}

void rnode_action(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string port = util::trim(req.a("port"));
    std::string what = util::trim(req.a("do"));
    if (!valid_port(port) || !valid_action(what)) {
        res.body = "{\"ok\":false,\"output\":\"invalid request\"}";
        return;
    }
    util::RunResult rr = util::run(
        {"/usr/bin/python3", "/usr/lib/rnsbox/rnode.py", "action", port, what},
        "", 50);
    std::string out = util::trim(rr.out);
    if (rr.timed_out) {
        res.body = "{\"ok\":false,\"output\":\"the modem did not answer in time\"}";
        return;
    }
    if (!out.empty() && out[0] == '{') {
        res.body = out;
        return;
    }
    res.body = "{\"ok\":false,\"output\":\"" +
               http::json_escape(out.empty() ? "rnode helper failed" : out) + "\"}";
}

}  // namespace routes
