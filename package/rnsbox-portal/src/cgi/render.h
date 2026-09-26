// render.h — server-side page rendering: base chrome, flash messages, PRG.
//
// The portal renders full HTML server-side (like the old Flask/Jinja app),
// NOT a JSON-dumping shell. Page bodies live as .html files under PAGES_DIR
// with __PLACEHOLDER__ tokens; a route handler builds the dynamic fragments
// (table rows, selected options, values) and wraps the result in the shared
// chrome via layout(). POST handlers use redirect_flash() (POST-redirect-GET)
// so a refresh never re-submits, exactly as the Flask app did.
#ifndef RNSBOX_RENDER_H
#define RNSBOX_RENDER_H

#include "http.h"
#include <string>

namespace render {

// HTML-escape (alias of http::html_escape) — the workhorse for injecting data.
std::string esc(const std::string& s);

// Read a page template file: <PAGES_DIR>/<name>.html (env RNSBOX_PAGES overrides
// the default /usr/share/rnsbox-portal/pages). Empty string if missing.
std::string page_file(const char* name);

// Wrap `content` in the base chrome (topbar + sidebar + footer). `active` is the
// sidebar id to highlight: "dashboard" | "network" | "wifi" | "reticulum" |
// "settings" | "donate". Consumes + clears the one-shot flash cookie (rendering
// it above the content) and sets the clearing Set-Cookie on `res`. Every chrome
// value except `content` (title, hostname, session user, flash text) is
// HTML-escaped here, and tokens are filled in one pass — a value containing
// "__SOMETHING__" is never expanded again.
std::string layout(const http::Request& req, http::Response& res,
                   const std::string& title, const char* active,
                   const std::string& content);

// Bare login-screen chrome (no sidebar). Also renders + clears the flash.
std::string login_layout(const http::Request& req, http::Response& res,
                          const std::string& content);

// POST-redirect-GET with a one-shot flash. `category` is "success"|"error"|
// "warn"; `msg` is shown once on the next page. Sets the flash cookie + 302.
void redirect_flash(http::Response& res, const std::string& location,
                    const char* category, const std::string& msg);

}  // namespace render
#endif
