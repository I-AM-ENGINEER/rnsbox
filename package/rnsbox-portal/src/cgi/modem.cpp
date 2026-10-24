// modem.cpp — see modem.h. Mirrors the tiny HTTP/1.0 client in routes_modem
// (connect, write, read-to-close) without libcurl; responses are <4 KB.
#include "modem.h"
#include "util.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace modem {

#ifndef _WIN32

namespace {

bool send_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = ::send(fd, data.data() + off, data.size() - off, 0);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

}  // namespace

bool post(const std::string& peer, const std::string& endpoint,
          const std::string& json, std::string& resp, int timeout_s) {
    resp.clear();
    if (peer.empty() || endpoint.empty()) return false;
    // The endpoint is a plain API word; never let a caller smuggle a request
    // line or path traversal through it.
    for (unsigned char c : endpoint)
        if (!(isalnum(c) || c == '_' || c == '.' || c == '-' || c == '/')) return false;
    if (endpoint.find("..") != std::string::npos) return false;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (timeout_s < 1) timeout_s = 1;
    struct timeval tv = { timeout_s, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa;
    ::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    if (::inet_pton(AF_INET, peer.c_str(), &sa.sin_addr) != 1 ||
        ::connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        ::close(fd);
        return false;
    }
    std::string req =
        "POST /api/" + endpoint + " HTTP/1.0\r\n"
        "Host: " + peer + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(json.size()) + "\r\n"
        "Connection: close\r\n\r\n" + json;
    if (!send_all(fd, req)) {
        ::close(fd);
        return false;
    }
    char buf[1024];
    for (;;) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        if (resp.size() < 64 * 1024) resp.append(buf, (size_t)r);
    }
    ::close(fd);
    size_t sp = resp.find(' ');
    int code = (sp == std::string::npos) ? 0 : std::atoi(resp.c_str() + sp + 1);
    return code >= 200 && code < 300;
}

bool get(const std::string& peer, const std::string& endpoint,
         std::string& resp, int timeout_s) {
    resp.clear();
    if (peer.empty() || endpoint.empty()) return false;
    for (unsigned char c : endpoint)
        if (!(isalnum(c) || c == '_' || c == '.' || c == '-' || c == '/')) return false;
    if (endpoint.find("..") != std::string::npos) return false;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (timeout_s < 1) timeout_s = 1;
    struct timeval tv = { timeout_s, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa;
    ::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    if (::inet_pton(AF_INET, peer.c_str(), &sa.sin_addr) != 1 ||
        ::connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        ::close(fd);
        return false;
    }
    std::string req = "GET /api/" + endpoint + " HTTP/1.0\r\nHost: " + peer +
                      "\r\nConnection: close\r\n\r\n";
    if (!send_all(fd, req)) { ::close(fd); return false; }
    char buf[1024];
    for (;;) {
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        if (resp.size() < 64 * 1024) resp.append(buf, (size_t)r);
    }
    ::close(fd);
    size_t sp = resp.find(' ');
    int code = (sp == std::string::npos) ? 0 : std::atoi(resp.c_str() + sp + 1);
    return code >= 200 && code < 300;
}

std::string local_ip_towards(const store::SlipConfig& sl) {
    if (sl.mode != "ip") return sl.local_ip.empty() ? "192.168.7.1" : sl.local_ip;
    int port = std::atoi(sl.tcp_port.c_str());
    if (port < 1 || port > 65535) port = 4242;
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return sl.local_ip;
    struct sockaddr_in sa;
    ::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (::inet_pton(AF_INET, sl.peer_ip.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        return sl.local_ip;
    }
    std::string out = sl.local_ip;
    if (::connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        char b[INET_ADDRSTRLEN] = "";
        if (::getsockname(fd, (struct sockaddr *)&local, &len) == 0 &&
            ::inet_ntop(AF_INET, &local.sin_addr, b, sizeof(b)))
            out = b;
    }
    ::close(fd);
    return out;
}

#else

bool post(const std::string&, const std::string&, const std::string&, std::string&, int) {
    return false;
}
bool get(const std::string&, const std::string&, std::string&, int) { return false; }
std::string local_ip_towards(const store::SlipConfig& sl) { return sl.local_ip; }

#endif

}  // namespace modem
