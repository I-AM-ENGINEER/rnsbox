// compat.h — the POSIX surface the portal uses, implemented on Windows.
//
// The portal's code (12k lines, shared with the Linux CGI build) assumes a
// Linux tree: /etc/rnsbox, /etc/reticulum, /var/lib/rnsbox, /usr/share/...
// Rather than rewrite every path in every route, the Windows build keeps ONE
// root directory (%ProgramData%\RNSBox by default, --root / RNSBOX_ROOT to
// override) and maps the absolute paths into it at the syscall boundary:
//
//   /etc/rnsbox/wan.conf  ->  <root>/etc/rnsbox/wan.conf
//
// Everything that only exists on a router (/proc, /sys, /dev, /run) maps to a
// path under the root that does not exist by default, so those reads fail
// quietly and the UI degrades exactly as it does on a board without the
// hardware. /dev/null maps to the Windows NUL device.
//
// This header is force-included (-include) by the Windows build, before the
// sources' own includes, so <winsock2.h> wins over <windows.h> ordering.
#ifndef RNSBOX_WIN_COMPAT_H
#define RNSBOX_WIN_COMPAT_H

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdarg>
#include <cstddef>
#include <string>
#include <vector>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
// flock(2) operation bits (the shim in compat.cpp interprets these).
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif

namespace compat {

// The portal's filesystem root, e.g. "C:\\ProgramData\\RNSBox".
std::string root();
void set_root(const std::string& r);

// Map one of the portal's absolute Linux paths into root(). Windows paths and
// relative names pass through unchanged.
std::string map_path(const std::string& p);

// Resolve a command the Linux build names by absolute path
// ("/usr/bin/rnstatus", "/usr/bin/python3") to something runnable here.
// Empty when it cannot be found.
std::string find_tool(const std::string& argv0);

// Interpreter that has Reticulum importable, or empty. RNSBOX_PYTHON wins.
std::string find_python();
// True if `python` can `import RNS`; on success version receives RNS.__version__.
bool rns_available(const std::string& python, std::string& version);

// Result of a child process: stdout+stderr merged, bounded.
struct ProcResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;
};

// Run argv[0] (resolved with find_tool) with argv, feeding `input` on stdin,
// killing it after timeout_s. No shell.
ProcResult run_process(const std::vector<std::string>& argv, const std::string& input,
                       int timeout_s, size_t out_cap);

// n cryptographically random bytes (empty on failure).
std::string random_bytes(size_t n);

// UTF-8 -> UTF-16 (for CreateProcessW and Win32 APIs).
std::wstring wide(const std::string& s);
// UTF-16 -> UTF-8.
std::string narrow(const std::wstring& s);

}  // namespace compat

// --- POSIX shims (declarations already in the CRT; macros below redirect) ---
int   rnsbox_open(const char* path, int flags, ...);
int   rnsbox_access(const char* path, int mode);
int   rnsbox_stat(const char* path, struct stat* st);
DIR*  rnsbox_opendir(const char* path);
int   rnsbox_mkdir(const char* path, int mode);
int   rnsbox_unlink(const char* path);
int   rnsbox_rename(const char* from, const char* to);
int   rnsbox_chmod(const char* path, int mode);
int   rnsbox_flock(int fd, int op);
int   rnsbox_fsync(int fd);

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

// Function-like macros, so `struct stat` and member names are untouched and
// `::stat(...)`, `::open(...)` (with the leading scope) still expand.
#define open(...)      rnsbox_open(__VA_ARGS__)
#define access(p, m)   rnsbox_access((p), (m))
#define stat(p, b)     rnsbox_stat((p), (b))
#define opendir(p)     rnsbox_opendir(p)
#define mkdir(p, m)    rnsbox_mkdir((p), (m))
#define unlink(p)      rnsbox_unlink(p)
#define rename(a, b)   rnsbox_rename((a), (b))
#define chmod(p, m)    rnsbox_chmod((p), (m))
#define flock(fd, op)  rnsbox_flock((fd), (op))
#define fsync(fd)      rnsbox_fsync(fd)

#endif  // _WIN32
#endif  // RNSBOX_WIN_COMPAT_H
