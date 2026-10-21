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
constexpr const char* EXTRA_CONF = "/etc/rnsbox/wifi-extra.conf";

static const char* KEYS[] = {
    "mode", "sta_ssid", "sta_psk", "sta_security", "sta_band",
    "sta_hidden", "sta_country",
    "ap_ssid", "ap_psk", "ap_security", "ap_band", "ap_standard",
    "ap_width", "ap_channel", "ap_htmode", "ap_hidden",
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
    // ap_ssid default is EMPTY on purpose: S35wifi names the hotspot after
    // the device hostname when it's blank, so the field shows that instead.
    cfg["mode"] = "off"; cfg["ap_ssid"] = ""; cfg["ap_channel"] = "6";
    cfg["ap_htmode"] = "ht20"; cfg["tx_power"] = "auto";
    cfg["sta_security"] = "wpa2"; cfg["sta_band"] = "auto";
    cfg["ap_security"] = "wpa3"; cfg["ap_band"] = "auto";
    cfg["ap_standard"] = "auto"; cfg["ap_width"] = "20";
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
    // Normalize / migrate the settings added after the first image:
    // ap_htmode (ht20|ht40) was the old width knob; ap_width (20|40|80) is
    // the current one. Keep both in sync so an older S35wifi still gets a
    // sane width if the portal is the only thing updated.
    if (cfg["sta_security"] != "open" && cfg["sta_security"] != "wpa3" &&
        cfg["sta_security"] != "wpa2+wpa3")
        cfg["sta_security"] = "wpa2";
    if (cfg["sta_band"] != "2.4" && cfg["sta_band"] != "5")
        cfg["sta_band"] = "auto";
    if (cfg["ap_security"] != "wpa2" && cfg["ap_security"] != "wpa3" &&
        cfg["ap_security"] != "wpa2+wpa3")
        cfg["ap_security"] = "wpa3";
    if (cfg["ap_band"] != "2.4" && cfg["ap_band"] != "5")
        cfg["ap_band"] = "auto";
    if (cfg["ap_standard"] != "n" && cfg["ap_standard"] != "ac" &&
        cfg["ap_standard"] != "ax")
        cfg["ap_standard"] = "auto";
    if (cfg["ap_width"] != "40" && cfg["ap_width"] != "80")
        cfg["ap_width"] = "20";
    // 2.4 GHz cannot do 802.11ac, and 80 MHz needs ac/ax on 5 GHz.
    if (cfg["ap_band"] != "5" && cfg["ap_standard"] == "ac")
        cfg["ap_standard"] = "auto";
    if (cfg["ap_width"] == "80" &&
        (cfg["ap_band"] != "5" || (cfg["ap_standard"] != "ac" && cfg["ap_standard"] != "ax")))
        cfg["ap_width"] = "20";
    cfg["ap_htmode"] = (cfg["ap_width"] == "40") ? "ht40" : "ht20";
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
    o += "# sta_security: wpa2 | wpa3 | wpa2+wpa3 | open\n";
    o += "# sta_band: auto | 2.4 | 5 (auto = whichever BSS wins)\n";
    for (const char* k : {"sta_ssid", "sta_psk", "sta_security", "sta_band",
                          "sta_hidden", "sta_country"})
        o += std::string(k) + "=" + cfg[k] + "\n";
    o += "\n# --- AP (hotspot) ---\n";
    o += "# Bridged into the LAN (br-lan): clients share 10.42.0.0/24 and its DHCP pool.\n";
    o += "# ap_psk: 8..63 chars — the hotspot will not start without one.\n";
    o += "# ap_security: wpa2 | wpa3 | wpa2+wpa3 (no open hotspot).\n";
    o += "# ap_band: 2.4 | 5; ap_standard: auto | n | ac | ax; ap_width: 20 | 40 | 80.\n";
    o += "# ap_channel: 1..13 (2.4), 36..165 (5), or auto (ACS).\n";
    for (const char* k : {"ap_ssid", "ap_psk", "ap_security", "ap_band",
                          "ap_standard", "ap_width", "ap_channel", "ap_htmode",
                          "ap_hidden"})
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

// iw prints signal/txpower with two decimals ("-35.00 dBm", "18.00 dBm");
// the radio reports whole dBm and the fraction just flickers, so drop it.
static std::string int_signal(const std::string& s) {
    size_t dot = s.find('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

struct Net { std::string bssid, ssid, signal, channel, band, security; };

static Net parse_block(const std::string& block) {
    Net n; n.security = "Open";
    size_t bsp = block.find_first_of(" (\n");
    n.bssid = util::trim(block.substr(0, bsp));   // block starts with "BSS <addr>"
    n.signal = int_signal(after_token(block, "signal: "));
    // The scan table is where the user sees the security protocol (the join
    // form deliberately does not show it). Parse the RSN/WPA authentication
    // suite, not just "is there encryption": WPA2 vs WPA3 vs transition
    // decides the wpa_supplicant key_mgmt we store on "Use".
    std::string auth = line_value(block, "* Authentication suites:");
    if (block.find("RSN:") != std::string::npos) {
        if (auth.find("SAE") != std::string::npos)
            n.security = (auth.find("PSK") != std::string::npos) ? "WPA2/WPA3" : "WPA3";
        else if (auth.find("OWE") != std::string::npos)
            n.security = "OWE";
        else if (auth.find("802.1X") != std::string::npos)
            n.security = "WPA2-Enterprise";
        else
            n.security = "WPA2";
    } else if (block.find("WPA:") != std::string::npos) {
        n.security = "WPA";
    } else if (block.find("Privacy") != std::string::npos) {
        n.security = "WEP";
    }
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

static const char* iw_path() {
    if (::access("/usr/sbin/iw", X_OK) == 0) return "/usr/sbin/iw";
    if (::access("/sbin/iw", X_OK) == 0) return "/sbin/iw";
    return nullptr;
}

std::string scan_json() {
    if (!present())
        return "{\"nets\":[],\"error\":\"no WiFi radio detected\"}";
    auto cfg = read_conf();
    // Scanning is a client-mode operation. Refuse it while STA is disabled
    // (mode off/ap): bringing wlan0 up and scanning would hop the single
    // radio away from the hotspot for no reason.
    if (cfg["mode"] != "sta" && cfg["mode"] != "sta+ap")
        return "{\"nets\":[],\"error\":\"STA mode is off — enable it above to scan\"}";
    const char* iw = iw_path();
    if (!iw)
        return "{\"nets\":[],\"error\":\"iw is not installed on this image\"}";

    util::RunResult up = util::run({"/sbin/ip", "link", "set", STA_IF, "up"}, "", 5);
    if (up.exit_code != 0) {
        std::string why = util::trim(up.out);
        return "{\"nets\":[],\"error\":\"cannot bring up " + std::string(STA_IF) +
               (why.empty() ? std::string() : ": " + http::json_escape(why)) + "\"}";
    }
    util::RunResult r = util::run({iw, "dev", STA_IF, "scan"}, "", 20);
    std::string out = "\n" + r.out;
    // split on "\nBSS "
    std::map<std::string, Net> nets;
    size_t i = out.find("\nBSS ");
    while (i != std::string::npos) {
        size_t next = out.find("\nBSS ", i + 5);
        std::string block = out.substr(i + 5, (next == std::string::npos ? out.size() : next) - (i + 5));
        Net e = parse_block(block);
        if (!e.ssid.empty()) {
            // One row per SSID: the same network on 2.4 and 5 GHz is one
            // entry with Band "2.4/5"; the client stores band=auto and lets
            // wpa_supplicant pick/roam between the BSSes.
            auto it = nets.find(e.ssid);
            if (it == nets.end()) {
                nets[e.ssid] = e;
            } else {
                Net& p = it->second;
                if (!e.band.empty() && p.band.find(e.band) == std::string::npos) {
                    if (p.band.empty()) p.band = e.band;
                    else if ((p.band.find("2.4") != std::string::npos && e.band == "5") ||
                             (p.band.find("5") != std::string::npos && e.band == "2.4"))
                        p.band = "2.4/5";
                    else
                        p.band += "/" + e.band;
                    p.channel.clear();   // one row spanning bands: no single channel
                }
                if (sig_val(e.signal) > sig_val(p.signal)) {
                    p.signal = e.signal;
                    if (p.band.find('/') == std::string::npos) p.channel = e.channel;
                }
                if (p.security == "Open" && e.security != "Open") p.security = e.security;
            }
        }
        i = next;
    }
    std::vector<Net> list;
    for (auto& kv : nets) list.push_back(kv.second);
    std::sort(list.begin(), list.end(), [](const Net& a, const Net& b) { return sig_val(a.signal) > sig_val(b.signal); });
    std::string o = "{\"nets\":[";
    for (size_t k = 0; k < list.size(); ++k) {
        if (k) o += ",";
        o += "{\"bssid\":\"" + http::json_escape(list[k].bssid) +
             "\",\"ssid\":\"" + http::json_escape(list[k].ssid) +
             "\",\"signal\":\"" + http::json_escape(list[k].signal) +
             "\",\"channel\":\"" + http::json_escape(list[k].channel) +
             "\",\"band\":\"" + http::json_escape(list[k].band) +
             "\",\"security\":\"" + http::json_escape(list[k].security) + "\"}";
    }
    o += "],\"error\":\"";
    if (list.empty()) {
        if (r.timed_out) o += "scan timed out";
        else if (r.exit_code != 0) {
            std::string why = util::trim(r.out);
            o += http::json_escape(why.empty() ? "scan failed" : why);
        }
    }
    o += "\"}";
    return o;
}

// Frequency -> band, using the same ranges as the scan parser above.
static std::string band_from_freq(int f) {
    if (f == 2484 || (f >= 2412 && f < 2484)) return "2.4";
    if (f > 5000 && f <= 5925) return "5";
    if (f > 5925 && f <= 7125) return "6";
    return "";
}

// The negotiated PHY mode of a connected STA is visible in the `iw link`
// rate line: EHT-MCS (Wi-Fi 7), HE-MCS (ax), VHT-MCS (ac), HT-MCS (n).
static std::string sta_standard(const std::string& link) {
    if (link.find("EHT-") != std::string::npos) return "802.11be";
    if (link.find("HE-")  != std::string::npos) return "802.11ax";
    if (link.find("VHT-") != std::string::npos) return "802.11ac";
    if (link.find("HT-")  != std::string::npos) return "802.11n";
    return "";
}

std::string security_label(const std::string& s) {
    if (s == "open") return "Open";
    if (s == "wpa3") return "WPA3";
    if (s == "wpa2+wpa3") return "WPA2/WPA3";
    return "WPA2";
}

static std::string norm_security(const std::string& s) {
    if (s == "open" || s == "wpa3" || s == "wpa2+wpa3") return s;
    return "wpa2";
}

static std::map<std::string, std::string> read_kv_map(const char* path) {
    std::map<std::string, std::string> m;
    std::string data = util::read_file(path);
    size_t i = 0;
    while (i <= data.size()) {
        size_t nl = data.find('\n', i);
        std::string s = util::trim(data.substr(i, nl == std::string::npos
                                                  ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? data.size() + 1 : nl + 1;
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq));
        std::string v = s.substr(eq + 1);
        size_t h = v.find('#');
        if (h != std::string::npos) v = v.substr(0, h);
        v = util::trim(v);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        m[k] = v;
    }
    return m;
}

std::vector<Network> read_extra_networks() {
    auto m = read_kv_map(EXTRA_CONF);
    int count = atoi(m["count"].c_str());
    if (count < 0 || count > 32) count = 0;
    std::vector<Network> out;
    for (int i = 1; i <= count; ++i) {
        std::string p = std::to_string(i) + "_";
        Network n;
        n.ssid = m[p + "ssid"];
        if (n.ssid.empty()) continue;
        n.psk = m[p + "psk"];
        n.security = norm_security(m[p + "security"]);
        n.hidden = (m[p + "hidden"] == "1");
        out.push_back(n);
    }
    return out;
}

bool has_sta_networks() {
    return !read_conf()["sta_ssid"].empty() || !read_extra_networks().empty();
}

bool write_extra_networks(const std::vector<Network>& list, std::string& err) {
    if (list.size() > 8) { err = "at most 8 additional networks"; return false; }
    std::string primary = read_conf()["sta_ssid"];
    std::string o = "# Additional STA networks — managed by rnsbox-portal.\ncount=" +
                    std::to_string(list.size()) + "\n";
    for (size_t i = 0; i < list.size(); ++i) {
        const Network& n = list[i];
        std::string sec = norm_security(n.security);
        if (n.ssid.empty() || n.ssid.size() > 32 || !conf_value_ok(n.ssid)) {
            err = "bad SSID"; return false;
        }
        if (sec != "open" && (n.psk.size() < 8 || n.psk.size() > 63 || !conf_value_ok(n.psk))) {
            err = "bad password"; return false;
        }
        for (size_t j = 0; j < i; ++j)
            if (list[j].ssid == n.ssid) { err = "duplicate SSID"; return false; }
        if (!primary.empty() && n.ssid == primary) {
            err = "SSID is already the primary network"; return false;
        }
        std::string p = std::to_string(i + 1) + "_";
        o += p + "ssid=" + n.ssid + "\n";
        o += p + "psk=" + (sec == "open" ? std::string() : n.psk) + "\n";
        o += p + "security=" + sec + "\n";
        o += p + "hidden=" + std::string(n.hidden ? "1" : "") + "\n";
    }
    ::unlink((std::string(EXTRA_CONF) + ".tmp").c_str());
    return util::write_file_atomic(EXTRA_CONF, o, 0600);
}

bool add_extra_network(const Network& n, std::string& err) {
    auto list = read_extra_networks();
    list.push_back(n);
    return write_extra_networks(list, err);
}

bool delete_extra_network(int idx, std::string& err) {
    auto list = read_extra_networks();
    if (idx < 0 || idx >= (int)list.size()) { err = "no such network"; return false; }
    list.erase(list.begin() + idx);
    return write_extra_networks(list, err);
}

// "channel 36 (5180 MHz), width: ..." from `iw dev <ap> info`.
static int ap_freq(const std::string& info) {
    size_t p = info.find("channel ");
    if (p == std::string::npos) return 0;
    size_t lp = info.find('(', p);
    if (lp == std::string::npos) return 0;
    return (int)atof(info.c_str() + lp + 1);
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
        s.sta.signal = int_signal(after_token(link, "signal: "));
        s.sta.ip = ip_of(STA_IF);
        s.sta.width = after_token(sta_info, "width: ");
        s.sta.txpower = int_signal(after_token(sta_info, "txpower "));
        s.sta.band = band_from_freq((int)atof(line_value(link, "freq:").c_str()));
        s.sta.standard = sta_standard(link);
        s.sta.channel = after_token(sta_info, "channel ");
        std::string sec = cfg["sta_security"];
        if (s.sta.ssid != cfg["sta_ssid"]) {
            for (const auto& n : read_extra_networks())
                if (n.ssid == s.sta.ssid) { sec = n.security; break; }
        }
        s.sta.security = security_label(sec);
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
        s.ap.txpower = int_signal(after_token(ap_info, "txpower "));
        s.ap.band = band_from_freq(ap_freq(ap_info));
        if (s.ap.band.empty() && (cfg["ap_band"] == "2.4" || cfg["ap_band"] == "5"))
            s.ap.band = cfg["ap_band"];
        const std::string& as = cfg["ap_standard"];
        s.ap.standard = (as == "n") ? "802.11n" : (as == "ac") ? "802.11ac" :
                        (as == "ax") ? "802.11ax" : "auto";
        s.ap.channel = after_token(ap_info, "channel ");
        s.ap.ssid = line_value(ap_info, "ssid ");
        if (s.ap.ssid.empty()) s.ap.ssid = cfg["ap_ssid"];
        s.ap.security = security_label(cfg["ap_security"]);
    }
    return s;
}

std::string status_json() {
    Status s = status();
    auto cfg = read_conf();
    std::string refused = util::trim(util::read_file("/run/wifi-ap-refused", 512));
    std::string o = std::string("{\"present\":") + (s.present ? "true" : "false") +
        ",\"mode\":\"" + http::json_escape(s.mode) + "\"" +
        ",\"concurrent\":" + (s.concurrent ? "true" : "false") +
        ",\"tx_power\":\"" + http::json_escape(s.tx_power) + "\"" +
        ",\"sta_ssid\":\"" + http::json_escape(cfg["sta_ssid"]) + "\"" +
        ",\"sta_configured\":" + std::string(has_sta_networks() ? "true" : "false") +
        ",\"ap_refused\":\"" + http::json_escape(refused) + "\"";
    if (!s.present) { o += ",\"sta\":{\"connected\":false},\"ap\":{\"active\":false}}"; return o; }
    // The dashboard card shows the same per-iface fields as the WAN/LAN cards
    // (address with prefix, gateway, MAC, counters); status() only knows what
    // `iw` reports, so pull the rest from the one network summary.
    sysinfo::Network net = sysinfo::network_summary();
    auto iface_of = [&](const std::string& n) -> const sysinfo::Iface* {
        for (const auto& i : net.ifaces) if (i.name == n) return &i;
        return nullptr;
    };
    auto addrs_json = [](const std::vector<std::string>& v) {
        std::string a = "[";
        for (size_t k = 0; k < v.size(); ++k) {
            if (k) a += ",";
            a += "\"" + http::json_escape(v[k]) + "\"";
        }
        return a + "]";
    };
    if (s.sta.connected) {
        const sysinfo::Iface* si = iface_of(STA_IF);
        std::string gw;
        auto git = net.gateway_of.find(STA_IF);
        if (git != net.gateway_of.end()) gw = git->second;
        o += ",\"sta\":{\"connected\":true,\"ssid\":\"" + http::json_escape(s.sta.ssid) +
             "\",\"signal\":\"" + http::json_escape(s.sta.signal) +
             "\",\"ip\":\"" + http::json_escape(s.sta.ip) +
             "\",\"mac\":\"" + http::json_escape(si ? si->mac : "") +
             "\",\"rx\":\"" + http::json_escape(si ? si->rx : "") +
             "\",\"tx\":\"" + http::json_escape(si ? si->tx : "") +
             "\",\"gateway\":\"" + http::json_escape(gw) +
             "\",\"addrs\":" + (si ? addrs_json(si->addrs) : std::string("[]")) +
             ",\"width\":\"" + http::json_escape(s.sta.width) +
             "\",\"txpower\":\"" + http::json_escape(s.sta.txpower) +
             "\",\"band\":\"" + http::json_escape(s.sta.band) +
             "\",\"standard\":\"" + http::json_escape(s.sta.standard) +
             "\",\"channel\":\"" + http::json_escape(s.sta.channel) +
             "\",\"security\":\"" + http::json_escape(s.sta.security) + "\"}";
    } else {
        // STA mode can be on with no link; keep its MAC/counters visible so
        // the dashboard card does not blank them between connection attempts.
        const sysinfo::Iface* si = (s.mode == "sta" || s.mode == "sta+ap") ? iface_of(STA_IF) : nullptr;
        o += ",\"sta\":{\"connected\":false";
        if (si) {
            o += ",\"mac\":\"" + http::json_escape(si->mac) +
                 "\",\"rx\":\"" + http::json_escape(si->rx) +
                 "\",\"tx\":\"" + http::json_escape(si->tx) + "\"";
        }
        o += "}";
    }
    if (s.ap.active) {
        const sysinfo::Iface* ai = iface_of(s.ap.iface);
        o += ",\"ap\":{\"active\":true,\"iface\":\"" + http::json_escape(s.ap.iface) +
             "\",\"clients\":" + std::to_string(s.ap.clients) +
             ",\"bridge\":\"" + http::json_escape(s.ap.bridge) +
             "\",\"ip\":\"" + http::json_escape(s.ap.ip) +
             "\",\"mac\":\"" + http::json_escape(ai ? ai->mac : "") +
             "\",\"rx\":\"" + http::json_escape(ai ? ai->rx : "") +
             "\",\"tx\":\"" + http::json_escape(ai ? ai->tx : "") +
             "\",\"width\":\"" + http::json_escape(s.ap.width) +
             "\",\"txpower\":\"" + http::json_escape(s.ap.txpower) +
             "\",\"band\":\"" + http::json_escape(s.ap.band) +
             "\",\"standard\":\"" + http::json_escape(s.ap.standard) +
             "\",\"channel\":\"" + http::json_escape(s.ap.channel) +
             "\",\"ssid\":\"" + http::json_escape(s.ap.ssid) +
             "\",\"security\":\"" + http::json_escape(s.ap.security) + "\"}";
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
