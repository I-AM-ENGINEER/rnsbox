// web.h — shared helpers for route handlers (auth gate, base URL, apply).
#ifndef RNSBOX_WEB_H
#define RNSBOX_WEB_H

#include "http.h"
#include <string>

namespace web {

// Base app URL = SCRIPT_NAME (where uhttpd mapped the CGI), default /cgi-bin/portal.
std::string base();

// Gate a route on a valid session. On failure, fills res (302 to login for a
// page, 401 JSON for an API route) and returns false.
bool require_auth(const http::Request& req, http::Response& res, bool json);

// Replace every occurrence of `from` with `to`.
std::string replace_all(std::string s, const std::string& from, const std::string& to);

// Run `/etc/init.d/<name> <arg>` (no shell). Returns true on exit 0.
bool run_init(const char* name, const char* arg = "restart");

// Emit a small JSON {"ok":true} / {"ok":false,"error":"..."} result.
void json_ok(http::Response& res);
void json_err(http::Response& res, const std::string& msg, const char* status = "400 Bad Request");

}  // namespace web
#endif
