// web.cpp — see web.h.
#include "web.h"
#include "session.h"
#include "util.h"
#include <cstdlib>

namespace web {

std::string base() {
    const char* s = getenv("SCRIPT_NAME");
    return s && *s ? std::string(s) : std::string("/cgi-bin/portal");
}

bool require_auth(const http::Request& req, http::Response& res, bool json) {
    session::Info s = session::check(req.cookie(session::COOKIE_NAME));
    if (s.valid) return true;
    if (json) {
        res.status = "401 Unauthorized";
        res.content_type = "application/json";
        res.body = "{\"error\":\"auth required\"}";
    } else {
        res.redirect(base() + "/login");
    }
    return false;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    return s;
}

bool run_init(const char* name, const char* arg) {
    std::string path = std::string("/etc/init.d/") + name;
    return util::run({path, arg}, "", 25).exit_code == 0;
}

void json_ok(http::Response& res) {
    res.content_type = "application/json";
    res.body = "{\"ok\":true}";
}

void json_err(http::Response& res, const std::string& msg, const char* status) {
    res.status = status;
    res.content_type = "application/json";
    res.body = std::string("{\"ok\":false,\"error\":\"") + http::json_escape(msg) + "\"}";
}

}  // namespace web
