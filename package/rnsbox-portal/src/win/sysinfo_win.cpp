// sysinfo_win.cpp — sysinfo.h on Windows (replaces cgi/sysinfo.cpp).
//
// The Linux implementation reads /proc and /sys; this one asks Win32:
// GlobalMemoryStatusEx for RAM, GetTickCount64 for uptime, GetAdaptersAddresses
// + GetIfEntry2 for the interface table, and the supervisor for rnsd. Anything
// that only exists on the router (bridges, dnsmasq leases, SLIP, DS3231) is
// reported absent, which is exactly what the pages expect from a board without
// that hardware.
#ifdef _WIN32

#include "sysinfo.h"
#include "compat.h"
#include "supervisor.h"
#include "util.h"

#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace {

std::string hb(double n) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    char buf[48];
    for (int u = 0; u < 5; ++u) {
        if (n < 1024.0) {
            if (u == 0) snprintf(buf, sizeof buf, "%lld B", (long long)n);
            else        snprintf(buf, sizeof buf, "%.1f %s", n, units[u]);
            return buf;
        }
        n /= 1024.0;
    }
    snprintf(buf, sizeof buf, "%.1f PiB", n);
    return buf;
}

std::string mac_str(const unsigned char* b, size_t n) {
    char buf[3 * 8 + 1];
    std::string o;
    for (size_t i = 0; i < n; ++i) {
        snprintf(buf, sizeof buf, i ? ":%02x" : "%02x", b[i]);
        o += buf;
    }
    return o;
}

std::string reg_string(const char* subkey, const char* value) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return std::string();
    char buf[512];
    DWORD sz = sizeof buf, type = 0;
    std::string out;
    if (RegQueryValueExA(k, value, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
        out.assign(buf, strnlen(buf, sz));
    RegCloseKey(k);
    return out;
}

}  // namespace

namespace sysinfo {

std::string hostname() {
    char buf[256];
    DWORD n = sizeof buf;
    if (GetComputerNameA(buf, &n) && n > 0) return std::string(buf, n);
    return "(unknown)";
}

Summary summary() {
    Summary s;
    ULONGLONG ms = GetTickCount64();
    long secs = (long)(ms / 1000);
    long d = secs / 86400; secs %= 86400;
    long h = secs / 3600;  secs %= 3600;
    long m = secs / 60;
    char buf[64];
    snprintf(buf, sizeof buf, "%ldd %ldh %ldm", d, h, m);
    s.uptime = buf;

    // Windows has no load average. Report the percentage of CPU busy since the
    // previous render instead, so the column is not a lie; it reads "—" until
    // there is a second sample.
    static ULONGLONG prev_idle = 0, prev_kernel = 0, prev_user = 0;
    FILETIME idle_ft, kernel_ft, user_ft;
    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        auto to_u64 = [](const FILETIME& f) {
            ULARGE_INTEGER u; u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime; return u.QuadPart;
        };
        ULONGLONG idle = to_u64(idle_ft), kernel = to_u64(kernel_ft), user = to_u64(user_ft);
        if (prev_kernel || prev_user) {
            ULONGLONG di = idle - prev_idle;
            ULONGLONG dt = (kernel - prev_kernel) + (user - prev_user);
            if (dt > 0) {
                int pct = (int)(((dt - di) * 100) / dt);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                snprintf(buf, sizeof buf, "%d%%", pct);
                s.load[0] = buf;
            }
        }
        prev_idle = idle; prev_kernel = kernel; prev_user = user;
    }
    for (int i = 0; i < 3; ++i) if (s.load[i].empty()) s.load[i] = "—";

    MEMORYSTATUSEX mi{};
    mi.dwLength = sizeof mi;
    if (GlobalMemoryStatusEx(&mi)) {
        s.mem.ok = true;
        s.mem.total_mb = (long)(mi.ullTotalPhys / (1024 * 1024));
        s.mem.avail_mb = (long)(mi.ullAvailPhys / (1024 * 1024));
        s.mem.used_mb = s.mem.total_mb - s.mem.avail_mb;
        s.mem.buffcache_mb = 0;
        s.mem.used_pct = s.mem.total_mb > 0
            ? (double)((int)(1000.0 * s.mem.used_mb / s.mem.total_mb + 0.5)) / 10.0 : 0.0;
        // Windows' "commit" is the closest thing to swap the page shows.
        unsigned long long page = mi.ullTotalPageFile > mi.ullTotalPhys
            ? mi.ullTotalPageFile - mi.ullTotalPhys : 0;
        if (page > 0) {
            s.mem.swap_total_mb = (long)(page / (1024 * 1024));
            unsigned long long avail_page = mi.ullAvailPageFile > mi.ullAvailPhys
                ? mi.ullAvailPageFile - mi.ullAvailPhys : 0;
            long used = s.mem.swap_total_mb - (long)(avail_page / (1024 * 1024));
            s.mem.swap_used_mb = used < 0 ? 0 : used;
        }
    }

    std::string product = reg_string("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName");
    std::string build   = reg_string("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber");
    std::string display = reg_string("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion");
    s.kernel = product.empty() ? "Windows" : product;
    if (!display.empty()) s.kernel += " " + display;
    if (!build.empty()) s.kernel += " (build " + build + ")";

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const char* arch = "x86_64";
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86";   break;
        case PROCESSOR_ARCHITECTURE_ARM:   arch = "arm";   break;
        default: break;
    }
    s.device = std::string("Windows PC (") + arch + ")";
    return s;
}

Network network_summary() {
    Network net;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    IP_ADAPTER_ADDRESSES* aa = nullptr;
    for (int tries = 0; tries < 3; ++tries) {
        buf.resize(size);
        ULONG rc = GetAdaptersAddresses(AF_UNSPEC,
                                        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS,
                                        nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                        &size);
        if (rc == ERROR_SUCCESS) {
            aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
            break;
        }
        if (rc != ERROR_BUFFER_OVERFLOW) break;
    }
    if (!aa) return net;

    // The default route, from the routing table rather than adapter order:
    // Windows may list a VPN, a WSL switch and the physical NIC in any order,
    // and only the route metric says which one actually carries traffic.
    NET_LUID def_luid{};
    bool have_def_luid = false;
    std::string def_nexthop;
    {
        PMIB_IPFORWARD_TABLE2 table = nullptr;
        if (GetIpForwardTable2(AF_INET, &table) == NO_ERROR && table) {
            ULONG best = 0xffffffff;
            for (ULONG k = 0; k < table->NumEntries; ++k) {
                const MIB_IPFORWARD_ROW2& r = table->Table[k];
                if (r.DestinationPrefix.Prefix.si_family != AF_INET) continue;
                if (r.DestinationPrefix.PrefixLength != 0) continue;   // default only
                if (r.Metric < best) {
                    best = r.Metric;
                    def_luid = r.InterfaceLuid;
                    have_def_luid = true;
                    char hop[INET_ADDRSTRLEN] = {0};
                    if (r.NextHop.si_family == AF_INET)
                        InetNtopA(AF_INET, &r.NextHop.Ipv4.sin_addr, hop, sizeof hop);
                    def_nexthop = hop;
                }
            }
            FreeMibTable(table);
        }
    }

    std::string best_def, best_gw;
    for (IP_ADAPTER_ADDRESSES* a = aa; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        Iface i;
        i.name = compat::narrow(a->FriendlyName);
        if (i.name.empty()) i.name = a->AdapterName;
        i.state = (a->OperStatus == IfOperStatusUp) ? "up" : "down";
        i.mac = mac_str(a->PhysicalAddress, a->PhysicalAddressLength > 6 ? 6 : a->PhysicalAddressLength);
        i.mtu = std::to_string(a->Mtu);
        i.bridge = false;
        i.master.clear();

        MIB_IF_ROW2 row{};
        row.InterfaceLuid = a->Luid;
        if (GetIfEntry2(&row) == NO_ERROR) {
            i.rx = hb((double)row.InOctets);
            i.tx = hb((double)row.OutOctets);
        }

        std::string v4;
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
            sockaddr* sa = u->Address.lpSockaddr;
            if (!sa) continue;
            char host[INET6_ADDRSTRLEN] = {0};
            if (sa->sa_family == AF_INET) {
                auto* v = reinterpret_cast<sockaddr_in*>(sa);
                InetNtopA(AF_INET, &v->sin_addr, host, sizeof host);
                int plen = u->OnLinkPrefixLength;
                i.addrs.push_back(std::string(host) + "/" + std::to_string(plen));
                if (v4.empty()) v4 = host;
            } else if (sa->sa_family == AF_INET6) {
                auto* v = reinterpret_cast<sockaddr_in6*>(sa);
                InetNtopA(AF_INET6, &v->sin6_addr, host, sizeof host);
                int plen = u->OnLinkPrefixLength;
                i.addrs.push_back(std::string(host) + "/" + std::to_string(plen));
            }
        }

        std::string gw;
        for (IP_ADAPTER_GATEWAY_ADDRESS* g = a->FirstGatewayAddress; g; g = g->Next) {
            sockaddr* sa = g->Address.lpSockaddr;
            if (!sa) continue;
            char host[INET6_ADDRSTRLEN] = {0};
            if (sa->sa_family == AF_INET) {
                auto* v = reinterpret_cast<sockaddr_in*>(sa);
                InetNtopA(AF_INET, &v->sin_addr, host, sizeof host);
                gw = host;
                break;
            }
        }
        if (!gw.empty()) net.gateway_of[i.name] = gw;

        for (IP_ADAPTER_DNS_SERVER_ADDRESS* d = a->FirstDnsServerAddress; d; d = d->Next) {
            sockaddr* sa = d->Address.lpSockaddr;
            if (!sa) continue;
            char host[INET6_ADDRSTRLEN] = {0};
            if (sa->sa_family == AF_INET)
                InetNtopA(AF_INET, &reinterpret_cast<sockaddr_in*>(sa)->sin_addr, host, sizeof host);
            else if (sa->sa_family == AF_INET6)
                InetNtopA(AF_INET6, &reinterpret_cast<sockaddr_in6*>(sa)->sin6_addr, host, sizeof host);
            if (host[0]) net.dns.push_back(host);
        }

        if (getenv("RNSBOX_DEBUG_NET"))
            fprintf(stderr, "net: '%s' type=%lu state=%s v4='%s' gw='%s'\n",
                    i.name.c_str(), (unsigned long)a->IfType, i.state.c_str(), v4.c_str(), gw.c_str());

        // The default route: the routing table's choice (best_def above), or,
        // when it is absent, the first up adapter with a real IPv4 gateway.
        if (have_def_luid && a->Luid.Value == def_luid.Value) {
            best_def = i.name;
            best_gw = def_nexthop;
        }
        if (best_def.empty() && i.state == "up" && !gw.empty() && !v4.empty() &&
            v4.rfind("169.254.", 0) != 0) {
            best_def = i.name;
            best_gw = gw;
        }
        net.ifaces.push_back(std::move(i));
    }
    std::sort(net.ifaces.begin(), net.ifaces.end(),
              [](const Iface& a, const Iface& b) { return a.name < b.name; });
    net.default_dev = best_def;
    net.default_gateway = best_gw;
    std::vector<std::string> uniq;
    for (const auto& d : net.dns)
        if (std::find(uniq.begin(), uniq.end(), d) == uniq.end()) uniq.push_back(d);
    net.dns = std::move(uniq);
    return net;
}

std::string wan_display_iface(const std::string& wan_setting, const Network& net) {
    if (wan_setting != "auto") return wan_setting;
    if (!net.default_dev.empty()) return net.default_dev;
    return "eth0";
}

std::string wan_gateway(const std::string& wan_setting, const Network& net) {
    if (wan_setting == "auto") return net.default_gateway;
    auto it = net.gateway_of.find(wan_setting);
    return it == net.gateway_of.end() ? std::string() : it->second;
}

std::string wan_label(const std::string& wan_setting, const Network& net) {
    std::string ifc = wan_display_iface(wan_setting, net);
    if (ifc == "eth0") return "Ethernet (eth0)";
    if (ifc == "wlan0") return "WiFi (wlan0)";
    if (ifc == "usb0" || ifc == "br-lan") return "USB-NCM (" + ifc + ")";
    return ifc.empty() ? std::string("Uplink") : ifc;
}

Rnsd rnsd_status(bool with_raw) {
    Rnsd r;
    supervisor::RnsdState st = supervisor::rnsd_state();
    if (!st.running) return r;
    r.running = true;
    r.pid = std::to_string(st.pid);
    r.uptime_s = st.uptime_s;
    r.rss_mb = -1;
    if (with_raw) {
        util::RunResult rr = util::run({"/usr/bin/rnstatus", "-a"}, "", 20);
        r.raw = rr.out.empty() ? std::string("(rnstatus not available)") : rr.out;
    }
    return r;
}

bool ifname_ok(const std::string& n) {
    if (n.empty() || n.size() > 63 || n == "." || n == "..") return false;
    for (char c : n)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == ' ' ||
              c == '(' || c == ')' || c == '{' || c == '}')) return false;
    return true;
}

std::string bridge_of(const std::string&) { return std::string(); }
std::string lan_iface() { return std::string(); }
std::vector<std::string> bridge_ports(const std::string&) { return {}; }
std::string lan_port_role(const std::string&) { return std::string(); }
std::vector<Lease> dnsmasq_leases() { return {}; }
Slip slip_status() { return Slip{}; }

std::string clock_source() {
    // Windows keeps the clock with the Windows Time service; that is the
    // only source of truth a PC has (there is no /run/clock-source).
    SC_HANDLE sc = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!sc) return std::string();
    std::string out;
    SC_HANDLE svc = OpenServiceA(sc, "W32Time", SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING)
            out = "ntp";
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(sc);
    return out;
}

Rtc rtc_status() { return Rtc{}; }

}  // namespace sysinfo
#endif  // _WIN32
