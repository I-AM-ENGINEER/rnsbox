// routes_wifi.cpp — WiFi tab: server-rendered page + PRG forms + scan/status JSON.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "wifi.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>

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

// WPA2-PSK / WPA3-SAE / transition, plus open for a client network. The
// hotspot always requires a passphrase (no open AP).
static std::string security_options(const std::string& cur, bool allow_open) {
    std::string o;
    o += opt("wpa2", "WPA2-PSK", cur);
    o += opt("wpa3", "WPA3-SAE", cur);
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

static std::string standard_options(const std::string& cur) {
    return opt("auto", "Auto (driver default)", cur) +
           opt("n", "Wi-Fi 4 (802.11n)", cur) +
           opt("ac", "Wi-Fi 5 (802.11ac)", cur) +
           opt("ax", "Wi-Fi 6 (802.11ax)", cur);
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

static std::string channel_options(const std::string& band, const std::string& cur) {
    std::string o = opt("auto", "auto (ACS)", cur);
    if (band == "5") {
        for (int c : {36, 40, 44, 48, 149, 153, 157, 161, 165})
            o += opt(std::to_string(c), std::to_string(c), cur);
    } else {
        for (int c = 1; c <= 13; ++c)
            o += opt(std::to_string(c), std::to_string(c), cur);
    }
    return o;
}

static std::string norm_sta_security(const std::string& s) {
    if (s == "open" || s == "wpa3" || s == "wpa2+wpa3") return s;
    return "wpa2";
}

static std::string norm_ap_security(const std::string& s) {
    if (s == "wpa3" || s == "wpa2+wpa3") return s;
    return "wpa2";
}

// ---- GET render ----

void wifi_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;

    if (!wifi::present()) {
        std::string h =
            "<h2>WiFi</h2>\n\n"
            "<section class=\"card\">\n"
            "  <p class=\"muted\">No WiFi radio detected. On the -W variant the AIC8800D80\n"
            "  (SDIO) radio should surface as wlan0 via S35wifi; non-W boards have no\n"
            "  radio and stay here. Check <code>/var/log/wifi.log</code> and\n"
            "  <code>dmesg | grep aic8800</code>.</p>\n"
            "</section>";
        res.body = render::layout(req, res, "WiFi", "wifi", h);
        return;
    }

    auto cfg = wifi::read_conf();
    wifi::Status st = wifi::status();
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
    h = R(std::move(h), "__STATUS_MODE__", mode_badge);

    // Client (STA) cell
    std::string sta;
    if (!sta_on) {
        sta = "<span class=\"badge\">off</span> <span class=\"muted\">STA mode is disabled</span>";
    } else if (st.sta.connected) {
        sta = "<span class=\"badge ok\">CONNECTED</span> " + E(st.sta.ssid) +
              " (" + E(st.sta.signal) + " dBm)";
        if (!st.sta.width.empty()) sta += ", " + E(st.sta.width) + " MHz";
        sta += " — " + (st.sta.ip.empty() ? std::string("no IP") : E(st.sta.ip));
    } else if (cfg["sta_ssid"].empty()) {
        sta = "<span class=\"badge\">not configured</span> "
              "<span class=\"muted\">scan for a network and connect</span>";
    } else {
        sta = "<span class=\"badge warn\">not connected</span>";
        if (concurrent) {
            sta += "\n        <div class=\"muted\" style=\"margin-top:4px\">In concurrent <em>STA + AP</em>\n"
                   "        mode the client follows the hotspot's band (single radio must share\n"
                   "        one channel). If your upstream network is on the other band, switch\n"
                   "        the hotspot band or disable AP mode.</div>";
        }
    }
    // The joined network overlaps this box's LAN subnet 10.42.0.0/24 (another
    // RNSBox's hotspot): /etc/dhcpcd.exit-hook recorded why it's no uplink.
    if (sta_on) {
        std::string why = util::trim(util::read_file("/run/wifi-sta-conflict", 1024));
        if (!why.empty())
            sta += "\n        <div class=\"flash warn\" style=\"margin-top:4px\">" + E(why) + "</div>";
    }
    h = R(std::move(h), "__STA_CELL__", sta);

    // Hotspot (AP) cell
    std::string ap;
    if (!ap_on) {
        ap = "<span class=\"badge\">off</span> <span class=\"muted\">AP mode is disabled</span>";
    } else if (st.ap.active) {
        ap = "<span class=\"badge ok\">UP</span> on " + E(st.ap.iface) + ",\n        " +
             std::to_string(st.ap.clients) + " client(s)";
        if (!st.ap.width.empty()) ap += ", " + E(st.ap.width) + " MHz";
        // Normally a port of the LAN bridge, sharing its subnet + DHCP pool. Not
        // bridged (no bridge in the kernel): nothing serves its clients.
        if (!st.ap.bridge.empty())
            ap += ", bridged into " + E(st.ap.bridge) + (st.ap.ip.empty() ? std::string() : " (" + E(st.ap.ip) + ")");
        else if (!st.ap.ip.empty())
            ap += ", " + E(st.ap.ip);
        else
            ap += ", <span class=\"badge warn\">not in the LAN bridge</span>";
    } else {
        ap = "<span class=\"badge warn\">not running</span>";
        std::string why = util::trim(util::read_file("/run/wifi-ap-refused", 512));
        if (!why.empty()) ap += " <span class=\"muted\">" + E(why) + "</span>";
    }
    h = R(std::move(h), "__AP_CELL__", ap);

    // TX power cell
    std::string tx = "configured <span class=\"badge\">" + E(st.tx_power) + "</span>";
    if (st.sta.connected && !st.sta.txpower.empty()) tx += " · STA " + E(st.sta.txpower) + " dBm";
    if (st.ap.active && !st.ap.txpower.empty())      tx += " · AP " + E(st.ap.txpower) + " dBm";
    h = R(std::move(h), "__TXPOWER_CELL__", tx);

    // ---- Mode toggles ----
    h = R(std::move(h), "__STA_CHECKED__", sta_on ? "checked" : "");
    h = R(std::move(h), "__AP_CHECKED__",  ap_on  ? "checked" : "");
    h = R(std::move(h), "__SCAN_DISABLED__", sta_on ? "" : "disabled");
    h = R(std::move(h), "__STA_OFF_NOTE__", sta_on ? "" :
        "<p class=\"muted\">Enable <strong>STA mode</strong> above to scan and join a network.</p>");
    h = R(std::move(h), "__AP_OFF_NOTE__", ap_on ? "" :
        "<p class=\"muted\">Enable <strong>AP mode</strong> above to run the hotspot.</p>");

    // Join-section concurrent note
    std::string note;
    if (concurrent) {
        note = "<p class=\"muted\">Concurrent mode is on: the client follows the\n"
               "  hotspot's band and the hotspot inherits the client's channel and width\n"
               "  (single radio). Switch to STA-only (AP off) to use the other band.</p>";
    }
    h = R(std::move(h), "__CONCURRENT_NOTE__", note);

    // ---- STA settings ----
    h = R(std::move(h), "__STA_SSID__", E(cfg["sta_ssid"]));
    h = R(std::move(h), "__STA_SECURITY_OPTIONS__", security_options(cfg["sta_security"], true));
    h = R(std::move(h), "__STA_BAND_OPTIONS__", band_options(cfg["sta_band"], true));
    h = R(std::move(h), "__STA_BAND_DISABLED__", concurrent ? "disabled" : "");
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
    h = R(std::move(h), "__AP_SECURITY_OPTIONS__", security_options(cfg["ap_security"], false));
    h = R(std::move(h), "__AP_BAND_OPTIONS__", band_options(cfg["ap_band"], false));
    h = R(std::move(h), "__AP_STANDARD_OPTIONS__", standard_options(cfg["ap_standard"]));

    std::string sel_chan = cfg["ap_channel"];
    if (!channel_valid(cfg["ap_band"], sel_chan, cfg["ap_width"])) sel_chan = "auto";
    h = R(std::move(h), "__AP_CHANNEL_OPTIONS__", channel_options(cfg["ap_band"], sel_chan));

    std::string width_opts;
    width_opts += opt("20", "20 MHz", cfg["ap_width"]);
    width_opts += opt("40", "40 MHz", cfg["ap_width"]);
    width_opts += opt("80", "80 MHz (5 GHz, 802.11ac/ax)", cfg["ap_width"]);
    h = R(std::move(h), "__AP_WIDTH_OPTIONS__", width_opts);
    h = R(std::move(h), "__AP_HIDDEN_CHECKED__", cfg["ap_hidden"] == "1" ? "checked" : "");

    // Mode says hotspot-on but S35wifi refuses a weak PSK (e.g. a box still on
    // the old 'changeme123' default) — explain the missing AP.
    std::string psk_note;
    if (ap_on && !wifi::ap_psk_ok(cfg["ap_psk"]))
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
    if (ssid.empty()) {
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
    if (security == "open") {
        psk.clear();                       // open network: no key at all
    } else if (psk.size() < 8 || psk.size() > 63) {
        render::redirect_flash(res, wifi_redirect(), "error",
            "WiFi password must be 8-63 characters (choose Open for a network without a password)");
        return;
    }
    auto cur = wifi::read_conf();
    std::string mode = (cur["mode"] == "ap" || cur["mode"] == "sta+ap") ? "sta+ap" : "sta";
    if (!wifi::write_conf({{"mode", mode}, {"sta_ssid", ssid}, {"sta_psk", psk},
                           {"sta_security", security}, {"sta_band", band},
                           {"sta_hidden", hidden ? "1" : ""}})) {
        render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
        return;
    }
    wifi::apply();
    // Joining still works with a weak stored hotspot PSK — S35wifi just keeps
    // the AP off (and logs why). Say so instead of a silent missing hotspot.
    std::string note = wan_note();
    if (mode == "sta+ap" && !wifi::ap_psk_ok(cur["ap_psk"]))
        render::redirect_flash(res, wifi_redirect(), "warn",
            "Connecting to '" + ssid + "'… The hotspot stays off until you set a hotspot password (8-63 characters) below." + note);
    else
        render::redirect_flash(res, wifi_redirect(), note.empty() ? "success" : "warn",
            "Connecting to '" + ssid + "'…" + note);
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

    if (security.empty()) security = "wpa2";
    if (security != "wpa2" && security != "wpa3" && security != "wpa2+wpa3") {
        render::redirect_flash(res, wifi_redirect(), "error", "Unknown hotspot security mode");
        return;
    }
    if (band.empty()) band = "2.4";
    if (band != "2.4" && band != "5") {
        render::redirect_flash(res, wifi_redirect(), "error", "Hotspot band must be 2.4 or 5 GHz");
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
    // A running hotspot must always have a good password; while AP mode is
    // off an empty password is allowed (the mode toggle is what refuses to
    // switch it on without one), but a typed bad value is still rejected.
    auto cur = wifi::read_conf();
    bool ap_on = (cur["mode"] == "ap" || cur["mode"] == "sta+ap");
    if (ap_on || !psk.empty()) {
        std::string why = ap_psk_problem(psk);
        if (!why.empty()) {
            render::redirect_flash(res, wifi_redirect(), "error", why);
            return;
        }
    }
    // ap_htmode is the pre-ap_width knob: keep it in sync for older readers.
    std::string htmode = (width == "40") ? "ht40" : "ht20";
    if (!wifi::write_conf({{"ap_ssid", ssid}, {"ap_psk", psk},
                           {"ap_security", security}, {"ap_band", band},
                           {"ap_standard", standard}, {"ap_width", width},
                           {"ap_channel", chan}, {"ap_htmode", htmode},
                           {"ap_hidden", hidden ? "1" : ""}})) {
        render::redirect_flash(res, wifi_redirect(), "error", "Could not save the WiFi config");
        return;
    }
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success", "AP settings applied");
}

void wifi_mode(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string mode = util::trim(req.f("mode"));
    // New UI: two independent toggles. Missing checkbox fields mean "off";
    // the legacy mode=<off|sta|ap|sta+ap> form still works.
    if (mode.empty()) {
        bool sta = req.f("sta_enable") == "on";
        bool ap  = req.f("ap_enable") == "on";
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
    if (mode == "ap" || mode == "sta+ap") {
        auto cur = wifi::read_conf();
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
