// rnstatusd_client.cpp — see rnstatusd_client.h.
#include "rnstatusd_client.h"
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32

namespace rnstatusd {
bool fetch_raw(std::string& out, int) { (void)out; return false; }
}  // namespace rnstatusd

#else

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace rnstatusd {

static const char* SOCK_PATH = "/run/rnsbox-rnstatusd.sock";
static const char* HELPER = "/usr/lib/rnsbox/rnstatusd.py";
static const char* PYTHON = "/usr/bin/python3";
// Below this the 35 MB RNS import is not worth it; the portal keeps using
// the cached CLI path (and its 120 s refresh floor).
static const unsigned long long MIN_MEM_KB = 180ULL * 1024ULL;
static const size_t MAX_REPLY = 1024 * 1024;

static unsigned long long mem_total_kb() {
    FILE* f = ::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    unsigned long long kb = 0;
    while (::fgets(line, sizeof(line), f)) {
        if (::strncmp(line, "MemTotal:", 9) == 0) {
            if (::sscanf(line + 9, "%llu", &kb) != 1) kb = 0;
            break;
        }
    }
    ::fclose(f);
    return kb;
}

// Returns false when this box will never run the helper (no python3, helper
// missing, too little RAM), so the caller can fall back without waiting.
static bool spawn_helper() {
    if (::access(PYTHON, X_OK) != 0 || ::access(HELPER, R_OK) != 0) return false;
    if (mem_total_kb() < MIN_MEM_KB) return false;

    pid_t pid = ::fork();
    if (pid != 0) return pid > 0;   // parent: retry the connect loop
    ::setsid();
    int nul = ::open("/dev/null", O_RDWR);
    if (nul >= 0) {
        ::dup2(nul, 0);
        ::dup2(nul, 1);
        ::dup2(nul, 2);
        if (nul > 2) ::close(nul);
    }
    ::execl(PYTHON, "rnstatusd", HELPER, (char*)nullptr);
    ::_exit(127);
    return true;
}

// One connect + request + reply. Returns true when a JSON object came back.
static bool try_once(std::string& out, int recv_timeout_s) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (::strlen(SOCK_PATH) >= sizeof(addr.sun_path)) { ::close(fd); return false; }
    ::strcpy(addr.sun_path, SOCK_PATH);

    struct timeval tv;
    tv.tv_sec = recv_timeout_s;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    const char req[] = "stats\n";
    if (::write(fd, req, sizeof(req) - 1) != (ssize_t)(sizeof(req) - 1)) {
        ::close(fd);
        return false;
    }

    std::string reply;
    char buf[4096];
    while (reply.size() < MAX_REPLY) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        reply.append(buf, (size_t)n);
        if (reply.find('\n') != std::string::npos) break;
    }
    ::close(fd);

    size_t nl = reply.find('\n');
    if (nl != std::string::npos) reply.resize(nl);
    size_t b = reply.find_first_not_of(" \t\r\n");
    if (b == std::string::npos || reply[b] != '{') return false;
    out = reply.substr(b);
    return true;
}

bool fetch_raw(std::string& out, int wait_ms) {
    out.clear();
    if (try_once(out, 10)) return true;

    if (!spawn_helper()) return false;

    // The first answer waits for the helper's RNS import. Poll the socket;
    // the helper only starts listening after the import, so failures here are
    // expected for several seconds and cost nothing.
    int remaining = wait_ms;
    while (remaining > 0) {
        if (try_once(out, 15)) return true;
        ::usleep(250 * 1000);
        remaining -= 250;
    }
    return false;
}

}  // namespace rnstatusd

#endif
