// websocket.cpp — RFC 6455 server endpoint.
#include "websocket.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/sha.h>

namespace term {

namespace {

constexpr size_t MAX_HTTP   = 32 * 1024;
constexpr size_t MAX_FRAME  = 1 * 1024 * 1024;
constexpr size_t MAX_MSG    = 2 * 1024 * 1024;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string ws_accept(const std::string& key) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string src = key + kGuid;
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(src.data()), src.size(), digest);
    unsigned char b64[64];
    int n = EVP_EncodeBlock(b64, digest, SHA_DIGEST_LENGTH);
    return std::string(reinterpret_cast<char*>(b64), n > 0 ? static_cast<size_t>(n) : 0);
}

const char* status_text(int status) {
    switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

}  // namespace

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexv(static_cast<unsigned char>(s[i + 1]));
            int lo = hexv(static_cast<unsigned char>(s[i + 2]));
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

std::string query_param(const std::string& q, const std::string& name) {
    size_t pos = 0;
    while (pos <= q.size()) {
        size_t amp = q.find('&', pos);
        std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos
                                                                : amp - pos);
        size_t eq = kv.find('=');
        std::string k = (eq == std::string::npos) ? kv : kv.substr(0, eq);
        if (url_decode(k) == name)
            return eq == std::string::npos ? std::string() : url_decode(kv.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

bool WsConn::feed(const char* p, size_t n) {
    if (state == State::DENIED || state == State::FAILED) return false;
    if (state == State::HTTP) {
        http_in_.append(p, n);
        if (http_in_.size() > MAX_HTTP) {
            fail();
            return false;
        }
        if (http_in_.find("\r\n\r\n") == std::string::npos) return true;
        return parse_http();
    }
    if (state == State::READY) {
        frame_in_.append(p, n);
        return true;
    }
    if (state == State::OPEN) {
        frame_in_.append(p, n);
        return parse_frames();
    }
    return true;
}

bool WsConn::parse_http() {
    size_t end = http_in_.find("\r\n\r\n");
    if (http_in_.size() > MAX_HTTP) { fail(); return false; }
    std::string head = http_in_.substr(0, end);
    frame_in_.assign(http_in_, end + 4, std::string::npos);
    http_in_.clear();

    size_t nl = head.find("\r\n");
    std::string reqline = (nl == std::string::npos) ? head : head.substr(0, nl);
    size_t s1 = reqline.find(' ');
    size_t s2 = (s1 == std::string::npos) ? std::string::npos : reqline.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) {
        deny(400, "bad request line");
        return false;
    }
    std::string method = reqline.substr(0, s1);
    std::string target = reqline.substr(s1 + 1, s2 - s1 - 1);
    std::string version = reqline.substr(s2 + 1);
    if (method != "GET" || version.find("HTTP/1.1") == std::string::npos) {
        deny(400, "websocket handshake needs GET HTTP/1.1");
        return false;
    }
    size_t qm = target.find('?');
    path = (qm == std::string::npos) ? target : target.substr(0, qm);
    query = (qm == std::string::npos) ? std::string() : target.substr(qm + 1);

    std::string upgrade, connection, key, ver;
    size_t pos = (nl == std::string::npos) ? head.size() : nl + 2;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos) eol = head.size();
        std::string line = head.substr(pos, eol - pos);
        pos = eol + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = lower(trim(line.substr(0, colon)));
        std::string v = trim(line.substr(colon + 1));
        if (k == "host") host = v;
        else if (k == "origin") origin = v;
        else if (k == "upgrade") upgrade = lower(v);
        else if (k == "connection") connection = lower(v);
        else if (k == "sec-websocket-key") key = v;
        else if (k == "sec-websocket-version") ver = v;
    }
    if (upgrade.find("websocket") == std::string::npos ||
        connection.find("upgrade") == std::string::npos || key.empty() ||
        ver != "13") {
        deny(400, "not a websocket handshake");
        return false;
    }
    accept_key_ = ws_accept(key);
    state = State::READY;
    return true;
}

void WsConn::accept() {
    if (state != State::READY) return;
    state = State::OPEN;
    out += "HTTP/1.1 101 Switching Protocols\r\n";
    out += "Upgrade: websocket\r\n";
    out += "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Accept: ";
    out += accept_key_;
    out += "\r\n\r\n";
    parse_frames();
}

void WsConn::deny(int status, const char* reason) {
    state = State::DENIED;
    std::string body = (reason && *reason) ? reason : "denied";
    body += '\n';
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += status_text(status);
    out += "\r\nContent-Type: text/plain; charset=utf-8\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
}

bool WsConn::send_frame(uint8_t opcode, const char* p, size_t n) {
    if (state != State::OPEN) return false;
    uint8_t h[10];
    size_t hn = 0;
    h[hn++] = static_cast<uint8_t>(0x80 | opcode);
    if (n < 126) {
        h[hn++] = static_cast<uint8_t>(n);
    } else if (n < 65536) {
        h[hn++] = 126;
        h[hn++] = static_cast<uint8_t>((n >> 8) & 0xFF);
        h[hn++] = static_cast<uint8_t>(n & 0xFF);
    } else {
        h[hn++] = 127;
        for (int i = 7; i >= 0; --i)
            h[hn++] = static_cast<uint8_t>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF);
    }
    out.append(reinterpret_cast<const char*>(h), hn);
    out.append(p, n);
    return true;
}

bool WsConn::send_binary(const char* p, size_t n) { return send_frame(0x2, p, n); }
bool WsConn::send_text(const char* p, size_t n) { return send_frame(0x1, p, n); }
bool WsConn::send_ping() { return send_frame(0x9, "", 0); }

bool WsConn::send_close(uint16_t code, const char* reason) {
    if (close_sent_) return false;
    close_sent_ = true;
    std::string p;
    p += static_cast<char>((code >> 8) & 0xFF);
    p += static_cast<char>(code & 0xFF);
    if (reason) p += reason;
    return send_frame(0x8, p.data(), p.size());
}

void WsConn::fail() {
    state = State::FAILED;
}

bool WsConn::parse_frames() {
    while (frame_in_.size() >= 2) {
        const uint8_t* d = reinterpret_cast<const uint8_t*>(frame_in_.data());
        bool fin = (d[0] & 0x80) != 0;
        uint8_t opcode = d[0] & 0x0F;
        bool masked = (d[1] & 0x80) != 0;
        uint64_t len = d[1] & 0x7F;
        size_t pos = 2;
        if (len == 126) {
            if (frame_in_.size() < 4) break;
            len = (static_cast<uint64_t>(d[2]) << 8) | d[3];
            pos = 4;
        } else if (len == 127) {
            if (frame_in_.size() < 10) break;
            len = 0;
            for (size_t i = 0; i < 8; ++i) len = (len << 8) | d[2 + i];
            pos = 10;
        }
        if (!masked || len > MAX_FRAME) {
            fail();
            return false;
        }
        if (pos + 4 + len > frame_in_.size()) break;
        uint8_t mask[4] = {d[pos], d[pos + 1], d[pos + 2], d[pos + 3]};
        pos += 4;
        std::string payload(reinterpret_cast<const char*>(d + pos),
                            static_cast<size_t>(len));
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
        frame_in_.erase(0, pos + static_cast<size_t>(len));

        if (opcode == 0x8) {  // close
            if (!close_sent_) send_close(1000, nullptr);
            messages.clear();
            return true;
        }
        if (opcode == 0x9) {  // ping
            if (payload.size() > 125) { fail(); return false; }
            send_frame(0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0xA) continue;  // pong
        if (opcode == 0x1 || opcode == 0x2) {
            if (!fin) {
                frag_ = payload;
                frag_active_ = true;
                frag_binary_ = (opcode == 0x2);
            } else {
                messages.push_back(Msg{opcode == 0x2, payload});
            }
            continue;
        }
        if (opcode == 0x0) {  // continuation
            if (!frag_active_) { fail(); return false; }
            frag_ += payload;
            if (frag_.size() > MAX_MSG) { fail(); return false; }
            if (fin) {
                messages.push_back(Msg{frag_binary_, frag_});
                frag_.clear();
                frag_active_ = false;
            }
            continue;
        }
        fail();
        return false;
    }
    return true;
}

}  // namespace term
