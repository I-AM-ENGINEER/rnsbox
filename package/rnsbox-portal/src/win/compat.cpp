// compat.cpp — Windows implementations behind compat.h (see that file).
#ifdef _WIN32

#include "compat.h"
#include "util.h"

#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

// The shims below must call the CRT functions, not themselves.
#undef open
#undef access
#undef stat
#undef opendir
#undef mkdir
#undef unlink
#undef rename
#undef chmod
#undef flock
#undef fsync

#pragma comment(lib, "bcrypt.lib")

namespace compat {

static std::string g_root;
static bool g_root_set = false;

std::string root() {
    if (g_root_set) return g_root;
    const char* env = getenv("RNSBOX_ROOT");
    std::string r;
    if (env && *env) {
        r = env;
    } else {
        // %ProgramData%\RNSBox — survives a service running as another user.
        char buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("ProgramData", buf, sizeof buf);
        r = (n > 0 && n < sizeof buf) ? std::string(buf) : std::string("C:\\ProgramData");
        r += "\\RNSBox";
    }
    while (!r.empty() && (r.back() == '\\' || r.back() == '/')) r.pop_back();
    if (!g_root_set) { g_root = r; g_root_set = true; }
    return r;
}

void set_root(const std::string& r) {
    // Absolute, so a child started with another cwd (or as a
    // service) resolves the same tree.
    char buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameA(r.c_str(), sizeof buf, buf, nullptr);
    g_root = (n > 0 && n < sizeof buf) ? std::string(buf) : r;
    while (!g_root.empty() && (g_root.back() == '\\' || g_root.back() == '/')) g_root.pop_back();
    g_root_set = true;
    // Keep the environment in sync: crash reports and helper scripts read it.
    SetEnvironmentVariableA("RNSBOX_ROOT", g_root.c_str());
}

std::string map_path(const std::string& p) {
    if (p.empty()) return p;
    if (p == "/dev/null") return "NUL";
    if (p[0] != '/') return p;                    // already Windows / relative
    if (p.size() >= 2 && p[1] == '/') return p;   // "//server/share" UNC
    return root() + p;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    UINT cp = (n > 0) ? CP_UTF8 : CP_ACP;
    if (n <= 0)
        n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---- command resolution ----

static bool file_exists(const std::string& p) {
    struct _stat st;
    return _stat(p.c_str(), &st) == 0 && ((st.st_mode & _S_IFMT) == _S_IFREG);
}

static std::string dir_of(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

static std::string base_of(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// First existing file matching a wildcard like "...\Python3*\python.exe".
static std::string glob_first(const std::string& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string dir = dir_of(pattern);
            found = (dir.empty() ? std::string() : dir + "\\") + narrow(fd.cFileName);
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static std::string find_in_path(const std::string& name) {
    char buf[32767];
    DWORD n = GetEnvironmentVariableA("PATH", buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return std::string();
    std::string path(buf, n), cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == ';') {
            if (!cur.empty()) {
                std::string p = cur + "\\" + name;
                if (file_exists(p)) return p;
                if (file_exists(p + ".exe")) return p + ".exe";
            }
            cur.clear();
        } else {
            cur += path[i];
        }
    }
    return std::string();
}

std::string find_python() {
    const char* env = getenv("RNSBOX_PYTHON");
    if (env && *env && file_exists(env)) return env;

    if (std::string p = find_in_path("python"); !p.empty()) return p;
    if (std::string p = find_in_path("python3"); !p.empty()) return p;

    // Standard per-user and per-machine installs (a service has a PATH
    // without the user's Python).
    std::string local, pf;
    char buf[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof buf))
        local = std::string(buf) + "\\Programs\\Python\\Python3*\\python.exe";
    if (GetEnvironmentVariableA("ProgramFiles", buf, sizeof buf))
        pf = std::string(buf) + "\\Python3*\\python.exe";
    if (!local.empty()) if (std::string p = glob_first(local); !p.empty()) return p;
    if (!pf.empty()) if (std::string p = glob_first(pf); !p.empty()) return p;

    // Last resort: the py launcher (works when one version is installed).
    if (std::string p = find_in_path("py"); !p.empty()) return p;
    return std::string();
}

static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string find_tool(const std::string& argv0) {
    std::string b = lower(base_of(argv0));
    if (b == "python3" || b == "python" || b == "python3.exe")
        return find_python();

    if (!argv0.empty() && (argv0[0] == '/' || (argv0.size() > 1 && argv0[1] == ':'))) {
        std::string mapped = map_path(argv0);
        if (argv0.size() > 1 && argv0[1] == ':') mapped = argv0;
        if (file_exists(mapped)) return mapped;
        // /usr/bin/rnstatus -> rnstatus.exe on PATH
    }
    std::string name = base_of(argv0);
    if (name.size() > 4 && lower(name.substr(name.size() - 4)) == ".exe")
        name.resize(name.size() - 4);
    return find_in_path(name);
}

bool rns_available(const std::string& python, std::string& version) {
    if (python.empty()) return false;
    ProcResult r = run_process({python, "-c", "import RNS;print(RNS.__version__)"}, "", 30, 4096);
    std::string v = r.out;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (r.exit_code != 0 || v.empty()) return false;
    version = v;
    return true;
}

// ---- process execution ----

static void append_arg(std::wstring& cmd, const std::wstring& a) {
    if (!cmd.empty()) cmd += L' ';
    bool quote = a.empty() || a.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!quote) { cmd += a; return; }
    cmd += L'"';
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
        if (i == a.size()) { cmd.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { cmd.append(bs * 2 + 1, L'\\'); cmd += L'"'; }
        else { cmd.append(bs, L'\\'); cmd += a[i]; }
    }
    cmd += L'"';
}

ProcResult run_process(const std::vector<std::string>& argv, const std::string& input,
                       int timeout_s, size_t out_cap) {
    ProcResult rr;
    if (argv.empty()) return rr;

    std::string exe = find_tool(argv[0]);
    if (exe.empty()) return rr;

    std::wstring cmd;
    append_arg(cmd, wide(exe));
    for (size_t i = 1; i < argv.size(); ++i) append_arg(cmd, wide(argv[i]));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return rr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r); CloseHandle(in_w); return rr;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w); CloseHandle(out_r);
        return rr;
    }

    if (!input.empty()) {
        DWORD wrote = 0;
        WriteFile(in_w, input.data(), (DWORD)input.size(), &wrote, nullptr);
    }
    CloseHandle(in_w);

    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_s > 0 ? timeout_s : 10) * 1000;
    bool killed = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(out_r, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[8192];
            DWORD got = 0;
            if (ReadFile(out_r, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &got, nullptr) && got) {
                if (rr.out.size() < out_cap) {
                    size_t room = out_cap - rr.out.size();
                    rr.out.append(buf, got < room ? got : room);
                }
                continue;
            }
        }
        DWORD st = WaitForSingleObject(pi.hProcess, 0);
        if (st == WAIT_OBJECT_0) {
            // Drain what is left, then finish.
            for (;;) {
                char buf[8192];
                DWORD got = 0;
                if (!ReadFile(out_r, buf, sizeof buf, &got, nullptr) || !got) break;
                if (rr.out.size() < out_cap) {
                    size_t room = out_cap - rr.out.size();
                    rr.out.append(buf, got < room ? got : room);
                }
            }
            break;
        }
        if (GetTickCount64() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            killed = true;
            break;
        }
        Sleep(15);
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    rr.exit_code = killed ? -1 : (int)code;
    rr.timed_out = killed;

    CloseHandle(out_r);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rr;
}

std::string random_bytes(size_t n) {
    std::string out;
    out.resize(n);
    if (n == 0) return out;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&out[0]), (ULONG)n,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return std::string();
    return out;
}

}  // namespace compat

// ---- POSIX shims ----

int rnsbox_open(const char* path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    std::string p = compat::map_path(path ? path : "");
    // The portal writes configs byte-for-byte; never translate CRLF.
    return _open(p.c_str(), flags | _O_BINARY | _O_NOINHERIT, mode);
}

int rnsbox_access(const char* path, int mode) {
    std::string p = compat::map_path(path ? path : "");
    // X_OK (1) has no Windows equivalent: existence is the best we can do,
    // which is exactly what the callers ask ("is this tool installed?").
    if (mode & 1) mode &= ~1;
    return _access(p.c_str(), mode);
}

int rnsbox_stat(const char* path, struct stat* st) {
    std::string p = compat::map_path(path ? path : "");
    struct _stat64 s{};
    int r = _stat64(p.c_str(), &s);
    if (r != 0) return r;
    st->st_dev = s.st_dev;
    st->st_ino = s.st_ino;
    st->st_mode = s.st_mode;
    st->st_nlink = s.st_nlink;
    st->st_uid = s.st_uid;
    st->st_gid = s.st_gid;
    st->st_rdev = s.st_rdev;
    st->st_size = (long long)s.st_size;
    st->st_atime = (time_t)s.st_atime;
    st->st_mtime = (time_t)s.st_mtime;
    st->st_ctime = (time_t)s.st_ctime;
    return 0;
}

DIR* rnsbox_opendir(const char* path) {
    std::string p = compat::map_path(path ? path : "");
    return opendir(p.c_str());
}

int rnsbox_mkdir(const char* path, int) {
    std::string p = compat::map_path(path ? path : "");
    return _mkdir(p.c_str());
}

int rnsbox_unlink(const char* path) {
    std::string p = compat::map_path(path ? path : "");
    return _unlink(p.c_str());
}

int rnsbox_rename(const char* from, const char* to) {
    std::string a = compat::map_path(from ? from : "");
    std::string b = compat::map_path(to ? to : "");
    // POSIX rename replaces an existing target; _rename does not.
    return MoveFileExA(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int rnsbox_chmod(const char* path, int mode) {
    std::string p = compat::map_path(path ? path : "");
    return _chmod(p.c_str(), mode);
}

int rnsbox_flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov{};
    if (op & 8 /* LOCK_UN */) return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
    DWORD flags = (op & 4 /* LOCK_NB */) ? LOCKFILE_FAIL_IMMEDIATELY : 0;
    if (op & 2 /* LOCK_EX */) flags |= LOCKFILE_EXCLUSIVE_LOCK;
    return LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
}

int rnsbox_fsync(int fd) { return _commit(fd); }

#endif  // _WIN32
