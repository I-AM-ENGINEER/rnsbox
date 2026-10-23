// supervisor.cpp — see supervisor.h.
#ifdef _WIN32

#include "supervisor.h"
#include "compat.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace supervisor {

std::string python;

namespace {

PROCESS_INFORMATION g_pi{};
bool g_started = false;

std::string log_dir() { return compat::map_path("/var/log"); }
std::string run_dir() { return compat::map_path("/var/run"); }

std::string rnsd_config_dir() { return compat::map_path("/etc/reticulum"); }

bool alive() {
    if (!g_started || !g_pi.hProcess) return false;
    return WaitForSingleObject(g_pi.hProcess, 0) == WAIT_TIMEOUT;
}

// Reticulum's shared instance port (default 37428). Another rnsd — the user's
// own NomadNet / a service install — may already own it; then this build must
// use it rather than start a second daemon that would only join as a client.
int shared_port() {
    std::string cfg = util::read_file(compat::map_path("/etc/reticulum/config"), 1 << 20);
    size_t p = cfg.find("shared_instance_port");
    if (p != std::string::npos) {
        size_t eq = cfg.find('=', p);
        if (eq != std::string::npos) {
            int v = atoi(cfg.c_str() + eq + 1);
            if (v > 0 && v < 65536) return v;
        }
    }
    return 37428;
}

bool shared_instance_alive() {
    WSADATA wsa{};
    bool own = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (own) WSACleanup();
        return false;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)shared_port());
    InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);
    bool up = false;
    if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
        up = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf, ef;
        FD_ZERO(&wf); FD_ZERO(&ef);
        FD_SET(s, &wf); FD_SET(s, &ef);
        timeval tv{0, 300 * 1000};
        int rc = select(0, nullptr, &wf, &ef, &tv);
        up = (rc > 0 && FD_ISSET(s, &wf));
    }
    closesocket(s);
    if (own) WSACleanup();
    return up;
}

void ensure_dirs() {
    // Map first, then create the Windows path directly (the shim maps again,
    // which is a no-op for an already-mapped path).
    std::string d = log_dir();
    for (size_t i = 0; i < d.size(); ++i) {
        if (d[i] == '/' || d[i] == '\\') {
            std::string p = d.substr(0, i);
            if (p.size() > 2 && p[1] != ':') _mkdir(p.c_str());
        }
    }
    _mkdir(d.c_str());
    _mkdir(run_dir().c_str());
    _mkdir(rnsd_config_dir().c_str());
}

bool write_pid(long pid) {
    FILE* f = fopen(compat::map_path("/var/run/rnsd.pid").c_str(), "wb");
    if (!f) return false;
    fprintf(f, "%ld\n", pid);
    fclose(f);
    return true;
}

void clear_pid() { _unlink(compat::map_path("/var/run/rnsd.pid").c_str()); }

}  // namespace

std::string log_path(const char* name) {
    return compat::map_path("/var/log/") + name + ".log";
}

void prepare_env() {
    SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");
    SetEnvironmentVariableA("PYTHONIOENCODING", "utf-8");
    SetEnvironmentVariableA("RNSBOX_ROOT", compat::root().c_str());
    // Reticulum writes its own config/identity under --config.
}

bool rnsd_start(std::string* error) {
    if (alive()) return true;
    if (shared_instance_alive()) {
        // Someone else's rnsd is already serving the shared instance: join it
        // instead of starting a duplicate (RNS warns loudly about that).
        return true;
    }
    if (python.empty()) {
        if (error) *error = "Python not found; run rnsbox.exe --doctor";
        return false;
    }
    std::string ver;
    if (!compat::rns_available(python, ver)) {
        if (error) *error = "Reticulum (rns) is not installed in " + python;
        return false;
    }
    ensure_dirs();

    // Command line: "<python>" -m RNS.Utilities.rnsd --config <dir>
    std::vector<std::string> argv = {python, "-m", "RNS.Utilities.rnsd",
                                     "--config", rnsd_config_dir()};
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += ' ';
        cmd += '"' + argv[i] + '"';
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    std::string logfile = log_path("rnsd");
    HANDLE log = CreateFileA(logfile.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) log = nullptr;

    STARTUPINFOA si{};
    si.cb = sizeof si;
    if (log) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log;
        si.hStdError = log;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back('\0');
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, log != nullptr,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (log) CloseHandle(log);
    if (!ok) {
        if (error) *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (g_started) {
        CloseHandle(g_pi.hThread);
        CloseHandle(g_pi.hProcess);
    }
    g_pi = pi;
    g_started = true;
    write_pid((long)pi.dwProcessId);

    // A Python/RNS import failure exits within a second: report it instead of
    // pretending the daemon started.
    if (WaitForSingleObject(pi.hProcess, 1200) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_pi = PROCESS_INFORMATION{};
        g_started = false;
        clear_pid();
        if (error) {
            std::string tail = util::read_file(logfile, 4096);
            *error = "rnsd exited immediately (code " + std::to_string(code) +
                     "). See " + logfile + (tail.empty() ? "" : ":\n" + tail);
        }
        return false;
    }
    return true;
}

bool rnsd_stop(std::string* error) {
    if (!alive()) {
        g_started = false;
        clear_pid();
        return true;
    }
    DWORD pid = g_pi.dwProcessId;
    // Ask the process group to stop. Python maps CTRL_BREAK_EVENT to
    // KeyboardInterrupt, which Reticulum handles by shutting down cleanly
    // (persisting its path table).
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
    if (WaitForSingleObject(g_pi.hProcess, 6000) != WAIT_OBJECT_0) {
        TerminateProcess(g_pi.hProcess, 1);
        WaitForSingleObject(g_pi.hProcess, 4000);
    }
    // A second rnsd started outside this process is not ours to reap.
    CloseHandle(g_pi.hThread);
    CloseHandle(g_pi.hProcess);
    g_pi = PROCESS_INFORMATION{};
    g_started = false;
    clear_pid();
    (void)error;
    return true;
}

bool rnsd_restart(std::string* error) {
    rnsd_stop();
    Sleep(300);
    return rnsd_start(error);
}

RnsdState rnsd_state() {
    RnsdState s;
    if (!alive()) {
        // A shared instance owned by another process still means Reticulum is
        // up and reachable; report it (pid unknown, uptime unknown).
        if (shared_instance_alive()) { s.running = true; s.pid = 0; }
        return s;
    }
    s.running = true;
    s.pid = (long)g_pi.dwProcessId;
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(g_pi.hProcess, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER c;
        c.LowPart = created.dwLowDateTime;
        c.HighPart = created.dwHighDateTime;
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER n;
        n.LowPart = now.dwLowDateTime;
        n.HighPart = now.dwHighDateTime;
        // 100-ns intervals -> seconds
        s.uptime_s = (long)((n.QuadPart - c.QuadPart) / 10000000ULL);
    }
    return s;
}

bool run_init(const char* name, const char* arg) {
    if (!name) return false;
    std::string n(name), a(arg ? arg : "restart");
    if (n == "S82rnsd") {
        std::string err;
        if (a == "stop") return rnsd_stop(&err);
        if (a == "kick") return alive() ? true : rnsd_start(&err);
        if (a == "kick-restart") return rnsd_restart(&err);
        if (a == "start") return rnsd_start(&err);
        return rnsd_restart(&err);
    }
    // No such service on Windows: the route reports success (there is nothing
    // to apply), and the page's "Restart" is honest about rnsd only.
    return true;
}

}  // namespace supervisor
#endif  // _WIN32
