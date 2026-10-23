// session.h — dead-simple auth + session for a tiny-device admin UI.
//
//  * password: verify against the existing werkzeug pbkdf2 hash in auth.json,
//    using the box's OpenSSL (no bundled crypto).
//  * session : several independent opaque tokens (one per login) in
//    /var/lib/rnsbox/portal.session, one "user<TAB>token" line each. They do
//    NOT expire: once logged in, the browser stays logged in until it logs
//    out or the password changes. A new login appends a token instead of
//    replacing the live one, so logging in from a phone does not kick the
//    desktop out. (The RTC-less box also boots at 1970; expiry arithmetic on
//    a wall clock was unreliable, which is why there is none.)
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
// Persistent storage: a reboot must not log the operator out. The home box is
// not a high-value target and the password prompt every day is worse than a
// long-lived token; a password change still revokes it (revoke_all).
constexpr const char* SESSION_FILE = "/var/lib/rnsbox/portal.session";
constexpr const char* COOKIE_NAME  = "rnsbox_session";
// Browser-side cookie age only (10 years). The stored token itself never
// expires; revoke_all() on a password change is the explicit invalidation.
constexpr long SESSION_COOKIE_AGE = 10L * 365 * 24 * 3600;

struct Info {
    bool valid = false;
    std::string user;
};

// --- auth (werkzeug pbkdf2:sha256 hash, verified via OpenSSL) ---
bool verify_password(const std::string& user, const std::string& password);
void ensure_default_auth();                 // seed admin/admin if auth.json absent/empty
bool set_password(const std::string& user, const std::string& newpass);
// End every live session (all tokens): after a password change so a session
// issued under the old password cannot outlive it.
void revoke_all();
bool valid_username(const std::string& u);  // ^[A-Za-z0-9_.-]{1,32}$
// True while the web login is still the factory default admin/admin (the
// stored admin hash verifies against "admin"). Drives the dashboard's
// default-password warning banner; recomputed per render, nothing persisted.
bool default_password_active();

// --- session token file (one "user<TAB>token" per login) ---
std::string start(const std::string& user); // appends a token; returns Set-Cookie value
// Logout: removes ONLY the caller's token (a stale/cookieless request cannot
// end someone else's session); always returns the clearing Set-Cookie.
std::string clear(const std::string& cookie_token);
Info check(const std::string& cookie_token);  // valid iff the token is in the file

}  // namespace session
#endif
