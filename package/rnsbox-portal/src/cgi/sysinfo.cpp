// sysinfo.cpp — see sysinfo.h. Port of sysinfo.py.
#include "sysinfo.h"
#include "util.h"
#include <algorithm>
#include <map>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

// A strictly numeric sysfs value (optional leading '-'); false for anything
// else, including an empty read (sysfs show() returned an error).
bool parse_ll(const std::string& s, long long& out) {
    if (s.empty() || s.size() > 19) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i == s.size()) return false;
    for (size_t k = i; k < s.size(); ++k) if (!isdigit((unsigned char)s[k])) return false;
    out = strtoll(s.c_str(), nullptr, 10);
    return true;
}

// One read of a small sysfs attribute, trimmed; err = the errno of a failed
// open/read, 0 on success. (read_file() can't tell an error from an empty
// value, and the RTC attributes report the driver's verdict as the errno.)
std::string sysfs_read(const std::string& p, int& err) {
    err = 0;
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { err = errno; return std::string(); }
    char buf[128];
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) err = errno;
    ::close(fd);
    return n > 0 ? util::trim(std::string(buf, (size_t)n)) : std::string();
}

// Canonical path (symlinks resolved); empty if it doesn't resolve.
std::string real_path(const std::string& p) {
    char* r = ::realpath(p.c_str(), nullptr);
    if (!r) return std::string();
    std::string s(r);
    free(r);
    return s;
}

}  // namespace

namespace sysinfo {

bool ifname_ok(const std::string& n) {
    if (n.empty() || n.size() > 15 || n == "." || n == "..") return false;
    for (char c : n) if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

std::string bridge_of(const std::string& iface) {
    if (!ifname_ok(iface)) return "";
    std::string m = real_path("/sys/class/net/" + iface + "/brport/bridge");
    return m.empty() ? std::string() : m.substr(m.rfind('/') + 1);
}

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
        i.bridge = is_dir(base + "/bridge");      // br-lan
        i.master = bridge_of(dev);                // usb0 / wlan1 -> br-lan
        net.ifaces.push_back(std::move(i));
    }
    // The default route in use: the lowest-metric IPv4 default route of the
    // main table whose iface has carrier. An unplugged eth0 keeps its lease's
    // route, which the kernel skips (S30eth: ignore_routes_with_linkdown), so
    // with WAN=auto the WiFi client's route is the live one. /proc/net/route
    // prints each __be32 as a host-order integer: read back into s_addr it is
    // the address again, on any endianness.
    // gateway_of keeps each iface's own (lowest-metric) default gateway, so a
    // pinned WAN card shows its iface's gateway even while another is in use.
    {
        long best = -1;
        std::map<std::string, long> dev_best;
        bool first = true;
        for (const auto& ln : split_lines(slurp("/proc/net/route"))) {
            auto p = split_ws(ln);
            if (first) { first = false; continue; }   // header
            if (p.size() < 8 || p[1] != "00000000" || p[7] != "00000000") continue;
            unsigned long flags = strtoul(p[3].c_str(), nullptr, 16);
            if (!(flags & 0x1) || !ifname_ok(p[0])) continue;   // RTF_UP
            long metric = strtol(p[6].c_str(), nullptr, 10);
            std::string gw;
            if (flags & 0x2) {                                  // RTF_GATEWAY
                struct in_addr a;
                a.s_addr = (in_addr_t)strtoul(p[2].c_str(), nullptr, 16);
                char buf[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &a, buf, sizeof buf)) gw = buf;
            }
            auto it = dev_best.find(p[0]);
            if (it == dev_best.end() || metric < it->second) {
                dev_best[p[0]] = metric;
                net.gateway_of[p[0]] = gw;
            }
            if (slurp_trim("/sys/class/net/" + p[0] + "/carrier") != "1") continue;
            if (best >= 0 && metric >= best) continue;
            best = metric;
            net.default_dev = p[0];
            net.default_gateway = gw;
        }
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

std::string wan_display_iface(const std::string& wan_setting, const Network& net) {
    if (wan_setting != "auto") return wan_setting;
    if (net.default_dev == "eth0" || net.default_dev == "wlan0") return net.default_dev;
    return "eth0";
}

std::string wan_gateway(const std::string& wan_setting, const Network& net) {
    if (wan_setting == "auto") return net.default_gateway;
    auto it = net.gateway_of.find(wan_setting);
    return it == net.gateway_of.end() ? std::string() : it->second;
}

std::string wan_label(const std::string& wan_setting, const Network& net) {
    if (wan_setting != "auto") return wan_setting;
    if (net.default_dev == "eth0" || net.default_dev == "wlan0") return "auto → " + net.default_dev;
    return "auto (no uplink)";
}

static std::string find_rnsd_pid() {
    // 1. pidfile (authoritative — S82rnsd writes /var/run/rnsd.pid). Digits
    // only: the pid is emitted raw into JSON ("pid":N) and a /proc path, so a
    // junk pidfile ("self", "../x") must not pass the exists() test.
    std::string pid = slurp_trim("/var/run/rnsd.pid");
    bool pid_num = !pid.empty() && pid.size() <= 10;
    for (char c : pid) if (!isdigit((unsigned char)c)) pid_num = false;
    if (pid_num && exists("/proc/" + pid)) return pid;
    // 2. /proc cmdline scan (pidfile lost/stale). rnsd's argv is
    // "/usr/bin/python /usr/bin/rnsd --config /etc/reticulum". Its syslog
    // companion, `logger -t rnsd[<pid>] ...` (S82rnsd), must never match
    // here: keep these patterns clear of a bare "rnsd[" tag.
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

std::string lan_iface() {
    // Same rule as S60routing read_lan() (and S82rnsd / S46wanwatch): only
    // the two names S30gadget_nic ever records are taken from the file; with
    // no valid record, br-lan if that bridge exists, else the gadget NIC.
    std::string l = util::trim(util::read_file("/run/lan-iface", 64));
    if (l == "br-lan" || l == "usb0") return l;
    return is_dir("/sys/class/net/br-lan/bridge") ? std::string("br-lan") : std::string("usb0");
}

std::vector<std::string> bridge_ports(const std::string& br) {
    std::vector<std::string> out;
    if (!ifname_ok(br)) return out;
    if (DIR* d = opendir(("/sys/class/net/" + br + "/brif").c_str())) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != ".." && ifname_ok(n)) out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string lan_port_role(const std::string& port) {
    if (port == "usb0") return "USB-C";
    if (port.compare(0, 4, "wlan") == 0) return "WiFi hotspot";
    return "";
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
                l.length = p[0]; l.mac = p[1]; l.ip = p[2];
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

std::string clock_source() {
    std::string s = util::trim(util::read_file("/run/clock-source", 64));
    return (s == "rtc" || s == "ntp" || s == "browser") ? s : std::string();
}

Rtc rtc_status() {
    Rtc r;
    const std::string rtc = "/sys/class/rtc/rtc0";
    if (!is_dir(rtc)) return r;   // no RTC fitted: the DS3231 probe failed quietly
    r.present = true;
    r.name = slurp_trim(rtc + "/name");
    // since_epoch's read fails with the driver's error: EINVAL while RNSBox's
    // rtc-ds1307 holds a DS3231's oscillator-stop flag (its time was lost; the
    // flag stays until the RTC is written), EIO & co. on a bus fault. A time it
    // does return must still lie in the 2020..2100 window S45ntpsync's boot
    // check and the browser sync apply.
    int err = 0;
    std::string se = sysfs_read(rtc + "/since_epoch", err);
    long long e = 0;
    if (err) {
        r.read_errno = err;
        r.osf = (err == EINVAL);
    } else if (parse_ll(se, e) && e >= 0) {
        time_t t = (time_t)e;
        struct tm tmv;
        char buf[40];
        if (gmtime_r(&t, &tmv) && strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv)) {
            r.epoch = e;
            r.time = buf;
            r.valid = (e >= CLOCK_FLOOR && e < 4102444800LL);
        }
    }
    // DS3231 die temperature: its hwmon device is a child of the same I2C client
    // as rtc0, so match on the resolved `device` link; fall back to the hwmon
    // name (the client name, "ds3231" from the DT compatible).
    std::string client = real_path(rtc + "/device");
    std::string hw, by_name;
    if (DIR* d = opendir("/sys/class/hwmon")) {
        while (struct dirent* ent = readdir(d)) {
            std::string n = ent->d_name;
            if (n.compare(0, 5, "hwmon") != 0) continue;
            std::string p = "/sys/class/hwmon/" + n;
            if (!client.empty() && real_path(p + "/device") == client) { hw = p; break; }
            if (by_name.empty() && slurp_trim(p + "/name") == "ds3231") by_name = p;
        }
        closedir(d);
    }
    if (hw.empty()) hw = by_name;
    long long mc = 0;
    if (!hw.empty() && parse_ll(slurp_trim(hw + "/temp1_input"), mc) && mc > -100000 && mc < 200000) {
        r.has_temp = true;
        r.temp_mc = (long)mc;
    }
    return r;
}

}  // namespace sysinfo
