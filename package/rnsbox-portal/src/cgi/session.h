// session.h — dead-simple auth + session for a tiny-device admin UI.
//
//  * password: verify against the existing werkzeug pbkdf2 hash in auth.json,
//    using the box's OpenSSL (no bundled crypto).
//  * session : an opaque random token written to a /run file and compared;
//    logout (by the token's holder) deletes it, reboot clears it (tmpfs). No
//    signing. Expiry is kept on CLOCK_BOOTTIME, so the NTP / browser-time step
//    of the RTC-less box (it boots at 1970) neither ends a live session nor
//    revives an expired one.
//  * CSRF    : mutating routes are POST-only (main.cpp answers anything else
//    with 405 before the handler runs) and every POST must pass a same-origin
//    Origin/Referer check (http::same_origin); on top of that the cookie is
//    SameSite=Lax, so browsers won't attach it to a cross-site POST. No token
//    machinery needed.
#ifndef RNSBOX_SESSION_H
#define RNSBOX_SESSION_H

#include <string>

namespace session {

constexpr const char* AUTH_FILE    = "/etc/rnsbox/auth.json";
constexpr const char* SESSION_FILE = "/run/rnsbox-portal.session";
constexpr const char* COOKIE_NAME  = "rnsbox_session";
constexpr long SESSION_TTL = 24 * 3600;   // session lifetime (seconds)

struct Info {
    bool valid = false;
    std::string user;
};

// --- auth (werkzeug pbkdf2:sha256 hash, verified via OpenSSL) ---
bool verify_password(const std::string& user, const std::string& password);
void ensure_default_auth();                 // seed admin/admin if auth.json absent/empty
bool set_password(const std::string& user, const std::string& newpass);
bool valid_username(const std::string& u);  // ^[A-Za-z0-9_.-]{1,32}$

// --- session token file ---
std::string start(const std::string& user); // writes token file; returns Set-Cookie value
// Logout: deletes the token file ONLY if `cookie_token` is the live, unexpired
// session (so an unauthenticated request can't end someone else's session);
// always returns the expiring Set-Cookie to clear the caller's own cookie.
std::string clear(const std::string& cookie_token);
Info check(const std::string& cookie_token);  // valid iff cookie matches the live token

}  // namespace session
#endif
