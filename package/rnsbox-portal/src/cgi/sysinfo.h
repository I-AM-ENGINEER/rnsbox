// sysinfo.h — read-only views over /proc, /sys, and a few command outputs.
// C++ port of the former sysinfo.py (hot path: hit on every page render, so
// native rather than a python spawn). halow_status() stays a python helper —
// it does HTTP-over-SLIP + JSON and is only fetched async on two tabs.
#ifndef RNSBOX_SYSINFO_H
#define RNSBOX_SYSINFO_H

#include <string>
#include <vector>

namespace sysinfo {

std::string hostname();

struct Mem {
    bool ok = false;
    long total_mb = 0, avail_mb = 0, used_mb = 0, buffcache_mb = 0;
    double used_pct = 0.0;
};
struct Summary {
    std::string uptime;       // "Nd Nh Nm"
    std::string load[3];      // 1/5/15-min load strings
    Mem mem;
    std::string kernel;       // osrelease
};
Summary summary();

struct Iface {
    std::string name, state, mac, mtu, rx, tx;
    std::vector<std::string> addrs;   // "addr/prefix"
};
struct Network {
    std::vector<Iface> ifaces;        // sorted, lo excluded
    std::string default_gateway;
    std::vector<std::string> dns;
};
Network network_summary();

struct Rnsd {
    bool running = false;
    std::string pid;          // empty when not running
    long rss_mb = -1;         // -1 = unknown
    std::string raw;          // rnstatus -a block (only when with_raw)
};
Rnsd rnsd_status(bool with_raw = false);

struct Lease { std::string expires, mac, ip, name, client_id; };
std::vector<Lease> dnsmasq_leases();

struct Slip { bool present = false, up = false; };
Slip slip_status();

}  // namespace sysinfo
#endif
