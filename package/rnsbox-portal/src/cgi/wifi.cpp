// wifi.cpp — see wifi.h. Native port of wifi.py (iw parsing by hand, no regex).
#include "wifi.h"
#include "util.h"
#include "http.h"
#include "sysinfo.h"
#include "web.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

namespace wifi {

// No AP address/DHCP keys: the hotspot is a port of the LAN bridge (br-lan,
// 10.42.0.1/24, one dnsmasq pool). Old ap_ip / ap_dhcp_start / ap_dhcp_end
// lines are ignored on read and dropped by the next write_conf().
static const char* KEYS[] = {
    "mode", "sta_ssid", "sta_psk", "sta_hidden", "sta_country",
    "ap_ssid", "ap_psk", "ap_channel", "ap_htmode", "ap_hidden",
    "tx_power",
};

bool present() {
    DIR* d = ::opendir("/sys/class/net");
    if (!d) return false;
    bool found = false;
    for (dirent* e; (e = ::readdir(d)) != nullptr; )
        if (strncmp(e->d_name, "wlan", 4) == 0) { found = true; break; }
    ::closedir(d);
    return found;
}

std::map<std::string, std::string> read_conf() {
    std::map<std::string, std::string> cfg;
    for (const char* k : KEYS) cfg[k] = "";
    cfg["mode"] = "off"; cfg["ap_ssid"] = "RNSBox-AP"; cfg["ap_channel"] = "6";
    cfg["ap_htmode"] = "ht20"; cfg["tx_power"] = "auto";
    std::string data = util::read_file(CONF);
    size_t i = 0;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        if (nl == std::string::npos) nl = data.size();
        std::string s = util::trim(data.substr(i, nl - i));
        i = nl + 1;
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq));
        std::string v = s.substr(eq + 1);
        size_t h = v.find('#'); if (h != std::string::npos) v = v.substr(0, h);
        v = util::trim(v);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        if (cfg.count(k)) cfg[k] = v;
    }
    return cfg;
}

bool conf_value_ok(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f || c == '#' || c == '"') return false;
    if (!s.empty() && (isspace((unsigned char)s.front()) || isspace((unsigned char)s.back())))
        return false;
    // read_conf() also strips a matching pair of surrounding single quotes
    // (S35wifi doesn't) — refuse the one value that would read back differently.
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') return false;
    return true;
}

bool ap_psk_ok(const std::string& psk) {
    if (psk.size() < 8 || psk.size() > 63) return false;          // WPA2 passphrase length
    for (unsigned char c : psk) if (c < 0x20 || c > 0x7e) return false;   // printable ASCII
    return conf_value_ok(psk) && psk != OLD_DEFAULT_AP_PSK;
}

bool write_conf(const std::map<std::string, std::string>& updates) {
    auto cfg = read_conf();
    for (const auto& kv : updates) {
        if (!conf_value_ok(kv.second)) return false;   // no line injection / truncation
        if (cfg.count(kv.first)) cfg[kv.first] = kv.second;
    }
    std::string o =
        "# RNSBox WiFi config — managed by rnsbox-portal.\n"
        "# mode: off | sta | ap | sta+ap\n\n";
    o += "mode=" + cfg["mode"] + "\n\n";
    o += "# --- STA (client) ---\n";
    for (const char* k : {"sta_ssid", "sta_psk", "sta_hidden", "sta_country"})
        o += std::string(k) + "=" + cfg[k] + "\n";
    o += "\n# --- AP (hotspot) ---\n";
    o += "# Bridged into the LAN (br-lan): clients share 10.42.0.0/24 and its DHCP pool.\n";
    o += "# ap_psk: 8..63 chars — the hotspot will not start without one.\n";
    for (const char* k : {"ap_ssid", "ap_psk", "ap_channel", "ap_htmode", "ap_hidden"})
        o += std::string(k) + "=" + cfg[k] + "\n";
    o += "\n# --- TX power (radio-wide): auto = chip max, or 1..18 dBm ---\n";
    o += "tx_power=" + cfg["tx_power"] + "\n";
    // The file holds both WiFi passwords: 0600, never wider than it already is
    // (the old portal wrote 0644 — this narrows those on the next save).
    int mode = 0600;
    struct stat st;
    if (::stat(CONF, &st) == 0) mode &= (int)(st.st_mode & 0777);
    // Drop a stale .tmp so write_file_atomic's O_CREAT makes it fresh at `mode`
    // (an existing one would keep its old, possibly 0644, mode until chmod).
    ::unlink((std::string(CONF) + ".tmp").c_str());
    return util::write_file_atomic(CONF, o, mode);
}

// ---- iw output parsing helpers ----
static std::string after_token(const std::string& s, const char* tok, size_t from = 0) {
    // return the numeric/token run after the FIRST occurrence of tok
    size_t p = s.find(tok, from);
    if (p == std::string::npos) return "";
    p += strlen(tok);
    size_t e = p;
    while (e < s.size() && (isdigit((unsigned char)s[e]) || s[e] == '-' || s[e] == '.')) ++e;
    return s.substr(p, e - p);
}

static std::string line_value(const std::string& block, const char* prefix) {
    // first line whose trimmed form starts with prefix -> the remainder trimmed
    size_t i = 0;
    size_t plen = strlen(prefix);
    while (i < block.size()) {
        size_t nl = block.find('\n', i);
        if (nl == std::string::npos) nl = block.size();
        std::string t = util::trim(block.substr(i, nl - i));
        i = nl + 1;
        if (t.size() >= plen && t.compare(0, plen, prefix) == 0)
            return util::trim(t.substr(plen));
    }
    return "";
}

struct Net { std::string ssid, signal, channel, band, security; };

static Net parse_block(const std::string& block) {
    Net n; n.security = "open";
    n.signal = after_token(block, "signal: ");
    if (block.find("RSN:") != std::string::npos || block.find("WPA:") != std::string::npos)
        n.security = "secured";
    // first "channel N"
    size_t cp = 0;
    while ((cp = block.find("channel ", cp)) != std::string::npos) {
        size_t s = cp + 8;
        if (s < block.size() && isdigit((unsigned char)block[s])) {
            size_t e = s; while (e < block.size() && isdigit((unsigned char)block[e])) ++e;
            n.channel = block.substr(s, e - s); break;
        }
        cp += 8;
    }
    n.ssid = line_value(block, "SSID:");
    // Band (and, when there's no "DS Parameter set: channel N" — i.e. every
    // 5/6 GHz BSS — the channel too) from the "freq: 5240" line (iw 6.x prints
    // "5240.0"). Band comes from freq first: 6 GHz channel numbers overlap the
    // 2.4 GHz ones, so "channel <= 14" alone can't tell them apart.
    int f = (int)atof(line_value(block, "freq:").c_str());
    int ch = 0;
    if (f == 2484)                  { n.band = "2.4"; ch = 14; }
    else if (f >= 2412 && f < 2484) { n.band = "2.4"; ch = (f - 2407) / 5; }
    else if (f > 5925 && f <= 7125) { n.band = "6";   ch = (f - 5950) / 5; }
    else if (f > 5000 && f <= 5925) { n.band = "5";   ch = (f - 5000) / 5; }
    if (n.channel.empty() && ch > 0) n.channel = std::to_string(ch);
    if (n.band.empty() && !n.channel.empty()) n.band = (atoi(n.channel.c_str()) <= 14) ? "2.4" : "5";
    return n;
}

static double sig_val(const std::string& s) { return s.empty() ? -999.0 : atof(s.c_str()); }

std::string scan_json() {
    if (!present()) return "[]";
    util::run({"/sbin/ip", "link", "set", STA_IF, "up"}, "", 5);
    std::string out = "\n" + util::run({"/usr/sbin/iw", "dev", STA_IF, "scan"}, "", 15).out;
    // split on "\nBSS "
    std::map<std::string, Net> nets;
    size_t i = out.find("\nBSS ");
    while (i != std::string::npos) {
        size_t next = out.find("\nBSS ", i + 5);
        std::string block = out.substr(i + 5, (next == std::string::npos ? out.size() : next) - (i + 5));
        Net e = parse_block(block);
        if (!e.ssid.empty()) {
            auto it = nets.find(e.ssid);
            if (it == nets.end()) nets[e.ssid] = e;
            else {
                bool prev2 = it->second.band == "2.4", ent2 = e.band == "2.4";
                if (ent2 && !prev2) it->second = e;
                else if (ent2 == prev2 && sig_val(e.signal) > sig_val(it->second.signal)) it->second = e;
            }
        }
        i = next;
    }
    std::vector<Net> list;
    for (auto& kv : nets) list.push_back(kv.second);
    std::sort(list.begin(), list.end(), [](const Net& a, const Net& b) { return sig_val(a.signal) > sig_val(b.signal); });
    std::string o = "[";
    for (size_t k = 0; k < list.size(); ++k) {
        if (k) o += ",";
        o += "{\"ssid\":\"" + http::json_escape(list[k].ssid) +
             "\",\"signal\":\"" + http::json_escape(list[k].signal) +
             "\",\"channel\":\"" + http::json_escape(list[k].channel) +
             "\",\"band\":\"" + http::json_escape(list[k].band) +
             "\",\"security\":\"" + http::json_escape(list[k].security) + "\"}";
    }
    o += "]";
    return o;
}

static std::string ip_of(const std::string& dev) {
    std::string out = util::run({"/sbin/ip", "-4", "addr", "show", "dev", dev}, "", 5).out;
    size_t p = out.find("inet ");
    if (p == std::string::npos) return "";
    p += 5; size_t e = p;
    while (e < out.size() && out[e] != '/' && out[e] != ' ') ++e;
    return out.substr(p, e - p);
}

// The running AP's iface: what S35wifi records in /run/wifi-ap-iface while
// an AP is up (wlan1 in sta+ap, wlan0 in ap-only); failing that, a wlan* port
// of the LAN bridge — only an AP vif can be bridged (a STA would need 4addr).
static std::string ap_iface() {
    std::string a = util::trim(util::read_file("/run/wifi-ap-iface", 64));
    if (sysinfo::ifname_ok(a) && !util::read_file("/sys/class/net/" + a + "/ifindex", 32).empty())
        return a;
    for (const auto& p : sysinfo::bridge_ports(sysinfo::lan_iface()))
        if (p.compare(0, 4, "wlan") == 0) return p;
    return "";
}

Status status() {
    Status s;
    auto cfg = read_conf();
    s.present = present();
    s.mode = cfg["mode"];
    s.concurrent = (cfg["mode"] == "sta+ap");
    s.tx_power = cfg["tx_power"].empty() ? "auto" : cfg["tx_power"];
    if (!s.present) return s;
    std::string link = util::run({"/usr/sbin/iw", "dev", STA_IF, "link"}, "", 5).out;
    if (link.find("Connected to") != std::string::npos) {
        std::string sta_info = util::run({"/usr/sbin/iw", "dev", STA_IF, "info"}, "", 5).out;
        s.sta.connected = true;
        s.sta.ssid = line_value(link, "SSID:");
        s.sta.signal = after_token(link, "signal: ");
        s.sta.ip = ip_of(STA_IF);
        s.sta.width = after_token(sta_info, "width: ");
        s.sta.txpower = after_token(sta_info, "txpower ");
    }
    std::string ap_if = ap_iface();
    if (!ap_if.empty()) {
        std::string dump = util::run({"/usr/sbin/iw", "dev", ap_if, "station", "dump"}, "", 5).out;
        std::string ap_info = util::run({"/usr/sbin/iw", "dev", ap_if, "info"}, "", 5).out;
        int clients = 0; size_t sp = 0;
        while ((sp = dump.find("Station ", sp)) != std::string::npos) { ++clients; sp += 8; }
        s.ap.active = true;
        s.ap.iface = ap_if;
        s.ap.clients = clients;
        // hostapd (bridge=br-lan) enslaves the AP iface, which then holds no
        // address: its clients reach the box at the bridge's.
        s.ap.bridge = sysinfo::bridge_of(ap_if);
        s.ap.ip = ip_of(ap_if);
        if (s.ap.ip.empty() && !s.ap.bridge.empty()) s.ap.ip = ip_of(s.ap.bridge);
        s.ap.width = after_token(ap_info, "width: ");
        s.ap.txpower = after_token(ap_info, "txpower ");
    }
    return s;
}

std::string status_json() {
    Status s = status();
    std::string o = std::string("{\"present\":") + (s.present ? "true" : "false") +
        ",\"mode\":\"" + http::json_escape(s.mode) + "\"" +
        ",\"concurrent\":" + (s.concurrent ? "true" : "false") +
        ",\"tx_power\":\"" + http::json_escape(s.tx_power) + "\"";
    if (!s.present) { o += ",\"sta\":{\"connected\":false},\"ap\":{\"active\":false}}"; return o; }
    if (s.sta.connected) {
        o += ",\"sta\":{\"connected\":true,\"ssid\":\"" + http::json_escape(s.sta.ssid) +
             "\",\"signal\":\"" + http::json_escape(s.sta.signal) +
             "\",\"ip\":\"" + http::json_escape(s.sta.ip) +
             "\",\"width\":\"" + http::json_escape(s.sta.width) +
             "\",\"txpower\":\"" + http::json_escape(s.sta.txpower) + "\"}";
    } else {
        o += ",\"sta\":{\"connected\":false}";
    }
    if (s.ap.active) {
        o += ",\"ap\":{\"active\":true,\"iface\":\"" + http::json_escape(s.ap.iface) +
             "\",\"clients\":" + std::to_string(s.ap.clients) +
             ",\"bridge\":\"" + http::json_escape(s.ap.bridge) +
             "\",\"ip\":\"" + http::json_escape(s.ap.ip) +
             "\",\"width\":\"" + http::json_escape(s.ap.width) +
             "\",\"txpower\":\"" + http::json_escape(s.ap.txpower) + "\"}";
    } else {
        o += ",\"ap\":{\"active\":false}";
    }
    o += "}";
    return o;
}

void apply() {
    web::run_init("S35wifi");
    web::run_init("S60routing");
}

}  // namespace wifi
