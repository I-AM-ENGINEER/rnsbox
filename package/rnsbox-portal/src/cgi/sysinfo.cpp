// sysinfo.cpp — see sysinfo.h. Port of sysinfo.py.
#include "sysinfo.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

// Split on ASCII whitespace into tokens.
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && isspace((unsigned char)s[i])) ++i;
        size_t j = i;
        while (j < n && !isspace((unsigned char)s[j])) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) j = n;
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

std::string slurp(const std::string& p) { return util::read_file(p, 1 << 20); }
std::string slurp_trim(const std::string& p) { return util::trim(util::read_file(p, 65536)); }

bool is_dir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

// One `ip` invocation; empty on failure.
std::string run_ip(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"/sbin/ip"};
    argv.insert(argv.end(), args.begin(), args.end());
    util::RunResult r = util::run(argv, "", 3);
    return r.exit_code == 0 ? r.out : std::string();
}

std::string human_bytes(const std::string& raw) {
    if (raw.empty()) return raw;
    char* end = nullptr;
    double n = strtod(raw.c_str(), &end);
    if (end == raw.c_str()) return raw;   // non-numeric: pass through
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    char buf[48];
    for (int u = 0; u < 5; ++u) {
        if (n < 1024.0) {
            if (u == 0) snprintf(buf, sizeof(buf), "%ld B", (long)n);
            else        snprintf(buf, sizeof(buf), "%.1f %s", n, units[u]);
            return buf;
        }
        n /= 1024.0;
    }
    snprintf(buf, sizeof(buf), "%.1f PiB", n);
    return buf;
}

long meminfo_kb(const std::string& mi, const char* key) {
    size_t p = mi.find(key);
    if (p == std::string::npos) return -1;
    return strtol(mi.c_str() + p + strlen(key), nullptr, 10);
}

}  // namespace

namespace sysinfo {

std::string hostname() {
    std::string h = slurp_trim("/proc/sys/kernel/hostname");
    return h.empty() ? std::string("(unknown)") : h;
}

Summary summary() {
    Summary s;
    // uptime
    std::string up = slurp("/proc/uptime");
    if (!up.empty()) {
        long secs = (long)strtod(up.c_str(), nullptr);
        long d = secs / 86400; secs %= 86400;
        long h = secs / 3600;  secs %= 3600;
        long m = secs / 60;
        char buf[64]; snprintf(buf, sizeof(buf), "%ldd %ldh %ldm", d, h, m);
        s.uptime = buf;
    }
    // load
    auto la = split_ws(slurp("/proc/loadavg"));
    for (int i = 0; i < 3 && i < (int)la.size(); ++i) s.load[i] = la[i];
    // mem
    std::string mi = slurp("/proc/meminfo");
    long total = meminfo_kb(mi, "MemTotal:");
    long avail = meminfo_kb(mi, "MemAvailable:");
    long mfree = meminfo_kb(mi, "MemFree:");
    long buffers = meminfo_kb(mi, "Buffers:");
    long cached = meminfo_kb(mi, "Cached:");
    if (avail < 0) avail = mfree;
    if (total > 0 && avail >= 0) {
        long used = total - avail;
        s.mem.ok = true;
        s.mem.total_mb = (total + 512) / 1024;
        s.mem.avail_mb = (avail + 512) / 1024;
        s.mem.used_mb = (used + 512) / 1024;
        long bc = (buffers > 0 ? buffers : 0) + (cached > 0 ? cached : 0);
        s.mem.buffcache_mb = (bc + 512) / 1024;
        s.mem.used_pct = (double)((int)(1000.0 * used / total + 0.5)) / 10.0;
    }
    s.kernel = slurp_trim("/proc/sys/kernel/osrelease");
    return s;
}

Network network_summary() {
    Network net;
    // addr map from one `ip -o -4 addr show`
    // line: "2: eth0    inet 192.168.3.83/24 brd ..."
    std::vector<std::pair<std::string, std::vector<std::string>>> addrs;  // preserve; small
    auto find_addrs = [&](const std::string& dev) -> std::vector<std::string>* {
        for (auto& kv : addrs) if (kv.first == dev) return &kv.second;
        return nullptr;
    };
    for (const auto& ln : split_lines(run_ip({"-o", "-4", "addr", "show"}))) {
        auto p = split_ws(ln);
        if (p.size() >= 4 && p[2] == "inet") {
            auto* v = find_addrs(p[1]);
            if (!v) { addrs.push_back({p[1], {}}); v = &addrs.back().second; }
            v->push_back(p[3]);
        }
    }
    // interfaces from /sys/class/net (skip lo), sorted
    std::vector<std::string> devs;
    if (DIR* d = opendir("/sys/class/net")) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == ".." || n == "lo") continue;
            devs.push_back(n);
        }
        closedir(d);
    }
    std::sort(devs.begin(), devs.end());
    for (const auto& dev : devs) {
        std::string base = "/sys/class/net/" + dev;
        std::string state = slurp_trim(base + "/operstate");
        if (state == "unknown") {
            long iff = strtol(slurp_trim(base + "/flags").c_str(), nullptr, 16);
            if ((iff & 0x1) && slurp_trim(base + "/carrier") == "1") state = "up";
        }
        Iface i;
        i.name = dev;
        i.state = state;
        i.mac = slurp_trim(base + "/address");
        i.mtu = slurp_trim(base + "/mtu");
        i.rx = human_bytes(slurp_trim(base + "/statistics/rx_bytes"));
        i.tx = human_bytes(slurp_trim(base + "/statistics/tx_bytes"));
        if (auto* v = find_addrs(dev)) i.addrs = *v;
        net.ifaces.push_back(std::move(i));
    }
    // default gateway
    for (const auto& ln : split_lines(run_ip({"-4", "route", "show", "default"}))) {
        auto p = split_ws(ln);
        if (p.size() >= 5 && p[0] == "default") { net.default_gateway = p[2]; break; }
    }
    // dns
    for (const auto& ln : split_lines(slurp("/etc/resolv.conf"))) {
        auto t = util::trim(ln);
        if (t.rfind("nameserver ", 0) == 0) {
            auto p = split_ws(t);
            if (p.size() >= 2) net.dns.push_back(p[1]);
        }
    }
    return net;
}

static std::string find_rnsd_pid() {
    // 1. pidfile (authoritative — S82rnsd writes /var/run/rnsd.pid).
    std::string pid = slurp_trim("/var/run/rnsd.pid");
    if (!pid.empty() && exists("/proc/" + pid)) return pid;
    // 2. /proc cmdline scan (rnsd's comm is "python3", so `pidof rnsd` fails).
    if (DIR* d = opendir("/proc")) {
        std::string found;
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.empty() || !isdigit((unsigned char)n[0])) continue;
            bool alldig = true;
            for (char c : n) if (!isdigit((unsigned char)c)) { alldig = false; break; }
            if (!alldig) continue;
            std::string cl = slurp("/proc/" + n + "/cmdline");
            for (char& c : cl) if (c == '\0') c = ' ';
            std::string t = util::trim(cl);
            bool ends_rnsd = t.size() >= 4 && t.compare(t.size() - 4, 4, "rnsd") == 0;
            if (t.find("/rnsd") != std::string::npos || t.find(" rnsd ") != std::string::npos || ends_rnsd) {
                found = n; break;
            }
        }
        closedir(d);
        if (!found.empty()) return found;
    }
    return "";
}

Rnsd rnsd_status(bool with_raw) {
    Rnsd r;
    std::string pid = find_rnsd_pid();
    if (!pid.empty()) {
        r.running = true;
        r.pid = pid;
        std::string st = slurp("/proc/" + pid + "/status");
        size_t p = st.find("VmRSS:");
        if (p != std::string::npos) r.rss_mb = strtol(st.c_str() + p + 6, nullptr, 10) / 1024;
        if (with_raw) {
            util::RunResult rr = util::run({"/usr/bin/rnstatus", "-a"}, "", 20);
            r.raw = rr.out.empty() ? std::string("(rnstatus not available)") : rr.out;
        }
    }
    return r;
}

std::vector<Lease> dnsmasq_leases() {
    std::vector<Lease> out;
    for (const char* path : {"/var/lib/misc/dnsmasq.leases", "/tmp/dnsmasq.leases"}) {
        std::string data = slurp(path);
        if (data.empty()) continue;
        for (const auto& ln : split_lines(data)) {
            auto p = split_ws(ln);
            if (p.size() >= 5) {
                Lease l;
                l.expires = p[0]; l.mac = p[1]; l.ip = p[2];
                l.name = (p[3] == "*") ? "(no name)" : p[3];
                l.client_id = p[4];
                out.push_back(std::move(l));
            }
        }
        break;
    }
    return out;
}

Slip slip_status() {
    Slip s;
    if (!is_dir("/sys/class/net/sl0")) return s;
    s.present = true;
    std::string st = slurp_trim("/sys/class/net/sl0/operstate");
    s.up = !(st == "down" || st.empty());
    return s;
}

}  // namespace sysinfo
