// util.cpp — see util.h.
#include "util.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

namespace util {

std::string read_file(const std::string& path, size_t max_bytes) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    std::string out;
    char buf[8192];
    while (out.size() < max_bytes) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        size_t room = max_bytes - out.size();
        out.append(buf, size_t(n) < room ? size_t(n) : room);
    }
    ::close(fd);
    return out;
}

bool write_file_atomic(const std::string& path, const std::string& data, int mode) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        p += n; left -= size_t(n);
    }
    ::fsync(fd);
    ::close(fd);
    ::chmod(tmp.c_str(), mode);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}

// Seconds on CLOCK_MONOTONIC, for run()'s deadline. Not time(): "Sync clock to
// browser" (date -s), or an NTP step, can run while a child is out, and a
// wall-clock deadline would then look long expired and SIGKILL a child that
// already finished (time_browser's own `date -s` included).
static time_t mono_now() {
    struct timespec ts;
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return ::time(nullptr);   // never on Linux
    return ts.tv_sec;
}

RunResult run(const std::vector<std::string>& argv, const std::string& input,
              int timeout_s, size_t out_cap) {
    RunResult rr;
    if (argv.empty()) return rr;

    int inpipe[2], outpipe[2];
    if (::pipe(inpipe) != 0) return rr;
    if (::pipe(outpipe) != 0) { ::close(inpipe[0]); ::close(inpipe[1]); return rr; }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inpipe[0]); ::close(inpipe[1]); ::close(outpipe[0]); ::close(outpipe[1]);
        return rr;
    }
    if (pid == 0) {
        // child
        ::dup2(inpipe[0], STDIN_FILENO);
        ::dup2(outpipe[1], STDOUT_FILENO);
        ::dup2(outpipe[1], STDERR_FILENO);
        ::close(inpipe[0]); ::close(inpipe[1]);
        ::close(outpipe[0]); ::close(outpipe[1]);
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execv(argv[0].c_str(), cargv.data());
        ::_exit(127);  // execv failed
    }

    // parent
    ::close(inpipe[0]);
    ::close(outpipe[1]);
    if (!input.empty()) {
        ::signal(SIGPIPE, SIG_IGN);
        const char* p = input.data(); size_t left = input.size();
        while (left > 0) { ssize_t n = ::write(inpipe[1], p, left); if (n <= 0) break; p += n; left -= size_t(n); }
    }
    ::close(inpipe[1]);

    // Read output with a monotonic deadline enforced via alarm-free polling:
    // set the read fd non-blocking and loop until child exits or timeout.
    ::fcntl(outpipe[0], F_SETFL, O_NONBLOCK);
    time_t deadline = mono_now() + timeout_s;
    bool killed = false, reaped = false;
    char buf[8192];
    int status = 0;
    for (;;) {
        ssize_t n = ::read(outpipe[0], buf, sizeof(buf));
        if (n > 0) {
            size_t room = out_cap > rr.out.size() ? out_cap - rr.out.size() : 0;
            if (room) rr.out.append(buf, size_t(n) < room ? size_t(n) : room);
            continue;
        }
        if (n == 0) break;  // EOF: child closed stdout
        // n < 0: EAGAIN or error
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { // child gone; drain any remaining then break
            reaped = true;
            while ((n = ::read(outpipe[0], buf, sizeof(buf))) > 0) {
                size_t room = out_cap > rr.out.size() ? out_cap - rr.out.size() : 0;
                if (!room) break;
                rr.out.append(buf, size_t(n) < room ? size_t(n) : room);
            }
            break;
        }
        if (mono_now() >= deadline) { ::kill(pid, SIGKILL); killed = true; break; }
        ::usleep(20 * 1000);
    }
    ::close(outpipe[0]);
    // EOF usually arrives before the child is reaped. Reap it here, still under
    // the deadline: `status` would otherwise stay at its initial 0, which reads
    // as "exited 0", so every failing command looked like a success.
    while (!reaped && !killed) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { reaped = true; break; }
        if (w < 0) break;
        if (mono_now() >= deadline) { ::kill(pid, SIGKILL); killed = true; break; }
        ::usleep(20 * 1000);
    }
    if (killed) { ::waitpid(pid, &status, 0); rr.timed_out = true; rr.exit_code = -1; }
    else rr.exit_code = (reaped && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return rr;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string conf_get(const std::string& path, const std::string& key) {
    std::string data = read_file(path);
    size_t i = 0;
    std::string want = key;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(i, nl - i);
        i = nl + 1;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        if (k == want) {
            std::string v = trim(t.substr(eq + 1));
            // strip an inline comment + surrounding quotes
            size_t h = v.find('#');
            if (h != std::string::npos) v = trim(v.substr(0, h));
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
                v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return std::string();
}

bool is_single_line_clean(const std::string& s) {
    for (unsigned char c : s) {
        if (c == '\n' || c == '\r' || c == '\0') return false;
        if (c < 0x20 && c != '\t') return false;
    }
    return true;
}

std::string to_hex(const std::string& raw) {
    static const char* h = "0123456789abcdef";
    std::string o; o.reserve(raw.size() * 2);
    for (unsigned char c : raw) { o.push_back(h[c >> 4]); o.push_back(h[c & 0xf]); }
    return o;
}

std::string random_hex(size_t nbytes) {
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) return std::string();
    std::string raw; raw.resize(nbytes);
    size_t got = 0;
    while (got < nbytes) {
        ssize_t r = ::read(fd, &raw[got], nbytes - got);
        if (r <= 0) { ::close(fd); return std::string(); }
        got += size_t(r);
    }
    ::close(fd);
    return to_hex(raw);
}

bool const_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace util
