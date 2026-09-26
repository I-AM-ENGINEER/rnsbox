// sysinfo.h — read-only views over /proc, /sys, and a few command outputs.
// C++ port of the former sysinfo.py (hot path: hit on every page render, so
// native rather than a python spawn). halow_status() stays a python helper —
// it does HTTP-over-SLIP + JSON and is only fetched async on two tabs.
#ifndef RNSBOX_SYSINFO_H
#define RNSBOX_SYSINFO_H

#include <map>
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
    bool bridge = false;              // a Linux bridge (br-lan)
    std::string master;               // bridge this iface is a port of ("" = none)
};
struct Network {
    std::vector<Iface> ifaces;        // sorted, lo excluded
    std::string default_gateway;      // of the default route in use ("" = none / no gateway)
    std::string default_dev;          // its iface, e.g. "wlan0" ("" = no usable default route)
    std::map<std::string, std::string> gateway_of;   // iface -> its own default gateway, carrier or not
    std::vector<std::string> dns;
};
Network network_summary();

// The iface the WAN card/labels describe for wan.conf's interface= value:
// eth0 / wlan0 as set; for "auto" the iface of the default route in use if it
// is one of the two, else eth0 (nothing is up: show the wired port's state).
std::string wan_display_iface(const std::string& wan_setting, const Network& net);

// The gateway to show for the WAN setting: the one in use for "auto", else
// the pinned iface's own default gateway ("" = none).
std::string wan_gateway(const std::string& wan_setting, const Network& net);

// Human label for the WAN setting: "eth0", "wlan0", "auto → wlan0",
// "auto (no uplink)". Plain text (the caller escapes).
std::string wan_label(const std::string& wan_setting, const Network& net);

struct Rnsd {
    bool running = false;
    std::string pid;          // empty when not running
    long rss_mb = -1;         // -1 = unknown
    std::string raw;          // rnstatus -a block (only when with_raw)
};
Rnsd rnsd_status(bool with_raw = false);

// A plausible interface name (IFNAMSIZ, kernel-legal charset). Names read from
// /run files become part of a /sys path and the page — check them first.
bool ifname_ok(const std::string& name);

// The bridge `iface` is a port of (its /sys/class/net/<iface>/brport/bridge
// link), e.g. "br-lan" for usb0; "" when it isn't bridged.
std::string bridge_of(const std::string& iface);

// The LAN interface in use, as S30gadget_nic records it in /run/lan-iface —
// what S60routing puts in nftables' @lan_ifaces and dnsmasq serves: "br-lan"
// (the bridge of the USB-C gadget NIC usb0 + the hotspot iface while an AP is
// up; one subnet, 10.42.0.1/24) or "usb0" when the bridge couldn't be made.
// Without a valid record (missing file, any other name): "br-lan" if that
// bridge exists, else "usb0" — the same rule as S60routing read_lan(), so
// the portal, nftgen and the init scripts always agree on the LAN.
std::string lan_iface();

// Member ports of bridge `br` (/sys/class/net/<br>/brif), sorted (usb0 before
// wlan1). Empty for a non-bridge iface — the usb0 fallback has no ports.
std::vector<std::string> bridge_ports(const std::string& br);

// What a LAN bridge port is, for labels: "USB-C" (usb0), "WiFi hotspot" (a
// wlan* port: only an AP vif can be bridged), else "".
std::string lan_port_role(const std::string& port);

// length: dnsmasq is built with HAVE_BROKEN_RTC, so the leases file's first
// field is the lease length in seconds, not an expiry epoch.
struct Lease { std::string length, mac, ip, name, client_id; };
std::vector<Lease> dnsmasq_leases();

struct Slip { bool present = false, up = false; };
Slip slip_status();

// Wall-clock provenance: /run/clock-source, written by S45ntpsync (boot: "rtc"
// when the kernel set the clock from rtc0; "ntp" from the NTP one-shot and the
// hourly RTC resync) and the Settings browser sync ("browser"). Returns exactly
// "rtc" | "ntp" | "browser", or "" when there is no trusted source yet (file
// absent or anything else).
std::string clock_source();

// Earliest epoch we accept as a set clock (2020-01-01 UTC) — the browser-sync
// plausibility floor, reused for the RTC.
constexpr long long CLOCK_FLOOR = 1577836800LL;

// The hardware clock, if one probed (DS3231 on i2c5 -> rtc0; the SoC RTC driver
// isn't shipped). Reads the chip over I2C, so Settings-page only — never on the
// 5 s dashboard refresh.
struct Rtc {
    bool present = false;     // /sys/class/rtc/rtc0 exists
    bool valid = false;       // it reads back a time in CLOCK_FLOOR..2100 (the
                              // window S45ntpsync's boot check applies)
    bool osf = false;         // the read failed with EINVAL: RNSBox's rtc-ds1307
                              // refuses the time while a DS3231's oscillator-stop
                              // flag is set (never set, or its backup cell ran
                              // flat while unpowered), until the RTC is written
    int read_errno = 0;       // errno of a failed read (EINVAL: osf)
    long long epoch = 0;      // its time (UTC) when it read one
    std::string time;         // "YYYY-MM-DD HH:MM:SS" (UTC) when it read one,
                              // in the window or not
    std::string name;         // rtc0/name, e.g. "rtc-ds1307 5-0068"
    bool has_temp = false;    // DS3231 die temperature via its hwmon device
    long temp_mc = 0;         // millidegrees C
};
Rtc rtc_status();

}  // namespace sysinfo
#endif
