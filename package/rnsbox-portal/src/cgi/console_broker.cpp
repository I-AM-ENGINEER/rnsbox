// console_broker.cpp — PTY broker + local client for the terminal tabs.
//
// One long-lived process owns each terminal session so the CGI tier can stay
// stateless. The browser streams raw VT output over plain HTTP (uhttpd relays
// CGI stdout as chunked transfer) and posts raw key bytes back; the broker
// broadcasts a session's PTY output to every attached stream and applies
// input/resize commands from the authenticated CGI routes. Only the root-owned
// Unix socket is exposed; there is no network listener and no second port.
//
// Sessions are named in the handshake ("shell" / "nomadnet"), so one broker
// serves both tabs. The shell is started at boot (as before); NomadNet is
// started lazily on its first attach, because its TUI costs RAM that a box
// nobody is watching the tab on should not spend.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "console.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

namespace console {

const char SOCK_PATH[] = "/run/rnsbox-console.sock";
const char PID_PATH[]  = "/run/rnsbox-console.pid";

const char SESSION_SHELL[]    = "shell";
const char SESSION_NOMADNET[] = "nomadnet";

namespace {

const char NOMADNET_EXE[] = "/usr/bin/nomadnet";

constexpr size_t RING_MAX       = 256 * 1024;   // replay window for a new/reconnected tab
constexpr size_t CLIENT_OUT_MAX = 1024 * 1024;  // a stalled browser is dropped at this point
constexpr size_t PTY_IN_MAX     = 1024 * 1024;  // queued keystrokes/paste
constexpr size_t INPUT_MAX      = 200 * 1024;   // one POST body
constexpr size_t HS_MAX         = 128;
constexpr int    HEARTBEAT_SEC  = 10;           // keeps the HTTP stream live and lets the page detect stalls
// A closed tab must not throw away a running NomadNet TUI: keep it attachable
// for five minutes, then reap it (it is started lazily to save RAM).
constexpr int    NOMADNET_IDLE_SEC = 5 * 60;

volatile sig_atomic_t g_stop = 0;

void on_term(int) { g_stop = 1; }


int clamp_int(int v, int lo, int hi, int def) {
    if (v <= 0) return def;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool write_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n > 0) { p += n; len -= static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

int read_pid() {
    FILE* f = fopen(PID_PATH, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v > 1 && v < 1 << 30 ? static_cast<int>(v) : -1;
}

bool already_running() {
    int pid = read_pid();
    if (pid > 0 && ::kill(pid, 0) == 0) return true;
    ::unlink(PID_PATH);
    return false;
}

void write_pid() {
    FILE* f = fopen(PID_PATH, "w");
    if (!f) return;
    fprintf(f, "%ld\n", static_cast<long>(getpid()));
    fclose(f);
    chmod(PID_PATH, 0644);
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    if (setsid() < 0) _exit(1);
    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    (void)chdir("/");
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
    }
    umask(077);
}

struct Session {
    std::string name;
    int         master = -1;
    pid_t       child  = 0;
    time_t      spawned = 0;
    time_t      respawn_at = 0;
    time_t      idle_since = 0;   // NomadNet: no attached stream since this time
    int         cols = 80;
    int         rows = 24;
    std::string ring;      // replay window
    std::string to_pty;    // queued keystrokes/paste
    size_t      pty_off = 0;
    std::string error;     // why the last spawn failed (empty when running)
};

struct Client {
    int fd = -1;
    enum State { HANDSHAKE, ATTACHED, INPUT } state = HANDSHAKE;
    std::string session;  // set by the handshake
    std::string hs;       // handshake bytes
    std::string in;       // input payload
    size_t need = 0;      // expected input bytes
    std::string out;      // pending output (attach stream)
    size_t off = 0;       // bytes of out already sent
    bool dead = false;
};

// ---- broker state ----
int                    g_listen = -1;
std::map<std::string, Session> g_sessions;
std::vector<Client>    g_clients;

Session* find_session(const std::string& name) {
    auto it = g_sessions.find(name);
    return it == g_sessions.end() ? nullptr : &it->second;
}

void set_winsize(Session& s, int cols, int rows) {
    s.cols = clamp_int(cols, 2, 400, 80);
    s.rows = clamp_int(rows, 2, 200, 24);
    if (s.master < 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = static_cast<unsigned short>(s.cols);
    ws.ws_row = static_cast<unsigned short>(s.rows);
    ioctl(s.master, TIOCSWINSZ, &ws);
}

// The account's login shell, preferring zsh when the image has it (same
// choice S10consoleuser makes for the serial console).
std::string account_shell() {
    std::string shell = "/bin/sh";
    struct passwd* pw = getpwnam("admin");
    if (pw && pw->pw_shell && *pw->pw_shell && access(pw->pw_shell, X_OK) == 0)
        shell = pw->pw_shell;
    if (access("/bin/zsh", X_OK) == 0) shell = "/bin/zsh";
    return shell;
}

// spawn_pty — allocate a PTY in `s`, fork, drop to `admin`, exec `exe` with
// `args`. The broker itself stays root (it owns the root-only socket); the
// terminal process is unprivileged.
bool spawn_pty(Session& s, const std::string& exe,
               const std::vector<std::string>& args) {
    struct passwd* pw = getpwnam("admin");
    bool have_admin = pw != nullptr;
    uid_t uid = 0;
    gid_t gid = 0;
    std::string home = "/root";
    std::string user = "root";
    if (pw) {
        uid = pw->pw_uid;
        gid = pw->pw_gid;
        home = (pw->pw_dir && *pw->pw_dir) ? pw->pw_dir : "/home/admin";
        user = (pw->pw_name && *pw->pw_name) ? pw->pw_name : "admin";
    }
    std::string shell = account_shell();

    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m < 0) return false;
    if (grantpt(m) != 0 || unlockpt(m) != 0) { close(m); return false; }
    char name[128];
    if (ptsname_r(m, name, sizeof name) != 0) { close(m); return false; }

    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = static_cast<unsigned short>(s.cols);
    ws.ws_row = static_cast<unsigned short>(s.rows);
    ioctl(m, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid < 0) { close(m); return false; }
    if (pid == 0) {
        setsid();
        int slave = open(name, O_RDWR);
        if (slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        // Let the line discipline treat a multibyte UTF-8 character as one
        // unit for erase/kill: Backspace over Cyrillic must remove the whole
        // letter, not one byte of it.
        struct termios tio;
        if (tcgetattr(slave, &tio) == 0) {
            tio.c_iflag |= IUTF8;
            (void)tcsetattr(slave, TCSANOW, &tio);
        }
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) close(slave);
        close(m);
        if (have_admin) {
            (void)initgroups(user.c_str(), gid);
            if (setgid(gid) != 0 || setuid(uid) != 0) _exit(127);
        }
        if (chdir(home.c_str()) != 0) (void)chdir("/");
        setenv("HOME", home.c_str(), 1);
        setenv("USER", user.c_str(), 1);
        setenv("LOGNAME", user.c_str(), 1);
        setenv("SHELL", shell.c_str(), 1);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        // zsh's ZLE and every utility see a UTF-8 locale, so Cyrillic input,
        // cursor movement and filenames work as multibyte text.
        setenv("LANG", "C.UTF-8", 1);
        setenv("LC_ALL", "C.UTF-8", 1);
        std::vector<char*> av;
        av.reserve(args.size() + 1);
        for (const std::string& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execv(exe.c_str(), av.data());
        _exit(127);
    }

    int fl = fcntl(m, F_GETFL, 0);
    if (fl >= 0) fcntl(m, F_SETFL, fl | O_NONBLOCK);
    s.master = m;
    s.child = pid;
    s.spawned = time(nullptr);
    s.respawn_at = 0;
    s.error.clear();
    s.ring.clear();      // a new process starts with a clean replay window
    s.to_pty.clear();
    s.pty_off = 0;
    return true;
}

bool spawn_session(Session& s) {
    if (s.name == SESSION_NOMADNET) {
        if (access(NOMADNET_EXE, X_OK) != 0) {
            s.error = std::string("NomadNet is not installed on this system (") +
                      NOMADNET_EXE + " missing).";
            return false;
        }
        // The TUI reads the same config rnsd/nomadnet use, or creates its own
        // under the admin home; it attaches to rnsd's shared instance when
        // that is up, exactly like running `nomadnet` over SSH.
        std::vector<std::string> args;
        args.push_back("nomadnet");
        if (!spawn_pty(s, NOMADNET_EXE, args)) {
            s.error = "Could not start NomadNet (PTY setup failed).";
            return false;
        }
        return true;
    }

    std::string shell = account_shell();
    std::vector<std::string> args;
    args.push_back(shell);
    args.push_back("-l");
    if (!spawn_pty(s, shell, args)) {
        s.error = "Could not start the login shell (PTY setup failed).";
        return false;
    }
    return true;
}

void ring_append(Session& s, const char* p, size_t n) {
    s.ring.append(p, n);
    if (s.ring.size() > RING_MAX)
        s.ring.erase(0, s.ring.size() - RING_MAX);
}

bool client_queue(Client& c, const char* p, size_t n) {
    if (c.out.size() - c.off + n > CLIENT_OUT_MAX) return false;
    c.out.append(p, n);
    return true;
}

bool client_flush(Client& c) {
    while (c.off < c.out.size()) {
        ssize_t n = ::send(c.fd, c.out.data() + c.off, c.out.size() - c.off, MSG_NOSIGNAL);
        if (n > 0) { c.off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    if (c.off == c.out.size()) { c.out.clear(); c.off = 0; }
    else if (c.off > 65536) { c.out.erase(0, c.off); c.off = 0; }
    return true;
}

void broadcast(Session& s, const char* p, size_t n) {
    for (Client& c : g_clients) {
        if (c.state != Client::ATTACHED || c.dead || c.session != s.name) continue;
        if (!client_queue(c, p, n)) c.dead = true;
    }
}

bool any_attached(const Session& s) {
    for (const Client& c : g_clients)
        if (c.state == Client::ATTACHED && !c.dead && c.session == s.name) return true;
    return false;
}

void flush_pty(Session& s) {
    if (s.master < 0) return;
    while (s.pty_off < s.to_pty.size()) {
        ssize_t n = ::write(s.master, s.to_pty.data() + s.pty_off, s.to_pty.size() - s.pty_off);
        if (n > 0) { s.pty_off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        return;
    }
    if (s.pty_off == s.to_pty.size()) { s.to_pty.clear(); s.pty_off = 0; }
    else if (s.pty_off > 65536) { s.to_pty.erase(0, s.pty_off); s.pty_off = 0; }
}

void finish_input(Session& s, Client& c) {
    if (c.in.size() < c.need) return;
    if (s.master >= 0 && s.to_pty.size() - s.pty_off + c.need <= PTY_IN_MAX)
        s.to_pty.append(c.in.data(), c.need);
    flush_pty(s);
    c.dead = true;
}

// master_dead — the PTY reached EOF (the session process exited). The shell
// is restarted immediately, as it always was: the Console tab is the operator's
// way in and must not stay dead after a stray `exit`. NomadNet is not
// restarted: quitting its TUI should not spawn it again forever. Its ring gets
// a note; the next attach (Reconnect) starts it.
void master_dead(Session& s) {
    if (s.master >= 0) close(s.master);
    s.master = -1;
    s.child = 0;
    s.idle_since = 0;
    s.to_pty.clear();
    s.pty_off = 0;

    if (s.name == SESSION_NOMADNET) {
        // Keep the last screen (a crash traceback is worth reading) and add
        // the hint; spawn_session() clears the ring when it starts again.
        static const char msg[] =
            "\r\n[NomadNet exited - press Reconnect to start it again]\r\n";
        ring_append(s, msg, sizeof msg - 1);
        broadcast(s, msg, sizeof msg - 1);
        s.respawn_at = 0;
        return;
    }

    s.ring.clear();
    broadcast(s, "\r\n", 2);
    // A shell that dies immediately (broken /dev/pts, missing account) must
    // not spin the event loop; normal `exit` is far more than two seconds in.
    if (time(nullptr) - s.spawned < 2) s.respawn_at = time(nullptr) + 2;
    else { spawn_session(s); s.respawn_at = 0; }
}

void accept_clients() {
    for (;;) {
        int fd = accept4(g_listen, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) { if (g_stop) break; continue; }
            break;
        }
        Client c;
        c.fd = fd;
        g_clients.push_back(c);
    }
}

void handle_handshake(Client& c) {
    size_t nl = c.hs.find('\n');
    if (nl == std::string::npos) {
        if (c.hs.size() > HS_MAX) c.dead = true;
        return;
    }
    std::string line = c.hs.substr(0, nl);
    std::string rest = c.hs.substr(nl + 1);
    c.hs.clear();

    char name[32];
    name[0] = '\0';

    if (!line.empty() && line[0] == 'A') {
        int cols = 0, rows = 0;
        if (sscanf(line.c_str() + 1, "%31s %d %d", name, &cols, &rows) != 3) {
            c.dead = true;
            return;
        }
        Session* s = find_session(name);
        if (!s) { c.dead = true; return; }
        c.session = name;
        // Apply the new client's geometry before a lazy spawn so the child
        // starts at the right size instead of 80x24.
        set_winsize(*s, cols, rows);
        c.state = Client::ATTACHED;
        s->idle_since = 0;
        if (s->master < 0) {
            s->error.clear();
            spawn_session(*s);
        }
        if (s->master < 0) {
            // Unavailable (not installed / PTY failure): tell the browser why
            // instead of leaving an empty terminal.
            std::string msg = "\r\n" +
                (s->error.empty() ? std::string("Session unavailable.") : s->error) +
                "\r\n";
            if (!client_queue(c, msg.data(), msg.size())) c.dead = true;
        } else {
            if (!client_queue(c, s->ring.data(), s->ring.size())) c.dead = true;
        }
        // Anything after the handshake on an attach socket is not used.
    } else if (!line.empty() && line[0] == 'I') {
        unsigned long n = 0;
        if (sscanf(line.c_str() + 1, "%31s %lu", name, &n) == 2 &&
            n <= INPUT_MAX && find_session(name)) {
            c.session = name;
            c.need = static_cast<size_t>(n);
            c.state = Client::INPUT;
            if (!rest.empty()) c.in += rest;
            Session* s = find_session(c.session);
            if (s) finish_input(*s, c);
        } else {
            c.dead = true;
        }
    } else if (!line.empty() && line[0] == 'R') {
        int cols = 0, rows = 0;
        if (sscanf(line.c_str() + 1, "%31s %d %d", name, &cols, &rows) == 3) {
            Session* s = find_session(name);
            if (s) set_winsize(*s, cols, rows);
        }
        c.dead = true;
    } else {
        c.dead = true;
    }
}

void read_client(Client& c) {
    char buf[8192];
    for (;;) {
        ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        if (n > 0) {
            if (c.state == Client::HANDSHAKE) {
                c.hs.append(buf, static_cast<size_t>(n));
                handle_handshake(c);
                if (c.dead) return;
            } else if (c.state == Client::INPUT) {
                c.in.append(buf, static_cast<size_t>(n));
                if (c.in.size() > INPUT_MAX) { c.dead = true; return; }
                Session* s = find_session(c.session);
                if (s) finish_input(*s, c);
                else c.dead = true;
                if (c.dead) return;
            }
            // ATTACHED sockets only carry output; ignore any stray bytes.
        } else if (n == 0) {
            c.dead = true;
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            c.dead = true;
            return;
        }
    }
}

void cleanup() {
    for (Client& c : g_clients) if (c.fd >= 0) close(c.fd);
    g_clients.clear();
    for (auto& kv : g_sessions) {
        Session& s = kv.second;
        if (s.master >= 0) {
            close(s.master);
            if (s.child > 0) ::kill(-s.child, SIGHUP);
            s.master = -1;
        }
    }
    if (g_listen >= 0) close(g_listen);
    unlink(SOCK_PATH);
    unlink(PID_PATH);
}

}  // namespace

int broker_main(bool foreground) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   // children are auto-reaped; PTY EOF is the signal

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    if (already_running()) return 0;
    if (!foreground) daemonize();

    unlink(SOCK_PATH);
    // The listener itself must be non-blocking: accept4()'s SOCK_NONBLOCK
    // applies to the accepted socket, not to the accept() call. A blocking
    // listener makes accept_clients() park in the kernel after the last
    // queued connection, starving every attached terminal.
    g_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_listen < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    if (bind(g_listen, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        close(g_listen);
        return 1;
    }
    chmod(SOCK_PATH, 0600);
    if (listen(g_listen, 16) != 0) { cleanup(); return 1; }
    write_pid();

    Session& shell_ref = g_sessions[SESSION_SHELL];
    shell_ref.name = SESSION_SHELL;
    Session& nomadnet_ref = g_sessions[SESSION_NOMADNET];
    nomadnet_ref.name = SESSION_NOMADNET;

    if (!spawn_session(shell_ref)) {
        cleanup();
        return 1;
    }

    time_t last_hb = time(nullptr);

    while (!g_stop) {
        time_t now = time(nullptr);
        for (auto& kv : g_sessions) {
            Session& s = kv.second;
            if (s.respawn_at && now >= s.respawn_at) {
                s.respawn_at = 0;
                if (s.master < 0) {
                    spawn_session(s);
                    broadcast(s, "\r\n", 2);
                }
            }
        }

        std::vector<struct pollfd> pfd;
        std::vector<std::string> session_refs;
        pfd.reserve(g_sessions.size() + g_clients.size() + 1);
        struct pollfd p;
        p.fd = g_listen; p.events = POLLIN; p.revents = 0;
        pfd.push_back(p);
        for (auto& kv : g_sessions) {
            Session& s = kv.second;
            if (s.master < 0) continue;
            p.fd = s.master;
            p.events = POLLIN | ((s.to_pty.size() > s.pty_off) ? POLLOUT : 0);
            p.revents = 0;
            session_refs.push_back(kv.first);
            pfd.push_back(p);
        }
        size_t base = pfd.size();
        size_t nclients = g_clients.size();
        for (const Client& c : g_clients) {
            p.fd = c.fd;
            p.events = POLLIN;
            if (c.state == Client::ATTACHED && c.out.size() > c.off) p.events |= POLLOUT;
            p.revents = 0;
            pfd.push_back(p);
        }

        int timeout = 1000;
        for (const auto& kv : g_sessions) {
            const Session& s = kv.second;
            if (!s.respawn_at) continue;
            long d = static_cast<long>(s.respawn_at - now);
            if (d < 1) d = 1;
            if (d * 1000 < timeout) timeout = static_cast<int>(d * 1000);
        }
        int rc = poll(pfd.data(), pfd.size(), timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (rc > 0) {
            // Clients start after the listener and the live sessions.
            // Snapshot the counts: accept_clients()/spawn_session() may append
            // to g_clients / start a PTY but the pfd array has no entries for
            // those fds yet.
            if (pfd[0].revents & POLLIN) accept_clients();

            for (size_t i = 0; i < session_refs.size(); ++i) {
                Session* s = find_session(session_refs[i]);
                if (!s || s->master < 0) continue;
                short re = pfd[1 + i].revents;
                if (re & (POLLIN | POLLHUP | POLLERR)) {
                    for (;;) {
                        char buf[8192];
                        ssize_t n = ::read(s->master, buf, sizeof buf);
                        if (n > 0) {
                            ring_append(*s, buf, static_cast<size_t>(n));
                            broadcast(*s, buf, static_cast<size_t>(n));
                            continue;
                        }
                        if (n < 0 && errno == EINTR) continue;
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                        master_dead(*s);
                        break;
                    }
                }
                if (s->master >= 0 && (re & POLLOUT)) flush_pty(*s);
            }

            for (size_t i = 0; i < nclients; ++i) {
                Client& c = g_clients[i];
                short re = pfd[base + i].revents;
                // Read first: an input client may send its bytes and close in
                // the same poll cycle, which reports POLLIN|POLLHUP. Dropping
                // on HUP before reading would lose every keystroke.
                if (re & POLLIN) read_client(c);
                if (!c.dead && (re & (POLLERR | POLLHUP | POLLNVAL))) c.dead = true;
                if (!c.dead && (re & POLLOUT) && !client_flush(c)) c.dead = true;
            }

            for (size_t i = 0; i < g_clients.size();) {
                if (g_clients[i].dead) {
                    if (g_clients[i].fd >= 0) close(g_clients[i].fd);
                    g_clients.erase(g_clients.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }
        }

        now = time(nullptr);

        // Keep NomadNet attachable for a grace period after the last stream
        // disconnects: leaving the tab must not restart the TUI on return.
        for (auto& kv : g_sessions) {
            Session& s = kv.second;
            if (s.name != SESSION_NOMADNET) continue;
            if (any_attached(s)) { s.idle_since = 0; continue; }
            if (s.master < 0) { s.idle_since = 0; continue; }
            if (!s.idle_since) { s.idle_since = now; continue; }
            if (now - s.idle_since >= NOMADNET_IDLE_SEC) {
                s.idle_since = 0;
                if (s.child > 0) ::kill(-s.child, SIGHUP);
            }
        }

        if (now - last_hb >= HEARTBEAT_SEC) {
            last_hb = now;
            static const char nul = '\0';
            for (auto& kv : g_sessions) {
                Session& s = kv.second;
                if (!any_attached(s)) continue;
                ring_append(s, &nul, 1);
                broadcast(s, &nul, 1);
            }
        }
    }

    cleanup();
    return 0;
}

int connect_session() {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof addr.sun_path - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    tv.tv_sec = 90;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

bool attach(int fd, const std::string& session, int cols, int rows) {
    char h[96];
    int n = snprintf(h, sizeof h, "A %s %d %d\n", session.c_str(),
                     clamp_int(cols, 2, 400, 80),
                     clamp_int(rows, 2, 200, 24));
    return n > 0 && write_all(fd, h, static_cast<size_t>(n));
}

bool send_input(int fd, const std::string& session, const std::string& data) {
    if (data.size() > INPUT_MAX) return false;
    char h[96];
    int n = snprintf(h, sizeof h, "I %s %zu\n", session.c_str(), data.size());
    if (n <= 0 || !write_all(fd, h, static_cast<size_t>(n))) return false;
    return data.empty() || write_all(fd, data.data(), data.size());
}

bool send_resize(int fd, const std::string& session, int cols, int rows) {
    char h[96];
    int n = snprintf(h, sizeof h, "R %s %d %d\n", session.c_str(),
                     clamp_int(cols, 2, 400, 80),
                     clamp_int(rows, 2, 200, 24));
    return n > 0 && write_all(fd, h, static_cast<size_t>(n));
}

}  // namespace console
