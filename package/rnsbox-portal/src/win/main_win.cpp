// main_win.cpp — the native Windows front end: one exe, a browser UI.
//
//   rnsbox.exe [--root DIR] [--listen ADDR:PORT] [--no-rnsd] [--doctor] [--setup]
//
// It serves the same portal the router' uhttpd serves, from the same C++ route
// handlers (cgi/dispatch.cpp): server-rendered pages, POST-redirect-GET forms,
// JSON endpoints. There is no GUI and no installer dependency — start it (or
// register it as a service) and open http://127.0.0.1:8080/ in a browser.
//
// Reticulum is not bundled: the exe uses the system Python + `rns` when they
// are present, and --setup offers to install them. rnsd is supervised as a
// child process (win/supervisor.cpp), replacing /etc/init.d/S82rnsd.
//
// Deliberately absent (no hardware, no PTY, no nftables): WiFi, Bluetooth,
// HaLow/SLIP, the browser consoles and the A/B OS updater. Those tabs are
// hidden and their routes answer 503 — see win/stubs.cpp.
#ifdef _WIN32

#include "compat.h"
#include "dispatch.h"
#include "http.h"
#include "render.h"
#include "session.h"
#include "supervisor.h"
#include "util.h"
#include "web.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* APP_BASE = "/cgi-bin/portal";

struct Options {
    std::string root;
    std::string host = "127.0.0.1";
    int port = 8080;
    bool start_rnsd = true;
    bool doctor = false;
    bool setup = false;
    bool service = false;
    bool install = false;
    bool uninstall = false;
};

std::atomic<bool> g_stop{false};

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) { return util::trim(s); }

// ---- filesystem bootstrap ----

void mkdirs(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        char c = dir[i];
        cur += c;
        if ((c == '/' || c == '\\') && cur.size() > 3) {
            // Do not try to create "C:" or "C:\".
            if (cur.size() == 3 && cur[1] == ':') continue;
            _mkdir(cur.c_str());
        }
    }
    _mkdir(dir.c_str());
}

bool file_exists(const std::string& p) {
    struct _stat st;
    return _stat(p.c_str(), &st) == 0;
}

bool dir_exists(const std::string& p) {
    struct _stat st;
    return _stat(p.c_str(), &st) == 0 && ((st.st_mode & _S_IFMT) == _S_IFDIR);
}

std::string exe_dir() {
    char buf[32768];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    if (!n || n >= sizeof buf) return std::string();
    std::string p(buf, n);
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

// Copy files that are missing at `dst` from `src` (recursively). Used to seed
// a configured root with the pages/static/helpers that ship next to the exe,
// so a first run with %ProgramData%\RNSBox is not a blank page. Existing
// files are never overwritten: the root belongs to the user.
void copy_missing(const std::string& src, const std::string& dst) {
    if (!dir_exists(src)) return;
    mkdirs(dst);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((src + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string s = src + "\\" + name;
        std::string d = dst + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            copy_missing(s, d);
        } else if (!file_exists(d)) {
            CopyFileA(s.c_str(), d.c_str(), FALSE);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Where the portal's bundle lives: <exe>\root (build/packaged layout) or the
// source tree (<exe>\..\..\cgi) when running from win/build.
void seed_from_bundle() {
    std::string exe = exe_dir();
    if (exe.empty()) return;
    struct Src { std::string from, to; };
    std::vector<Src> pairs;
    std::string bundle = exe + "\\root";
    if (dir_exists(bundle)) {
        pairs.push_back({bundle + "\\usr\\share\\rnsbox-portal", compat::map_path("/usr/share/rnsbox-portal")});
        pairs.push_back({bundle + "\\usr\\lib\\rnsbox", compat::map_path("/usr/lib/rnsbox")});
        pairs.push_back({bundle + "\\www", compat::map_path("/www")});
    } else {
        std::string src = exe + "\\..\\..\\cgi";
        pairs.push_back({src + "\\pages", compat::map_path("/usr/share/rnsbox-portal/pages")});
        pairs.push_back({src + "\\helpers", compat::map_path("/usr/lib/rnsbox")});
        pairs.push_back({src + "\\www", compat::map_path("/www")});
    }
    for (const auto& p : pairs) copy_missing(p.from, p.to);
}

void ensure_tree() {
    const char* dirs[] = {
        "/etc/rnsbox", "/etc/reticulum", "/var/lib/rnsbox", "/var/run", "/var/log",
        "/usr/share/rnsbox-portal/pages", "/usr/lib/rnsbox", "/www",
    };
    for (const char* d : dirs) mkdirs(compat::map_path(d));

    // Reticulum's default config. rnsd would create a stock one, but the
    // point of this build is to be useful out of the box: transport on, a TCP
    // listener, a LAN auto-interface and one public testnet node.
    std::string cfg = compat::map_path("/etc/reticulum/config");
    if (!file_exists(cfg)) {
        std::string c =
            "# Reticulum config — RNSBox Windows build.\n"
            "# Edited live from the portal (Reticulum tab).\n"
            "\n"
            "[reticulum]\n"
            "  enable_transport = Yes\n"
            "  share_instance = Yes\n"
            "  shared_instance_port = 37428\n"
            "  instance_control_port = 37429\n"
            "  panic_on_interface_error = No\n"
            "\n"
            "[logging]\n"
            "  loglevel = 4\n"
            "\n"
            "[interfaces]\n"
            "  # LAN discovery (IPv6 link-local multicast). No `devices` list:\n"
            "  # on Windows the adapter names are not stable, so Reticulum uses\n"
            "  # every eligible interface.\n"
            "  [[LAN AutoInterface]]\n"
            "    type = AutoInterface\n"
            "    enabled = True\n"
            "    bitrate = 10000000\n"
            "\n"
            "  [[TCP Server]]\n"
            "    type = TCPServerInterface\n"
            "    enabled = True\n"
            "    listen_ip = 0.0.0.0\n"
            "    listen_port = 4242\n"
            "\n"
            "  [[RNS Testnet]]\n"
            "    type = TCPClientInterface\n"
            "    enabled = yes\n"
            "    target_host = 77.37.146.243\n"
            "    target_port = 4242\n";
        FILE* f = fopen(cfg.c_str(), "wb");
        if (f) {
            fwrite(c.data(), 1, c.size(), f);
            fclose(f);
        }
    }

    // rnsbox flat configs the portal reads. Empty defaults are honest: the
    // reader applies its own defaults for a missing key.
    const char* defaults[][2] = {
        {"wan.conf", "interface=auto\n"},
        {"openports.conf", "tcp 4242\n"},
        {"portforward.conf", ""},
        {"rnsdmon.conf", "watchdog=no\nautorestart=no\n"},
        {"nomadnet.conf", "enabled=no\n"},
        {"lxmd.conf", "enabled=no\n"},
        {"echobot.conf", "enabled=no\n"},
        {"rnsh.conf", "enabled=no\n"},
        {"ssh.conf", "enabled=no\n"},
        {"slip.conf", "enabled=no\n"},
    };
    for (const auto& d : defaults) {
        std::string p = compat::map_path(std::string("/etc/rnsbox/") + d[0]);
        if (!file_exists(p)) {
            FILE* f = fopen(p.c_str(), "wb");
            if (f) { fputs(d[1], f); fclose(f); }
        }
    }

    // Pages, static assets and the Python helpers ship next to the exe; a
    // fresh ProgramData root gets its own copy on first start.
    seed_from_bundle();
}

// ---- environment ----

bool page_tree_present() {
    return file_exists(compat::map_path("/usr/share/rnsbox-portal/pages/base.html"));
}

// ---- static files ----

std::string content_type(const std::string& p) {
    auto ends = [&](const char* s) {
        size_t n = strlen(s);
        return p.size() >= n && lower(p.substr(p.size() - n)) == s;
    };
    if (ends(".css"))  return "text/css; charset=utf-8";
    if (ends(".js"))   return "application/javascript; charset=utf-8";
    if (ends(".svg"))  return "image/svg+xml";
    if (ends(".png"))  return "image/png";
    if (ends(".ico"))  return "image/x-icon";
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".json")) return "application/json";
    if (ends(".woff2")) return "font/woff2";
    if (ends(".txt") || ends(".md")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool serve_static(const std::string& path, http::Response& res) {
    std::string rel = path;
    if (rel.empty() || rel[0] != '/') return false;
    rel.erase(0, 1);
    if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos ||
        rel.find(':') != std::string::npos)
        return false;
    std::string full = compat::map_path("/www") + "/" + rel;
    // C stdio, not std::ifstream: compat.h rewrites every `open(` token,
    // including the one inside libstdc++'s basic_filebuf.
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) return false;
    std::string body;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    fclose(f);
    res.status = "200 OK";
    res.content_type = content_type(rel);
    res.body = std::move(body);
    return true;
}

// ---- HTTP plumbing ----

bool read_request(SOCKET s, http::Request& req, std::string& raw_path, std::string& err) {
    std::string data;
    char buf[8192];
    const size_t MAX_HEAD = 64 * 1024;
    size_t head_end;
    for (;;) {
        head_end = data.find("\r\n\r\n");
        if (head_end != std::string::npos) break;
        if (data.size() > MAX_HEAD) { err = "header too large"; return false; }
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) { err = "connection closed"; return false; }
        data.append(buf, (size_t)n);
    }

    std::string head = data.substr(0, head_end);
    std::string rest = data.substr(head_end + 4);

    std::istringstream hs(head);
    std::string line;
    if (!std::getline(hs, line)) { err = "no request line"; return false; }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream ls(line);
        std::string target;
        ls >> req.method >> target;
        if (target.empty()) { err = "bad request line"; return false; }
        size_t q = target.find('?');
        raw_path = (q == std::string::npos) ? target : target.substr(0, q);
        req.query = (q == std::string::npos) ? std::string() : target.substr(q + 1);
        req.path = http::url_decode(raw_path);
    }

    std::map<std::string, std::string> h;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = lower(trim(line.substr(0, colon)));
        std::string v = trim(line.substr(colon + 1));
        h[k] = v;
    }
    req.host = h["host"];
    req.origin = h["origin"];
    req.referer = h["referer"];
    if (h.count("cookie")) http::parse_cookies(h["cookie"], req.cookies);

    http::parse_urlencoded(req.query, req.args);

    long want = 0;
    if (h.count("content-length")) {
        char* end = nullptr;
        long v = strtol(h["content-length"].c_str(), &end, 10);
        if (end && *end == '\0' && v >= 0) want = v;
    }
    if (want > http::MAX_BODY) { req.too_large = true; return true; }
    if (want > 0) {
        req.body = rest;
        while ((long)req.body.size() < want) {
            int n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.body.append(buf, (size_t)n);
            if ((long)req.body.size() > http::MAX_BODY) { req.too_large = true; break; }
        }
        if ((long)req.body.size() > want) req.body.resize((size_t)want);
    }
    std::string ct = lower(h["content-type"]);
    if (ct.find("application/x-www-form-urlencoded") != std::string::npos)
        http::parse_urlencoded(req.body, req.form);

    sockaddr_in peer{};
    int plen = sizeof peer;
    if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &plen) == 0) {
        char ip[INET_ADDRSTRLEN] = {0};
        InetNtopA(AF_INET, &peer.sin_addr, ip, sizeof ip);
        req.remote_addr = ip;
    }
    return true;
}

void send_all(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = send(s, data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

void handle_client(SOCKET s) {
    http::Request req;
    http::Response res;
    std::string raw_path, err;
    if (!read_request(s, req, raw_path, err)) {
        if (!err.empty() && err != "connection closed") {
            res.status = "400 Bad Request";
            res.content_type = "text/plain; charset=utf-8";
            res.body = err + "\n";
            send_all(s, res.bytes(false));
        }
        closesocket(s);
        return;
    }

    const std::string base = APP_BASE;
    bool head = (req.method == "HEAD");
    bool is_app = false;

    if (req.path == "/") {
        // The app lives under its CGI-style base so every link the shared
        // handlers generate stays valid. Answer here: dispatch would add a
        // second Location (the auth redirect) to the same response.
        res.redirect(base + "/");
        std::string out = res.bytes(false);
        send_all(s, out);
        std::printf("GET / -> %s\n", res.status.c_str());
        std::fflush(stdout);
        closesocket(s);
        return;
    } else if (req.path == base) {
        req.path = "/";
        is_app = true;
    } else if (req.path.compare(0, base.size() + 1, base + "/") == 0) {
        req.path = req.path.substr(base.size());
        is_app = true;
    }

    if (req.path.empty()) req.path = "/";

    if (!is_app) {
        // Static asset, or a stale link into a router-only feature.
        if (!serve_static(req.path, res)) {
            res.status = "404 Not Found";
            res.content_type = "text/plain; charset=utf-8";
            res.body = "Not found. The portal is at " + base + "/\n";
        }
    } else {
        dispatch::handle(req, res);
    }

    std::string out = res.bytes(false);
    if (head) {
        size_t e = out.find("\r\n\r\n");
        if (e != std::string::npos) out.resize(e + 4);
    }
    send_all(s, out);

    if (req.path != "/health")
        std::printf("%s %s -> %s\n", req.method.c_str(),
                    (base + (req.path == "/" ? "/" : req.path)).c_str(),
                    res.status.c_str());
    std::fflush(stdout);
    closesocket(s);
}

// ---- setup / diagnostics ----

bool ask_yes(const char* question) {
    std::printf("%s [y/N] ", question);
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    std::string t = lower(trim(line));
    return t == "y" || t == "yes" || t == "1";
}

void stream_result(const compat::ProcResult& r) {
    if (!r.out.empty()) std::printf("%s\n", r.out.c_str());
    std::printf("  exit code: %d%s\n", r.exit_code, r.timed_out ? " (timed out)" : "");
}

// Semi-automatic: explain, ask, run. Never installs silently.
bool setup_wizard() {
    std::string py = compat::find_python();
    std::string ver;

    if (py.empty()) {
        std::printf("Python was not found.\n"
                    "This build runs Reticulum on the system Python (3.9+).\n");
        std::string winget = compat::find_tool("winget");
        if (!winget.empty() && ask_yes("Install Python 3.13 with winget now?")) {
            std::printf("Running winget...\n");
            stream_result(compat::run_process(
                {winget, "install", "--id", "Python.Python.3.13", "-e",
                 "--accept-package-agreements", "--accept-source-agreements"},
                "", 900, 1 << 20));
            py = compat::find_python();
        }
        if (py.empty()) {
            std::printf("Install Python from https://www.python.org/downloads/ "
                        "(tick \"Add python.exe to PATH\"), then run --setup again.\n");
            return false;
        }
    }

    std::printf("Python: %s\n", py.c_str());
    if (!compat::rns_available(py, ver)) {
        if (!ask_yes("Reticulum (rns) is not installed for this Python. Install it now?"))
            return false;
        std::printf("Running pip install rns lxmf nomadnet...\n");
        stream_result(compat::run_process(
            {py, "-m", "pip", "install", "--upgrade", "rns", "lxmf", "nomadnet"},
            "", 1800, 1 << 20));
        if (!compat::rns_available(py, ver)) {
            std::printf("Reticulum is still not importable. Check the pip output above.\n");
            return false;
        }
    }
    std::printf("Reticulum: %s\n", ver.c_str());
    supervisor::python = py;
    return true;
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--root") o.root = next();
        else if (a == "--listen") {
            std::string v = next();
            size_t c = v.rfind(':');
            if (c == std::string::npos) {
                o.port = atoi(v.c_str());
            } else {
                o.host = v.substr(0, c);
                o.port = atoi(v.substr(c + 1).c_str());
                if (o.host.empty()) o.host = "0.0.0.0";
                if (o.host == "*") o.host = "0.0.0.0";
            }
            if (o.port <= 0 || o.port > 65535) o.port = 8080;
        }
        else if (a == "--no-rnsd") o.start_rnsd = false;
        else if (a == "--doctor") o.doctor = true;
        else if (a == "--service") o.service = true;
        else if (a == "--install-service" || a == "--install") o.install = true;
        else if (a == "--uninstall-service" || a == "--uninstall") o.uninstall = true;
        else if (a == "--setup") o.setup = true;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "rnsbox.exe — RNSBox portal for Windows\n"
                "\n"
                "  --listen ADDR:PORT   listen address (default 127.0.0.1:8080)\n"
                "  --root DIR           config/data root (default %%ProgramData%%\\RNSBox)\n"
                "  --no-rnsd            do not start the Reticulum daemon\n"
                "  --setup              install Python/Reticulum if missing (asks first)\n"
                "  --doctor             print the environment report and exit\n");
            std::exit(0);
        }
    }
    return o;
}

BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

}  // namespace

// ---- server -------------------------------------------------------------

static SOCKET g_listen = INVALID_SOCKET;
static SERVICE_STATUS_HANDLE g_status_handle = nullptr;
static SERVICE_STATUS g_status{};
static Options g_service_opts;

static int run_server(const Options& opt) {
    if (!opt.root.empty()) compat::set_root(opt.root);

    // Handlers read these exactly as they do under uhttpd.
    _putenv_s("SCRIPT_NAME", APP_BASE);
    _putenv_s("SERVER_PORT", std::to_string(opt.port).c_str());
    {
        std::string pages = compat::map_path("/usr/share/rnsbox-portal/pages");
        _putenv_s("RNSBOX_PAGES", pages.c_str());
    }

    ensure_tree();
    supervisor::prepare_env();

    // Reticulum lookup. A missing Python/RNS does not stop the portal: the UI
    // still renders and the Reticulum tab explains what to install.
    supervisor::python = compat::find_python();
    std::string rns_version;
    if (!supervisor::python.empty())
        compat::rns_available(supervisor::python, rns_version);

    session::ensure_default_auth();

    std::printf("RNSBox running\n");
    std::printf("  root      : %s\n", compat::root().c_str());
    std::printf("  python    : %s\n", supervisor::python.empty() ? "(not found; run --setup)" : supervisor::python.c_str());
    std::printf("  reticulum : %s\n", rns_version.empty() ? "(not installed; run --setup)" : rns_version.c_str());
    if (!page_tree_present())
        std::printf("  WARNING   : pages are missing from %s\n",
                    compat::map_path("/usr/share/rnsbox-portal/pages").c_str());

    if (opt.start_rnsd) {
        if (rns_version.empty()) {
            std::printf("  rnsd      : not started (Reticulum is not installed)\n");
        } else {
            std::string err;
            if (supervisor::rnsd_start(&err)) {
                long pid = supervisor::rnsd_state().pid;
                if (pid == 0)
                    std::printf("  rnsd      : using the existing shared Reticulum instance\n");
                else
                    std::printf("  rnsd      : started (pid %ld)\n", pid);
            } else {
                std::printf("  rnsd      : FAILED — %s\n", err.c_str());
            }
        }
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup failed\n");
        return 1;
    }

    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { std::printf("socket() failed\n"); return 1; }
    int yes = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)opt.port);
    if (InetPtonA(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) {
        std::printf("bad listen address: %s\n", opt.host.c_str());
        return 1;
    }
    if (bind(g_listen, (sockaddr*)&addr, sizeof addr) != 0) {
        std::printf("bind %s:%d failed (port in use?)\n", opt.host.c_str(), opt.port);
        return 1;
    }
    listen(g_listen, 32);

    std::printf("\n  Open http://%s:%d/  (login admin / admin)\n\n",
                opt.host == "0.0.0.0" ? "127.0.0.1" : opt.host.c_str(), opt.port);
    if (opt.host == "0.0.0.0")
        std::printf("  WARNING: listening on all interfaces — the admin UI is "
                    "reachable from the LAN. Change the default password.\n\n");
    std::fflush(stdout);

    while (!g_stop) {
        sockaddr_in peer{};
        int plen = sizeof peer;
        SOCKET c = accept(g_listen, (sockaddr*)&peer, &plen);
        if (c == INVALID_SOCKET) {
            if (g_stop) break;
            Sleep(20);
            continue;
        }
        std::thread(handle_client, c).detach();
    }

    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    g_listen = INVALID_SOCKET;
    WSACleanup();
    if (opt.start_rnsd) supervisor::rnsd_stop();
    std::printf("stopped\n");
    std::fflush(stdout);
    return 0;
}

// ---- Windows service (SCM) ----------------------------------------------

static void WINAPI service_ctrl(DWORD code) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_status.dwWaitHint = 5000;
        SetServiceStatus(g_status_handle, &g_status);
        g_stop = true;
        // Unblock accept() so the loop can fall out.
        if (g_listen != INVALID_SOCKET) closesocket(g_listen);
        return;
    }
    if (code == SERVICE_CONTROL_INTERROGATE) SetServiceStatus(g_status_handle, &g_status);
}

static void WINAPI service_main(DWORD, LPWSTR*) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    g_status.dwWaitHint = 15000;
    g_status_handle = RegisterServiceCtrlHandlerW(L"rnsbox", service_ctrl);
    if (!g_status_handle) return;
    SetServiceStatus(g_status_handle, &g_status);

    // A service has no console: keep the startup report in the log file.
    mkdirs(compat::map_path("/var/log"));
    std::string log = compat::map_path("/var/log/portal.log");
    if (FILE* f = freopen(log.c_str(), "a", stdout)) { (void)f; }
    freopen(log.c_str(), "a", stderr);
    std::printf("\n--- rnsbox service start ---\n");
    std::fflush(stdout);

    g_status.dwCurrentState = SERVICE_RUNNING;
    g_status.dwWaitHint = 0;
    SetServiceStatus(g_status_handle, &g_status);

    g_stop = false;
    run_server(g_service_opts);

    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    g_status.dwCurrentState = SERVICE_STOPPED;
    g_status.dwWin32ExitCode = 0;
    SetServiceStatus(g_status_handle, &g_status);
}

static std::string exe_path() {
    char buf[32768];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return (n > 0 && n < sizeof buf) ? std::string(buf, n) : std::string();
}

static bool install_service(const Options& opt) {
    std::string bin = "\"" + exe_path() + "\" --service";
    bin += " --root \"" + compat::root() + "\"";
    bin += " --listen " + opt.host + ":" + std::to_string(opt.port);
    if (!opt.start_rnsd) bin += " --no-rnsd";

    SC_HANDLE sc = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!sc) {
        std::printf("OpenSCManager failed (%lu) — run this from an elevated prompt.\n",
                    GetLastError());
        return false;
    }
    SC_HANDLE svc = CreateServiceA(
        sc, "rnsbox", "RNSBox portal",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_EXISTS)
            std::printf("The rnsbox service already exists. Remove it first (--uninstall-service).\n");
        else
            std::printf("CreateService failed (%lu).\n", e);
        CloseServiceHandle(sc);
        return false;
    }
    SERVICE_DESCRIPTIONA desc{};
    desc.lpDescription = const_cast<char*>(
        "RNSBox portal: web UI for Reticulum on http://127.0.0.1:8080/");
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    CloseServiceHandle(svc);
    CloseServiceHandle(sc);
    std::printf("Installed service \"rnsbox\" (automatic start).\n  %s\n"
                "Start it with: sc start rnsbox\n", bin.c_str());
    return true;
}

static bool uninstall_service() {
    SC_HANDLE sc = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!sc) {
        std::printf("OpenSCManager failed (%lu) — run elevated.\n", GetLastError());
        return false;
    }
    SC_HANDLE svc = OpenServiceA(sc, "rnsbox", SERVICE_STOP | DELETE);
    if (!svc) {
        std::printf("OpenService failed (%lu).\n", GetLastError());
        CloseServiceHandle(sc);
        return false;
    }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    bool ok = DeleteService(svc) != 0;
    if (ok) std::printf("Service \"rnsbox\" removed.\n");
    else    std::printf("DeleteService failed (%lu).\n", GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(sc);
    return ok;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options opt = parse_args(argc, argv);
    if (!opt.root.empty()) compat::set_root(opt.root);

    if (opt.doctor) {
        ensure_tree();
        supervisor::python = compat::find_python();
        std::string ver;
        if (!supervisor::python.empty()) compat::rns_available(supervisor::python, ver);
        std::printf("RNSBox (Windows native build)\n");
        std::printf("  root      : %s\n", compat::root().c_str());
        std::printf("  python    : %s\n", supervisor::python.empty() ? "(not found)" : supervisor::python.c_str());
        std::printf("  reticulum : %s\n", ver.empty() ? "(not installed)" : ver.c_str());
        std::printf("  pages     : %s\n", page_tree_present() ? "present" : "MISSING");
        std::printf("  config    : %s\n", compat::map_path("/etc/reticulum/config").c_str());
        return ver.empty() ? 2 : 0;
    }
    if (opt.setup) return setup_wizard() ? 0 : 1;
    if (opt.install)   { ensure_tree(); return install_service(opt) ? 0 : 1; }
    if (opt.uninstall) return uninstall_service() ? 0 : 1;

    if (opt.service) {
        g_service_opts = opt;
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(L"rnsbox"), (LPSERVICE_MAIN_FUNCTIONW)service_main},
            {nullptr, nullptr},
        };
        if (!StartServiceCtrlDispatcherW(table)) {
            std::printf("Not started by the service manager (%lu). "
                        "Install it with --install-service, or run without --service.\n",
                        GetLastError());
            return 1;
        }
        return 0;
    }

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    return run_server(opt);
}

#endif  // _WIN32
