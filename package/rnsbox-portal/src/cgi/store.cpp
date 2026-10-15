// store.cpp — see store.h. Formats mirror store.py exactly.
#include "store.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace store {

// ---- shared helpers ----
static std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> out;
    std::string data = util::read_file(path);
    size_t i = 0;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        if (nl == std::string::npos) nl = data.size();
        out.push_back(data.substr(i, nl - i));
        i = nl + 1;
    }
    return out;
}

static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

static bool to_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = (int)v;
    return true;
}

bool valid_ipv4(const std::string& s) {
    if (s.empty()) return false;
    int parts = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t dot = s.find('.', i);
        std::string p = (dot == std::string::npos) ? s.substr(i) : s.substr(i, dot - i);
        if (p.empty() || p.size() > 3) return false;
        for (char c : p) if (c < '0' || c > '9') return false;
        int n = atoi(p.c_str());
        if (n < 0 || n > 255) return false;
        ++parts;
        if (dot == std::string::npos) break;
        i = dot + 1;
        if (i == s.size()) return false;  // trailing dot
    }
    return parts == 4;
}

bool valid_dns_list(const std::string& s) {
    for (const auto& ip : split_ws(s)) if (!valid_ipv4(ip)) return false;
    return true;
}

std::string sanitize_comment(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '\n' || c == '\r' || c == '#') continue;       // would break the file / delimiter
        if (c < 0x20 && c != '\t') continue;
        o.push_back((char)c);
    }
    return util::trim(o);
}

bool conf_value_ok(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return false;   // incl. CR, LF, NUL, TAB
    return true;
}

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// ---- port forwards ----
std::vector<PortForward> read_portforwards() {
    std::vector<PortForward> out;
    for (const auto& ln : read_lines(PF_FILE)) {
        std::string s = util::trim(ln);
        if (s.empty() || s[0] == '#') continue;
        size_t h = s.find('#');
        std::string head = (h == std::string::npos) ? s : s.substr(0, h);
        std::string comment = (h == std::string::npos) ? "" : util::trim(s.substr(h + 1));
        auto p = split_ws(head);
        if (p.size() < 4) continue;
        PortForward pf;
        pf.proto = p[0];
        if (!to_int(p[1], pf.wan_port)) continue;
        pf.lan_ip = p[2];
        if (!to_int(p[3], pf.lan_port)) continue;
        pf.comment = comment;
        out.push_back(pf);
    }
    return out;
}

static bool write_portforwards(const std::vector<PortForward>& items) {
    std::string o =
        "# proto  wan_port  lan_ip          lan_port  [# comment]\n"
        "# managed by rnsbox-portal — hand-edits clobbered when UI saves\n";
    for (const auto& it : items) {
        o += it.proto + " " + std::to_string(it.wan_port) + " " + it.lan_ip + " " + std::to_string(it.lan_port);
        if (!it.comment.empty()) o += "  # " + it.comment;
        o += "\n";
    }
    return util::write_file_atomic(PF_FILE, o, 0644);
}

bool add_portforward(const PortForward& pf, std::string& err) {
    if (pf.proto != "tcp" && pf.proto != "udp") { err = "proto must be tcp or udp"; return false; }
    if (pf.wan_port < 1 || pf.wan_port > 65535) { err = "wan_port out of range"; return false; }
    if (pf.lan_port < 1 || pf.lan_port > 65535) { err = "lan_port out of range"; return false; }
    if (!valid_ipv4(pf.lan_ip)) { err = "lan_ip is not a valid IPv4 address"; return false; }
    if (!conf_value_ok(pf.comment)) { err = "comment must not contain line breaks or control characters"; return false; }
    // WAN ports the box serves itself. Prerouting DNAT runs before INPUT, so a
    // forward would silently take them over. The portal's cross-port redirects
    // (main.cpp MODEM_PORT) would then send the admin, session cookie included
    // (cookies aren't port-scoped), to the forward's target.
    if (pf.proto == "tcp" && pf.wan_port == 8081) { err = "WAN tcp 8081 is reserved for the HaLow modem page"; return false; }
    if (pf.proto == "tcp" && pf.wan_port == 80)
        for (const auto& op : read_openports())
            if (op.proto == "tcp" && op.port == 80) { err = "WAN tcp 80 is the web UI (Open ports) - remove it there first"; return false; }
    auto items = read_portforwards();
    PortForward n = pf;
    n.comment = sanitize_comment(pf.comment);
    items.push_back(n);
    if (!write_portforwards(items)) { err = "write failed"; return false; }
    return true;
}

bool delete_portforward(int idx) {
    auto items = read_portforwards();
    if (idx < 0 || idx >= (int)items.size()) return false;
    items.erase(items.begin() + idx);
    return write_portforwards(items);
}

// ---- open ports ----
std::vector<OpenPort> read_openports() {
    std::vector<OpenPort> out;
    for (const auto& ln : read_lines(OP_FILE)) {
        std::string s = util::trim(ln);
        if (s.empty() || s[0] == '#') continue;
        size_t h = s.find('#');
        std::string head = (h == std::string::npos) ? s : s.substr(0, h);
        std::string comment = (h == std::string::npos) ? "" : util::trim(s.substr(h + 1));
        auto p = split_ws(head);
        if (p.size() < 2) continue;
        OpenPort op;
        op.proto = p[0];
        if (!to_int(p[1], op.port)) continue;
        op.comment = comment;
        out.push_back(op);
    }
    return out;
}

static bool write_openports(const std::vector<OpenPort>& items) {
    std::string o = "# proto  port  [# comment]\n# managed by rnsbox-portal\n";
    for (const auto& it : items) {
        o += it.proto + " " + std::to_string(it.port);
        if (!it.comment.empty()) o += "  # " + it.comment;
        o += "\n";
    }
    return util::write_file_atomic(OP_FILE, o, 0644);
}

bool add_openport(const OpenPort& op, std::string& err) {
    if (op.proto != "tcp" && op.proto != "udp") { err = "proto must be tcp or udp"; return false; }
    if (op.port < 1 || op.port > 65535) { err = "port out of range"; return false; }
    if (!conf_value_ok(op.comment)) { err = "comment must not contain line breaks or control characters"; return false; }
    auto items = read_openports();
    OpenPort n = op;
    n.comment = sanitize_comment(op.comment);
    items.push_back(n);
    if (!write_openports(items)) { err = "write failed"; return false; }
    return true;
}

bool delete_openport(int idx) {
    auto items = read_openports();
    if (idx < 0 || idx >= (int)items.size()) return false;
    items.erase(items.begin() + idx);
    return write_openports(items);
}

// ---- WAN ----
WanConfig read_wan() {
    WanConfig c;
    for (const auto& ln : read_lines(WAN_FILE)) {
        std::string s = util::trim(ln);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq != std::string::npos) {
            std::string k = util::trim(s.substr(0, eq));
            std::string v = s.substr(eq + 1);
            size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
            v = util::trim(v);
            if (k == "interface") c.interface = v;
            else if (k == "mode") c.mode = v;
            else if (k == "address") c.address = v;
            else if (k == "prefix") c.prefix = v;
            else if (k == "gateway") c.gateway = v;
            else if (k == "dns") c.dns = v;
        } else if (s == "eth0" || s == "wlan0") {
            c.interface = s;   // legacy single-line
        }
    }
    // Same rule as S30eth wan_iface / S60routing read_wan: unknown -> auto.
    if (c.interface != "auto" && c.interface != "eth0" && c.interface != "wlan0") c.interface = "auto";
    if (c.mode != "dhcp" && c.mode != "static") c.mode = "dhcp";
    return c;
}

bool write_wan(const WanConfig& in, std::string& err) {
    WanConfig cfg = in;
    if (cfg.interface != "auto" && cfg.interface != "eth0" && cfg.interface != "wlan0") { err = "WAN interface must be auto, eth0 or wlan0"; return false; }
    if (cfg.mode != "dhcp" && cfg.mode != "static") { err = "WAN mode must be dhcp or static"; return false; }
    if (cfg.mode == "static") {
        if (!conf_value_ok(cfg.address) || !conf_value_ok(cfg.prefix) ||
            !conf_value_ok(cfg.gateway) || !conf_value_ok(cfg.dns)) {
            err = "static fields must not contain line breaks or control characters"; return false;
        }
        if (!valid_ipv4(cfg.address)) { err = "static address is not a valid IPv4"; return false; }
        int pfx = -1;
        if (all_digits(cfg.prefix)) to_int(cfg.prefix, pfx);   // no sign / junk ("+24", "24x")
        if (pfx < 0 || pfx > 32) { err = "prefix must be 0-32"; return false; }
        if (!cfg.gateway.empty() && !valid_ipv4(cfg.gateway)) { err = "gateway is not a valid IPv4"; return false; }
        // The Network tab says "space- or comma-separated": accept commas, then
        // store the canonical single-space list S30eth iterates over.
        std::string d = cfg.dns;
        for (char& c : d) if (c == ',') c = ' ';
        if (!valid_dns_list(d)) { err = "dns must be space- or comma-separated IPv4 addresses"; return false; }
        cfg.dns.clear();
        for (const auto& ip : split_ws(d)) { if (!cfg.dns.empty()) cfg.dns += " "; cfg.dns += ip; }
    }
    std::string o =
        "# WAN uplink config — managed by the rnsbox-portal Network tab.\n"
        "# interface: auto (eth0 while it has a cable and an address, else the\n"
        "#            wlan0 WiFi client), eth0 (wired RJ45 only) or wlan0 (WiFi\n"
        "#            client only, requires STA mode). Never changed by the box.\n"
        "# mode:      dhcp (lease from upstream) or static.\n"
        "# Static mode (applied to eth0 by S30eth): address/prefix/gateway/dns.\n";
    o += "interface=" + cfg.interface + "\n";
    o += "mode=" + cfg.mode + "\n";
    if (cfg.mode == "static") {
        o += "address=" + cfg.address + "\n";
        o += "prefix=" + cfg.prefix + "\n";
        o += "gateway=" + cfg.gateway + "\n";
        o += "dns=" + cfg.dns + "\n";
    }
    if (!util::write_file_atomic(WAN_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}

// ---- cron.conf (rnsd_restart_days + update_check) ----
static bool cron_truthy(const std::string& v) {
    std::string s = util::trim(v);
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return !(s == "no" || s == "0" || s == "false" || s == "off" || s.empty());
}

int read_rnsd_restart_days() {
    int n = 0;
    if (to_int(util::trim(util::conf_get(CRON_FILE, "rnsd_restart_days")), n)) {
        if (n >= 0 && n <= 7) return n;
    }
    return 0;
}

bool write_rnsd_restart_days(int days, std::string& err) {
    if (days < 0 || days > 7) { err = "days must be 0..7"; return false; }
    // preserve update_check (default yes when the key is absent)
    std::string ucv = util::conf_get(CRON_FILE, "update_check");
    bool uc = ucv.empty() ? true : cron_truthy(ucv);
    std::string o =
        "# RNSBox scheduled tasks — managed by the rnsbox-portal Reticulum tab.\n"
        "# rnsd_restart_days: 0 = off (default); N = restart rnsd every N days at\n"
        "#   04:00 (memory hygiene). update_check: yes (default) / no — daily\n"
        "#   check for a newer rnsd (notify-only). Applied by S84cron; live via UI.\n";
    o += "rnsd_restart_days=" + std::to_string(days) + "\n";
    o += std::string("update_check=") + (uc ? "yes" : "no") + "\n";
    if (!util::write_file_atomic(CRON_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}
bool write_update_check(bool enabled, std::string& err) {
    // symmetric to write_rnsd_restart_days: preserve rnsd_restart_days (default 0)
    int days = read_rnsd_restart_days();
    std::string o =
        "# RNSBox scheduled tasks — managed by the rnsbox-portal Reticulum tab.\n"
        "# rnsd_restart_days: 0 = off (default); N = restart rnsd every N days at\n"
        "#   04:00 (memory hygiene). update_check: yes (default) / no — daily\n"
        "#   check for a newer rnsd (notify-only). Applied by S84cron; live via UI.\n";
    o += "rnsd_restart_days=" + std::to_string(days) + "\n";
    o += std::string("update_check=") + (enabled ? "yes" : "no") + "\n";
    if (!util::write_file_atomic(CRON_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}

// ---- reticulum config (raw text) ----
std::string read_reticulum_config() {
    return util::read_file(RETICULUM_CONFIG, 256 * 1024);
}

bool write_reticulum_config(const std::string& text, std::string& err) {
    if (text.size() > 128 * 1024) { err = "config too large"; return false; }
    // normalize CRLF -> LF (a browser textarea may submit CRLF)
    std::string norm;
    norm.reserve(text.size());
    for (char c : text) { if (c != '\r') norm.push_back(c); }
    // 0600: it may hold IFAC passphrases or rpc_key, and every reader
    // (rnsd, rnstatus, lxmd, nomadnet, the portal) runs as root.
    if (!util::write_file_atomic(RETICULUM_CONFIG, norm, 0600)) { err = "write failed"; return false; }
    return true;
}

// ---- NTP servers ----
// A leading '-' is refused: after `server` ntp.conf reads -4/-6 as qualifiers,
// and S45ntpsync's `ntpdate -u -b <servers>` would take it as an option (it
// skips such names too).
static bool valid_ntp_server(const std::string& s) {
    if (s.empty() || s.size() > 253 || s[0] == '-') return false;
    for (char c : s) if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':')) return false;
    return true;
}

std::vector<std::string> read_ntp_servers() {
    std::vector<std::string> out;
    for (const auto& ln : read_lines(NTP_FILE)) {
        auto p = split_ws(util::trim(ln));
        if (p.size() >= 2 && (p[0] == "server" || p[0] == "pool")) out.push_back(p[1]);
    }
    return out;
}

std::vector<std::string> write_ntp_servers(const std::vector<std::string>& servers) {
    std::vector<std::string> clean;
    for (const auto& s : servers) if (valid_ntp_server(s)) clean.push_back(s);
    std::vector<std::string> kept;
    for (const auto& ln : read_lines(NTP_FILE)) {
        auto p = split_ws(util::trim(ln));
        if (!p.empty() && (p[0] == "server" || p[0] == "pool")) continue;
        kept.push_back(ln);
    }
    while (!kept.empty() && util::trim(kept.front()).empty()) kept.erase(kept.begin());
    std::string o;
    for (const auto& s : clean) o += "server " + s + " iburst\n";
    if (!clean.empty() && !kept.empty()) o += "\n";
    for (const auto& l : kept) o += l + "\n";
    util::write_file_atomic(NTP_FILE, o, 0644);
    return clean;
}

// ---- SLIP ----
static bool valid_tty(const std::string& s) {
    if (s == "/dev/ttyS0" || s == "/dev/console") return false;   // don't hijack the console
    if (s.rfind("/dev/", 0) != 0 || s.size() > 64) return false;
    for (char c : s) if (!(isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-')) return false;
    return true;
}

SlipConfig read_slip() {
    SlipConfig d;
    for (const auto& ln : read_lines(SLIP_FILE)) {
        std::string s = util::trim(ln);
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq));
        std::string v = s.substr(eq + 1);
        size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
        v = util::trim(v);
        if (k == "enabled") d.enabled = v; else if (k == "device") d.device = v;
        else if (k == "baud") d.baud = v; else if (k == "local_ip") d.local_ip = v;
        else if (k == "peer_ip") d.peer_ip = v;
    }
    return d;
}

bool write_slip(const SlipConfig& c, std::string& err) {
    std::string en = cron_truthy(c.enabled) ? "yes" : "no";
    std::string dev = c.device.empty() ? "/dev/serial0" : c.device;
    if (!valid_tty(dev)) { err = "bad device path"; return false; }
    // Digits only (atoi would take "2000000<junk>"); the cap is a sanity
    // bound, the real ceiling is per board (see the board README: 2 Mbaud
    // exact on the reclocked arm64 boards, 1562500 on 25 MHz-clocked ones).
    int baud = 0;
    if (all_digits(c.baud) && c.baud.size() <= 8) baud = atoi(c.baud.c_str());
    if (baud < 1 || baud > SLIP_MAX_BAUD) { err = "baud out of range (1-" + std::to_string(SLIP_MAX_BAUD) + ")"; return false; }
    std::string lip = c.local_ip.empty() ? "192.168.7.1" : c.local_ip;
    std::string pip = c.peer_ip.empty() ? "192.168.7.2" : c.peer_ip;
    if (!valid_ipv4(lip) || !valid_ipv4(pip)) { err = "bad IP address"; return false; }
    std::string o =
        "# SLIP-over-UART link to an external WiFi-HaLow (RNode) modem.\n"
        "# Managed by the rnsbox-portal Reticulum tab; applied by S31slip. OFF\n"
        "# by default. baud must match the modem (Nano max 1562500);\n"
        "# local_ip/peer_ip must match the modem's SLIP config.\n";
    o += "enabled=" + en + "\ndevice=" + dev + "\nbaud=" + std::to_string(baud) +
         "\nlocal_ip=" + lip + "\npeer_ip=" + pip + "\n";
    if (!util::write_file_atomic(SLIP_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}

// ---- add HaLow modem as a TCPClientInterface in the reticulum config ----
static std::string sanitize_iface_name(const std::string& in) {
    std::string n;
    for (char c : in) if (c != '[' && c != ']') n += c;
    auto parts = split_ws(n);
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) r += " "; r += parts[i]; }
    if (r.size() > 48) r = r.substr(0, 48);
    return r;
}

static std::vector<std::string> reticulum_iface_names() {
    std::vector<std::string> names;
    for (const auto& ln : read_lines(RETICULUM_CONFIG)) {
        std::string s = util::trim(ln);
        if (s.rfind("[[", 0) == 0 && s.rfind("[[[", 0) != 0 &&
            s.size() >= 4 && s.substr(s.size() - 2) == "]]")
            names.push_back(util::trim(s.substr(2, s.size() - 4)));
    }
    return names;
}

// port <= 0 matches any target_port (see add_halow_interface: the modem's
// bridge port may have been hand-edited in the config since).
static bool reticulum_has_tcp_client(const std::string& host, int port) {
    std::string ps = std::to_string(port);
    bool is_tcp = false;
    std::string cur_host, cur_port;
    for (const auto& ln : read_lines(RETICULUM_CONFIG)) {
        std::string s = util::trim(ln);
        if (!s.empty() && s[0] == '[') { is_tcp = false; cur_host.clear(); cur_port.clear(); continue; }
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq));
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        std::string v = s.substr(eq + 1);
        size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
        v = util::trim(v);
        if (k == "type") is_tcp = (v == "TCPClientInterface");
        else if (k == "target_host") cur_host = v;
        else if (k == "target_port") cur_port = v;
        if (is_tcp && cur_host == host && (port <= 0 || cur_port == ps)) return true;
    }
    return false;
}

bool add_halow_interface(const std::string& name_in, const std::string& host, int port, std::string& msg) {
    if (!valid_ipv4(host)) { msg = "bad modem IP"; return false; }
    if (port < 1 || port > 65535) { msg = "bad port"; return false; }
    // The name lands in a "[[name]]" header line: a CR/LF would start new
    // config lines (e.g. a spawned-command interface). It can come from the
    // modem's own hostname via the page JS, so never trust it.
    if (!conf_value_ok(name_in)) { msg = "interface name must not contain line breaks or control characters"; return false; }
    auto lines = read_lines(RETICULUM_CONFIG);
    if (lines.empty()) { msg = "no reticulum config"; return false; }
    if (reticulum_has_tcp_client(host, 0)) { msg = "already"; return false; }
    std::string name = sanitize_iface_name(name_in);
    if (name.empty()) name = "RNode-Halow";
    auto existing = reticulum_iface_names();
    auto has = [&](const std::string& n) { for (const auto& e : existing) if (e == n) return true; return false; };
    if (has(name)) { std::string base = name; int i = 2; while (has(name)) { name = base + " " + std::to_string(i); ++i; } }
    while (!lines.empty() && util::trim(lines.back()).empty()) lines.pop_back();
    lines.push_back("");
    lines.push_back("  # HaLow RNode modem over the SLIP link — added from the portal.");
    lines.push_back("  [[" + name + "]]");
    lines.push_back("    type = TCPClientInterface");
    lines.push_back("    enabled = yes");
    lines.push_back("    target_host = " + host);
    lines.push_back("    target_port = " + std::to_string(port));
    std::string o;
    for (const auto& l : lines) o += l + "\n";
    if (!util::write_file_atomic(RETICULUM_CONFIG, o, 0600)) { msg = "write failed"; return false; }  // see write_reticulum_config
    msg = name;
    return true;
}


// ---- Bluetooth LE transport ([[BLE Interface]] stanza in reticulum config)
// The stanza ships in /etc/reticulum/config (enabled by default). The portal
// edits it IN PLACE - comments, key order and the rest of the file stay
// byte-identical - so the raw-config editor keeps showing the whole truth.
// rnsd has to restart (S82rnsd) for a change to apply.
constexpr const char* BLE_SECTION = "ble interface";

BleConfig read_ble() {
    BleConfig c;
    bool in = false;
    for (const auto& ln : read_lines(RETICULUM_CONFIG)) {
        std::string s = util::trim(ln);
        if (!s.empty() && s[0] == '[') {
            std::string low = s;
            for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
            in = (low.find(BLE_SECTION) != std::string::npos);
            continue;
        }
        if (!in || s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq)), v = s.substr(eq + 1);
        size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
        v = util::trim(v);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        if (k == "enabled") c.enabled = v.empty() ? "no" : v;
        else if (k == "device_name") c.device_name = v;
// ---- RNode LoRa modem (USB serial / BLE), see store.h ----
// Ranges mirror RNS/Interfaces/RNodeInterface.py __init__: an out-of-range
// radio value raises inside rnsd's interface loop and ends in RNS.panic(),
// so the portal and S81rnode both reject them before rnsd ever sees them.
static bool rnode_num_in_range(const std::string& s, long lo, long hi) {
    if (!all_digits(s) || s.size() > 10) return false;
    long v = strtol(s.c_str(), nullptr, 10);
    return v >= lo && v <= hi;
}

static bool rnode_valid_ble_address(const std::string& s) {
    // AA:BB:CC:DD:EE:FF — what RNodeInterface's ble:// parser expects
    // (6 colon groups, 17 chars); it uppercases internally.
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!isxdigit((unsigned char)c)) return false;
    }
    return true;
}

RnodeConfig read_rnode() {
    RnodeConfig c;
    for (const auto& ln : read_lines(RNODE_FILE)) {
        std::string s = util::trim(ln);
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq));
        std::string v = s.substr(eq + 1);
        size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
        v = util::trim(v);
        if (k == "transport") c.transport = v;
        else if (k == "device") c.device = v;
        else if (k == "ble_address") c.ble_address = v;
        else if (k == "ble_name") c.ble_name = v;
        else if (k == "name") c.name = v;
        else if (k == "frequency") c.frequency = v;
        else if (k == "bandwidth") c.bandwidth = v;
        else if (k == "txpower") c.txpower = v;
        else if (k == "spreadingfactor") c.spreadingfactor = v;
        else if (k == "codingrate") c.codingrate = v;
        else if (k == "flow_control") c.flow_control = v;
    }
    return c;
}

bool write_ble(const BleConfig& cfg, std::string& err) {
    std::string en = cron_truthy(cfg.enabled) ? "yes" : "no";
    // The name lands inside a config value and is advertised over the air:
    // no line breaks/control chars (conf_value_ok), none of the config
    // syntax chars, bounded length (BLE ad payloads are tiny anyway).
    std::string name = util::trim(cfg.device_name);
    if (!name.empty()) {
        if (!conf_value_ok(name)) { err = "device name must not contain line breaks or control characters"; return false; }
        if (name.find_first_of("[]#=\"'") != std::string::npos) { err = "device name must not contain []#= or quote characters"; return false; }
        if (name.size() > 16) { err = "device name too long (max 16 characters)"; return false; }
    }
    auto lines = read_lines(RETICULUM_CONFIG);
    if (lines.empty()) { err = "no reticulum config"; return false; }

    bool in = false, saw_section = false, saw_enabled = false, saw_name = false;
    size_t insert_after = std::string::npos;   // line index a missing key goes under
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string& ln = lines[i];
        std::string s = util::trim(ln);
        if (!s.empty() && s[0] == '[') {
            std::string low = s;
            for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
            in = (low.find(BLE_SECTION) != std::string::npos);
            if (in) saw_section = true;
            continue;
        }
        if (!in) continue;
        // Remember the stanza's first key line: a key the config lacks (a
        // hand-trimmed file) is inserted under it rather than dropping the
        // save or appending outside the stanza.
        if (!s.empty() && s[0] != '#' && insert_after == std::string::npos) insert_after = i;
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::trim(s.substr(0, eq));
        for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
        if (k != "enabled" && k != "device_name") continue;
        std::string indent;
        for (char ch : ln) { if (ch == ' ' || ch == '\t') indent += ch; else break; }
        if (k == "enabled") {
            ln = indent + "enabled = " + en;
            saw_enabled = true;
        } else {
            // Blank = advertise the service UUID only, the driver default:
            // drop the key entirely (the form reads back blank).
            if (!name.empty()) ln = indent + "device_name = " + name;
            else ln.clear();
            saw_name = true;
        }
    }
    if (!saw_section) { err = "no [[BLE Interface]] stanza in the reticulum config"; return false; }
    if (!saw_enabled) { err = "[[BLE Interface]] stanza is missing the enabled key"; return false; }
    if (!saw_name && !name.empty()) {
        if (insert_after == std::string::npos) { err = "[[BLE Interface]] stanza has no keys to anchor device_name under"; return false; }
        lines.insert(lines.begin() + insert_after + 1, "    device_name = " + name);
    }
    // A cleared device_name left an empty line behind: drop it so a
    // save-toggle-save cycle never grows the file.
    lines.erase(std::remove(lines.begin(), lines.end(), std::string()), lines.end());

    std::string o;
    for (const auto& l : lines) o += l + "\n";
    if (!util::write_file_atomic(RETICULUM_CONFIG, o, 0600)) { err = "write failed"; return false; }  // see write_reticulum_config
    return true;
}
bool write_rnode(const RnodeConfig& c, std::string& err) {
    std::string transport = util::trim(c.transport);
    for (auto& ch : transport) ch = (char)tolower((unsigned char)ch);
    if (transport != "off" && transport != "usb" && transport != "ble") {
        err = "transport must be off, usb or ble"; return false;
    }

    // [[section]] name: no [ ] (header line), no control chars (config line
    // injection), collapsed whitespace, bounded — mirrors S81rnode's tr.
    std::string name;
    {
        std::string n;
        for (unsigned char ch : c.name)
            if (ch != '[' && ch != ']' && !(ch < 0x20 && ch != '\t') && ch != 0x7f) n += (char)ch;
        for (const auto& p : split_ws(n)) { if (!name.empty()) name += " "; name += p; }
        if (name.size() > 48) name = name.substr(0, 48);
    }
    if (name.empty()) name = "RNode";

    std::string dev = util::trim(c.device);
    bool usb = (transport == "usb");
    if (usb && dev.empty()) dev = "auto";
    if (usb && dev != "auto" && !valid_tty(dev)) { err = "bad device path"; return false; }

    std::string ble_addr = util::trim(c.ble_address);
    bool ble = (transport == "ble");
    if (ble && !ble_addr.empty() && !rnode_valid_ble_address(ble_addr)) {
        err = "BLE address must look like AA:BB:CC:DD:EE:FF"; return false;
    }
    // ble://<name> is matched verbatim by RNS's scanner; keep it one line
    // and short (conf_value_ok rejects control chars incl. CR/LF).
    std::string ble_name = util::trim(c.ble_name);
    if (ble && ble_name.size() > 48) { err = "BLE name too long (max 48)"; return false; }
    if (ble && !ble_name.empty() && !conf_value_ok(ble_name)) {
        err = "BLE name must not contain control characters"; return false;
    }

    std::string freq = util::trim(c.frequency);
    if (!rnode_num_in_range(freq, 137000000, 3000000000)) { err = "frequency must be 137000000..3000000000 Hz"; return false; }
    std::string bw = util::trim(c.bandwidth);
    if (!rnode_num_in_range(bw, 7800, 1625000)) { err = "bandwidth must be 7800..1625000 Hz"; return false; }
    std::string tx = util::trim(c.txpower);
    if (!rnode_num_in_range(tx, 0, 37)) { err = "txpower must be 0..37 dBm"; return false; }
    std::string sf = util::trim(c.spreadingfactor);
    if (!rnode_num_in_range(sf, 5, 12)) { err = "spreading factor must be 5..12"; return false; }
    std::string cr = util::trim(c.codingrate);
    if (!rnode_num_in_range(cr, 5, 8)) { err = "coding rate must be 5..8"; return false; }

    std::string flow = cron_truthy(c.flow_control) ? "yes" : "no";

    std::string o =
        "# RNode LoRa modem over USB (CDC-ACM / USB-serial) or BLE.\n"
        "# Managed by the rnsbox-portal Reticulum tab; applied by S81rnode,\n"
        "# which generates the [[RNode ...]] section in /etc/reticulum/config\n"
        "# between its BEGIN/END RNODE markers. BLE needs the BLE stack in the\n"
        "# image (bluez/dbus + bleak) and a one-time pairing (RNS attaches to\n"
        "# bonded devices only).\n";
    o += "transport=" + transport + "\n";
    o += "device=" + (usb ? dev : std::string("auto")) + "\n";
    o += "ble_address=" + (ble ? ble_addr : std::string("")) + "\n";
    o += "ble_name=" + (ble ? ble_name : std::string("")) + "\n";
    o += "name=" + name + "\n";
    o += "frequency=" + freq + "\n";
    o += "bandwidth=" + bw + "\n";
    o += "txpower=" + tx + "\n";
    o += "spreadingfactor=" + sf + "\n";
    o += "codingrate=" + cr + "\n";
    o += "flow_control=" + flow + "\n";
    if (!util::write_file_atomic(RNODE_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}

RnodeStatus read_rnode_status() {
    RnodeStatus st;
    std::string data = util::read_file(RNODE_STATUS, 4096);
    st.present = !data.empty();
    size_t i = 0;
    while (i <= data.size()) {
        size_t nl = data.find('\n', i);
        std::string ln = util::trim(data.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? data.size() + 1 : nl + 1;
        size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::trim(ln.substr(0, eq)), v = util::trim(ln.substr(eq + 1));
        if (k == "transport") st.transport = v;
        else if (k == "port") st.port = v;
        else if (k == "state") st.state = v;
        else if (k == "message") st.message = v;
    }
    return st;
}

std::vector<std::string> rnode_serial_devices() {
    std::vector<std::string> out;
    DIR* d = opendir("/dev");
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        bool acm = n.rfind("ttyACM", 0) == 0;
        bool usbs = n.rfind("ttyUSB", 0) == 0;
        if (!acm && !usbs) continue;
        std::string rest = n.substr(acm ? 6 : 6);
        if (rest.empty() || rest.find_first_not_of("0123456789") != std::string::npos) continue;
        out.push_back("/dev/" + n);
    }
    closedir(d);
    // native CDC-ACM first (nRF52 boards), then USB-UART bridges, numeric order
    for (size_t i = 0; i + 1 < out.size(); ++i)
        for (size_t j = 0; j + 1 < out.size() - i; ++j)
            if (out[j].compare(out[j + 1]) > 0) std::swap(out[j], out[j + 1]);
    return out;
}

bool rnode_ble_stack_present() {
    // bleak (RNS imports it in BLEConnection and panics without it)
    bool bleak = false;
    for (size_t ver = 8; ver <= 14; ++ver) {
        struct stat st;
        std::string p = "/usr/lib/python3." + std::to_string(ver) + "/site-packages/bleak/__init__.py";
        if (::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) { bleak = true; break; }
    }
    if (!bleak) return false;
    // bluetoothd (buildroot bluez: /usr/libexec/bluetooth/bluetoothd) plus a
    // system bus socket for it (packaged by the ble branch)
    bool daemon = ::access("/usr/libexec/bluetooth/bluetoothd", F_OK) == 0 ||
                  ::access("/usr/bin/bluetoothd", F_OK) == 0 ||
                  ::access("/usr/sbin/bluetoothd", F_OK) == 0;
    if (!daemon) return false;
    return ::access("/run/dbus/system_bus_socket", F_OK) == 0 ||
           ::access("/var/run/dbus/system_bus_socket", F_OK) == 0;
}

}  // namespace store
