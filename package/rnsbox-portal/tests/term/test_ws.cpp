// test_ws.cpp — handshake, framing, masking, ping/pong and query parsing.
#include <cstdio>
#include <cstring>
#include <string>

#include "term/websocket.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (c) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static std::string mkframe(uint8_t opcode, const std::string& payload,
                           bool fin = true, bool mask = true) {
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));
    size_t n = payload.size();
    uint8_t mb = mask ? 0x80 : 0;
    if (n < 126) {
        f.push_back(static_cast<char>(mb | n));
    } else if (n < 65536) {
        f.push_back(static_cast<char>(mb | 126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>(mb | 127));
        for (int i = 7; i >= 0; --i)
            f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF));
    }
    if (mask) {
        uint8_t k[4] = {0x12, 0x34, 0x56, 0x78};
        f.append(reinterpret_cast<char*>(k), 4);
        for (size_t i = 0; i < n; ++i)
            f.push_back(static_cast<char>(payload[i] ^ k[i & 3]));
    } else {
        f.append(payload);
    }
    return f;
}

static bool handshake(term::WsConn& c) {
    std::string req =
        "GET /term?session=nomadnet&token=abc&cols=80&rows=24 HTTP/1.1\r\n"
        "Host: 127.0.0.1:7681\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: http://127.0.0.1\r\n\r\n";
    return c.feed(req.data(), req.size()) && c.ready();
}

int main() {
    // ---- query / decode ----
    CHECK(term::url_decode("a%2Eb+c") == "a.b c");
    CHECK(term::query_param("a=1&session=nomadnet&token=x.y", "session") == "nomadnet");
    CHECK(term::query_param("a=1&session=nomadnet", "missing").empty());

    // ---- handshake ----
    {
        term::WsConn c;
        CHECK(handshake(c));
        CHECK(c.path == "/term");
        CHECK(c.host == "127.0.0.1:7681");
        CHECK(c.origin == "http://127.0.0.1");
        CHECK(term::query_param(c.query, "cols") == "80");
        c.accept();
        CHECK(c.open());
        CHECK(c.out.find("101 Switching Protocols") != std::string::npos);
        // RFC 6455's own example key/accept pair.
        CHECK(c.out.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    }

    // ---- binary and text messages ----
    {
        term::WsConn c;
        CHECK(handshake(c));
        c.accept();
        c.out.clear();
        std::string f = mkframe(0x2, std::string("\x01hello", 6));
        CHECK(c.feed(f.data(), f.size()));
        CHECK(c.messages.size() == 1);
        CHECK(c.messages[0].binary);
        CHECK(c.messages[0].data == std::string("\x01hello", 6));
        c.messages.clear();

        f = mkframe(0x1, "R 100 30");
        CHECK(c.feed(f.data(), f.size()));
        CHECK(c.messages.size() == 1);
        CHECK(!c.messages[0].binary);
        CHECK(c.messages[0].data == "R 100 30");
    }

    // ---- fragmentation ----
    {
        term::WsConn c;
        CHECK(handshake(c));
        c.accept();
        std::string f = mkframe(0x1, "he", false);
        f += mkframe(0x0, "llo", true);
        CHECK(c.feed(f.data(), f.size()));
        CHECK(c.messages.size() == 1);
        CHECK(c.messages[0].data == "hello");
    }

    // ---- ping -> pong ----
    {
        term::WsConn c;
        CHECK(handshake(c));
        c.accept();
        c.out.clear();
        std::string f = mkframe(0x9, "ping");
        CHECK(c.feed(f.data(), f.size()));
        CHECK(c.out == std::string("\x8a\x04ping", 6));
    }

    // ---- close ----
    {
        term::WsConn c;
        CHECK(handshake(c));
        c.accept();
        std::string f = mkframe(0x8, std::string("\x03\xe8", 2));
        CHECK(c.feed(f.data(), f.size()));
        CHECK(c.closing());
    }

    // ---- unmasked client frame is a protocol error ----
    {
        term::WsConn c;
        CHECK(handshake(c));
        c.accept();
        std::string f = mkframe(0x1, "x", true, false);
        CHECK(!c.feed(f.data(), f.size()));
        CHECK(c.state == term::WsConn::State::FAILED);
    }

    // ---- a bad handshake is denied, not accepted ----
    {
        term::WsConn c;
        std::string req = "POST /term HTTP/1.1\r\nHost: x\r\n\r\n";
        CHECK(!c.feed(req.data(), req.size()));
        CHECK(c.denied());
        CHECK(c.out.find("400 Bad Request") != std::string::npos);
    }

    std::printf("ws: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
