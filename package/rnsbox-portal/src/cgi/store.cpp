// store.cpp — see store.h. Formats mirror store.py exactly.
#include "store.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

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
    if (c.interface != "eth0" && c.interface != "wlan0") c.interface = "eth0";
    if (c.mode != "dhcp" && c.mode != "static") c.mode = "dhcp";
    return c;
}

bool write_wan(const WanConfig& cfg, std::string& err) {
    if (cfg.interface != "eth0" && cfg.interface != "wlan0") { err = "WAN interface must be eth0 or wlan0"; return false; }
    if (cfg.mode != "dhcp" && cfg.mode != "static") { err = "WAN mode must be dhcp or static"; return false; }
    if (cfg.mode == "static") {
        if (!valid_ipv4(cfg.address)) { err = "static address is not a valid IPv4"; return false; }
        int pfx = -1; to_int(cfg.prefix, pfx);
        if (pfx < 0 || pfx > 32) { err = "prefix must be 0-32"; return false; }
        if (!cfg.gateway.empty() && !valid_ipv4(cfg.gateway)) { err = "gateway is not a valid IPv4"; return false; }
        if (!valid_dns_list(cfg.dns)) { err = "dns must be space-separated IPv4 addresses"; return false; }
    }
    std::string o =
        "# WAN uplink config — managed by the rnsbox-portal Network tab.\n"
        "# interface: eth0 (wired RJ45) or wlan0 (WiFi client, requires STA mode).\n"
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
    if (!util::write_file_atomic(RETICULUM_CONFIG, norm, 0644)) { err = "write failed"; return false; }
    return true;
}

// ---- NTP servers ----
static bool valid_ntp_server(const std::string& s) {
    if (s.empty() || s.size() > 253) return false;
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
    int baud = atoi(c.baud.c_str());
    if (baud < 1 || baud > 4000000) { err = "baud out of range (1-4000000)"; return false; }
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
        if (is_tcp && cur_host == host && cur_port == ps) return true;
    }
    return false;
}

bool add_halow_interface(const std::string& name_in, const std::string& host, int port, std::string& msg) {
    if (!valid_ipv4(host)) { msg = "bad modem IP"; return false; }
    if (port < 1 || port > 65535) { msg = "bad port"; return false; }
    auto lines = read_lines(RETICULUM_CONFIG);
    if (lines.empty()) { msg = "no reticulum config"; return false; }
    if (reticulum_has_tcp_client(host, port)) { msg = "already"; return false; }
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
    if (!util::write_file_atomic(RETICULUM_CONFIG, o, 0644)) { msg = "write failed"; return false; }
    msg = name;
    return true;
}

}  // namespace store
