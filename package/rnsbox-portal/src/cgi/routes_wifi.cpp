// routes_wifi.cpp — WiFi tab: server-rendered page + PRG forms + scan/status JSON.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "wifi.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
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

// ---- GET render ----

void wifi_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;

    if (!wifi::present()) {
        std::string h =
            "<h2>WiFi</h2>\n\n"
            "<section class=\"card\">\n"
            "  <p class=\"muted\">No WiFi radio detected (unexpected on this board &mdash; it has\n"
            "  onboard WiFi). Check <code>/var/log/wifi.log</code> and\n"
            "  <code>dmesg | grep brcmfmac</code>.</p>\n"
            "</section>";
        res.body = render::layout(req, res, "WiFi", "wifi", h);
        return;
    }

    auto cfg = wifi::read_conf();
    wifi::Status st = wifi::status();
    std::string base = web::base();

    std::string h = render::page_file("wifi");
    h = R(std::move(h), "__BASE__", base);

    // ---- Status table ----
    h = R(std::move(h), "__STATUS_MODE__", E(st.mode));

    // Client (STA) cell
    std::string sta;
    if (st.sta.connected) {
        sta = "<span class=\"badge ok\">CONNECTED</span> " + E(st.sta.ssid) +
              " (" + E(st.sta.signal) + " dBm)";
        if (!st.sta.width.empty()) sta += ", " + E(st.sta.width) + " MHz";
        sta += " — " + (st.sta.ip.empty() ? std::string("no IP") : E(st.sta.ip));
    } else {
        sta = "<span class=\"badge warn\">not connected</span>";
        if (st.concurrent) {
            sta += "\n        <div class=\"muted\" style=\"margin-top:4px\">In concurrent <em>sta+ap</em>\n"
                   "        mode the client is limited to <strong>2.4&nbsp;GHz</strong> (single radio\n"
                   "        must share the hotspot's channel). If your upstream network is\n"
                   "        <strong>5&nbsp;GHz-only</strong>, it can't be joined here — switch to\n"
                   "        <em>Client only (sta)</em> mode for a 5&nbsp;GHz uplink (this turns the\n"
                   "        hotspot off).</div>";
        }
    }
    h = R(std::move(h), "__STA_CELL__", sta);

    // Hotspot (AP) cell
    std::string ap;
    if (st.ap.active) {
        ap = "<span class=\"badge ok\">UP</span> on " + E(st.ap.iface) + ",\n        " +
             std::to_string(st.ap.clients) + " client(s)";
        if (!st.ap.width.empty()) ap += ", " + E(st.ap.width) + " MHz";
        ap += ", " + E(st.ap.ip);
    } else {
        ap = "<span class=\"badge warn\">off</span>";
    }
    h = R(std::move(h), "__AP_CELL__", ap);

    // TX power cell
    std::string tx = "configured <span class=\"badge\">" + E(st.tx_power) + "</span>";
    if (st.sta.connected && !st.sta.txpower.empty()) tx += " · STA " + E(st.sta.txpower) + " dBm";
    if (st.ap.active && !st.ap.txpower.empty())      tx += " · AP " + E(st.ap.txpower) + " dBm";
    h = R(std::move(h), "__TXPOWER_CELL__", tx);

    // Mode select markers
    const std::string& m = cfg["mode"];
    h = R(std::move(h), "__SEL_MODE_OFF__",   m == "off"    ? "selected" : "");
    h = R(std::move(h), "__SEL_MODE_STA__",   m == "sta"    ? "selected" : "");
    h = R(std::move(h), "__SEL_MODE_AP__",    m == "ap"     ? "selected" : "");
    h = R(std::move(h), "__SEL_MODE_STAAP__", m == "sta+ap" ? "selected" : "");

    bool concurrent = (m == "sta+ap");

    // Join-section concurrent note
    std::string note;
    if (concurrent) {
        note = "<p class=\"muted\">Concurrent mode is on, so the client is restricted to\n"
               "  <strong>2.4&nbsp;GHz</strong> networks (same radio as the hotspot).\n"
               "  5&nbsp;GHz networks are shown greyed out — they can't be joined until you\n"
               "  switch to <em>Client only</em> mode.</p>";
    }
    h = R(std::move(h), "__CONCURRENT_NOTE__", note);

    h = R(std::move(h), "__STA_SSID__", E(cfg["sta_ssid"]));

    // Hotspot form
    h = R(std::move(h), "__AP_ENABLE_CHECKED__", (m == "ap" || m == "sta+ap") ? "checked" : "");
    h = R(std::move(h), "__AP_SSID__", E(cfg["ap_ssid"]));
    h = R(std::move(h), "__AP_PSK__",  E(cfg["ap_psk"]));

    std::string lock = concurrent ? " <span class=\"muted\">(locked — follows client link)</span>" : "";
    h = R(std::move(h), "__AP_CHANNEL_LOCK__", lock);
    h = R(std::move(h), "__AP_WIDTH_LOCK__",   lock);
    h = R(std::move(h), "__AP_CHANNEL_DISABLED__", concurrent ? "disabled" : "");
    h = R(std::move(h), "__AP_HTMODE_DISABLED__",  concurrent ? "disabled" : "");

    // Channel options: auto + 1..13
    std::string chopts = "<option value=\"auto\" " + std::string(cfg["ap_channel"] == "auto" ? "selected" : "") + ">auto (ACS)</option>";
    for (int c = 1; c <= 13; ++c) {
        std::string cs = std::to_string(c);
        chopts += "\n        <option value=\"" + cs + "\" " + (cfg["ap_channel"] == cs ? "selected" : "") + ">" + cs + "</option>";
    }
    h = R(std::move(h), "__AP_CHANNEL_OPTIONS__", chopts);

    h = R(std::move(h), "__SEL_HT20__", cfg["ap_htmode"] != "ht40" ? "selected" : "");
    h = R(std::move(h), "__SEL_HT40__", cfg["ap_htmode"] == "ht40" ? "selected" : "");

    // TX power options: auto + 18..1
    const std::string& tp = cfg["tx_power"];
    bool autoSel = (tp == "auto" || tp == "max" || tp.empty());
    std::string txopts = "<option value=\"auto\" " + std::string(autoSel ? "selected" : "") + ">auto (chip max ≈ 18 dBm)</option>";
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

void wifi_connect(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string ssid = util::trim(req.f("ssid"));
    std::string psk = req.f("psk");
    if (ssid.empty()) {
        render::redirect_flash(res, wifi_redirect(), "error", "SSID required");
        return;
    }
    auto cur = wifi::read_conf();
    std::string mode = (cur["mode"] == "ap" || cur["mode"] == "sta+ap") ? "sta+ap" : "sta";
    wifi::write_conf({{"mode", mode}, {"sta_ssid", ssid}, {"sta_psk", psk}});
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success", "Connecting to '" + ssid + "'…");
}

void wifi_ap(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string ssid = util::trim(req.f("ap_ssid"));
    std::string psk = req.f("ap_psk");
    bool enable = req.f("ap_enable") == "on";
    if (enable && (ssid.empty() || psk.size() < 8)) {
        render::redirect_flash(res, wifi_redirect(), "error", "AP needs an SSID and a password of at least 8 chars");
        return;
    }
    auto cur = wifi::read_conf();
    bool sta_on = (cur["mode"] == "sta" || cur["mode"] == "sta+ap") && !cur["sta_ssid"].empty();
    std::string mode = enable ? (sta_on ? "sta+ap" : "ap") : (sta_on ? "sta" : "off");
    std::map<std::string, std::string> updates = {{"mode", mode}, {"ap_ssid", ssid}, {"ap_psk", psk}};
    // Channel/width are settable only in AP-only mode (concurrent forces them to
    // follow the STA, and the disabled <select> submits nothing). Key off the
    // *rendered* mode so we don't clobber stored AP-only values with form defaults.
    if (cur["mode"] != "sta+ap") {
        std::string chan = util::trim(req.f("ap_channel"));   if (chan.empty()) chan = "6";
        std::string htmode = util::trim(req.f("ap_htmode"));  if (htmode.empty()) htmode = "ht20";
        if (chan != "auto") {
            int n = atoi(chan.c_str());
            if (!all_digits(chan) || n < 1 || n > 13) {
                render::redirect_flash(res, wifi_redirect(), "error", "AP channel must be 1-13 or auto");
                return;
            }
        }
        if (htmode != "ht20" && htmode != "ht40") htmode = "ht20";
        updates["ap_channel"] = chan;
        updates["ap_htmode"] = htmode;
    }
    wifi::write_conf(updates);
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success", "AP settings applied");
}

void wifi_mode(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string mode = req.f("mode");
    if (mode != "off" && mode != "sta" && mode != "ap" && mode != "sta+ap") mode = "off";
    wifi::write_conf({{"mode", mode}});
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success", "WiFi mode set to " + mode);
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
    wifi::write_conf({{"tx_power", tp}});
    wifi::apply();
    render::redirect_flash(res, wifi_redirect(), "success", std::string("TX power set to ") + (tp == "auto" ? "auto" : tp + " dBm"));
}

}  // namespace routes