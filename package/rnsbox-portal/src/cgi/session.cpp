// session.cpp — see session.h.
#include "session.h"
#include "util.h"
#include <openssl/evp.h>
#include <cctype>
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
    std::string hex = pbkdf2_hex(password, salt, 25000, 32);
    return "pbkdf2:sha256:25000$" + salt + "$" + hex;
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
}

bool set_password(const std::string& user, const std::string& newpass) {
    if (!valid_username(user) || newpass.empty() || newpass.size() > 512) return false;
    auto m = parse_auth(util::read_file(AUTH_FILE));
    m[user] = make_hash(newpass);
    return util::write_file_atomic(AUTH_FILE, serialize_auth(m), 0600);
}

// ---- session token file: "user\ntoken\nexpiry" ----
static std::string cookie(const std::string& token, long max_age) {
    return std::string(COOKIE_NAME) + "=" + token +
           "; HttpOnly; SameSite=Lax; Path=/; Max-Age=" + std::to_string(max_age);
}

std::string start(const std::string& user) {
    std::string token = util::random_hex(24);   // 48 hex chars
    long expiry = (long)::time(nullptr) + SESSION_TTL;
    std::string body = user + "\n" + token + "\n" + std::to_string(expiry) + "\n";
    util::write_file_atomic(SESSION_FILE, body, 0600);
    return cookie(token, SESSION_TTL);
}

std::string clear() {
    ::unlink(SESSION_FILE);
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
    if ((long)::time(nullptr) >= expiry) { ::unlink(SESSION_FILE); return info; }
    if (!util::const_time_eq(cookie_token, token)) return info;
    info.valid = true;
    info.user = user;
    return info;
}

}  // namespace session
