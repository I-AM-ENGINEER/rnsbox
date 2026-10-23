// routes_wifi.cpp — WiFi tab: server-rendered page + PRG forms + scan/status JSON.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "wifi.h"
#include "sysinfo.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }
static std::string wifi_redirect() { return web::base() + "/wifi"; }

static bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    return true;
}

static std::string opt(const std::string& v, const std::string& label, const std::string& cur) {
    return "<option value=\"" + v + "\"" + (v == cur ? " selected" : "") + ">" + label + "</option>";
}

// WPA2-PSK / WPA3-SAE / transition, plus open when allow_open (the operator
// can run an open hotspot; S35wifi's hostapd generator supports it).
static std::string security_options(const std::string& cur, bool allow_open) {
    std::string o;
    // WPA3 first: it is the hotspot default. "WPA3-PSK" is what users call
    // SAE; the generated hostapd config still uses SAE.
    o += opt("wpa3", "WPA3-PSK", cur);
    o += opt("wpa2", "WPA2-PSK", cur);
    o += opt("wpa2+wpa3", "WPA2 + WPA3 (mixed)", cur);
    if (allow_open) o += opt("open", "Open (no password)", cur);
    return o;
}

static std::string band_options(const std::string& cur, bool with_auto) {
    std::string o;
    if (with_auto) o += opt("auto", "Auto", cur);
    o += opt("2.4", "2.4 GHz", cur);
    o += opt("5", "5 GHz", cur);
    return o;
}

// Regulatory domain for both roles. Empty ("world") keeps the driver's own
// channel set and lets S35wifi restrict it to channels every client accepts;
// a country makes hostapd filter ACS and lets 5 GHz operate legally.
static std::string country_options(const std::string& cur) {
    static const struct { const char* code; const char* name; } list[] = {
        {"RU", "Russia"},        {"US", "United States"}, {"GB", "United Kingdom"},
        {"DE", "Germany"},       {"FR", "France"},        {"IT", "Italy"},
        {"ES", "Spain"},         {"PT", "Portugal"},      {"NL", "Netherlands"},
        {"BE", "Belgium"},       {"CH", "Switzerland"},   {"AT", "Austria"},
        {"PL", "Poland"},        {"CZ", "Czechia"},       {"SK", "Slovakia"},
        {"HU", "Hungary"},       {"RO", "Romania"},       {"BG", "Bulgaria"},
        {"GR", "Greece"},        {"HR", "Croatia"},       {"SI", "Slovenia"},
        {"EE", "Estonia"},       {"LV", "Latvia"},        {"LT", "Lithuania"},
        {"FI", "Finland"},       {"SE", "Sweden"},        {"NO", "Norway"},
        {"DK", "Denmark"},       {"IE", "Ireland"},       {"IS", "Iceland"},
        {"UA", "Ukraine"},       {"BY", "Belarus"},       {"KZ", "Kazakhstan"},
        {"TR", "Turkey"},        {"IL", "Israel"},        {"AE", "United Arab Emirates"},
        {"SA", "Saudi Arabia"},  {"IN", "India"},         {"CN", "China"},
        {"JP", "Japan"},         {"KR", "South Korea"},   {"TW", "Taiwan"},
        {"HK", "Hong Kong"},     {"SG", "Singapore"},     {"MY", "Malaysia"},
        {"TH", "Thailand"},      {"VN", "Vietnam"},       {"ID", "Indonesia"},
        {"PH", "Philippines"},   {"AU", "Australia"},     {"NZ", "New Zealand"},
        {"CA", "Canada"},        {"MX", "Mexico"},        {"BR", "Brazil"},
        {"AR", "Argentina"},     {"CL", "Chile"},         {"CO", "Colombia"},
        {"ZA", "South Africa"},  {"EG", "Egypt"},         {"NG", "Nigeria"},
        {"KE", "Kenya"},
    };
    std::string o = opt("", "Auto — world (default)", cur);
    for (const auto& c : list) {
        std::string code = c.code;
        o += "<option value=\"" + code + "\"" + (code == cur ? " selected" : "") + ">" +
             code + " — " + c.name + "</option>";
    }
    return o;
}

// Only the standards this radio reports. mt7603e: HT only -> no ac/ax.
// Unknown hardware (iw missing) keeps the full list rather than lying by
// omission.
static std::string standard_options(const std::string& cur, const wifi::Caps& c) {
    std::string o = opt("auto", "Auto (driver default)", cur);
    if (c.ht  || !c.known) o += opt("n",  "Wi-Fi 4 (802.11n)", cur);
    if (c.vht || !c.known) o += opt("ac", "Wi-Fi 5 (802.11ac)", cur);
    if (c.he  || !c.known) o += opt("ax", "Wi-Fi 6 (802.11ax)", cur);
    return o;
}

static bool is_5g_chan(int n) {
    switch (n) {
        case 36: case 40: case 44: case 48:
        case 149: case 153: case 157: case 161: case 165:
            return true;
        default:
            return false;
    }
}

static bool channel_valid(const std::string& band, const std::string& chan,
                          const std::string& width) {
    // "auto" band is hostapd ACS on 2.4/5: no fixed channel makes sense.
    if (band == "auto") return chan == "auto";
    if (chan == "auto") return true;
    if (!all_digits(chan)) return false;
    int n = atoi(chan.c_str());
    if (band == "5") {
        if (!is_5g_chan(n)) return false;
        // 40/80 MHz need a channel pair/segment: 165 is 20 MHz only.
        if ((width == "40" || width == "80") && n == 165) return false;
    } else if (n < 1 || n > 13) {
        return false;
    }
    return true;
}

static std::string channel_options(const std::string& band, const std::string& cur,
                                   const wifi::Caps& cp) {
    std::string o = opt("auto", "auto (ACS)", cur);
    if (band == "5") {
        std::vector<int> ch = cp.ch5;
        if (ch.empty()) ch = {36, 40, 44, 48, 149, 153, 157, 161, 165};
        for (int c : ch) o += opt(std::to_string(c), std::to_string(c), cur);
    } else if (band == "2.4") {
        std::vector<int> ch = cp.ch24;
        if (ch.empty()) { for (int c = 1; c <= 13; ++c) ch.push_back(c); }
        for (int c : ch) o += opt(std::to_string(c), std::to_string(c), cur);
    }
    return o;
}

static std::string norm_sta_security(const std::string& s) {
    if (s == "open" || s == "wpa3" || s == "wpa2+wpa3") return s;
    return "wpa2";
}

static std::string norm_ap_security(const std::string& s) {
    if (s == "open" || s == "wpa3" || s == "wpa2+wpa3") return s;
    return "wpa2";
}

// ---- GET render ----

void wifi_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;

    if (!wifi::present()) {
        std::string h =
            "<h2>WiFi</h2>\n\n"
            "<section class=\"card\">\n"
            "  <p class=\"muted\">No WiFi radio detected. Depending on the board the\n"
            "  radio is an AIC8800D80 (SDIO) or the SoC's built-in MT7628 WMAC\n"
            "  (mt7603e); it should surface as wlan0 via S35wifi. Check that its\n"
            "  kernel module is loaded (<code>lsmod</code>, <code>dmesg</code>) and see\n"
            "  <code>/var/log/wifi.log</code>. Boards without a radio stay here.</p>\n"
            "</section>";
        res.body = render::layout(req, res, "WiFi", "wifi", h);
        return;
    }

    auto cfg = wifi::read_conf();
    wifi::Status st = wifi::status();
    // The WiFi tab shows the same interface fields as the dashboard cards
    // (address with prefix, gateway, MAC, counters), so read the network
    // summary once and look up the STA/AP iface by name.
    sysinfo::Network net = sysinfo::network_summary();
    auto iface_of = [&](const std::string& n) -> const sysinfo::Iface* {
        for (const auto& i : net.ifaces) if (i.name == n) return &i;
        return nullptr;
    };
    auto join_addrs = [](const std::vector<std::string>& v) {
        std::string o;
        for (size_t k = 0; k < v.size(); ++k) { if (k) o += ", "; o += v[k]; }
        return o;
    };
    std::string base = web::base();
    const std::string& m = cfg["mode"];
    bool sta_on = (m == "sta" || m == "sta+ap");
    bool ap_on  = (m == "ap"  || m == "sta+ap");
    bool concurrent = (m == "sta+ap");

    std::string h = render::page_file("wifi");
    h = R(std::move(h), "__BASE__", base);

    // ---- Status ----
    // "off" is a normal operating state (radio idle), not an error: neutral
    // badge. An enabled mode gets the green badge; the connection cells below
    // carry the actual success/failure state.
    std::string mode_badge;
    if (m.empty() || m == "off")
        mode_badge = "<span class=\"badge\">off</span>";
    else
        mode_badge = "<span class=\"badge ok\">" + E(m) + "</span>";

    h = R(std::move(h), "__WIFI_MODE_BADGE__", mode_badge);

    const sysinfo::Iface* si = sta_on ? iface_of(wifi::STA_IF) : nullptr;
    std::string sta_gw;
    if (st.sta.connected) {
        auto git = net.gateway_of.find(wifi::STA_IF);
        if (git != net.gateway_of.end()) sta_gw = git->second;
    }
    std::string sta_rxtx = "—";
    if (si) {
        sta_rxtx = (si->rx.empty() ? std::string("0") : E(si->rx)) + " / " +
                   (si->tx.empty() ? std::string("0") : E(si->tx));
    }
    std::string sta_rows;
    if (!sta_on)
        sta_rows += "<tr><th>Status</th><td id=\"w-sta-state\"><span class=\"badge\">off</span></td></tr>\n";
    else if (st.sta.connected)
        sta_rows += "<tr><th>Status</th><td id=\"w-sta-state\"><span class=\"badge ok\">CONNECTED</span></td></tr>\n";
    else if (!wifi::has_sta_networks())
        sta_rows += "<tr><th>Status</th><td id=\"w-sta-state\"><span class=\"badge\">not configured</span></td></tr>\n";
    else
        sta_rows += "<tr><th>Status</th><td id=\"w-sta-state\"><span class=\"badge warn\">not connected</span></td></tr>\n";
    sta_rows += "<tr><th>Network</th><td id=\"w-sta-ssid\">" +
        ((st.sta.connected && !st.sta.ssid.empty()) ? E(st.sta.ssid) : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Signal</th><td id=\"w-sta-signal\">" +
        ((st.sta.connected && !st.sta.signal.empty()) ? E(st.sta.signal) + " dBm" : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Band</th><td id=\"w-sta-band\">" +
        ((st.sta.connected && !st.sta.band.empty()) ? E(st.sta.band) + " GHz" : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Channel</th><td id=\"w-sta-channel\">" +
        ((st.sta.connected && !st.sta.channel.empty()) ? E(st.sta.channel) : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Width</th><td id=\"w-sta-width\">" +
        ((st.sta.connected && !st.sta.width.empty()) ? E(st.sta.width) + " MHz" : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Standard</th><td id=\"w-sta-standard\">" +
        ((st.sta.connected && !st.sta.standard.empty()) ? E(st.sta.standard) : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Security</th><td id=\"w-sta-security\">" +
        ((st.sta.connected && !st.sta.security.empty()) ? E(st.sta.security) : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Address</th><td id=\"w-sta-ip\">" +
        ((st.sta.connected && si && !si->addrs.empty()) ? E(join_addrs(si->addrs))
         : ((st.sta.connected && !st.sta.ip.empty()) ? E(st.sta.ip) : std::string("—"))) +
        "</td></tr>\n";
    sta_rows += "<tr><th>Gateway</th><td id=\"w-sta-gw\">" +
        ((st.sta.connected && !sta_gw.empty()) ? E(sta_gw) : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>MAC</th><td id=\"w-sta-mac\">" +
        ((si && !si->mac.empty()) ? E(si->mac) : std::string("—")) +
        "</td></tr>\n";
    sta_rows += "<tr><th>RX / TX</th><td id=\"w-sta-rxtx\">" + sta_rxtx + "</td></tr>\n";

    const sysinfo::Iface* ai = (ap_on && st.ap.active) ? iface_of(st.ap.iface) : nullptr;
    std::string ap_rxtx = "—";
    if (ai) {
        ap_rxtx = (ai->rx.empty() ? std::string("0") : E(ai->rx)) + " / " +
                  (ai->tx.empty() ? std::string("0") : E(ai->tx));
    }
    std::string ap_rows;
    if (!ap_on)
        ap_rows += "<tr><th>Status</th><td id=\"w-ap-state\"><span class=\"badge\">off</span></td></tr>\n";
    else if (st.ap.active)
        ap_rows += "<tr><th>Status</th><td id=\"w-ap-state\"><span class=\"badge ok\">UP</span></td></tr>\n";
    else
        ap_rows += "<tr><th>Status</th><td id=\"w-ap-state\"><span class=\"badge warn\">not running</span></td></tr>\n";
    ap_rows += "<tr><th>SSID</th><td id=\"w-ap-ssid\">" +
        (ap_on && st.ap.active && !st.ap.ssid.empty() ? E(st.ap.ssid) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Interface</th><td id=\"w-ap-iface\">" +
        (ap_on && st.ap.active && !st.ap.iface.empty() ? E(st.ap.iface) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Clients</th><td id=\"w-ap-clients\">" +
        (ap_on && st.ap.active ? std::to_string(st.ap.clients) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Band</th><td id=\"w-ap-band\">" +
        (ap_on && st.ap.active && !st.ap.band.empty() ? E(st.ap.band) + " GHz" : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Channel</th><td id=\"w-ap-channel\">" +
        (ap_on && st.ap.active && !st.ap.channel.empty() ? E(st.ap.channel) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Width</th><td id=\"w-ap-width\">" +
        (ap_on && st.ap.active && !st.ap.width.empty() ? E(st.ap.width) + " MHz" : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Standard</th><td id=\"w-ap-standard\">" +
        (ap_on && st.ap.active && !st.ap.standard.empty() ? E(st.ap.standard) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Security</th><td id=\"w-ap-security\">" +
        (ap_on && st.ap.active && !st.ap.security.empty() ? E(st.ap.security) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>Bridge</th><td id=\"w-ap-bridge\">" +
        (ap_on && st.ap.active && !st.ap.bridge.empty() ? E(st.ap.bridge) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>MAC</th><td id=\"w-ap-mac\">" +
        ((ai && !ai->mac.empty()) ? E(ai->mac) : std::string("—")) +
        "</td></tr>\n";
    ap_rows += "<tr><th>RX / TX</th><td id=\"w-ap-rxtx\">" + ap_rxtx + "</td></tr>\n";

    h = R(std::move(h), "__WIFI_STA_ROWS__", sta_rows);
    h = R(std::move(h), "__WIFI_AP_ROWS__", ap_rows);

    // Connected hotspot clients: what each one is, where it got its lease,
    // how strong it is and how long it has been up.
    std::string client_rows;
    if (ap_on && st.ap.active && !st.ap.client_list.empty()) {
        for (const auto& c : st.ap.client_list) {
            client_rows += "<tr><td>" +
                (c.host.empty() ? std::string("<span class=\"muted\">&mdash;</span>") : E(c.host)) +
                "</td><td>" + (c.ip.empty() ? std::string("—") : E(c.ip)) +
                "</td><td>" + (c.signal.empty() ? std::string("—") : E(c.signal) + " dBm") +
                "</td><td><code>" + E(c.mac) + "</code></td><td>" + E(c.connected) +
                "</td><td>" + E(c.rx) + " / " + E(c.tx) + "</td></tr>\n";
        }
    } else {
        client_rows = "<tr><td colspan=\"6\" class=\"muted\">No clients connected.</td></tr>\n";
    }
    h = R(std::move(h), "__AP_CLIENTS_ROWS__", client_rows);

    // One list of known networks: the primary one (edited by the form above)
    // plus the extras. wpa_supplicant treats them as one pool and falls back
    // to the next one when the current signal disappears.
    std::string net_rows;
    std::string primary_ssid = cfg["sta_ssid"];
    auto net_state = [&](const std::string& ssid) {
        bool on = st.sta.connected && !st.sta.ssid.empty() && st.sta.ssid == ssid;
        return on ? std::string("<span class=\"badge ok\">CONNECTED</span>")
                  : std::string("<span class=\"badge\">saved</span>");
    };
    if (!primary_ssid.empty()) {
        net_rows += "<tr><td><strong>" + E(primary_ssid) + "</strong></td><td>" +
                    E(wifi::security_label(cfg["sta_security"])) + "</td><td>" +
                    "<span class=\"net-state\" data-ssid=\"" + E(primary_ssid) + "\">" +
                    net_state(primary_ssid) + "</span></td><td>" +
                    "<button type=\"button\" class=\"btn\" onclick=\"editPrimary()\">Edit</button>"
                    "</td></tr>\n";
    }
    auto extras = wifi::read_extra_networks();
    for (size_t i = 0; i < extras.size(); ++i) {
        net_rows += "<tr><td><strong>" + E(extras[i].ssid) + "</strong></td><td>" +
                    E(wifi::security_label(extras[i].security)) + "</td><td>" +
                    "<span class=\"net-state\" data-ssid=\"" + E(extras[i].ssid) + "\">" +
                    net_state(extras[i].ssid) + "</span></td><td>" +
                    "<form method=\"post\" action=\"" + base +
                    "/wifi/net/delete\" class=\"inline\">"
                    "<input type=\"hidden\" name=\"idx\" value=\"" +
                    std::to_string(i) + "\">"
                    "<button type=\"submit\" class=\"btn-danger\">Delete</button></form>"
                    "</td></tr>\n";
    }
    if (net_rows.empty())
        net_rows = "<tr><td colspan=\"4\" class=\"muted\">No networks configured</td></tr>\n";
    h = R(std::move(h), "__WIFI_NET_ROWS__", net_rows);

    // ---- Mode toggles ----
    h = R(std::move(h), "__STA_CHECKED__", sta_on ? "checked" : "");
    h = R(std::move(h), "__AP_CHECKED__",  ap_on  ? "checked" : "");
    h = R(std::move(h), "__SCAN_DISABLED__", "");


    // ---- STA settings ----
    // Security and band are properties of the scanned BSS, not operator
    // choices: the form carries them as hidden fields filled by "Use".
    h = R(std::move(h), "__STA_SSID__", E(cfg["sta_ssid"]));
    h = R(std::move(h), "__STA_SECURITY__", E(cfg["sta_security"]));
    h = R(std::move(h), "__STA_BAND__", E(cfg["sta_band"]));
    h = R(std::move(h), "__STA_HIDDEN_CHECKED__", cfg["sta_hidden"] == "1" ? "checked" : "");
    h = R(std::move(h), "__STA_ENABLED_JS__", sta_on ? "true" : "false");

    // ---- AP settings ----
    // A blank ap_ssid in wifi.conf is not "no SSID": S35wifi names the
    // hotspot after the device hostname then (the out-of-the-box default, so
    // the SSID follows S10uuid's rename). Show the name the AP actually
    // broadcasts — saving pins it explicitly.
    std::string ap_ssid = cfg["ap_ssid"];
    if (ap_ssid.empty()) {
        ap_ssid = util::trim(util::read_file("/proc/sys/kernel/hostname", 128));
        if (ap_ssid.empty() || ap_ssid == "localhost") ap_ssid = "rnsbox";
    }
    h = R(std::move(h), "__AP_SSID__", E(ap_ssid));
    h = R(std::move(h), "__AP_PSK__",  E(cfg["ap_psk"]));
    h = R(std::move(h), "__AP_SECURITY_OPTIONS__", security_options(cfg["ap_security"], true));
    std::string country = util::trim(cfg["sta_country"]);
    if (country == "00") country.clear();
    h = R(std::move(h), "__COUNTRY_OPTIONS__", country_options(country));

    // Bands/standards/widths come from `iw phy`, not from a generic list: an
    // MT7628/mt7603e shows 2.4 GHz only and no ac/ax, while a 5 GHz radio
    // shows both. Unknown hardware (no iw) keeps the full set.
    wifi::Caps cp = wifi::caps();
    std::string band_radios;
    auto radio = [&](const char* v, const std::string& label, bool on) {
        return std::string("<label><input type=\"radio\" name=\"ap_band\" value=\"") + v +
               "\" " + (on ? "checked" : "") + "> " + label + "</label>";
    };
    bool want24 = cfg["ap_band"] == "2.4", want5 = cfg["ap_band"] == "5";
    bool wantAuto = cfg["ap_band"] == "auto";
    if (cp.known) {
        if (cp.band24 && cp.band5) {
            band_radios += radio("auto", "2.4/5 GHz (auto)", wantAuto);
            band_radios += radio("2.4", "2.4 GHz", want24);
            band_radios += radio("5", "5 GHz", want5);
        } else if (cp.band24) {
            band_radios += radio("2.4", "2.4 GHz (only band)", want24 || wantAuto || want5);
        } else if (cp.band5) {
            band_radios += radio("5", "5 GHz (only band)", want5 || wantAuto || want24);
        }
    } else {
        band_radios += radio("auto", "2.4/5 GHz (auto)", wantAuto);
        band_radios += radio("2.4", "2.4 GHz", want24);
        band_radios += radio("5", "5 GHz", want5);
    }
    h = R(std::move(h), "__AP_BAND_RADIOS__", band_radios);
    h = R(std::move(h), "__AP_STANDARD_OPTIONS__", standard_options(cfg["ap_standard"], cp));

    std::string sel_chan = cfg["ap_channel"];
    if (!channel_valid(cfg["ap_band"], sel_chan, cfg["ap_width"])) sel_chan = "auto";
    h = R(std::move(h), "__AP_CHANNEL_OPTIONS__", channel_options(cfg["ap_band"], sel_chan, cp));

    std::string width_opts;
    width_opts += opt("20", "20 MHz", cfg["ap_width"]);
    if (cp.ht || cp.vht || !cp.known) width_opts += opt("40", "40 MHz", cfg["ap_width"]);
    if (cp.vht || cp.he || !cp.known) width_opts += opt("80", "80 MHz (5 GHz, 802.11ac/ax)", cfg["ap_width"]);
    h = R(std::move(h), "__AP_WIDTH_OPTIONS__", width_opts);

    // Channel lists the page's JS rebuilds on a band switch. Fall back to the
    // standard lists when the phy did not report frequencies.
    auto js_list = [](std::vector<int> v, const std::vector<int>& fallback) {
        if (v.empty()) v = fallback;
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
        return s + "]";
    };
    std::vector<int> ch24_fb, ch5_fb;
    for (int i = 1; i <= 13; ++i) ch24_fb.push_back(i);
    for (int c : {36, 40, 44, 48, 149, 153, 157, 161, 165}) ch5_fb.push_back(c);
    h = R(std::move(h), "__CH24_JS__", js_list(cp.ch24, ch24_fb));
    h = R(std::move(h), "__CH5_JS__",  js_list(cp.ch5, ch5_fb));
    h = R(std::move(h), "__AP_HIDDEN_CHECKED__", cfg["ap_hidden"] == "1" ? "checked" : "");

    // Mode says hotspot-on but S35wifi refuses a weak PSK (e.g. a box still on
    // the old 'changeme123' default) — explain the missing AP.
    std::string psk_note;
    if (ap_on && cfg["ap_security"] != "open" && !wifi::ap_psk_ok(cfg["ap_psk"]))
        psk_note = "<div class=\"flash warn\">The hotspot is switched on but is <strong>not running</strong>:\n"
                   "  its password is empty, too short, or the old published default\n"
                   "  <code>changeme123</code>. Set a new one below and apply.</div>";
    // Any other refusal S35wifi recorded (today: no LAN bridge br-lan to put
    // the AP in — a kernel without CONFIG_BRIDGE) — show its reason.
    else if (ap_on && !st.ap.active) {
        std::string why = util::trim(util::read_file("/run/wifi-ap-refused", 512));
        if (!why.empty())
            psk_note = "<div class=\"flash warn\">The hotspot is switched on but is <strong>not running</strong>:\n"
                       "  " + E(why) + "</div>";
    }
    h = R(std::move(h), "__AP_PSK_NOTE__", psk_note);

    // TX power options: auto + 18..1
    const std::string& tp = cfg["tx_power"];
    bool autoSel = (tp == "auto" || tp == "max" || tp.empty());
    std::string txopts = "<option value=\"auto\" " + std::string(autoSel ? "selected" : "") + ">auto (chip max)</option>";
    for (int d = 18; d >= 1; --d) {
        std::string ds = std::to_string(d);
        txopts += "\n      <option value=\"" + ds + "\" " + (tp == ds ? "selected" : "") + ">" + ds + " dBm" + (d == 18 ? " (max)" : "") + "</option>";
    }
    h = R(std::move(h), "__TXPOWER_OPTIONS__", txopts);

    h = R(std::move(h), "__CONCURRENT_JS__", concurrent ? "true" : "false");

    res.body = render::layout(req, res, "WiFi", "wifi", h);
}

// ---- AJAX GET JSON (kept as JSON, exactly as the old template's fetch expects) ----

void wifi_scan(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.body = wifi::scan_json();
}

void wifi_status(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.body = wifi::status_json();
}

// ---- POST-redirect-GET handlers ----

// Shared error text for an SSID/password wifi.conf can't store (see
// wifi::conf_value_ok): nothing is written when this fires.
static const char* BAD_FIELD_MSG =
    "SSID and password must not contain line breaks, control characters, '#' or '\"', "
    "or start/end with a space (the WiFi config file can't store them)";

// Why the hotspot can't be switched on with this stored/typed PSK ("" = OK).
static std::string ap_psk_problem(const std::string& psk) {
    if (psk == wifi::OLD_DEFAULT_AP_PSK)
        return "'changeme123' was the published factory default — choose your own hotspot password (8-63 characters)";
    if (!wifi::ap_psk_ok(psk))
        return "The hotspot needs a password of 8-63 printable ASCII characters";
    return "";
}

// Appended to a flash that turns the WiFi client on: with the WAN pinned to
// eth0 (Network tab) the box itself may use the WiFi uplink, but the LAN and
// hotspot get no internet through it (no NAT there). "" otherwise.
static std::string wan_note() {
    if (store::read_wan().interface != "eth0") return "";
    return " Note: the WAN is set to eth0 only, so the LAN and hotspot won't get internet over WiFi"
           " — choose Automatic (or wlan0) under Network → WAN uplink.";
}

void wifi_connect(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string ssid = util::trim(req.f("ssid"));
    std::string psk = req.f("psk");
    std::string security = norm_sta_security(util::trim(req.f("sta_security")));
    std::string band = util::trim(req.f("sta_band"));
    if (band != "2.4" && band != "5") band = "auto";
    bool hidden = req.f("sta_hidden") == "on";

    auto cur = wifi::read_conf();
    bool connect_action = req.f("do") == "connect";
    // ui_mode=1 forms carry the toggle; older clients always enabled STA.
    bool sta_on = connect_action ? true
                 : ((req.f("ui_mode") == "1") ? (req.f("sta_enable") == "on") : true);
    bool ap_on = (cur["mode"] == "ap" || cur["mode"] == "sta+ap");

    bool has_extra = !wifi::read_extra_networks().empty();
    if (ssid.empty()) {
        psk.clear();
        security = "wpa2";
        band = "auto";
        hidden = false;
    } else if (security == "open") {
        psk.clear();
    } else if (psk.empty() && ssid == cur["sta_ssid"]) {
        psk = cur["sta_psk"];
    }
    if (sta_on && ssid.empty() && !has_extra) {
        render::redirect_flash(res, wifi_redirect(), "error", "SSID required");
        return;
    }
    if (!wifi::conf_value_ok(ssid) || !wifi::conf_value_ok(psk)) {
        render::redirect_flash(res, wifi_redirect(), "error", BAD_FIELD_MSG);
        return;
    }
    if (ssid.size() > 32) {
        render::redirect_flash(res, wifi_redirect(), "error", "SSID is at most 32 bytes");
        return;
    }
    if (!ssid.empty() && !psk.empty() && (psk.size() < 8 || psk.size() > 63)) {
        render::redirect_flash(res, wifi_redirect(), "error",
            "WiFi password must be 8-63 characters (choose Open for a network without a password)");
        return;
    }
    if (!ssid.empty() && sta_on && security != "open" && psk.empty()) {
        render::redirect_flash(res, wifi_redirect(), "error",
            "WiFi password is required for an encrypted network");
        return;
    }
    std::string mode = (sta_on && ap_on) ? "sta+ap" : sta_on ? "sta" : ap_on ? "ap" : "off";
    if (!wifi::write_conf({{"mode", mode}, {"sta_ssid", ssid}, {"sta_psk", psk},
                           {"sta_security", security}, {"sta_band", band},
                           {"sta_hidden", hidden ? "1" : ""}})) {
        render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
        return;
    }
    wifi::apply();
    if (!sta_on) {
        render::redirect_flash(res, wifi_redirect(), "success", "WiFi settings saved — STA mode is off.");
        return;
    }
    // Joining still works with a weak stored hotspot PSK — S35wifi just keeps
    // the AP off (and logs why). Say so instead of a silent missing hotspot.
    std::string note = wan_note();
    std::string what = connect_action
        ? (ssid.empty() ? std::string("Connecting…") : "Connecting to '" + ssid + "'…")
        : std::string("WiFi settings saved.");
    if (mode == "sta+ap" && !wifi::ap_psk_ok(cur["ap_psk"]))
        render::redirect_flash(res, wifi_redirect(), "warn",
            what + " The hotspot stays off until you set a hotspot password (8-63 characters) below." + note);
    else
        render::redirect_flash(res, wifi_redirect(), note.empty() ? "success" : "warn",
            what + note);
}

void wifi_ap(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string ssid = util::trim(req.f("ap_ssid"));
    std::string psk = req.f("ap_psk");
    std::string security = util::trim(req.f("ap_security"));
    std::string band = util::trim(req.f("ap_band"));
    std::string standard = util::trim(req.f("ap_standard"));
    std::string width = util::trim(req.f("ap_width"));
    std::string chan = util::trim(req.f("ap_channel"));
    bool hidden = req.f("ap_hidden") == "on";
    std::string country = util::trim(req.f("sta_country"));
    std::string cu;
    for (char c : country)
        if (!std::isspace((unsigned char)c)) cu += (char)std::toupper((unsigned char)c);
    country = cu;
    if (!country.empty() &&
        (country.size() != 2 || !std::isalpha((unsigned char)country[0]) ||
         !std::isalpha((unsigned char)country[1]) || country == "00")) {
        render::redirect_flash(res, wifi_redirect(), "error",
            "Country must be a two-letter code (or Auto for the world regdomain)");
        return;
    }

    auto cur = wifi::read_conf();
    bool sta_on = (cur["mode"] == "sta" || cur["mode"] == "sta+ap");
    // Older clients did not carry the toggle; their AP state stays unchanged.
    bool ap_on = (req.f("ui_mode") == "1")
        ? (req.f("ap_enable") == "on")
        : (cur["mode"] == "ap" || cur["mode"] == "sta+ap");

    if (security.empty()) security = "wpa3";
    if (security != "open" && security != "wpa2" && security != "wpa3" &&
        security != "wpa2+wpa3") {
        render::redirect_flash(res, wifi_redirect(), "error", "Unknown hotspot security mode");
        return;
    }
    if (band.empty()) band = "auto";
    if (band != "auto" && band != "2.4" && band != "5") {
        render::redirect_flash(res, wifi_redirect(), "error", "Hotspot band must be auto, 2.4 or 5 GHz");
        return;
    }
    if (standard.empty()) standard = "auto";
    if (standard != "auto" && standard != "n" && standard != "ac" && standard != "ax") {
        render::redirect_flash(res, wifi_redirect(), "error", "Unknown Wi-Fi standard");
        return;
    }
    if (width.empty()) width = "20";
    if (width != "20" && width != "40" && width != "80") {
        render::redirect_flash(res, wifi_redirect(), "error", "Hotspot width must be 20, 40 or 80 MHz");
        return;
    }
    if (band == "2.4" && standard == "ac") {
        render::redirect_flash(res, wifi_redirect(), "error", "802.11ac is 5 GHz-only; choose band 5 GHz or another standard");
        return;
    }
    if (width == "80" && (band != "5" || (standard != "ac" && standard != "ax"))) {
        render::redirect_flash(res, wifi_redirect(), "error",
            "80 MHz requires 5 GHz and Wi-Fi 5 (ac) or Wi-Fi 6 (ax)");
        return;
    }
    // Reject what this radio cannot do (a stale form or a hand-edited conf):
    // mt7603e has no 5 GHz, VHT, HE or 80 MHz.
    wifi::Caps cp = wifi::caps();
    if (cp.known) {
        if (band == "5" && !cp.band5) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "This radio has no 5 GHz band");
            return;
        }
        if (band == "2.4" && !cp.band24) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "This radio has no 2.4 GHz band");
            return;
        }
        if (standard == "n" && !cp.ht) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "This radio does not support 802.11n");
            return;
        }
        if (standard == "ac" && !cp.vht) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "This radio does not support 802.11ac (Wi-Fi 5)");
            return;
        }
        if (standard == "ax" && !cp.he) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "This radio does not support 802.11ax (Wi-Fi 6)");
            return;
        }
        if (width == "40" && !(cp.ht || cp.vht)) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "This radio does not support 40 MHz");
            return;
        }
        if (width == "80" && !(cp.vht || cp.he)) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "This radio does not support 80 MHz");
            return;
        }
        if (chan != "auto" && all_digits(chan)) {
            int c = atoi(chan.c_str());
            const std::vector<int>& list = (band == "5") ? cp.ch5 : cp.ch24;
            if (!list.empty() && std::find(list.begin(), list.end(), c) == list.end()) {
                render::redirect_flash(res, wifi_redirect(), "error",
                    "Channel " + chan + " is not available on this radio");
                return;
            }
        }
    }
    if (chan.empty()) chan = "auto";
    if (!channel_valid(band, chan, width)) {
        render::redirect_flash(res, wifi_redirect(), "error",
            "Hotspot channel is not valid for the selected band/width");
        return;
    }
    if (!wifi::conf_value_ok(ssid) || !wifi::conf_value_ok(psk)) {
        render::redirect_flash(res, wifi_redirect(), "error", BAD_FIELD_MSG);
        return;
    }
    if (ssid.size() > 32) {
        render::redirect_flash(res, wifi_redirect(), "error", "Hotspot SSID is at most 32 bytes");
        return;
    }
    // A blank SSID is allowed: S35wifi then names the hotspot after the
    // device hostname (the field above already shows that name).
    // Enabling the hotspot requires a good password; a bad typed value is
    // rejected even while it stays off. An explicitly open hotspot needs none.
    if (security != "open" && (ap_on || !psk.empty())) {
        std::string why = ap_psk_problem(psk);
        if (!why.empty()) {
            render::redirect_flash(res, wifi_redirect(), "error", why);
            return;
        }
    }
    // ap_htmode is the pre-ap_width knob: keep it in sync for older readers.
    std::string htmode = (width == "40") ? "ht40" : "ht20";
    std::string mode = (sta_on && ap_on) ? "sta+ap" : ap_on ? "ap" : sta_on ? "sta" : "off";
    if (!wifi::write_conf({{"mode", mode}, {"ap_ssid", ssid}, {"ap_psk", psk},
                           {"ap_security", security}, {"ap_band", band},
                           {"ap_standard", standard}, {"ap_width", width},
                           {"ap_channel", chan}, {"ap_htmode", htmode},
                           {"ap_hidden", hidden ? "1" : ""},
                           {"sta_country", country}})) {
        render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
        return;
    }
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success",
        ap_on ? "AP settings applied: hotspot on." : "AP settings applied: hotspot off.");
}

void wifi_mode(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    auto cur = wifi::read_conf();
    std::string mode = util::trim(req.f("mode"));
    std::string only = util::trim(req.f("only"));
    // Legacy toggle endpoint (cached pages, external forms): "only" says
    // which mode was submitted, so the other one keeps its state;
    // mode=<off|sta|ap|sta+ap> still works unchanged.
    if (mode.empty()) {
        bool sta = (cur["mode"] == "sta" || cur["mode"] == "sta+ap");
        bool ap  = (cur["mode"] == "ap"  || cur["mode"] == "sta+ap");
        if (only == "sta") {
            sta = req.f("sta_enable") == "on";
        } else if (only == "ap") {
            ap = req.f("ap_enable") == "on";
        } else {
            sta = req.f("sta_enable") == "on";
            ap  = req.f("ap_enable") == "on";
        }
        mode = (sta && ap) ? "sta+ap" : sta ? "sta" : ap ? "ap" : "off";
    }
    // Reject rather than default to "off": turning the radio off can drop a
    // wlan0 WAN, so only an explicit, valid choice may change the mode.
    if (mode != "off" && mode != "sta" && mode != "ap" && mode != "sta+ap") {
        render::redirect_flash(res, wifi_redirect(), "error", "Unknown WiFi mode");
        return;
    }
    // ap / sta+ap switch the hotspot on with the STORED ssid/password — hold
    // them to the same rule as the Hotspot form (blank ssid = hostname).
    if ((mode == "ap" || mode == "sta+ap") && cur["ap_security"] != "open") {
        std::string why = ap_psk_problem(cur["ap_psk"]);
        if (!why.empty()) {
            render::redirect_flash(res, wifi_redirect(), "error",
                why + " — set it in the Hotspot section below, then enable AP mode there.");
            return;
        }
    }
    if (!wifi::write_conf({{"mode", mode}})) {
        render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
        return;
    }
    wifi::apply();
    std::string label = (mode == "off") ? std::string("off") :
                        (mode == "sta") ? std::string("STA on, AP off") :
                        (mode == "ap")  ? std::string("STA off, AP on") :
                                          std::string("STA + AP on");
    std::string note = (mode == "sta" || mode == "sta+ap") ? wan_note() : std::string();
    render::redirect_flash(res, wifi_redirect(), note.empty() ? "success" : "warn",
                           "WiFi modes: " + label + "." + note);
}

void wifi_net_add(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    wifi::Network n;
    n.ssid = util::trim(req.f("ssid"));
    n.psk = req.f("psk");
    n.security = util::trim(req.f("security"));
    n.hidden = req.f("hidden") == "on";
    std::string err;
    if (!wifi::add_extra_network(n, err)) {
        render::redirect_flash(res, wifi_redirect(), "error", "Network: " + err);
        return;
    }
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success",
        "Network '" + n.ssid + "' added.");
}

void wifi_net_delete(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    int idx = atoi(req.f("idx").c_str());
    std::string err;
    if (!wifi::delete_extra_network(idx, err)) {
        render::redirect_flash(res, wifi_redirect(), "error", "Network: " + err);
        return;
    }
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success", "Network deleted.");
}

// Connect to a network chosen in the scan table (or typed into the manual
// dialog). The chosen network becomes the primary one; the previous primary
// moves to the fallback list. That is what makes an explicit Connect win:
// wpa_supplicant prefers the primary, so merely appending a lower-priority
// network would leave the box on the old one.
void wifi_net_connect(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string ssid = util::trim(req.f("ssid"));
    std::string psk = req.f("psk");
    std::string security = norm_sta_security(util::trim(req.f("security")));
    bool manual = req.f("manual") == "1";
    bool hidden = req.f("hidden") == "on";

    if (ssid.empty()) {
        render::redirect_flash(res, wifi_redirect(), "error", "SSID required");
        return;
    }
    if (ssid.size() > 32 || !wifi::conf_value_ok(ssid)) {
        render::redirect_flash(res, wifi_redirect(), "error", BAD_FIELD_MSG);
        return;
    }

    auto cfg = wifi::read_conf();
    std::string old_ssid = cfg["sta_ssid"];
    std::string old_psk = cfg["sta_psk"];
    std::string old_sec = norm_sta_security(cfg["sta_security"]);
    bool old_hidden = cfg["sta_hidden"] == "1";

    auto extras = wifi::read_extra_networks();
    int extra_idx = -1;
    for (size_t i = 0; i < extras.size(); ++i) {
        if (extras[i].ssid == ssid) { extra_idx = (int)i; break; }
    }
    bool is_primary = !old_ssid.empty() && ssid == old_ssid;

    // A blank password keeps the stored one for a known network; a manual
    // add without a password means an open network.
    if (psk.empty() && security != "open") {
        if (is_primary) psk = old_psk;
        else if (extra_idx >= 0) psk = extras[extra_idx].psk;
        else if (manual) security = "open";
        else {
            render::redirect_flash(res, wifi_redirect(), "error",
                "WiFi password is required for an encrypted network");
            return;
        }
    }
    if (security == "open") psk.clear();
    if (!wifi::conf_value_ok(psk)) {
        render::redirect_flash(res, wifi_redirect(), "error", BAD_FIELD_MSG);
        return;
    }
    if (!psk.empty() && (psk.size() < 8 || psk.size() > 63)) {
        render::redirect_flash(res, wifi_redirect(), "error",
            "WiFi password must be 8-63 characters (leave it blank for an open network)");
        return;
    }

    std::string mode = cfg["mode"];
    if (mode != "sta" && mode != "sta+ap")
        mode = (mode == "ap") ? "sta+ap" : "sta";

    if (is_primary) {
        if (!wifi::write_conf({{"mode", mode}, {"sta_ssid", ssid}, {"sta_psk", psk},
                               {"sta_security", security}, {"sta_band", "auto"},
                               {"sta_hidden", hidden ? "1" : ""}})) {
            render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
            return;
        }
    } else {
        wifi::Network chosen;
        chosen.ssid = ssid;
        chosen.psk = psk;
        chosen.security = security;
        chosen.hidden = hidden;
        if (extra_idx >= 0) {
            chosen.hidden = extras[extra_idx].hidden || hidden;
            extras.erase(extras.begin() + extra_idx);
        }
        if (!old_ssid.empty()) {
            // Keep the previous primary as the first fallback. Skip it if its
            // stored password could not form a valid network block anyway.
            bool old_ok = old_sec == "open" ||
                          (old_psk.size() >= 8 && old_psk.size() <= 63 &&
                           wifi::conf_value_ok(old_psk));
            if (old_ok) {
                wifi::Network old;
                old.ssid = old_ssid;
                old.psk = old_psk;
                old.security = old_sec;
                old.hidden = old_hidden;
                extras.insert(extras.begin(), old);
            }
        }
        if (extras.size() > 8) {
            render::redirect_flash(res, wifi_redirect(), "error",
                "At most 8 networks can be saved");
            return;
        }
        if (!wifi::write_conf({{"mode", mode}, {"sta_ssid", chosen.ssid},
                               {"sta_psk", chosen.psk}, {"sta_security", chosen.security},
                               {"sta_band", "auto"},
                               {"sta_hidden", chosen.hidden ? "1" : ""}})) {
            render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
            return;
        }
        std::string err;
        if (!wifi::write_extra_networks(extras, err)) {
            render::redirect_flash(res, wifi_redirect(), "error", "Network: " + err);
            return;
        }
    }

    wifi::apply();
    std::string what = "Connecting to '" + ssid + "'…";
    std::string note = wan_note();
    if (mode == "sta+ap" && !wifi::ap_psk_ok(cfg["ap_psk"]))
        render::redirect_flash(res, wifi_redirect(), "warn",
            what + " The hotspot stays off until you set a hotspot password (8-63 characters) below." + note);
    else
        render::redirect_flash(res, wifi_redirect(), note.empty() ? "success" : "warn",
            what + note);
}

void wifi_txpower(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string tp = util::trim(req.f("tx_power"));
    for (auto& c : tp) c = (char)tolower((unsigned char)c);
    if (tp == "auto" || tp == "max" || tp.empty()) {
        tp = "auto";
    } else {
        int n = atoi(tp.c_str());
        if (!all_digits(tp) || n < 1 || n > 18) {
            render::redirect_flash(res, wifi_redirect(), "error", "TX power must be 'auto' or an integer 1-18 dBm");
            return;
        }
    }
    if (!wifi::write_conf({{"tx_power", tp}})) {
        render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
        return;
    }
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success", std::string("TX power set to ") + (tp == "auto" ? "auto" : tp + " dBm"));
}

}  // namespace routes
