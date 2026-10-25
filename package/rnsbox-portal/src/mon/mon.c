// rnsbox-mon — one small daemon for the periodic jobs that used to be
// BusyBox shell loops (S46memwatch, S47memlog, S83rnsstatus) plus the BLE
// peers cache.
//
// Each shell loop kept ~0.4-1 MB RSS just by living (a BusyBox ash retains
// its heap after running pipelines every 30 s). On a 64 MB target that was
// ~2.2 MB of pure overhead for three timers. This process does the same work
// in ~150-250 KB and without spawning a process tree per sample: it reads
// /proc directly and only forks for an rnsd restart or an rnstatus refresh.
//
// Jobs:
//   * memwatch (every 30 s): restart rnsd when (MemTotal - MemAvailable) stays
//     at or above mem_threshold for two checks. 10 min cooldown, 2 min boot
//     grace, only when rnsd is running. Config: /etc/rnsbox/rnsdmon.conf.
//   * memlog (every interval, 30 s default): one metrics line per tick to
//     /var/log/memlog.log (or /var/lib/memlog.log on a read-only root), plus
//     EVENT lines for rnsd PID changes. Config: /etc/rnsbox/memlog.conf.
//   * CPU metric (every 5 s): publish {"pct":N,"ts":T} to /run/rnsbox-cpu
//     for the dashboard's "CPU load" (busy excludes idle/iowait).
//   * rnstatus warm (every 600 s, 10 s while the cache is still empty):
//     run `rnsbox-portal rnstatus-refresh` at nice 10, single-flight via
//     /run/rnsstatus.lock. The command is a stat() while the cache is warm,
//     or a query to the resident helper; only its first run at boot pays the
//     RNS import.
//   * BLE peers cache (every 60 s idle, every 15 s while a portal page is
//     polling /reticulum/bluetooth/peers): rebuild the connected-device list
//     with a 60 s grace period, so a transient disconnect does not make the
//     dashboard row blink.
//
// The on-disk log format is unchanged from S47memlog, so existing analysis
// keeps working.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_DIRECT   "/var/log/memlog.log"
#define LOG_FALLBACK "/var/lib/memlog.log"
#define RNSD_PIDFILE "/var/run/rnsd.pid"
#define RNSD_CONF    "/etc/rnsbox/rnsdmon.conf"
#define MEMLOG_CONF  "/etc/rnsbox/memlog.conf"
#define RNSTATUS_CACHE "/run/rnsbox-rnstatus-v2.json"
#define RNSTATUS_LOCK  "/run/rnsstatus.lock"
#define BT_PEERS_REQ   "/run/rnsbox-bt-peers.req"
#define CPU_FILE       "/run/rnsbox-cpu"
#define NOMADNET_CONF  "/etc/rnsbox/nomadnet.conf"
#define NOMADNET_PID   "/var/run/nomadnet.pid"
#define NOMADNET_REQ   "/run/rnsbox-nomadnet.req"
#define NOMADNET_TUI   "/run/rnsbox-nomadnet-tui"
#define NOMADNET_INIT  "/etc/init.d/S83nomadnet"

#define CPU_INTERVAL 5
#define NOMADNET_CHECK 30
#define MEMWATCH_INTERVAL 30
#define MEMWATCH_COOLDOWN 600
#define MEMWATCH_MIN_UPTIME 120
#define RNSTATUS_INTERVAL 600
#define RNSTATUS_FAST 60
#define RNSTATUS_RETRY 10
#define BT_PEERS_INTERVAL 60
#define BT_PEERS_FAST 15
#define RNODE_CONF     "/etc/rnsbox/rnode.conf"
#define RNODE_WATCH_INTERVAL 60

static volatile sig_atomic_t g_stop = 0;
static int g_log_fd = -1;
static char g_log_path[256];
static pid_t g_rn_pid = -1;
static pid_t g_bt_pid = -1;
static pid_t g_rw_pid = -1;
static long g_bt_last = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static long now_s(void) { return (long)time(NULL); }
static long mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static long read_uptime(void) {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) return 0;
    double d = 0;
    if (fscanf(f, "%lf", &d) != 1) d = 0;
    fclose(f);
    return (long)d;
}

// ---- tiny key=value reader (same semantics as the shell cfg()) -------------
static int cfg_get(const char* path, const char* key, char* out, size_t n) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p == '#' || !*p) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* k = p;
        char* ke = eq;
        while (ke > k && isspace((unsigned char)ke[-1])) --ke;
        if ((size_t)(ke - k) != strlen(key) || strncmp(k, key, ke - k) != 0)
            continue;
        char* v = eq + 1;
        while (*v && isspace((unsigned char)*v)) ++v;
        char* c = strchr(v, '#');
        if (c) *c = 0;
        char* ve = v + strlen(v);
        while (ve > v && isspace((unsigned char)ve[-1])) --ve;
        *ve = 0;
        snprintf(out, n, "%s", v);
        found = 1;              // last assignment wins
    }
    fclose(f);
    return found;
}

static int cfg_truthy(const char* path, const char* key, int dflt) {
    char v[32];
    if (!cfg_get(path, key, v, sizeof(v)) || !v[0]) return dflt;
    return !(strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 ||
             strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0);
}

static int cfg_int(const char* path, const char* key, int dflt) {
    char v[32];
    if (!cfg_get(path, key, v, sizeof(v)) || !v[0]) return dflt;
    char* end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return dflt;
    return (int)n;
}

// ---- memlog file -----------------------------------------------------------
static void log_open(void) {
    if (g_log_fd >= 0) return;
    int fd = open(LOG_DIRECT, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        snprintf(g_log_path, sizeof(g_log_path), "%s", LOG_DIRECT);
    } else {
        fd = open(LOG_FALLBACK, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd < 0) return;
        snprintf(g_log_path, sizeof(g_log_path), "%s", LOG_FALLBACK);
    }
    g_log_fd = fd;
}

static void log_rotate(void) {
    int max_kb = cfg_int(MEMLOG_CONF, "max_kb", 512);
    if (max_kb < 16) max_kb = 16;
    struct stat st;
    if (stat(g_log_path, &st) != 0) return;
    if (st.st_size <= (off_t)max_kb * 1024) return;
    if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
    char old[300];
    snprintf(old, sizeof(old), "%s.1", g_log_path);
    rename(g_log_path, old);
    log_open();
}

static void log_line(const char* fmt, ...) {
    log_open();
    if (g_log_fd < 0) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(g_log_fd, buf + off, (size_t)(n - off));
        if (w <= 0) break;
        off += w;
    }
}

// ---- /proc readers ---------------------------------------------------------
struct Mem {
    unsigned long total, free_kb, avail, anon, mapped, shmem;
    unsigned long slab, srec, sunrec, ptab, kstack, stotal, sfree;
};

static unsigned long meminfo_value(const char* line) {
    unsigned long v = 0;
    sscanf(line, "%*s %lu", &v);
    return v;
}

static void read_mem(struct Mem* m) {
    memset(m, 0, sizeof(*m));
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9))            m->total  = meminfo_value(line);
        else if (!strncmp(line, "MemFree:", 8))        m->free_kb = meminfo_value(line);
        else if (!strncmp(line, "MemAvailable:", 13))  m->avail  = meminfo_value(line);
        else if (!strncmp(line, "AnonPages:", 10))     m->anon   = meminfo_value(line);
        else if (!strncmp(line, "Mapped:", 7))         m->mapped = meminfo_value(line);
        else if (!strncmp(line, "Shmem:", 6))          m->shmem  = meminfo_value(line);
        else if (!strncmp(line, "Slab:", 5))           m->slab   = meminfo_value(line);
        else if (!strncmp(line, "SReclaimable:", 13))  m->srec   = meminfo_value(line);
        else if (!strncmp(line, "SUnreclaim:", 11))    m->sunrec = meminfo_value(line);
        else if (!strncmp(line, "PageTables:", 11))    m->ptab   = meminfo_value(line);
        else if (!strncmp(line, "KernelStack:", 12))   m->kstack = meminfo_value(line);
        else if (!strncmp(line, "SwapTotal:", 10))     m->stotal = meminfo_value(line);
        else if (!strncmp(line, "SwapFree:", 9))       m->sfree  = meminfo_value(line);
    }
    fclose(f);
}

static int mem_used_pct(const struct Mem* m) {
    if (!m->total) return 0;
    unsigned long used = m->avail <= m->total ? m->total - m->avail : 0;
    return (int)((used * 100) / m->total);
}

static int pid_alive(int pid) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/stat", pid);
    return access(p, F_OK) == 0;
}

static int find_rnsd_pid(void) {
    FILE* f = fopen(RNSD_PIDFILE, "r");
    if (f) {
        int pid = 0;
        if (fscanf(f, "%d", &pid) == 1 && pid > 0 && pid_alive(pid)) {
            fclose(f);
            return pid;
        }
        fclose(f);
    }
    DIR* d = opendir("/proc");
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[64], comm[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE* cf = fopen(path, "r");
        if (!cf) continue;
        if (fgets(comm, sizeof(comm), cf)) {
            comm[strcspn(comm, "\r\n")] = 0;
            if (strcmp(comm, "rnsd") == 0) {
                int pid = atoi(e->d_name);
                if (pid > 0) { found = pid; fclose(cf); break; }
            }
        }
        fclose(cf);
    }
    closedir(d);
    return found;
}

// rss is in kB; threads is 0 when unknown.
static void pid_mem(int pid, long* rss_kb, int* threads) {
    *rss_kb = 0;
    *threads = 0;
    if (pid <= 0) return;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    FILE* f = fopen(path, "r");
    if (f) {
        unsigned long size = 0, resident = 0;
        if (fscanf(f, "%lu %lu", &size, &resident) == 2)
            *rss_kb = (long)resident * (sysconf(_SC_PAGESIZE) / 1024);
        fclose(f);
    }
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "Threads:", 8)) {
                *threads = atoi(line + 8);
                break;
            }
        }
        fclose(f);
    }
}

static long zram_used_kb(void) {
    FILE* f = fopen("/proc/swaps", "r");
    if (!f) return 0;
    char line[256];
    long used = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "zram0")) {
            char dev[64], type[64];
            long size = 0, u = 0;
            if (sscanf(line, "%63s %63s %ld %ld", dev, type, &size, &u) == 4)
                used = u;
            break;
        }
    }
    fclose(f);
    return used;
}

static void read_load(char* out, size_t n) {
    FILE* f = fopen("/proc/loadavg", "r");
    if (!f) { snprintf(out, n, "0/0/0"); return; }
    char a[32] = "0", b[32] = "0", c[32] = "0";
    if (fscanf(f, "%31s %31s %31s", a, b, c) != 3) { a[0] = b[0] = c[0] = '0'; }
    fclose(f);
    snprintf(out, n, "%s/%s/%s", a, b, c);
}

struct CpuSample { unsigned long long total, idle; };
static struct CpuSample g_prev_cpu;

static struct CpuSample read_cpu(void) {
    struct CpuSample s = {0, 0};
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return s;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        unsigned long long v[8] = {0};
        if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
            for (int i = 0; i < 8; ++i) s.total += v[i];
            s.idle = v[3] + v[4];
        }
    }
    fclose(f);
    return s;
}

static int cpu_pct(struct CpuSample cur) {
    int pct = 0;
    if (g_prev_cpu.total && cur.total > g_prev_cpu.total) {
        unsigned long long dt = cur.total - g_prev_cpu.total;
        unsigned long long di = cur.idle >= g_prev_cpu.idle ? cur.idle - g_prev_cpu.idle : 0;
        if (di > dt) di = dt;
        pct = (int)((100ULL * (dt - di)) / dt);
    }
    g_prev_cpu = cur;
    return pct;
}

// Dashboard CPU metric. The kernel on this board does not advance
// /proc/stat's idle counter (it stays frozen since boot), so the usual
// (total-idle)/total formula reads 100% forever. Sum the per-process
// utime+stime deltas instead: that is actual work, and it never lies.
static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned long long proc_cpu_ticks(void) {
    unsigned long long sum = 0;
    DIR* d = opendir("/proc");
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        if (fgets(line, sizeof(line), f)) {
            char* r = strrchr(line, ')');
            if (r) {
                unsigned long long u = 0, s = 0;
                if (sscanf(r + 2,
                           "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                           &u, &s) == 2)
                    sum += u + s;
            }
        }
        fclose(f);
    }
    closedir(d);
    return sum;
}

static unsigned long long g_prev_proc5;
static long g_prev_mono5;

static void sample_cpu5(void) {
    unsigned long long cur = proc_cpu_ticks();
    long now = mono_ms();
    if (g_prev_proc5 && cur >= g_prev_proc5 && now > g_prev_mono5) {
        unsigned long long dt = cur - g_prev_proc5;
        long ticks = sysconf(_SC_CLK_TCK);
        if (ticks < 1) ticks = 100;
        unsigned long long elapsed =
            (unsigned long long)(now - g_prev_mono5) * (unsigned long long)ticks / 1000;
        int pct = elapsed ? (int)((100ULL * dt) / elapsed) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        char out[96], tmp[64];
        snprintf(out, sizeof(out), "{\"pct\":%d,\"ts\":%ld}\n", pct, now_s());
        snprintf(tmp, sizeof(tmp), CPU_FILE ".tmp");
        FILE* f = fopen(tmp, "w");
        if (f) {
            fputs(out, f);
            fclose(f);
            if (rename(tmp, CPU_FILE) != 0) remove(tmp);
        }
    }
    g_prev_proc5 = cur;
    g_prev_mono5 = now;
}

struct TopProc { long rss; char name[32]; };

static void top_procs(struct TopProc top[4]) {
    for (int i = 0; i < 4; ++i) { top[i].rss = 0; top[i].name[0] = 0; }
    DIR* d = opendir("/proc");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[64];
        long rss = 0;
        snprintf(path, sizeof(path), "/proc/%s/statm", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        unsigned long size = 0, resident = 0;
        if (fscanf(f, "%lu %lu", &size, &resident) == 2)
            rss = (long)resident * (sysconf(_SC_PAGESIZE) / 1024);
        fclose(f);
        if (rss <= 0) continue;
        char name[32] = "?";
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fgets(name, sizeof(name), f)) name[strcspn(name, "\r\n")] = 0;
            fclose(f);
        }
        for (int i = 0; i < 4; ++i) {
            if (rss > top[i].rss) {
                for (int j = 3; j > i; --j) top[j] = top[j - 1];
                top[i].rss = rss;
                snprintf(top[i].name, sizeof(top[i].name), "%s", name);
                break;
            }
        }
    }
    closedir(d);
}

// ---- NomadNet idle stop ---------------------------------------------------
// The portal's NomadNet page / attached TUI touch NOMADNET_REQ every few
// seconds. Without it, an enabled daemon is stopped after idle_stop seconds
// (config), so it does not burn CPU while nobody uses NomadNet. The portal
// starts the TUI on demand; /run/rnsbox-nomadnet-tui means the TUI currently
// owns the same config dir, so never start or stop the daemon from here then.
static long g_nomad_last_stop = -99999;

static int nomadnet_running(void) {
    FILE* f = fopen(NOMADNET_PID, "r");
    if (!f) return 0;
    long pid = 0;
    int n = fscanf(f, "%ld", &pid);
    fclose(f);
    return n == 1 && pid > 0 && kill((pid_t)pid, 0) == 0;
}

static void touch_nomadnet_req(void) {
    int fd = open(NOMADNET_REQ, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
    utimensat(AT_FDCWD, NOMADNET_REQ, NULL, 0);
}

static void stop_nomadnet_daemon(void) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        pid_t inner = fork();
        if (inner != 0) _exit(0);
        setpriority(PRIO_PROCESS, 0, 10);
        (void)chdir("/");
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        execl(NOMADNET_INIT, "S83nomadnet", "stop", (char*)NULL);
        _exit(127);
    }
    int st = 0;
    (void)waitpid(pid, &st, 0);   // reap the short-lived intermediate child
}

static void maybe_stop_idle_nomadnet(void) {
    if (!cfg_truthy(NOMADNET_CONF, "enabled", 0)) return;
    if (access(NOMADNET_TUI, F_OK) == 0) {
        // The TUI owns the config, but a broker crash can leave the flag
        // behind. Clear it once the console broker itself is gone.
        FILE* bf = fopen("/run/rnsbox-console.pid", "r");
        long bpid = 0;
        int alive = 0;
        if (bf) {
            if (fscanf(bf, "%ld", &bpid) == 1) alive = kill((pid_t)bpid, 0) == 0;
            fclose(bf);
        }
        if (alive) return;
        unlink(NOMADNET_TUI);
    }
    int idle = cfg_int(NOMADNET_CONF, "idle_stop", 600);
    if (idle <= 0) return;

    struct stat st;
    long age = -1;
    if (stat(NOMADNET_REQ, &st) == 0) age = now_s() - (long)st.st_mtime;
    if (age < 0) {
        // No request file yet (fresh boot) or the clock moved backwards:
        // establish a fresh baseline and let the timeout run from here.
        touch_nomadnet_req();
        return;
    }
    if (age < idle) return;
    if (!nomadnet_running()) return;
    long mono = mono_s();
    if (mono - g_nomad_last_stop < 120) return;
    g_nomad_last_stop = mono;
    syslog(LOG_NOTICE, "nomadnet idle for %lds - stopping daemon", age);
    stop_nomadnet_daemon();
}

// ---- jobs ------------------------------------------------------------------
static void usage_event(long up, long avail, long zram) {
    log_line("EVENT memlog_start log=%s interval=%ds\n", g_log_path,
             cfg_int(MEMLOG_CONF, "interval", 30));
    (void)up; (void)avail; (void)zram;
}

static void sample_log(void) {
    struct Mem m;
    read_mem(&m);
    int pid = find_rnsd_pid();
    long rss = 0;
    int thr = 0;
    pid_mem(pid, &rss, &thr);
    char load[96];
    read_load(load, sizeof(load));
    int cpu = cpu_pct(read_cpu());
    struct TopProc top[4];
    top_procs(top);
    char tops[256];
    int off = 0;
    for (int i = 0; i < 4; ++i) {
        if (!top[i].name[0]) continue;
        off += snprintf(tops + off, sizeof(tops) - (size_t)off, "%s%s:%ld",
                        off ? "," : "", top[i].name, top[i].rss);
        if ((size_t)off >= sizeof(tops)) break;
    }
    unsigned long sused = m.stotal >= m.sfree ? m.stotal - m.sfree : 0;
    long zram = zram_used_kb();
    long up = read_uptime();
    char pids[16];
    if (pid > 0) snprintf(pids, sizeof(pids), "%d", pid);
    else snprintf(pids, sizeof(pids), "none");
    char now[32] = "1970-01-01T00:00:00Z";
    time_t t = time(NULL);
    struct tm g;
    if (gmtime_r(&t, &g)) strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &g);

    log_line("%s up=%lds mem_total=%lukb free=%lukb avail=%lukb anon=%lukb "
             "mapped=%lukb shmem=%lukb slab=%lukb srec=%lukb sunrec=%lukb "
             "ptab=%lukb kstack=%lukb swap_total=%lukb swap_used=%lukb "
             "zram_used=%ld rnsd_pid=%s rnsd_rss=%ldkb rnsd_thr=%d "
             "load=%s cpu=%d%% top=%s\n",
             now, up, m.total, m.free_kb, m.avail, m.anon, m.mapped, m.shmem,
             m.slab, m.srec, m.sunrec, m.ptab, m.kstack, m.stotal, sused,
             zram, pids, rss, thr, load, cpu, tops);
    // Keep the file bounded even if the caller never rotates.
    log_rotate();
    (void)pid;
}

static void restart_rnsd(int used, int threshold) {
    syslog(LOG_NOTICE, "RAM %d%% >= %d%% - restarting rnsd", used, threshold);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        execl("/bin/sh", "sh", "/etc/init.d/S82rnsd", "restart", (char*)NULL);
        _exit(127);
    }
    if (pid < 0) return;
    // The restart script is quick on this box, but bound it so a stuck init
    // cannot wedge the monitor.
    int status = 0;
    for (int i = 0; i < 120; ++i) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (i == 119) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
        sleep(1);
    }
    struct Mem m;
    read_mem(&m);
    syslog(LOG_NOTICE, "rnsd restarted; RAM now %d%%", mem_used_pct(&m));
}

static int start_bt_refresh(void) {
    if (g_bt_pid > 0) {
        int st = 0;
        pid_t r = waitpid(g_bt_pid, &st, WNOHANG);
        if (r == 0) return 0;               // previous poll still running
        g_bt_pid = -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        setpriority(PRIO_PROCESS, 0, 10);
        (void)chdir("/");
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        execl("/usr/sbin/rnsbox-portal", "rnsbox-portal", "bt-peers-refresh",
              (char*)NULL);
        _exit(127);
    }
    if (pid <= 0) return 0;
    g_bt_pid = pid;
    return 1;
}

// start_rnode_watch — free a BLE RNode stuck behind a stale BlueZ connection
// (bluetoothctl says Connected: yes, the peer stops advertising, and rnsd's
// scan-only discovery never finds it). Only spawned when rnode.conf really
// points at a BLE peer; the check itself is a short-lived process.
static int rnode_ble_configured(void) {
    char tr[16], addr[64];
    if (!cfg_get(RNODE_CONF, "transport", tr, sizeof(tr))) return 0;
    if (strcasecmp(tr, "ble") != 0) return 0;
    if (!cfg_get(RNODE_CONF, "ble_address", addr, sizeof(addr))) return 0;
    return addr[0] != 0;
}

static int start_rnode_watch(void) {
    if (g_rw_pid > 0) {
        int st = 0;
        pid_t r = waitpid(g_rw_pid, &st, WNOHANG);
        if (r == 0) return 0;               // previous check still running
        g_rw_pid = -1;
    }
    if (!rnode_ble_configured()) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        setpriority(PRIO_PROCESS, 0, 10);
        (void)chdir("/");
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        execl("/usr/sbin/rnsbox-portal", "rnsbox-portal", "rnode-watch",
              (char*)NULL);
        _exit(127);
    }
    if (pid <= 0) return 0;
    g_rw_pid = pid;
    return 1;
}

static void start_refresh(void) {
    if (g_rn_pid > 0) {
        int st = 0;
        pid_t r = waitpid(g_rn_pid, &st, WNOHANG);
        if (r == 0) return;                 // still running: skip this tick
        g_rn_pid = -1;
        rmdir(RNSTATUS_LOCK);
    }
    if (mkdir(RNSTATUS_LOCK, 0700) != 0) {
        struct stat st;
        // A killed refresh can leave the lock behind; expire it after 3 min.
        if (stat(RNSTATUS_LOCK, &st) == 0 && now_s() - (long)st.st_mtime > 180)
            rmdir(RNSTATUS_LOCK);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        setpriority(PRIO_PROCESS, 0, 10);
        (void)chdir("/");
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
            if (nul > 2) close(nul);
        }
        execl("/usr/sbin/rnsbox-portal", "rnsbox-portal", "rnstatus-refresh",
              (char*)NULL);
        _exit(127);
    }
    if (pid < 0) { rmdir(RNSTATUS_LOCK); return; }
    g_rn_pid = pid;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    openlog("rnsbox-mon", LOG_PID, LOG_DAEMON);

    log_open();
    if (g_log_fd < 0)
        syslog(LOG_WARNING, "cannot open memlog (%s or %s)", LOG_DIRECT, LOG_FALLBACK);
    else
        usage_event(read_uptime(), 0, 0);

    int high = 0;
    long last_restart = -99999;
    char last_pid[16] = "";
    long next_watch = mono_s() + MEMWATCH_INTERVAL;
    long next_log = mono_s() + 1;          // first sample right away
    long next_rn = mono_s() + 5;
    long next_bt = mono_s() + 2;
    long next_rw = mono_s() + 20;
    long next_cpu = mono_s();              // baseline now, publish after 5 s
    long next_nomad = mono_s() + 5;

    while (!g_stop) {
        long now = mono_s();

        if (now >= next_cpu) {
            next_cpu = now + CPU_INTERVAL;
            sample_cpu5();
        }

        if (now >= next_nomad) {
            next_nomad = now + NOMADNET_CHECK;
            maybe_stop_idle_nomadnet();
        }

        // The rnstatus warm-up can now finish in milliseconds (resident
        // helper), so reap it here instead of leaving a zombie until the
        // next 60 s tick. The same applies to the BT peers refresher.
        if (g_rn_pid > 0 && waitpid(g_rn_pid, NULL, WNOHANG) == g_rn_pid) {
            g_rn_pid = -1;
            rmdir(RNSTATUS_LOCK);
        }
        if (g_bt_pid > 0 && waitpid(g_bt_pid, NULL, WNOHANG) == g_bt_pid)
            g_bt_pid = -1;

        if (now >= next_watch) {
            next_watch = now + MEMWATCH_INTERVAL;
            if (cfg_truthy(RNSD_CONF, "mem_enabled", 1)) {
                int th = cfg_int(RNSD_CONF, "mem_threshold", 90);
                if (th < 50) th = 50;
                if (th > 95) th = 95;
                struct Mem m;
                read_mem(&m);
                int used = mem_used_pct(&m);
                int pid = find_rnsd_pid();
                long up = read_uptime();
                if (up >= MEMWATCH_MIN_UPTIME && pid > 0 && used >= th) {
                    if (++high >= 2) {
                        high = 0;
                        if (up - last_restart >= MEMWATCH_COOLDOWN) {
                            restart_rnsd(used, th);
                            last_restart = read_uptime();
                            next_watch = mono_s() + MEMWATCH_COOLDOWN;
                        }
                    }
                } else {
                    high = 0;
                }
            } else {
                high = 0;
            }
        }

        if (now >= next_log) {
            int interval = cfg_int(MEMLOG_CONF, "interval", 30);
            if (interval < 5) interval = 5;
            if (interval > 3600) interval = 3600;
            next_log = now + interval;
            if (cfg_truthy(MEMLOG_CONF, "enabled", 1)) {
                int pid = find_rnsd_pid();
                char cur[16];
                if (pid > 0) snprintf(cur, sizeof(cur), "%d", pid);
                else snprintf(cur, sizeof(cur), "none");
                sample_log();
                if (last_pid[0] && strcmp(last_pid, "none") != 0 &&
                    strcmp(cur, last_pid) != 0) {
                    struct Mem m;
                    read_mem(&m);
                    log_line("EVENT rnsd_restart %s->%s up=%lds mem_avail=%lukb zram_used=%ld\n",
                             last_pid, cur, read_uptime(), m.avail, zram_used_kb());
                    syslog(LOG_NOTICE, "rnsd restarted %s->%s", last_pid, cur);
                }
                snprintf(last_pid, sizeof(last_pid), "%s", cur);
            }
        }

        if (now >= next_rn) {
            struct stat st;
            int warm = stat(RNSTATUS_CACHE, &st) == 0 && st.st_size > 0;
            // The portal's rnstatus endpoint touches BT_PEERS_REQ every 5 s
            // while a page is open. Fresh while open: 60 s. Idle: 10 min.
            int page_open = stat(BT_PEERS_REQ, &st) == 0 &&
                            now_s() - (long)st.st_mtime <= 30;
            // A configured BLE RNode needs a reasonably fresh snapshot: the
            // rnode-watch recovery refuses to act on a stale one, and after an
            // rnsd restart the old "Up" would otherwise hide a dead link.
            int idle = rnode_ble_configured() ? RNODE_WATCH_INTERVAL
                                              : RNSTATUS_INTERVAL;
            next_rn = now + (warm ? (page_open ? RNSTATUS_FAST : idle)
                                  : RNSTATUS_RETRY);
            start_refresh();
        }

        {
            struct stat st;
            int fast = stat(BT_PEERS_REQ, &st) == 0 &&
                       now_s() - (long)st.st_mtime <= 15;
            if (now >= next_bt) {
                next_bt = now + (fast ? BT_PEERS_FAST : BT_PEERS_INTERVAL);
                if (start_bt_refresh()) g_bt_last = now;
            } else if (fast && now - g_bt_last >= BT_PEERS_FAST) {
                // A portal page just opened: tighten the cadence without
                // waiting for the idle 30 s timer to elapse.
                if (start_bt_refresh()) {
                    g_bt_last = now;
                    next_bt = now + BT_PEERS_FAST;
                }
            }
        }

        if (now >= next_rw) {
            next_rw = now + RNODE_WATCH_INTERVAL;
            start_rnode_watch();
        }

        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    if (g_rn_pid > 0) {
        kill(g_rn_pid, SIGTERM);
        waitpid(g_rn_pid, NULL, 0);
    }
    if (g_bt_pid > 0) {
        kill(g_bt_pid, SIGTERM);
        waitpid(g_bt_pid, NULL, 0);
    }
    if (g_rw_pid > 0) {
        kill(g_rw_pid, SIGTERM);
        waitpid(g_rw_pid, NULL, 0);
    }
    rmdir(RNSTATUS_LOCK);
    if (g_log_fd >= 0) close(g_log_fd);
    syslog(LOG_NOTICE, "stopped");
    closelog();
    return 0;
}
