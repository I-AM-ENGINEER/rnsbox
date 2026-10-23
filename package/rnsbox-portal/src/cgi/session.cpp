// session.cpp — see session.h.
#include "session.h"
#include "util.h"
#include <openssl/evp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <vector>
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
    // All tokens live in one file: unlinking it ends every live session.
    // Called after a password change, so a session issued under the old
    // password cannot outlive it.
    ::unlink(SESSION_FILE);
}

// ---- session token file: one "user<TAB>token" line per login ----
// No expiry: the operator logs in once and stays logged in (until logout or
// a password change). Each login appends; logging in on a second device does
// not invalidate the first. Old 3-line files ("user\ntoken\nexpiry") have no
// tab and are ignored; the next login supersedes them.
static std::string cookie(const std::string& token, long max_age) {
    return std::string(COOKIE_NAME) + "=" + token +
           "; HttpOnly; SameSite=Lax; Path=/; Max-Age=" + std::to_string(max_age);
}

static bool append_session(const std::string& user, const std::string& token) {
    ::mkdir("/var/lib/rnsbox", 0700);   // EEXIST is fine
    // Bound the file: compact to the last 31 live tokens past 64 KiB.
    struct stat st;
    if (::stat(SESSION_FILE, &st) == 0 && st.st_size > 64 * 1024) {
        std::string body = util::read_file(SESSION_FILE, 512 * 1024);
        std::vector<std::string> keep;
        size_t p = 0;
        while (p < body.size()) {
            size_t e = body.find('\n', p);
            if (e == std::string::npos) e = body.size();
            std::string line = body.substr(p, e - p);
            if (line.find('\t') != std::string::npos) keep.push_back(line);
            p = e + 1;
        }
        std::string out;
        size_t from = keep.size() > 31 ? keep.size() - 31 : 0;
        for (size_t i = from; i < keep.size(); ++i) out += keep[i] + "\n";
        util::write_file_atomic(SESSION_FILE, out, 0600);
    }
    int fd = ::open(SESSION_FILE, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return false;
    std::string line = user + "\t" + token + "\n";
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = ::write(fd, line.data() + off, line.size() - off);
        if (n <= 0) { ::close(fd); return false; }
        off += static_cast<size_t>(n);
    }
    ::close(fd);
    return true;
}

std::string start(const std::string& user) {
    std::string token = util::random_hex(24);   // 48 hex chars
    append_session(user, token);
    return cookie(token, SESSION_COOKIE_AGE);
}

std::string clear(const std::string& cookie_token) {
    // Remove only the caller's token; every other browser stays logged in.
    if (!cookie_token.empty()) {
        std::string body = util::read_file(SESSION_FILE, 512 * 1024);
        std::string out;
        bool removed = false;
        size_t p = 0;
        while (p < body.size()) {
            size_t e = body.find('\n', p);
            if (e == std::string::npos) e = body.size();
            std::string line = body.substr(p, e - p);
            size_t tab = line.find('\t');
            if (tab != std::string::npos &&
                util::const_time_eq(cookie_token, line.substr(tab + 1))) {
                removed = true;
            } else if (!line.empty()) {
                out += line + "\n";
            }
            p = e + 1;
        }
        if (removed) {
            if (out.empty()) ::unlink(SESSION_FILE);
            else util::write_file_atomic(SESSION_FILE, out, 0600);
        }
    }
    return cookie("", 0);
}

Info check(const std::string& cookie_token) {
    Info info;
    if (cookie_token.empty()) return info;
    std::string body = util::read_file(SESSION_FILE, 512 * 1024);
    size_t p = 0;
    while (p < body.size()) {
        size_t e = body.find('\n', p);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(p, e - p);
        size_t tab = line.find('\t');
        if (tab != std::string::npos &&
            util::const_time_eq(cookie_token, line.substr(tab + 1))) {
            info.valid = true;
            info.user = line.substr(0, tab);
            return info;
        }
        p = e + 1;
    }
    return info;
}

}  // namespace session
