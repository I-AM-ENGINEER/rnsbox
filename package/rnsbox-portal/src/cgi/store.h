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
    std::string interface = "auto";   // auto (eth0 + wlan0, kernel picks by metric) | eth0 | wlan0
    std::string mode = "dhcp";        // dhcp | static
    std::string address, prefix, gateway, dns;
};

// --- validation helpers ---
bool valid_ipv4(const std::string& s);
bool valid_dns_list(const std::string& s);        // space-separated IPv4s (may be empty)
std::string sanitize_comment(const std::string& s); // strip '#' + trim (control chars are rejected first)
// True if s may be stored as ONE value/entry in our line-per-entry conf files:
// no CR/LF/NUL/TAB or other C0/DEL control char. A CR/LF would inject a whole
// extra line (e.g. a second mode= or a new [[interface]]). Callers reject with
// an error flash and write nothing.
bool conf_value_ok(const std::string& s);

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
bool write_wan(const WanConfig& cfg, std::string& err);         // validates all fields; dns may be comma- or space-separated

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
bool write_slip(const SlipConfig& c, std::string& err);   // baud 1..SLIP_MAX_BAUD
// SG2002 UART: 25 MHz base clock / 16 = 1 562 500 baud is the ceiling.
// 4 M: headroom for the reclocked arm64 boards (exact 2 Mbaud from
// the 96 MHz uart clock); 25 MHz-clocked boards top out at 1562500.
constexpr int SLIP_MAX_BAUD = 4000000;

// Append a TCPClientInterface for the HaLow modem to the reticulum config.
// Returns true + sets msg to the interface name; false + msg="already" if the
// target is already present, else msg=error (incl. a name with control chars —
// it is written into a [[section]] header line).
bool add_halow_interface(const std::string& name, const std::string& host, int port, std::string& msg);

// --- RNode LoRa modem over USB serial or BLE ---
// /etc/rnsbox/rnode.conf is the single source of truth; S81rnode turns it
// into the managed [[RNode ...]] section of /etc/reticulum/config on every
// boot / `S81rnode reload`. Ranges below mirror RNodeInterface itself —
// an out-of-range value makes rnsd panic at interface creation, so both
// this writer and S81rnode reject them.
constexpr const char* RNODE_FILE   = "/etc/rnsbox/rnode.conf";
constexpr const char* RNODE_STATUS = "/run/rnsbox-rnode.status";

struct RnodeConfig {
    std::string transport = "off";        // off | usb | ble
    std::string device = "auto";          // usb: auto (first ttyACM*, else ttyUSB*) | /dev/ttyACM0 ...
    std::string ble_address = "";         // ble: AA:BB:CC:DD:EE:FF, or blank
    std::string ble_name = "";            // ble: exact advert name, or blank
    std::string name = "RNode";           // [[section]] name in the rns config
    std::string frequency = "867500000";  // Hz (137000000..3000000000)
    std::string bandwidth = "125000";     // Hz (7800..1625000)
    std::string txpower = "7";            // dBm (0..37)
    std::string spreadingfactor = "7";    // 5..12
    std::string codingrate = "5";         // 5..8
    std::string flow_control = "no";      // yes|no
};
RnodeConfig read_rnode();
bool write_rnode(const RnodeConfig& c, std::string& err);

// The live state S81rnode left behind (absent file: transport "off").
struct RnodeStatus {
    std::string transport, port, state, message;
    bool present = false;
};
RnodeStatus read_rnode_status();

// USB serial candidates for the device dropdown (/dev/ttyACM*, /dev/ttyUSB*).
std::vector<std::string> rnode_serial_devices();

// True when the BLE stack RNS needs for port = ble:// is in the image
// (bleak + bluetoothd). Cheap globs/binary checks, no python startup.
bool rnode_ble_stack_present();

}  // namespace store
#endif
