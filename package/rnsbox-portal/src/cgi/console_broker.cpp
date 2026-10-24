// console_broker.cpp — PTY broker + local client for the terminal tabs.
//
// One long-lived process owns each terminal session so the CGI tier can stay
// stateless. Two transports share those sessions:
//
//   * legacy: the CGI bridges a raw VT stream over chunked HTTP and POSTs key
//     bytes back (one CGI process per keystroke);
//   * fast:   the broker itself speaks WebSocket on a token-authenticated TCP
//     port, keeps a server-side VT model, and sends one compact snapshot
//     followed by coalesced cell diffs at a bounded frame rate.
//
// The fast path is what makes NomadNet usable on the small boards. A slow
// client never queues a backlog of full-screen redraws: every frame is a diff
// from the last screen the client was sent, so it converges on the latest
// state instead of replaying history. The legacy path remains as a fallback
// for browsers that cannot open ws:// or images where the port is blocked.
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
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "term/screen.h"
#include "term/token.h"
#include "term/websocket.h"

namespace console {

const char SOCK_PATH[] = "/run/rnsbox-console.sock";
const char PID_PATH[]  = "/run/rnsbox-console.pid";

const char SESSION_SHELL[]    = "shell";
const char SESSION_NOMADNET[] = "nomadnet";

// Runtime paths.  Production uses /run; the test harness points
// RNSBOX_TERM_RUN_DIR at a writable temporary directory.
std::string run_dir() {
    const char* d = getenv("RNSBOX_TERM_RUN_DIR");
    return (d && *d) ? std::string(d) : std::string("/run");
}
std::string secret_path() { return run_dir() + "/rnsbox-term.secret"; }
std::string port_path()   { return run_dir() + "/rnsbox-term.port"; }

namespace {

const char NOMADNET_EXE[] = "/usr/bin/nomadnet";
// The TUI and the daemon share one NomadNet identity/storage. The TUI runs
// as root (like the daemon) with these explicit paths; it cannot use the
// admin home because that would be a second, unrelated identity.
const char NOMADNET_CONFIG[]     = "/var/lib/nomadnetwork";
const char NOMADNET_RNS_CONFIG[] = "/etc/reticulum";
const char NOMADNET_PIDFILE[]    = "/var/run/nomadnet.pid";
const char NOMADNET_INIT[]       = "/etc/init.d/S83nomadnet";

std::string sock_path() { return run_dir() + "/rnsbox-console.sock"; }
std::string pid_path()  { return run_dir() + "/rnsbox-console.pid"; }
std::string nomadnet_flag_path() { return run_dir() + "/rnsbox-nomadnet-tui"; }
std::string nomadnet_req_path()  { return run_dir() + "/rnsbox-nomadnet.req"; }

bool nomadnet_daemon_running() {
    FILE* f = ::fopen(NOMADNET_PIDFILE, "r");
    if (!f) return false;
    long pid = 0;
    int n = ::fscanf(f, "%ld", &pid);
    ::fclose(f);
    return n == 1 && pid > 0 && ::kill((pid_t)pid, 0) == 0;
}

// Stop the daemon before the TUI opens the same config dir. The init script
// is serialized by its own lock and normally exits in ~2 s; wait for it so
// the TUI never races the daemon for the identity/storage.
// Fire-and-forget: the init script takes seconds and the broker must keep
// serving the Console/NomadNet WebSockets meanwhile. spawn_session() sets
// nomad_stop_at and the main loop starts the TUI once the pidfile is gone.
void kick_nomadnet_stop() {
    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        ::setsid();
        pid_t inner = ::fork();
        if (inner != 0) ::_exit(0);
        int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) {
            ::dup2(nul, 0); ::dup2(nul, 1); ::dup2(nul, 2);
            if (nul > 2) ::close(nul);
        }
        ::execl(NOMADNET_INIT, "S83nomadnet", "stop", (char*)nullptr);
        ::_exit(127);
    }
    int st = 0;
    (void)::waitpid(pid, &st, 0);   // reap the short-lived intermediate child
}

// rnsbox-mon uses this file's mtime to stop the daemon after an idle period;
// the page and the attached terminal keep it fresh.
void touch_nomadnet_req() {
    std::string p = nomadnet_req_path();
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) ::close(fd);
    ::utimensat(AT_FDCWD, p.c_str(), nullptr, 0);
}

long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool dbg() {
    static int v = -1;
    if (v < 0) v = getenv("RNSBOX_TERM_DEBUG") ? 1 : 0;
    return v == 1;
}

constexpr int WS_PORT_DEFAULT   = 7681;
constexpr int FRAME_MS_DEFAULT  = 12;          // coalesce TUI redraws into ~80 fps
constexpr size_t WS_OUT_MAX     = 256 * 1024;  // one client's queued frames

constexpr size_t RING_MAX       = 256 * 1024;   // replay window for a new/reconnected tab
constexpr size_t CLIENT_OUT_MAX = 1024 * 1024;  // a stalled browser is dropped at this point
constexpr size_t PTY_IN_MAX     = 1024 * 1024;  // queued keystrokes/paste
constexpr size_t INPUT_MAX      = 200 * 1024;   // one POST body
constexpr size_t HS_MAX         = 128;
constexpr int    HEARTBEAT_SEC  = 10;           // keeps the HTTP stream live and lets the page detect stalls
// A closed tab must not throw away a running NomadNet TUI: keep it attachable
// for five minutes, then reap it (it is started lazily to save RAM).
constexpr int    NOMADNET_IDLE_SEC = 5 * 60;
// A detached TUI keeps running its event loop (and can burn 20-30% of this
// single core). Stop the process group after this long without a client;
// the next attach sends SIGCONT and the session resumes in place.
constexpr int    NOMADNET_SUSPEND_SEC = 30;

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
    FILE* f = fopen(pid_path().c_str(), "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v > 1 && v < 1 << 30 ? static_cast<int>(v) : -1;
}

bool already_running() {
    int pid = read_pid();
    if (pid > 0 && ::kill(pid, 0) == 0) return true;
    ::unlink(pid_path().c_str());
    return false;
}

void write_pid() {
    FILE* f = fopen(pid_path().c_str(), "w");
    if (!f) return;
    fprintf(f, "%ld\n", static_cast<long>(getpid()));
    fclose(f);
    chmod(pid_path().c_str(), 0644);
}

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    if (setsid() < 0) _exit(1);
    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);
    (void)!chdir("/");
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
    time_t      nomad_stop_at = 0;// NomadNet: waiting for the daemon to exit
    bool        suspended = false;// NomadNet TUI stopped while detached
    int         cols = 80;
    int         rows = 24;
    std::string ring;      // replay window (legacy transport)
    std::string to_pty;    // queued keystrokes/paste
    size_t      pty_off = 0;
    std::string error;     // why the last spawn failed (empty when running)

    // Fast path.  grid sessions (NomadNet) keep a server-side screen model and
    // send diffs; raw sessions (shell) forward coalesced VT so xterm.js keeps
    // its scrollback.
    bool grid = false;
    std::unique_ptr<term::ScreenModel> screen;
    long long   flush_at = 0;
    bool        dirty = false;
};

struct Client {
    int fd = -1;
    enum State { HANDSHAKE, ATTACHED, INPUT } state = HANDSHAKE;
    std::string session;  // set by the handshake
    std::string hs;       // legacy handshake bytes
    std::string in;       // legacy input payload
    size_t need = 0;      // legacy expected input bytes
    std::string out;      // legacy pending output
    size_t off = 0;       // legacy bytes already sent
    bool dead = false;

    bool ws = false;
    term::WsConn wsc;
    std::string ws_raw;   // pending raw VT for a WS shell client
    bool need_snapshot = true;
    term::Snap base;      // screen state the client has been sent
    uint64_t last_mode_epoch = 0;
    bool close_after_flush = false;
};

// ---- broker state ----
int                    g_listen = -1;
int                    g_ws_listen = -1;
int                    g_frame_ms = FRAME_MS_DEFAULT;
std::map<std::string, Session> g_sessions;
std::vector<Client>    g_clients;

Session* find_session(const std::string& name) {
    auto it = g_sessions.find(name);
    return it == g_sessions.end() ? nullptr : &it->second;
}

void set_winsize(Session& s, int cols, int rows) {
    int nc = clamp_int(cols, 2, 400, 80);
    int nr = clamp_int(rows, 2, 200, 24);
    bool changed = (nc != s.cols || nr != s.rows);
    s.cols = nc;
    s.rows = nr;
    if (s.screen && changed) {
        s.screen->resize(s.cols, s.rows);
        s.dirty = true;
        s.flush_at = now_ms() + g_frame_ms;
        for (Client& c : g_clients) {
            if (c.ws && c.state == Client::ATTACHED && c.session == s.name)
                c.need_snapshot = true;
        }
    }
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
               const std::vector<std::string>& args, bool as_root = false) {
    struct passwd* pw = getpwnam("admin");
    bool have_admin = pw != nullptr;
    bool drop_admin = have_admin && !as_root;
    uid_t uid = 0;
    gid_t gid = 0;
    std::string home = "/root";
    std::string user = "root";
    if (drop_admin) {
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
        if (drop_admin) {
            (void)initgroups(user.c_str(), gid);
            if (setgid(gid) != 0 || setuid(uid) != 0) _exit(127);
        }
        if (chdir(home.c_str()) != 0) (void)!chdir("/");
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

void ensure_screen(Session& s) {
    if (!s.screen) s.screen.reset(new term::ScreenModel(s.cols, s.rows));
    else s.screen->resize(s.cols, s.rows);
    s.grid = true;
}

// Defined with the session plumbing below; used for the startup notice.
void ring_append(Session& s, const char* p, size_t n);
void feed_screen(Session& s, const char* p, size_t n);
void broadcast(Session& s, const char* p, size_t n);

bool spawn_session(Session& s) {
    if (s.name == SESSION_NOMADNET) {
        const char* env_exe = getenv("RNSBOX_TERM_NOMADNET_EXE");
        std::string exe = (env_exe && *env_exe) ? std::string(env_exe) : std::string(NOMADNET_EXE);
        s.grid = true;
        ensure_screen(s);
        static const char starting[] =
            "\r\n[NomadNet: starting the TUI with the node identity...]\r\n";
        ring_append(s, starting, sizeof(starting) - 1);
        feed_screen(s, starting, sizeof(starting) - 1);
        broadcast(s, starting, sizeof(starting) - 1);
        if (access(exe.c_str(), X_OK) != 0) {
            s.error = std::string("NomadNet is not installed on this system (") +
                      exe + " missing).";
            return false;
        }
        // Use the daemon's NomadNet config/identity (not a second identity
        // under the admin home): the browser TUI is the same node. Stop the
        // daemon first — one config dir, one instance at a time — and leave
        // /run/rnsbox-nomadnet-tui for S83nomadnet/wanwatch so they cannot
        // start a second instance while the TUI is alive.
        std::vector<std::string> args;
        args.push_back("nomadnet");
        args.push_back("--config");
        args.push_back(NOMADNET_CONFIG);
        args.push_back("--rnsconfig");
        args.push_back(NOMADNET_RNS_CONFIG);
        int flag = ::open(nomadnet_flag_path().c_str(),
                          O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (flag >= 0) ::close(flag);
        touch_nomadnet_req();
        if (nomadnet_daemon_running()) {
            kick_nomadnet_stop();
            s.nomad_stop_at = time(nullptr) + 25;
            s.error = "Stopping the NomadNet daemon...";
            return true;   // no PTY yet; the main loop starts it when gone
        }
        s.nomad_stop_at = 0;
        if (!spawn_pty(s, exe, args, true)) {
            ::unlink(nomadnet_flag_path().c_str());
            s.error = "Could not start NomadNet (PTY setup failed).";
            return false;
        }
        s.screen->reset();
        ring_append(s, starting, sizeof(starting) - 1);
        feed_screen(s, starting, sizeof(starting) - 1);
        broadcast(s, starting, sizeof(starting) - 1);
        s.dirty = true;
        s.flush_at = now_ms() + g_frame_ms;
        for (Client& c : g_clients)
            if (c.ws && c.session == s.name) c.need_snapshot = true;
        return true;
    }

    s.grid = false;
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

// ws_queue — one binary frame: [type][payload].  Types are defined by term.js:
//   1 diff, 2 snapshot, 3 raw VT bytes.
bool ws_queue(Client& c, uint8_t type, const char* p, size_t n) {
    if (!c.wsc.open()) return false;
    if (c.wsc.out.size() - c.wsc.out_off + n + 16 > WS_OUT_MAX) return false;
    std::string payload;
    payload.reserve(n + 1);
    payload.push_back(static_cast<char>(type));
    payload.append(p, n);
    return c.wsc.send_binary(payload.data(), payload.size());
}

bool out_empty(const Client& c) {
    return c.ws ? (c.wsc.out.size() == c.wsc.out_off) : (c.out.size() == c.off);
}

bool client_flush(Client& c) {
    std::string& o = c.ws ? c.wsc.out : c.out;
    size_t& off = c.ws ? c.wsc.out_off : c.off;
    while (off < o.size()) {
        ssize_t n = ::send(c.fd, o.data() + off, o.size() - off, MSG_NOSIGNAL);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    if (off == o.size()) {
        o.clear();
        off = 0;
        if (c.ws) {
            // A frame just drained. If the screen moved on while it was in
            // flight, schedule the next flush one interval later, not now:
            // this is where the frame-rate cap and the coalescing of a burst
            // actually happen.
            Session* s = find_session(c.session);
            if (s) {
                bool more = !c.ws_raw.empty();
                if (s->screen)
                    more = c.need_snapshot || s->screen->snapshot() != c.base;
                if (more) {
                    long long next = now_ms() + g_frame_ms;
                    if (!s->flush_at || s->flush_at < next) s->flush_at = next;
                }
            }
        }
    } else if (off > 65536) {
        o.erase(0, off);
        off = 0;
    }
    return true;
}

void broadcast(Session& s, const char* p, size_t n) {
    bool ws_pending = false;
    for (Client& c : g_clients) {
        if (c.state != Client::ATTACHED || c.dead || c.session != s.name) continue;
        if (c.ws) {
            // Grid sessions rebuild the screen; raw sessions forward the bytes.
            if (!s.grid) {
                c.ws_raw.append(p, n);
                if (c.ws_raw.size() > WS_OUT_MAX)
                    c.ws_raw.erase(0, c.ws_raw.size() - WS_OUT_MAX);
                ws_pending = true;
            }
            continue;
        }
        if (!client_queue(c, p, n)) c.dead = true;
    }
    if (ws_pending && !s.flush_at) s.flush_at = now_ms() + g_frame_ms;
}

bool any_attached(const Session& s) {
    for (const Client& c : g_clients)
        if (c.state == Client::ATTACHED && !c.dead && c.session == s.name) return true;
    return false;
}

bool any_ws_attached(const Session& s) {
    for (const Client& c : g_clients)
        if (c.ws && c.state == Client::ATTACHED && !c.dead && c.session == s.name)
            return true;
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

// feed_screen — apply PTY output to the session's VT model and answer any
// terminal queries (DSR/DA) on the PTY side.  The model's replies matter now
// that xterm.js is no longer parsing the app's output directly.
void feed_screen(Session& s, const char* p, size_t n) {
    if (!s.grid || !s.screen) return;
    s.screen->feed(p, n);
    std::string replies = s.screen->take_replies();
    if (!replies.empty()) {
        if (s.master >= 0 && s.to_pty.size() - s.pty_off + replies.size() <= PTY_IN_MAX)
            s.to_pty.append(replies);
        flush_pty(s);
    }
    s.dirty = true;
    if (dbg() && s.screen) {
        std::string r = s.screen->row_text(2);
        if (r.find("key:") != std::string::npos)
            fprintf(stderr, "screen row3 [%s]\n", r.c_str());
    }
    if (any_ws_attached(s) && !s.flush_at) s.flush_at = now_ms() + g_frame_ms;
}

// Resume a suspended NomadNet TUI; called when a client attaches.
void resume_nomadnet(Session& s) {
    if (s.name != SESSION_NOMADNET) return;
    if (s.suspended && s.child > 0) ::kill(-s.child, SIGCONT);
    s.suspended = false;
    s.idle_since = 0;
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
    s.suspended = false;
    s.nomad_stop_at = 0;
    s.to_pty.clear();
    s.pty_off = 0;

    if (s.name == SESSION_NOMADNET) {
        // The TUI no longer owns /var/lib/nomadnetwork; the next start may
        // bring the daemon back (wanwatch/operator).
        ::unlink(nomadnet_flag_path().c_str());
        // Keep the last screen (a crash traceback is worth reading) and add
        // the hint; spawn_session() clears the ring when it starts again.
        static const char msg[] =
            "\r\n[NomadNet exited - press Reconnect to start it again]\r\n";
        ring_append(s, msg, sizeof msg - 1);
        broadcast(s, msg, sizeof msg - 1);
        feed_screen(s, msg, sizeof msg - 1);
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

// flush_session — emit at most one frame per session per deadline.
//
// Grid sessions: every attached WS client is brought from the screen it was
// last sent to the current screen, as one diff (or one snapshot when a diff
// cannot express the change).  Raw sessions: the bytes accumulated since the
// last frame are sent as one binary message.  A client whose queue is full is
// dropped; the reconnect gets a fresh snapshot instead of a growing backlog.
void flush_session(Session& s) {
    s.flush_at = 0;
    s.dirty = false;

    if (!s.grid) {
        for (Client& c : g_clients) {
            if (!c.ws || c.state != Client::ATTACHED || c.dead || c.session != s.name)
                continue;
            if (c.wsc.out.size() > c.wsc.out_off) continue;  // one frame in flight
            if (c.ws_raw.empty()) continue;
            if (!ws_queue(c, 3, c.ws_raw.data(), c.ws_raw.size())) c.dead = true;
            c.ws_raw.clear();
        }
        return;
    }
    if (!s.screen) return;

    term::Snap cur = s.screen->snapshot();
    uint64_t epoch = s.screen->mode_epoch();
    std::string modes;
    bool modes_ready = false;

    for (Client& c : g_clients) {
        if (!c.ws || c.state != Client::ATTACHED || c.dead || c.session != s.name)
            continue;
        // Never queue a second frame for a client that has not drained the
        // previous one: the next diff is computed from c.base after it drains,
        // so intermediate screens are skipped instead of replayed.
        if (c.wsc.out.size() > c.wsc.out_off) continue;
        if (!modes_ready) {
            modes = term::ScreenModel::modes_json(cur.modes, cur.cur);
            modes_ready = true;
        }
        bool snap = false;
        if (c.need_snapshot) {
            std::string a = term::ScreenModel::snapshot_ansi(cur);
            if (!ws_queue(c, 2, a.data(), a.size())) { c.dead = true; continue; }
            c.base = cur;
            c.need_snapshot = false;
            snap = true;
        } else if (cur != c.base) {
            bool need = false;
            std::string d = term::ScreenModel::diff_ansi(c.base, cur, &need);
            if (need) {
                std::string a = term::ScreenModel::snapshot_ansi(cur);
                if (!ws_queue(c, 2, a.data(), a.size())) { c.dead = true; continue; }
            } else if (!d.empty()) {
                if (dbg())
                    fprintf(stderr, "ws diff %zu bytes key=%d\n", d.size(),
                            d.find("key:") != std::string::npos ? 1 : 0);
                if (!ws_queue(c, 1, d.data(), d.size())) { c.dead = true; continue; }
            }
            c.base = cur;
        }
        if (snap || epoch != c.last_mode_epoch) {
            c.wsc.send_text(modes.data(), modes.size());
            c.last_mode_epoch = epoch;
        }
    }
}

std::string hostname_of(const std::string& s) {
    size_t p = s.find("://");
    std::string h = (p == std::string::npos) ? s : s.substr(p + 3);
    size_t slash = h.find('/');
    if (slash != std::string::npos) h = h.substr(0, slash);
    if (!h.empty() && h[0] == '[') {
        size_t rb = h.find(']');
        if (rb != std::string::npos) h = h.substr(1, rb - 1);
    } else {
        size_t colon = h.rfind(':');
        if (colon != std::string::npos) h = h.substr(0, colon);
    }
    for (char& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return h;
}

bool origin_ok(const std::string& origin, const std::string& host) {
    if (origin.empty()) return true;   // non-browser clients may omit it
    return hostname_of(origin) == hostname_of(host);
}

void process_ws_messages(Client& c) {
    if (!c.wsc.open()) return;
    Session* s = find_session(c.session);
    if (!s) { c.dead = true; return; }
    for (const term::WsConn::Msg& m : c.wsc.messages) {
        if (m.binary) {
            if (m.data.empty()) continue;
            uint8_t t = static_cast<uint8_t>(m.data[0]);
            const char* p = m.data.data() + 1;
            size_t n = m.data.size() - 1;
            if (t == 1 || t == 2) {
                if (dbg())
                    fprintf(stderr, "ws input t=%u n=%zu master=%d queued=%zu\n",
                            t, n, s->master, s->to_pty.size() - s->pty_off);
                if (s->master >= 0 && s->to_pty.size() - s->pty_off + n <= PTY_IN_MAX)
                    s->to_pty.append(p, n);
                flush_pty(*s);
            }
        } else if (m.data.size() > 1 && m.data[0] == 'R') {
            int cols = 0, rows = 0;
            if (sscanf(m.data.c_str() + 1, "%d %d", &cols, &rows) == 2)
                set_winsize(*s, cols, rows);
        }
    }
    c.wsc.messages.clear();
    if (c.wsc.closing()) c.close_after_flush = true;
    if (c.wsc.state == term::WsConn::State::FAILED) c.dead = true;
}

void handle_ws_handshake(Client& c) {
    term::WsConn& w = c.wsc;
    if (!w.ready()) return;

    std::string session = term::query_param(w.query, "session");
    std::string token = term::query_param(w.query, "token");
    int cols = atoi(term::query_param(w.query, "cols").c_str());
    int rows = atoi(term::query_param(w.query, "rows").c_str());

    Session* s = find_session(session);
    if (!s) {
        w.deny(404, "unknown session");
        c.close_after_flush = true;
        return;
    }
    if (!term::check_token(secret_path(), session, token)) {
        w.deny(401, "bad or expired token");
        c.close_after_flush = true;
        return;
    }
    if (!origin_ok(w.origin, w.host)) {
        w.deny(403, "origin mismatch");
        c.close_after_flush = true;
        return;
    }

    w.accept();
    c.session = session;
    c.state = Client::ATTACHED;
    c.need_snapshot = true;
    c.last_mode_epoch = 0;
    resume_nomadnet(*s);

    set_winsize(*s, cols, rows);
    if (s->master < 0) {
        s->error.clear();
        spawn_session(*s);
    }
    if (dbg())
        fprintf(stderr, "ws attach session=%s master=%d grid=%d cols=%d rows=%d\n",
                session.c_str(), s->master, s->grid ? 1 : 0, s->cols, s->rows);
    if (s->master < 0) {
        std::string msg = "\r\n" +
            (s->error.empty() ? std::string("Session unavailable.") : s->error) +
            "\r\n";
        if (s->grid && s->screen) {
            s->screen->feed(msg.data(), msg.size());
            s->dirty = true;
        } else if (!ws_queue(c, 3, msg.data(), msg.size())) {
            c.dead = true;
        }
    } else if (!s->grid) {
        // Type 4 means "reset the terminal, then these raw bytes are the
        // replay ring".  A reconnect must not duplicate the shell output.
        if (!ws_queue(c, 4, s->ring.data(), s->ring.size())) c.dead = true;
    }
    if (!s->flush_at) s->flush_at = now_ms() + g_frame_ms;
}

void accept_ws_clients() {
    for (;;) {
        int fd = accept4(g_ws_listen, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) { if (g_stop) break; continue; }
            break;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        Client c;
        c.fd = fd;
        c.ws = true;
        c.state = Client::HANDSHAKE;
        g_clients.push_back(c);
    }
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
        resume_nomadnet(*s);
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
            if (c.ws) {
                if (!c.wsc.feed(buf, static_cast<size_t>(n))) {
                    if (c.wsc.denied()) c.close_after_flush = true;
                    else c.dead = true;
                    return;
                }
                if (c.wsc.ready()) {
                    handle_ws_handshake(c);
                    if (c.dead || c.close_after_flush) return;
                }
                process_ws_messages(c);
                if (c.dead || c.close_after_flush) return;
                continue;
            }
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
            // ATTACHED legacy sockets only carry output; ignore stray bytes.
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
    if (g_ws_listen >= 0) close(g_ws_listen);
    unlink(nomadnet_flag_path().c_str());
    unlink(sock_path().c_str());
    unlink(pid_path().c_str());
    unlink(port_path().c_str());
}

}  // namespace

int broker_main(bool foreground, int ws_port, const char* ws_bind) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   // children are auto-reaped; PTY EOF is the signal

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // WebSocket configuration: CLI wins over the environment, which wins over
    // the built-in default.  ws_port == 0 disables the fast path entirely.
    int ws_port_cfg = ws_port;
    if (ws_port_cfg < 0) {
        const char* e = getenv("RNSBOX_TERM_WS_PORT");
        ws_port_cfg = (e && *e) ? atoi(e) : WS_PORT_DEFAULT;
    }
    std::string bind_addr = (ws_bind && *ws_bind) ? std::string(ws_bind) : std::string();
    if (bind_addr.empty()) {
        const char* e = getenv("RNSBOX_TERM_WS_BIND");
        bind_addr = (e && *e) ? std::string(e) : std::string("0.0.0.0");
    }
    const char* eframe = getenv("RNSBOX_TERM_FRAME_MS");
    if (eframe && *eframe) {
        int v = atoi(eframe);
        if (v >= 1 && v <= 200) g_frame_ms = v;
    }

    if (already_running()) return 0;
    if (!foreground) daemonize();

    (void)term::ensure_secret(secret_path());

    unlink(sock_path().c_str());
    // The listener itself must be non-blocking: accept4()'s SOCK_NONBLOCK
    // applies to the accepted socket, not to the accept() call. A blocking
    // listener makes accept_clients() park in the kernel after the last
    // queued connection, starving every attached terminal.
    g_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_listen < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path().c_str(), sizeof addr.sun_path - 1);
    if (bind(g_listen, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        close(g_listen);
        return 1;
    }
    chmod(sock_path().c_str(), 0600);
    if (listen(g_listen, 16) != 0) { cleanup(); return 1; }
    write_pid();

    // Fast-path listener.  A failure here is not fatal: the legacy HTTP path
    // keeps working and the CGI simply stops offering the WS transport.
    if (ws_port_cfg > 0) {
        g_ws_listen = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (g_ws_listen >= 0) {
            int one = 1;
            setsockopt(g_ws_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_port = htons(static_cast<uint16_t>(ws_port_cfg));
            if (bind_addr.empty() || bind_addr == "0.0.0.0" || bind_addr == "*" ||
                inet_pton(AF_INET, bind_addr.c_str(), &a.sin_addr) != 1)
                a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(g_ws_listen, reinterpret_cast<struct sockaddr*>(&a), sizeof a) != 0 ||
                ::listen(g_ws_listen, 16) != 0) {
                close(g_ws_listen);
                g_ws_listen = -1;
            } else {
                FILE* pf = fopen(port_path().c_str(), "w");
                if (pf) {
                    fprintf(pf, "%d\n", ws_port_cfg);
                    fclose(pf);
                    chmod(port_path().c_str(), 0644);
                }
            }
        }
    }
    if (g_ws_listen < 0) unlink(port_path().c_str());

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
            // The NomadNet TUI waited for the daemon to release the shared
            // config; start it as soon as the pidfile is gone (or on timeout).
            if (s.name == SESSION_NOMADNET && s.nomad_stop_at && s.master < 0) {
                if (!nomadnet_daemon_running() || now >= s.nomad_stop_at) {
                    s.nomad_stop_at = 0;
                    spawn_session(s);
                }
            }
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
        pfd.reserve(g_sessions.size() + g_clients.size() + 2);
        struct pollfd p;
        p.fd = g_listen; p.events = POLLIN; p.revents = 0;
        pfd.push_back(p);
        size_t idx_ws = static_cast<size_t>(-1);
        if (g_ws_listen >= 0) {
            idx_ws = pfd.size();
            p.fd = g_ws_listen; p.events = POLLIN; p.revents = 0;
            pfd.push_back(p);
        }
        size_t sess_base = pfd.size();
        for (auto& kv : g_sessions) {
            Session& s = kv.second;
            if (s.master < 0) continue;
            p.fd = s.master;
            p.events = POLLIN | ((s.to_pty.size() > s.pty_off) ? POLLOUT : 0);
            p.revents = 0;
            session_refs.push_back(kv.first);
            pfd.push_back(p);
        }
        size_t cli_base = pfd.size();
        size_t nclients = g_clients.size();
        for (const Client& c : g_clients) {
            p.fd = c.fd;
            p.events = POLLIN;
            bool pending = c.ws ? (c.wsc.out.size() > c.wsc.out_off)
                                : (c.out.size() > c.off);
            if (pending) p.events |= POLLOUT;
            p.revents = 0;
            pfd.push_back(p);
        }

        long long t_ms = now_ms();
        int timeout = 1000;
        for (const auto& kv : g_sessions) {
            const Session& s = kv.second;
            if (!s.respawn_at) continue;
            long d = static_cast<long>(s.respawn_at - now);
            if (d < 1) d = 1;
            if (d * 1000 < timeout) timeout = static_cast<int>(d * 1000);
        }
        for (const auto& kv : g_sessions) {
            const Session& s = kv.second;
            if (!s.flush_at) continue;
            long long d = s.flush_at - t_ms;
            if (d < 0) d = 0;
            if (d < timeout) timeout = static_cast<int>(d);
        }

        int rc = poll(pfd.data(), pfd.size(), timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (rc > 0) {
            // Clients start after the listeners and the live sessions.
            // Snapshot the counts: accept_*()/spawn_session() may append to
            // g_clients / start a PTY but the pfd array has no entries yet.
            if (pfd[0].revents & POLLIN) accept_clients();
            if (idx_ws != static_cast<size_t>(-1) && (pfd[idx_ws].revents & POLLIN))
                accept_ws_clients();

            for (size_t i = 0; i < session_refs.size(); ++i) {
                Session* s = find_session(session_refs[i]);
                if (!s || s->master < 0) continue;
                short re = pfd[sess_base + i].revents;
                if (re & (POLLIN | POLLHUP | POLLERR)) {
                    for (;;) {
                        char buf[8192];
                        ssize_t n = ::read(s->master, buf, sizeof buf);
                        if (n > 0) {
                            ring_append(*s, buf, static_cast<size_t>(n));
                            broadcast(*s, buf, static_cast<size_t>(n));
                            feed_screen(*s, buf, static_cast<size_t>(n));
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
                short re = pfd[cli_base + i].revents;
                // Read first: an input client may send its bytes and close in
                // the same poll cycle, which reports POLLIN|POLLHUP.
                if (re & POLLIN) read_client(c);
                if (!c.dead && (re & (POLLERR | POLLHUP | POLLNVAL))) c.dead = true;
                if (!c.dead && (re & POLLOUT) && !client_flush(c)) c.dead = true;
                if (!c.dead && c.close_after_flush && out_empty(c)) c.dead = true;
            }
        }

        // Frame deadlines are independent of socket readiness: a quiet session
        // still needs its coalesced frame flushed on time.
        long long t2 = now_ms();
        for (auto& kv : g_sessions) {
            Session& s = kv.second;
            if (s.flush_at && s.flush_at <= t2) flush_session(s);
        }

        for (size_t i = 0; i < g_clients.size();) {
            if (g_clients[i].dead) {
                if (g_clients[i].fd >= 0) close(g_clients[i].fd);
                g_clients.erase(g_clients.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }

        now = time(nullptr);

        // Keep NomadNet attachable for a grace period after the last stream
        // disconnects: leaving the tab must not restart the TUI on return.
        for (auto& kv : g_sessions) {
            Session& s = kv.second;
            if (s.name != SESSION_NOMADNET) continue;
            if (any_attached(s)) { resume_nomadnet(s); continue; }
            if (s.master < 0) { s.idle_since = 0; continue; }
            if (!s.idle_since) { s.idle_since = now; continue; }
            long idle = now - s.idle_since;
            if (idle >= NOMADNET_SUSPEND_SEC && !s.suspended && s.child > 0) {
                // No client: the TUI's event loop still burns CPU. Freeze the
                // whole session; the next attach sends SIGCONT.
                ::kill(-s.child, SIGSTOP);
                s.suspended = true;
            }
            if (idle >= NOMADNET_IDLE_SEC) {
                s.idle_since = 0;
                if (s.child > 0) {
                    ::kill(-s.child, SIGHUP);
                    ::kill(-s.child, SIGCONT);  // let a stopped group exit
                }
            }
        }

        if (now - last_hb >= HEARTBEAT_SEC) {
            last_hb = now;
            static const char nul = '\0';
            for (auto& kv : g_sessions) {
                Session& s = kv.second;
                if (!any_attached(s)) continue;
                if (s.name == SESSION_NOMADNET) touch_nomadnet_req();
                ring_append(s, &nul, 1);
                for (Client& c : g_clients) {
                    if (!c.ws && c.state == Client::ATTACHED && !c.dead &&
                        c.session == s.name)
                        client_queue(c, &nul, 1);
                }
            }
            // WebSocket liveness is visible to the page: protocol pings keep
            // the TCP path honest, and the 'H' text resets the page watchdog.
            for (Client& c : g_clients) {
                if (c.ws && c.state == Client::ATTACHED && !c.dead) {
                    c.wsc.send_ping();
                    c.wsc.send_text("H", 1);
                }
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
    strncpy(addr.sun_path, sock_path().c_str(), sizeof addr.sun_path - 1);
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
