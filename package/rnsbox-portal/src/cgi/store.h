// store.h — read/write the flat /etc/rnsbox/*.conf files, byte-compatible with
// store.py so Python (nftgen, boot scripts) and this CGI can share them.
#ifndef RNSBOX_STORE_H
#define RNSBOX_STORE_H

#include <string>
#include <vector>

namespace store {

constexpr const char* PF_FILE   = "/etc/rnsbox/portforward.conf";
constexpr const char* OP_FILE   = "/etc/rnsbox/openports.conf";
constexpr const char* WAN_FILE  = "/etc/rnsbox/wan.conf";
constexpr const char* CRON_FILE = "/etc/rnsbox/cron.conf";
constexpr const char* RETICULUM_CONFIG = "/etc/reticulum/config";
constexpr const char* NTP_FILE  = "/etc/ntp.conf";
constexpr const char* SLIP_FILE = "/etc/rnsbox/slip.conf";

struct PortForward {
    std::string proto;      // "tcp" | "udp"
    int wan_port = 0;
    std::string lan_ip;
    int lan_port = 0;
    std::string comment;
};

struct OpenPort {
    std::string proto;
    int port = 0;
    std::string comment;
};

struct WanConfig {
    std::string interface = "eth0";   // eth0 | wlan0
    std::string mode = "dhcp";        // dhcp | static
    std::string address, prefix, gateway, dns;
};

// --- validation helpers ---
bool valid_ipv4(const std::string& s);
bool valid_dns_list(const std::string& s);        // space-separated IPv4s (may be empty)
std::string sanitize_comment(const std::string& s); // strip control chars + '#' + trim

// --- port forwards ---
std::vector<PortForward> read_portforwards();
bool add_portforward(const PortForward& pf, std::string& err);  // validates
bool delete_portforward(int idx);

// --- open ports ---
std::vector<OpenPort> read_openports();
bool add_openport(const OpenPort& op, std::string& err);
bool delete_openport(int idx);

// --- WAN ---
WanConfig read_wan();
bool write_wan(const WanConfig& cfg, std::string& err);         // validates all fields

// --- cron (rnsd auto-restart), preserving the update_check key ---
int  read_rnsd_restart_days();                 // 0..7 (0 = off)
bool write_rnsd_restart_days(int days, std::string& err);
bool write_update_check(bool enabled, std::string& err);   // cron.conf update_check=yes|no

// --- reticulum config (raw text editor) ---
std::string read_reticulum_config();
bool write_reticulum_config(const std::string& text, std::string& err);

// --- NTP servers (edit the server/pool lines in /etc/ntp.conf) ---
std::vector<std::string> read_ntp_servers();
std::vector<std::string> write_ntp_servers(const std::vector<std::string>& servers); // returns kept

// --- SLIP link to an external HaLow (RNode) modem ---
struct SlipConfig {
    std::string enabled = "no";
    std::string device = "/dev/serial0";
    std::string baud = "2000000";
    std::string local_ip = "192.168.7.1";
    std::string peer_ip = "192.168.7.2";
};
SlipConfig read_slip();
bool write_slip(const SlipConfig& c, std::string& err);

// Append a TCPClientInterface for the HaLow modem to the reticulum config.
// Returns true + sets msg to the interface name; false + msg="already" if the
// target is already present, else msg=error.
bool add_halow_interface(const std::string& name, const std::string& host, int port, std::string& msg);

}  // namespace store
#endif
