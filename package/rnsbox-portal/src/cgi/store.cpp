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
    // tcp/4242 — the rnsd TCPServerInterface — is how WAN Reticulum peers
    // reach this box. It is pinned: added here when the operator's file
    // lacks it, and delete_openport() refuses to drop it. nftgen and the
    // Network page both read this list, so the pin holds everywhere.
    bool have4242 = false;
    for (const auto& o : out) if (o.proto == "tcp" && o.port == 4242) have4242 = true;
    if (!have4242) out.push_back({"tcp", 4242, "rnsd TCP interface (always open)"});
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
    // The pinned rnsd TCP port survives every removal attempt.
    if (items[idx].proto == "tcp" && items[idx].port == 4242) return false;
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
            else if (k == "nat") c.nat = v;
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
    if (c.nat != "no") c.nat = "yes";   // absent/unknown -> the default
    return c;
}

bool write_wan(const WanConfig& in, std::string& err) {
    WanConfig cfg = in;
    if (cfg.interface != "auto" && cfg.interface != "eth0" && cfg.interface != "wlan0") { err = "WAN interface must be auto, eth0 or wlan0"; return false; }
    if (cfg.mode != "dhcp" && cfg.mode != "static") { err = "WAN mode must be dhcp or static"; return false; }
    if (cfg.nat != "yes" && cfg.nat != "no") { err = "NAT must be yes or no"; return false; }
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
        "# nat:       yes = masquerade the LAN out the WAN; no = route it\n"
        "#            without source NAT (the upstream needs a route back).\n"
        "# Static mode (applied to eth0 by S30eth): address/prefix/gateway/dns.\n";
    o += "interface=" + cfg.interface + "\n";
    o += "mode=" + cfg.mode + "\n";
    o += "nat=" + cfg.nat + "\n";
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

static bool hhmm_valid(const std::string& t) {
    if (t.size() != 5 || t[2] != ':') return false;
    for (size_t i = 0; i < 5; ++i) {
        if (i == 2) continue;
        if (t[i] < '0' || t[i] > '9') return false;
    }
    int h = (t[0] - '0') * 10 + (t[1] - '0');
    int m = (t[3] - '0') * 10 + (t[4] - '0');
    return h <= 23 && m <= 59;
}

int read_rnsd_restart_days() {
    int n = 0;
    if (to_int(util::trim(util::conf_get(CRON_FILE, "rnsd_restart_days")), n)) {
        if (n >= 0 && n <= 7) return n;
    }
    return 0;
}

std::string read_rnsd_restart_time() {
    std::string t = util::trim(util::conf_get(CRON_FILE, "rnsd_restart_time"));
    return hhmm_valid(t) ? t : std::string("04:00");
}

bool read_update_check() {
    std::string v = util::conf_get(CRON_FILE, "update_check");
    return v.empty() ? true : cron_truthy(v);   // default yes
}

bool read_update_install() {
    // absent/empty => false: automatic installation is opt-in.
    return cron_truthy(util::conf_get(CRON_FILE, "update_install"));
}

bool write_cron_settings(int days, const std::string& time, bool update_check,
                         bool update_install, std::string& err) {
    if (days < 0 || days > 7) { err = "days must be 0..7"; return false; }
    if (!hhmm_valid(time)) { err = "time must be HH:MM"; return false; }
    if (!update_check) update_install = false;   // install needs the daily check
    std::string o =
        "# RNSBox scheduled tasks — managed by the rnsbox-portal Reticulum tab.\n"
        "# rnsd_restart_days: 0 = off (default); N = restart rnsd every N days at\n"
        "#   rnsd_restart_time (HH:MM, default 04:00).\n"
        "# update_check: yes (default) / no — daily check for a newer rnsd and\n"
        "#   download its wheel. update_install: no (default) / yes — install the\n"
        "#   downloaded wheel automatically, rolling back if rnsd does not start.\n";
    o += "rnsd_restart_days=" + std::to_string(days) + "\n";
    o += "rnsd_restart_time=" + time + "\n";
    o += std::string("update_check=") + (update_check ? "yes" : "no") + "\n";
    o += std::string("update_install=") + (update_install ? "yes" : "no") + "\n";
    if (!util::write_file_atomic(CRON_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}

bool write_rnsd_restart_days(int days, const std::string& time, std::string& err) {
    return write_cron_settings(days, time, read_update_check(),
                               read_update_install(), err);
}

bool write_update_check(bool enabled, std::string& err) {
    // symmetric to write_rnsd_restart_days: preserve the schedule/install flag
    return write_cron_settings(read_rnsd_restart_days(), read_rnsd_restart_time(),
                               enabled, read_update_install(), err);
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
    // bound, the real ceiling is per board: every rnsbox CVITEK board
    // reclocks its UART clock to 96 MHz, where the modem's factory
    // 2 Mbaud is generated exactly. The one exception is the classic
    // LicheeRV Nano-e, whose 25 MHz UART clock tops out below 2 Mbaud —
    // use 1500000 there.
    int baud = 0;
    if (all_digits(c.baud) && c.baud.size() <= 8) baud = atoi(c.baud.c_str());
    if (baud < 1 || baud > SLIP_MAX_BAUD) { err = "baud out of range (1-" + std::to_string(SLIP_MAX_BAUD) + ")"; return false; }
    std::string lip = c.local_ip.empty() ? "192.168.7.1" : c.local_ip;
    std::string pip = c.peer_ip.empty() ? "192.168.7.2" : c.peer_ip;
    if (!valid_ipv4(lip) || !valid_ipv4(pip)) { err = "bad IP address"; return false; }
    std::string o =
        "# SLIP-over-UART link to an external WiFi-HaLow (RNode) modem.\n"
        "# Managed by the rnsbox-portal Reticulum tab; applied by S31slip. OFF\n"
        "# by default. baud must match the modem (2 Mbaud on every rnsbox\n"
        "# board except the classic LicheeRV Nano-e: use 1500000 there);\n"
        "# local_ip/peer_ip must match the modem's SLIP config.\n";
    o += "enabled=" + en + "\ndevice=" + dev + "\nbaud=" + std::to_string(baud) +
         "\nlocal_ip=" + lip + "\npeer_ip=" + pip + "\n";
    if (!util::write_file_atomic(SLIP_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}

// ---- USB role of the OTG port ----
// mode=device (default, the CDC-NCM gadget LAN on 10.42.0.1) | host. The file
// only exists once the portal has saved it; before that S07usbmode stays out
// of the way of the shipped defaults. Applied by S07usbmode.
UsbConfig read_usb() {
    UsbConfig c;
    std::string m = util::conf_get(USB_FILE, "mode");
    if (m == "host") c.mode = "host";   // anything else (missing/empty/typo) = device
    return c;
}

bool write_usb(const UsbConfig& c, std::string& err) {
    if (c.mode != "device" && c.mode != "host") { err = "bad mode"; return false; }
    std::string o =
        "# RNSBox USB role of the OTG port.\n"
        "#   mode=device — the port is a CDC-NCM gadget: the box's LAN\n"
        "#                (portal at http://10.42.0.1, see S08usbdev/S10usbdev).\n"
        "#   mode=host   — the port drives USB devices (sticks, modems, RNode)\n"
        "#                through an OTG adapter; the gadget LAN disappears.\n"
        "# Managed by the portal Settings tab; applied by S07usbmode (at boot,\n"
        "# and live on the CVITEK boards).\n";
    o += "mode=" + c.mode + "\n";
    if (!util::write_file_atomic(USB_FILE, o, 0644)) { err = "write failed"; return false; }
    return true;
}

// ---- system (OS image) update config ----
UpdateConfig read_update_config() {
    UpdateConfig d;
    for (const auto& ln : read_lines(UPDATE_FILE)) {
        std::string s = util::trim(ln);
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq));
        std::string v = s.substr(eq + 1);
        size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
        v = util::trim(v);
        if (k == "enabled") d.enabled = v;
        else if (k == "provider") d.provider = v;
        else if (k == "repo") d.repo = v;
        else if (k == "url") d.url = v;
        else if (k == "board") d.board = v;
        else if (k == "allow_prerelease") d.allow_prerelease = v;
        else if (k == "allow_insecure_http") d.allow_insecure_http = v;
        else if (k == "auto_apply") d.auto_apply = v;
        else if (k == "interval_min_days") d.interval_min_days = v;
        else if (k == "interval_max_days") d.interval_max_days = v;
        else if (k == "next_check") d.next_check = v;
    }
    return d;
}

bool write_update_config(const UpdateConfig& c, std::string& err) {
    // every value is a single clean line (no config injection via CR/LF)
    auto clean = [](const std::string& s) {
        return util::is_single_line_clean(s);
    };
    std::string provider = c.provider.empty() ? "github-releases" : c.provider;
    if (provider != "github-releases" && provider != "generic") {
        err = "unknown provider"; return false;
    }
    std::string board = c.board.empty() ? "auto" : c.board;
    if (board != "auto" && board != "rpi-universal" && board != "licheerv-nano") {
        err = "unknown board"; return false;
    }
    if (provider == "github-releases") {
        // owner/repo — the repo name goes into an API URL path only, but
        // keep it strict anyway: no '/', '\' or scheme tricks
        const std::string& r = c.repo;
        auto slash = r.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= r.size() ||
            r.find('/', slash + 1) != std::string::npos ||
            !clean(r) || r.find("..") != std::string::npos) {
            err = "repo must look like owner/repo"; return false;
        }
    } else {
        // generic: https-only unless explicitly allowed; the worker re-checks
        const std::string& u = c.url;
        bool http_ok = cron_truthy(c.allow_insecure_http);
        if (u.empty() || !clean(u) || u.size() > 900 ||
            (u.rfind("https://", 0) != 0 &&
             !(http_ok && u.rfind("http://", 0) == 0))) {
            err = "url must be an https:// manifest.json URL"; return false;
        }
    }
    long lo = -1, hi = -1;
    auto to_long = [](const std::string& s, long* out) {
        if (!all_digits(s) || s.empty() || s.size() > 3) return false;
        *out = atol(s.c_str()); return true;
    };
    to_long(c.interval_min_days, &lo);
    to_long(c.interval_max_days, &hi);
    if (lo < 1 || hi < 1 || lo > 180 || hi > 180 || lo > hi) {
        err = "interval must be 1..180 days, min <= max"; return false;
    }
    std::string o =
        "# RNSBox system (OS image) update settings - edited by the portal\n"
        "# Updates tab and by rnsbox-sysupdate (next_check is managed). Manual\n"
        "# edits are fine; values are documented in docs/AUTO-UPDATE.md.\n";
    auto yn = [](const std::string& v) -> std::string {
        return cron_truthy(v) ? "yes" : "no";
    };
    o += "enabled=" + yn(c.enabled) + "\n";
    o += "provider=" + provider + "\n";
    o += "repo=" + c.repo + "\n";
    o += "url=" + c.url + "\n";
    o += "board=" + board + "\n";
    o += "allow_prerelease=" + yn(c.allow_prerelease) + "\n";
    o += "allow_insecure_http=" + yn(c.allow_insecure_http) + "\n";
    o += "auto_apply=" + yn(c.auto_apply) + "\n";
    o += "interval_min_days=" + std::to_string(lo) + "\n";
    o += "interval_max_days=" + std::to_string(hi) + "\n";
    if (!c.next_check.empty() && all_digits(c.next_check))
        o += "next_check=" + c.next_check + "\n";
    // preserve keys we don't manage (token, sig_*) from the current file
    for (const auto& ln : read_lines(UPDATE_FILE)) {
        std::string s = util::trim(ln);
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        std::string k = util::trim(s.substr(0, s.find('=')));
        if (k == "token" || k == "sig_require" || k == "sig_key") o += s + "\n";
    }
    if (!util::write_file_atomic(UPDATE_FILE, o, 0644)) {
        err = "write failed"; return false;
    }
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


// ---- TCP client interfaces (see store.h) ---------------------------------
// A stanza is one [[name]] header .. the next '[' header / EOF. "type" is
// matched case-sensitively against TCPClientInterface (the RNS class name,
// like the HaLow scan above) but keys are compared lowercased.

namespace {

struct TcpStanza {
    std::string type, name, host, port;
    size_t header_line = 0;   // index of the [[name]] line (update/delete)
};

// Parse one key line inside a stanza; k is lowercased, v comment-stripped.
static void tcp_stanza_key(const std::string& ln, std::string& k, std::string& v) {
    size_t eq = ln.find('=');
    if (eq == std::string::npos) { k.clear(); v.clear(); return; }
    k = util::trim(ln.substr(0, eq));
    v = ln.substr(eq + 1);
    size_t h = v.find('#');
    if (h != std::string::npos) v = v.substr(0, h);
    v = util::trim(v);
    for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
}

static bool is_stanza_header(const std::string& s) {
    return !s.empty() && s[0] == '[';
}

static std::vector<TcpStanza> tcp_stanzas() {
    std::vector<TcpStanza> out;
    TcpStanza cur;
    bool in_stanza = false;
    auto flush = [&]() {
        if (in_stanza && cur.type == "TCPClientInterface" && !cur.host.empty())
            out.push_back(cur);
        cur = TcpStanza();
        in_stanza = false;
    };
    const auto lines = read_lines(RETICULUM_CONFIG);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string s = util::trim(lines[i]);
        if (is_stanza_header(s)) {
            flush();
            in_stanza = true;
            cur.header_line = i;
            if (s.rfind("[[", 0) == 0 && s.rfind("[[[", 0) != 0 &&
                s.size() >= 4 && s.substr(s.size() - 2) == "]]")
                cur.name = util::trim(s.substr(2, s.size() - 4));
            continue;
        }
        if (!in_stanza || s.empty() || s[0] == '#') continue;
        std::string k, v;
        tcp_stanza_key(s, k, v);
        if (k == "type") cur.type = v;
        else if (k == "target_host") cur.host = v;
        else if (k == "target_port") cur.port = v;
    }
    flush();
    return out;
}

// Fields of one add/update, validated. err set on refusal.
static bool tcp_client_fields(std::string name, const std::string& host,
                              const std::string& port, std::string& err) {
    if (!conf_value_ok(name) || !conf_value_ok(host) || !conf_value_ok(port)) {
        err = "fields must not contain line breaks or control characters";
        return false;
    }
    name = sanitize_iface_name(name);
    if (name.empty()) name = "TCP Client";
    if (host.empty() || host.size() > 253) { err = "bad target host"; return false; }
    for (char c : host)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == ':')) {
            err = "target host may only contain letters, digits, . - _ :"; return false;
        }
    if (host.find("..") != std::string::npos) { err = "bad target host"; return false; }
    if (!all_digits(port) || port.size() > 5) { err = "bad port"; return false; }
    long p = strtol(port.c_str(), nullptr, 10);
    if (p < 1 || p > 65535) { err = "port out of range"; return false; }
    return true;
}

static std::string tcp_client_name(std::string name, const std::string& keep) {
    name = sanitize_iface_name(name);
    if (name.empty()) name = "TCP Client";
    // unique [[section]] names: RNS keys interfaces by name. `keep` is the
    // name a stanza being updated already carries — renaming it to its own
    // current name must not trip the dedupe.
    auto existing = reticulum_iface_names();
    auto has = [&](const std::string& n) {
        if (n == keep) return false;
        for (const auto& e : existing) if (e == n) return true;
        return false;
    };
    if (!has(name)) return name;
    std::string base = name;
    for (int i = 2; has(name); ++i) name = base + " " + std::to_string(i);
    return name;
}

}  // namespace

std::vector<TcpClientIface> read_tcp_clients() {
    std::vector<TcpClientIface> out;
    for (const auto& s : tcp_stanzas())
        out.push_back({s.name, s.host, s.port});
    return out;
}

bool add_tcp_client(const std::string& name_in, const std::string& host,
                    const std::string& port, std::string& err) {
    if (!tcp_client_fields(name_in, host, port, err)) return false;
    auto lines = read_lines(RETICULUM_CONFIG);
    if (lines.empty()) { err = "no reticulum config"; return false; }
    std::string name = tcp_client_name(name_in, "");
    while (!lines.empty() && util::trim(lines.back()).empty()) lines.pop_back();
    lines.push_back("");
    lines.push_back("  [[" + name + "]]");
    lines.push_back("    type = TCPClientInterface");
    lines.push_back("    enabled = yes");
    lines.push_back("    target_host = " + host);
    lines.push_back("    target_port = " + std::to_string(strtol(port.c_str(), nullptr, 10)));
    std::string o;
    for (const auto& l : lines) o += l + "\n";
    if (!util::write_file_atomic(RETICULUM_CONFIG, o, 0600)) { err = "write failed"; return false; }  // see write_reticulum_config
    err.clear();
    return true;
}

// The stanza's line range: from its [[name]] header to the next header /
// EOF, trailing blank lines of the stanza dropped (they belong to nobody).
static bool tcp_stanza_range(const std::vector<std::string>& lines, int idx,
                             size_t& begin, size_t& end) {
    const auto stanzas = tcp_stanzas();
    if (idx < 0 || idx >= (int)stanzas.size()) return false;
    begin = stanzas[idx].header_line;
    end = lines.size();
    for (size_t i = begin + 1; i < lines.size(); ++i)
        if (is_stanza_header(util::trim(lines[i]))) { end = i; break; }
    while (end > begin + 1 && util::trim(lines[end - 1]).empty()) --end;
    return true;
}

bool update_tcp_client(int idx, const std::string& name_in, const std::string& host,
                       const std::string& port, std::string& err) {
    if (!tcp_client_fields(name_in, host, port, err)) return false;
    auto stanzas = tcp_stanzas();
    if (idx < 0 || idx >= (int)stanzas.size()) { err = "no such interface"; return false; }
    auto lines = read_lines(RETICULUM_CONFIG);
    size_t begin = 0, end = 0;
    if (!tcp_stanza_range(lines, idx, begin, end)) { err = "no such interface"; return false; }
    std::string name = tcp_client_name(name_in, stanzas[idx].name);
    // Header keeps its own indent; keys keep the stanza's key indent.
    std::string hind;
    for (char ch : lines[begin]) { if (ch == ' ' || ch == '\t') hind += ch; else break; }
    lines[begin] = hind + "[[" + name + "]]";
    std::string kind = "    ";
    bool have_host = false, have_port = false;
    size_t type_line = std::string::npos;
    for (size_t i = begin + 1; i < end; ++i) {
        std::string s = util::trim(lines[i]);
        if (s.empty() || s[0] == '#') continue;
        std::string ind;
        for (char ch : lines[i]) { if (ch == ' ' || ch == '\t') ind += ch; else break; }
        if (!ind.empty()) kind = ind;
        std::string k, v;
        tcp_stanza_key(s, k, v);
        if (k == "type") type_line = i;
        else if (k == "target_host") { lines[i] = ind + "target_host = " + host; have_host = true; }
        else if (k == "target_port") { lines[i] = ind + "target_port = " + std::to_string(strtol(port.c_str(), nullptr, 10)); have_port = true; }
    }
    // Missing keys are inserted after the type line (so a hand-trimmed
    // stanza still ends up valid), or at the stanza's end without one.
    size_t insert_at = (type_line != std::string::npos) ? type_line + 1 : end;
    if (!have_host) {
        lines.insert(lines.begin() + insert_at, kind + "target_host = " + host);
        ++insert_at;
    }
    if (!have_port)
        lines.insert(lines.begin() + insert_at,
                     kind + "target_port = " + std::to_string(strtol(port.c_str(), nullptr, 10)));
    std::string o;
    for (const auto& l : lines) o += l + "\n";
    if (!util::write_file_atomic(RETICULUM_CONFIG, o, 0600)) { err = "write failed"; return false; }  // see write_reticulum_config
    err.clear();
    return true;
}

bool save_tcp_clients(const std::vector<TcpClientIface>& list, std::string& err) {
    auto cur = read_tcp_clients();
    if (list.size() != cur.size()) { err = "interface list changed; reload the page"; return false; }
    for (const auto& t : list) {
        std::string e;
        if (!tcp_client_fields(t.name, t.host, t.port, e)) { err = e; return false; }
    }
    // Descending: inserting a missing key into an earlier stanza must not
    // shift the line range of a stanza still waiting to be written.
    for (int i = (int)list.size() - 1; i >= 0; --i) {
        std::string e;
        if (!update_tcp_client(i, list[i].name, list[i].host, list[i].port, e)) {
            err = e.empty() ? "write failed" : e;
            return false;
        }
    }
    return true;
}

bool delete_tcp_client(int idx, std::string& err) {
    auto lines = read_lines(RETICULUM_CONFIG);
    size_t begin = 0, end = 0;
    if (!tcp_stanza_range(lines, idx, begin, end)) { err = "no such interface"; return false; }
    // One blank separator above the stanza is ours too — drop it so repeated
    // add/delete cycles never grow the file.
    size_t from = begin;
    if (from > 0 && util::trim(lines[from - 1]).empty()) --from;
    lines.erase(lines.begin() + from, lines.begin() + end);
    std::string o;
    for (const auto& l : lines) o += l + "\n";
    if (!util::write_file_atomic(RETICULUM_CONFIG, o, 0600)) { err = "write failed"; return false; }  // see write_reticulum_config
    err.clear();
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
        else if (k == "advertise_interval_ms") {
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos)
                c.advertise_interval_ms = v;
        }
        else if (k == "enable_central") c.enable_central = cron_truthy(v) ? "yes" : "no";
    }
    return c;
}

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
        else if (k == "discover") c.discover = v;
        else if (k == "latitude") c.latitude = v;
        else if (k == "longitude") c.longitude = v;
        else if (k == "height") c.height = v;
        else if (k == "discovery_name") c.discovery_name = v;
        else if (k == "announce_interval") c.announce_interval = v;
        else if (k == "reachable_on") c.reachable_on = v;
    }
    return c;
}

bool write_ble(const BleConfig& cfg, std::string& err) {
    std::string en = cron_truthy(cfg.enabled) ? "yes" : "no";
    std::string ec = cron_truthy(cfg.enable_central) ? "yes" : "no";
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

    // LE advertisement interval: 20..10000 ms, or 0 (BlueZ picks). Same
    // rules on write as the driver enforces on read.
    std::string iv = util::trim(cfg.advertise_interval_ms);
    if (iv.empty() || iv.find_first_not_of("0123456789") != std::string::npos) iv = "100";
    {
        long v = strtol(iv.c_str(), nullptr, 10);
        if (v != 0 && (v < 20 || v > 10000)) { err = "advertising interval must be 20..10000 ms or 0"; return false; }
    }

    bool in = false, saw_section = false, saw_enabled = false, saw_name = false, saw_interval = false;
    bool saw_central = false;
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
        if (k != "enabled" && k != "device_name" && k != "advertise_interval_ms" && k != "enable_central") continue;
        std::string indent;
        for (char ch : ln) { if (ch == ' ' || ch == '\t') indent += ch; else break; }
        if (k == "enabled") {
            ln = indent + "enabled = " + en;
            saw_enabled = true;
        } else if (k == "advertise_interval_ms") {
            ln = indent + "advertise_interval_ms = " + iv;
            saw_interval = true;
        } else if (k == "enable_central") {
            ln = indent + "enable_central = " + ec;
            saw_central = true;
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
    if (!saw_interval) {
        if (insert_after == std::string::npos) { err = "[[BLE Interface]] stanza has no keys to anchor advertise_interval_ms under"; return false; }
        lines.insert(lines.begin() + insert_after + 1, "    advertise_interval_ms = " + iv);
    }
    if (!saw_name && !name.empty()) {
        if (insert_after == std::string::npos) { err = "[[BLE Interface]] stanza has no keys to anchor device_name under"; return false; }
        lines.insert(lines.begin() + insert_after + 1, "    device_name = " + name);
    }
    if (!saw_central) {
        if (insert_after == std::string::npos) { err = "[[BLE Interface]] stanza has no keys to anchor enable_central under"; return false; }
        lines.insert(lines.begin() + insert_after + 1, "    enable_central = " + ec);
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

    // Interface discovery: coordinates and the optional map metadata. Invalid
    // values are rejected here; rnsd would otherwise panic while parsing them.
    std::string discover = cron_truthy(c.discover) ? "yes" : "no";
    std::string lat = util::trim(c.latitude), lon = util::trim(c.longitude);
    std::string hgt = util::trim(c.height), dname = util::trim(c.discovery_name);
    std::string reach = util::trim(c.reachable_on);
    std::string interval = util::trim(c.announce_interval);
    if (!rnode_num_in_range(interval, 5, 10080)) {
        err = "discovery announce interval must be 5..10080 minutes"; return false;
    }
    auto float_ok = [](const std::string& s, double lo, double hi, double& out) {
        if (s.empty()) return false;
        char* end = nullptr;
        double v = strtod(s.c_str(), &end);
        if (!end || *end != '\0') return false;
        out = v;
        return v >= lo && v <= hi;
    };
    if (discover == "yes") {
        double v = 0;
        if (!float_ok(lat, -90.0, 90.0, v)) { err = "latitude must be -90..90"; return false; }
        if (!float_ok(lon, -180.0, 180.0, v)) { err = "longitude must be -180..180"; return false; }
        if (!hgt.empty() && !float_ok(hgt, -1000.0, 10000.0, v)) {
            err = "height must be -1000..10000 metres"; return false;
        }
    }
    if (dname.size() > 64 || !conf_value_ok(dname) ||
        dname.find_first_of("[]#=\"'") != std::string::npos) {
        err = "discovery name must be a single clean line (max 64)"; return false;
    }
    if (reach.size() > 64 || !conf_value_ok(reach) ||
        reach.find_first_of("[]#=\"'") != std::string::npos) {
        err = "reachable_on must be a single clean line (max 64)"; return false;
    }

    std::string o =
        "# RNode LoRa modem over USB (CDC-ACM / USB-serial) or BLE.\n"
        "# Managed by the rnsbox-portal Reticulum tab; applied by S81rnode,\n"
        "# which generates the [[RNode ...]] section in /etc/reticulum/config\n"
        "# between its BEGIN/END RNODE markers. BLE needs the BLE stack in the\n"
        "# image (bluez/dbus + bleak); rnsd connects and bonds the peer on its\n"
        "# own (BlueZ JustWorks via the bundled agent).\n";
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
    o += "discover=" + discover + "\n";
    o += "latitude=" + lat + "\n";
    o += "longitude=" + lon + "\n";
    o += "height=" + hgt + "\n";
    o += "discovery_name=" + dname + "\n";
    o += "announce_interval=" + interval + "\n";
    o += "reachable_on=" + reach + "\n";
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

std::string bt_peer_bond(const std::string& address) {
    std::string addr;
    for (char c : address) {
        if (c == ':') { addr += c; continue; }
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) addr += c;
    }
    if (addr.size() != 17) return "";
    DIR* d = opendir("/var/lib/bluetooth");
    if (!d) return "";
    bool found = false, paired = false, bonded = false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string info = std::string("/var/lib/bluetooth/") + e->d_name + "/" + addr + "/info";
        std::string data = util::read_file(info);
        if (data.empty()) continue;
        found = true;
        size_t i = 0;
        while (i < data.size()) {
            size_t nl = data.find('\n', i);
            std::string ln = util::trim(data.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
            i = (nl == std::string::npos) ? data.size() : nl + 1;
            if (ln == "Paired: yes") paired = true;
            else if (ln == "Bonded: yes") bonded = true;
        }
        if (bonded) break;
    }
    closedir(d);
    if (!found) return "none";
    if (bonded) return "bonded";
    if (paired) return "paired";
    return "none";
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

// ---- SSH server gate ----
SshConfig read_ssh() {
    SshConfig c;
    std::string v = util::conf_get(SSH_FILE, "enabled");
    for (auto& ch : v) ch = (char)tolower((unsigned char)ch);
    c.enabled = !(v == "no" || v == "0" || v == "false" || v == "off" || v.empty());
    return c;
}

bool write_ssh(bool enabled, std::string& err) {
    (void)err;
    std::string o =
        "# SSH server (sshd) gate — read by /etc/init.d/S50sshd at boot and\n"
        "# rewritten by the rnsbox-portal Settings -> SSH card. SSH is OFF\n"
        "# unless enabled=yes. The WAN side additionally stays closed unless\n"
        "# tcp 22 is opened under Network -> Open ports.\n";
    o += std::string("enabled=") + (enabled ? "yes" : "no") + "\n";
    return util::write_file_atomic(SSH_FILE, o, 0644);
}

}  // namespace store
