// token.cpp — HMAC-SHA256 tokens shared by the CGI and the broker.
#include "token.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace term {

namespace {

constexpr size_t SECRET_LEN = 32;

bool read_secret(const std::string& path, std::string& key) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[SECRET_LEN];
    ssize_t n = ::read(fd, buf, sizeof buf);
    ::close(fd);
    if (n < 16) return false;
    key.assign(buf, static_cast<size_t>(n));
    return true;
}

std::string hex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 0x0F];
    }
    return s;
}

std::string random_hex(size_t n) {
    unsigned char buf[32];
    if (n > sizeof buf) n = sizeof buf;
    if (RAND_bytes(buf, static_cast<int>(n)) != 1) return std::string();
    return hex(buf, n);
}

std::string hmac_hex(const std::string& key, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len);
    if (len == 0) return std::string();
    return hex(out, len);
}

bool parse_ll(const std::string& s, long long& v) {
    if (s.empty()) return false;
    long long r = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        r = r * 10 + (c - '0');
        if (r > 9999999999LL) return false;
    }
    v = r;
    return true;
}

}  // namespace

bool ensure_secret(const std::string& path) {
    std::string key;
    if (read_secret(path, key)) return true;
    unsigned char buf[SECRET_LEN];
    if (RAND_bytes(buf, sizeof buf) != 1) return false;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < sizeof buf) {
        ssize_t n = ::write(fd, buf + off, sizeof buf - off);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    ::close(fd);
    (void)::chmod(path.c_str(), 0600);
    return true;
}

std::string make_token(const std::string& secret_path, const std::string& session,
                       int ttl_sec) {
    std::string key;
    if (!read_secret(secret_path, key)) return std::string();
    long long exp = static_cast<long long>(::time(nullptr)) +
                    (ttl_sec != 0 ? ttl_sec : 120);
    std::string nonce = random_hex(8);
    if (nonce.empty()) return std::string();
    std::string data = session + "|" + std::to_string(exp) + "|" + nonce;
    std::string mac = hmac_hex(key, data);
    if (mac.empty()) return std::string();
    return std::to_string(exp) + "." + nonce + "." + mac;
}

bool check_token(const std::string& secret_path, const std::string& session,
                 const std::string& token) {
    size_t d1 = token.find('.');
    if (d1 == std::string::npos) return false;
    size_t d2 = token.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    std::string exp_s = token.substr(0, d1);
    std::string nonce = token.substr(d1 + 1, d2 - d1 - 1);
    std::string mac = token.substr(d2 + 1);
    if (nonce.empty() || mac.empty()) return false;

    long long exp = 0;
    if (!parse_ll(exp_s, exp)) return false;
    long long now = static_cast<long long>(::time(nullptr));
    // Allow a small amount of clock skew, but never a token minted far in the
    // future.
    if (exp < now - 60 || exp > now + 900) return false;

    std::string key;
    if (!read_secret(secret_path, key)) return false;
    std::string data = session + "|" + exp_s + "|" + nonce;
    std::string want = hmac_hex(key, data);
    if (want.size() != mac.size()) return false;
    return CRYPTO_memcmp(want.data(), mac.data(), want.size()) == 0;
}

}  // namespace term
