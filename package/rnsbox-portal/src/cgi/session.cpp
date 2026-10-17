// session.cpp — see session.h.
#include "session.h"
#include "util.h"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <unistd.h>

namespace session {

// ---- minimal flat JSON object {"k":"v",...} (auth.json is exactly this) ----
static bool read_json_string(const std::string& j, size_t& p, std::string& out) {
    if (p >= j.size() || j[p] != '"') return false;
    ++p; out.clear();
    while (p < j.size()) {
        char c = j[p++];
        if (c == '\\') {
            if (p >= j.size()) return false;
            char e = j[p++];
            switch (e) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break;
                case 'r': out += '\r'; break; case '"': out += '"';  break;
                case '\\': out += '\\'; break; case '/': out += '/';  break;
                default: out += e;
            }
        } else if (c == '"') {
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

static std::map<std::string, std::string> parse_auth(const std::string& j) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < j.size()) {
        while (i < j.size() && j[i] != '"') ++i;
        if (i >= j.size()) break;
        std::string k;
        if (!read_json_string(j, i, k)) break;
        while (i < j.size() && j[i] != ':') ++i;
        if (i >= j.size()) break;
        ++i;
        while (i < j.size() && (j[i]==' '||j[i]=='\t'||j[i]=='\n'||j[i]=='\r')) ++i;
        std::string v;
        if (!read_json_string(j, i, v)) break;
        m[k] = v;
    }
    return m;
}

static std::string json_esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

static std::string serialize_auth(const std::map<std::string, std::string>& m) {
    std::string o = "{";
    bool first = true;
    for (const auto& kv : m) {
        if (!first) o += ",";
        first = false;
        o += "\"" + json_esc(kv.first) + "\":\"" + json_esc(kv.second) + "\"";
    }
    o += "}";
    return o;
}

// PBKDF2-HMAC-SHA256 via OpenSSL -> lowercase hex (dklen bytes).
static std::string pbkdf2_hex(const std::string& password, const std::string& salt,
                              int iterations, int dklen) {
    std::string out; out.resize(dklen);
    if (PKCS5_PBKDF2_HMAC(password.data(), (int)password.size(),
                          reinterpret_cast<const unsigned char*>(salt.data()), (int)salt.size(),
                          iterations, EVP_sha256(), dklen,
                          reinterpret_cast<unsigned char*>(&out[0])) != 1)
        return std::string();
    return util::to_hex(out);
}

// werkzeug hash: "pbkdf2:sha256:<iter>$<salt>$<hexdigest>"
static bool verify_werkzeug(const std::string& hash, const std::string& password) {
    size_t d1 = hash.find('$');
    if (d1 == std::string::npos) return false;
    size_t d2 = hash.find('$', d1 + 1);
    if (d2 == std::string::npos) return false;
    std::string method = hash.substr(0, d1);
    std::string salt   = hash.substr(d1 + 1, d2 - d1 - 1);
    std::string digest = hash.substr(d2 + 1);
    // method = pbkdf2:sha256:<iter>
    if (method.rfind("pbkdf2:sha256:", 0) != 0) return false;
    size_t c = method.rfind(':');
    long iter = strtol(method.c_str() + c + 1, nullptr, 10);
    if (iter <= 0 || iter > 10000000) return false;
    int dklen = (int)(digest.size() / 2);
    if (dklen <= 0 || dklen > 64) return false;
    std::string got = pbkdf2_hex(password, salt, (int)iter, dklen);
    if (got.empty()) return false;
    return util::const_time_eq(got, digest);
}

static std::string make_hash(const std::string& password) {
    std::string salt = util::random_hex(8);   // 16 hex chars
    if (salt.empty()) salt = "0000000000000000";
    // 100k iterations: offline-cracking cost for auth.json (which lives on the
    // SD card). Still instant enough on the Zero 2 W for a login (~50 ms).
    // verify_werkzeug accepts any iter count, so old 25k hashes keep working.
    std::string hex = pbkdf2_hex(password, salt, 100000, 32);
    return "pbkdf2:sha256:100000$" + salt + "$" + hex;
}

bool valid_username(const std::string& u) {
    if (u.empty() || u.size() > 32) return false;
    for (char c : u)
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

bool verify_password(const std::string& user, const std::string& password) {
    if (!valid_username(user) || password.empty() || password.size() > 512) return false;
    auto m = parse_auth(util::read_file(AUTH_FILE));
    auto it = m.find(user);
    if (it == m.end()) return false;
    return verify_werkzeug(it->second, password);
}

void ensure_default_auth() {
    auto m = parse_auth(util::read_file(AUTH_FILE));
    if (!m.empty()) return;
    m["admin"] = make_hash("admin");
    util::write_file_atomic(AUTH_FILE, serialize_auth(m), 0600);
    // Prime the dashboard banner cache while we already are in a one-off
    // setup path, so the first dashboard render does not pay the PBKDF2.
    (void)default_password_active();
}

bool default_password_active() {
    // This runs on EVERY dashboard render (the factory-password banner).
    // verify_werkzeug is a full PBKDF2 (werkzeug default ~600k iterations):
    // ~2.3 s on the RISC-V target — the dashboard's "it probes the system
    // before answering" stall. Cache the verdict in /run keyed by the
    // auth.json mtime: set_password() rewrites the file, so the mtime
    // changes and the next render recomputes exactly once.
    static const char* CACHE = "/run/rnsbox-defpass.cache";
    long mtime = 0;
    struct stat st;
    if (::stat(AUTH_FILE, &st) == 0) mtime = (long)st.st_mtime;
    std::string c = util::read_file(CACHE);
    long cm = -1; int cv = -1;
    if (sscanf(c.c_str(), "%ld %d", &cm, &cv) == 2 && cm == mtime && (cv == 0 || cv == 1))
        return cv == 1;
    int v = 0;
    {
        auto m = parse_auth(util::read_file(AUTH_FILE));
        auto it = m.find("admin");
        // The stored record is the werkzeug-format pbkdf2 hash; verify it
        // against the factory password. Salted per install, so this is exact
        // (never true for a user-chosen password except by a 1/16^16 chance
        // coincidence).
        if (it != m.end() && verify_werkzeug(it->second, "admin")) v = 1;
    }
    util::write_file_atomic(CACHE, std::to_string(mtime) + " " + std::to_string(v), 0644);
    return v == 1;
}

bool set_password(const std::string& user, const std::string& newpass) {
    if (!valid_username(user) || newpass.empty() || newpass.size() > 512) return false;
    auto m = parse_auth(util::read_file(AUTH_FILE));
    m[user] = make_hash(newpass);
    bool ok = util::write_file_atomic(AUTH_FILE, serialize_auth(m), 0600);
    // Rewrite the banner cache now: the next dashboard render would otherwise
    // pay the full PBKDF2 once because the auth.json mtime changed.
    if (ok) (void)default_password_active();
    return ok;
}

void revoke_all() {
    // The session store is a single global token file, so unlinking it ends
    // every live session. Called after a password change: a session issued
    // under the old password must not outlive it.
    ::unlink(SESSION_FILE);
}

// ---- session token file: "user\ntoken\nexpiry" ----
// The file lives on /var/lib (persistent), so a reboot does not log anyone
// out; expiry is wall-clock, one year. The board has no RTC and boots at 1970
// until NTP steps it, so a login in that pre-sync window stores expiry 0
// ("never") — a stamp made at 1970 would otherwise already be in the past.
static long wall_now() { return (long)::time(nullptr); }

static std::string cookie(const std::string& token, long max_age) {
    return std::string(COOKIE_NAME) + "=" + token +
           "; HttpOnly; SameSite=Lax; Path=/; Max-Age=" + std::to_string(max_age);
}

static void write_session(const std::string& user, const std::string& token, long expiry) {
    std::string body = user + "\n" + token + "\n" + std::to_string(expiry) + "\n";
    ::mkdir("/var/lib/rnsbox", 0700);   // EEXIST is fine
    util::write_file_atomic(SESSION_FILE, body, 0600);
}

std::string start(const std::string& user) {
    std::string token = util::random_hex(24);   // 48 hex chars
    long now = wall_now();
    // 0 = no expiry (clock was still at 1970); otherwise a year.
    long expiry = (now >= 1600000000) ? now + SESSION_TTL : 0;
    write_session(user, token, expiry);
    return cookie(token, SESSION_TTL);
}

std::string clear(const std::string& cookie_token) {
    // check() only says valid for the holder of the live token, so a cookieless
    // or stale-cookie /logout leaves the (single, global) session alone.
    if (check(cookie_token).valid) ::unlink(SESSION_FILE);
    return cookie("", 0);
}

Info check(const std::string& cookie_token) {
    Info info;
    if (cookie_token.empty()) return info;
    std::string body = util::read_file(SESSION_FILE, 4096);
    if (body.empty()) return info;
    // parse the three lines
    size_t n1 = body.find('\n');
    if (n1 == std::string::npos) return info;
    size_t n2 = body.find('\n', n1 + 1);
    if (n2 == std::string::npos) return info;
    std::string user  = body.substr(0, n1);
    std::string token = body.substr(n1 + 1, n2 - n1 - 1);
    long expiry = strtol(body.c_str() + n2 + 1, nullptr, 10);
    // Token first: a request that doesn't hold the live token never touches the
    // file (no unlink).
    if (!util::const_time_eq(cookie_token, token)) return info;
    // expiry 0 = issued before the clock was synced: valid until revoke_all
    // (password change) or logout. Otherwise wall clock, at most one TTL out —
    // a further-future stamp can't have come from start().
    long now = wall_now();
    if (expiry != 0 && (now < 0 || now >= expiry || expiry > now + SESSION_TTL)) {
        ::unlink(SESSION_FILE); return info;
    }
    info.valid = true;
    info.user = user;
    return info;
}

}  // namespace session
