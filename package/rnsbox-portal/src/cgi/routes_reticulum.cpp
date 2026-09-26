// routes_reticulum.cpp — Reticulum tab: server-rendered page + PRG forms +
// AJAX JSON endpoints (rnsd status / live HaLow modem). Faithful port of the
// old Flask reticulum() view + its POST/AJAX routes.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "sysinfo.h"
#include "updatecheck.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }
static std::string ret_redirect() { return web::base() + "/reticulum"; }

// store._truthy: falsey iff stripped/lowered value in {no,0,false,off,""}.
static bool truthy(const std::string& s) {
    std::string v = util::trim(s);
    for (char& c : v) c = (char)tolower((unsigned char)c);
    return !(v == "no" || v == "0" || v == "false" || v == "off" || v.empty());
}

// --- tiny best-effort JSON field readers (state file / python helper out) ---
static std::string json_str(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size() || j[p] != '"') return "";   // null / number / missing
    ++p;
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            ++p;
            switch (j[p]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                default:  out += j[p]; break;
            }
        } else {
            out += j[p];
        }
        ++p;
    }
    return out;
}
static bool json_true(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    return j.compare(p, 4, "true") == 0;
}
static long json_num(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return 0;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    return strtol(j.c_str() + p + 1, nullptr, 10);
}

// Port of store.reticulum_has_tcp_client (the C++ store keeps this private).
static bool reticulum_has_tcp_client(const std::string& host, int port_i) {
    std::string cfg = util::read_file(store::RETICULUM_CONFIG, 256 * 1024);
    std::string port = std::to_string(port_i);
    bool is_tcp = false, have_host = false, have_port = false;
    std::string cur_host, cur_port;
    size_t i = 0;
    while (i <= cfg.size()) {
        size_t nl = cfg.find('\n', i);
        std::string ln = cfg.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? cfg.size() + 1 : nl + 1;
        std::string s = util::trim(ln);
        if (!s.empty() && s[0] == '[') { is_tcp = false; have_host = have_port = false; cur_host.clear(); cur_port.clear(); continue; }
        if (s.empty() || s[0] == '#' || s.find('=') == std::string::npos) continue;
        size_t eq = s.find('=');
        std::string k = util::trim(s.substr(0, eq));
        std::string v = util::trim(s.substr(eq + 1));
        size_t hash = v.find('#');
        if (hash != std::string::npos) v = util::trim(v.substr(0, hash));
        for (char& c : k) c = (char)tolower((unsigned char)c);
        if (k == "type") is_tcp = (v == "TCPClientInterface");
        else if (k == "target_host") { cur_host = v; have_host = true; }
        else if (k == "target_port") { cur_port = v; have_port = true; }
        if (is_tcp && have_host && have_port && cur_host == host && cur_port == port) return true;
    }
    return false;
}

// ===================== GET render =====================

void reticulum_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string base = web::base();

    sysinfo::Rnsd rn = sysinfo::rnsd_status();     // cheap liveness (running/pid)
    store::SlipConfig sl = store::read_slip();
    sysinfo::Slip slst = sysinfo::slip_status();
    bool slip_on = truthy(sl.enabled);
    int days = store::read_rnsd_restart_days();

    std::string cfg = store::read_reticulum_config();
    if (cfg.empty())
        cfg = "# /etc/reticulum/config not present yet — rnsd creates a\n"
              "# default on first start. Edit here to override.\n";

    // update-check state (cheap, no python spawn on page paint): installed comes
    // from the dist-info (always known); latest/error/checked_at from the last
    // check's persisted JSON; update_available is recomputed against installed.
    updatecheck::State u = updatecheck::display();
    std::string installed = u.installed;
    std::string latest    = u.latest;
    std::string uerror    = u.error;
    bool update_available = u.update_available;
    long checked_at       = u.checked_at;

    std::string ucv = util::conf_get(store::CRON_FILE, "update_check");
    bool auto_update = truthy(ucv.empty() ? "yes" : ucv);   // default on

    std::string h = render::page_file("reticulum");
    h = R(std::move(h), "__BASE__", base);

    h = R(std::move(h), "__RNSD_STATE__", rn.running
        ? "<span class=\"badge ok\">RUNNING</span> (pid " + E(rn.pid) + ")"
        : "<span class=\"badge err\">STOPPED</span>");

    h = R(std::move(h), "__RNS_VERSION__", E(installed.empty() ? "unknown" : installed));

    std::string badge;
    if (update_available)
        badge = "<span class=\"badge warn\">update available: " + E(latest) + "</span>";
    else if (!latest.empty() && uerror.empty())
        badge = "<span class=\"badge ok\">up to date</span>";
    h = R(std::move(h), "__UPDATE_BADGE__", badge);

    std::string chk;
    if (checked_at > 0) {
        char buf[64]; time_t t = (time_t)checked_at; struct tm g;
        gmtime_r(&t, &g);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &g);
        chk = "last checked " + E(buf);
    } else {
        chk = "not checked yet";
    }
    if (!uerror.empty())
        chk += " <span class=\"muted\">— last attempt failed (offline?)</span>";
    h = R(std::move(h), "__UPDATE_CHECK_LINE__", chk);

    std::string apply_form;
    if (update_available) {
        std::string le = E(latest);
        // Async button (see doUpdate() in reticulum.html): fetch + spinner, no
        // page navigation, so a long in-place apply never blank-hangs or lands
        // the browser on a phantom-logout page.
        apply_form =
            "<button type=\"button\" class=\"btn-primary\" onclick=\"doUpdate('applyupdate', this, true)\">Update rnsd to " + le + " now</button>";
    }
    h = R(std::move(h), "__APPLY_UPDATE_FORM__", apply_form);

    h = R(std::move(h), "__AUTO_UPDATE_CHECKED__", auto_update ? "checked" : "");

    std::string opts = "<option value=\"0\" " + std::string(days == 0 ? "selected" : "") + ">None (default)</option>";
    for (int d = 1; d <= 7; ++d)
        opts += "\n      <option value=\"" + std::to_string(d) + "\" " +
                (days == d ? "selected" : "") + ">every " + std::to_string(d) +
                " day" + (d > 1 ? "s" : "") + "</option>";
    h = R(std::move(h), "__RESTART_DAYS_OPTIONS__", opts);

    std::string link;
    if (slst.up)
        link = "<span class=\"badge ok\">UP</span> " + E(sl.local_ip) + " &rarr; " + E(sl.peer_ip);
    else if (slst.present)
        link = "<span class=\"badge warn\">DOWN</span> (attached, no address)";
    else if (slip_on)
        link = "<span class=\"badge warn\">not attached</span> (modem wired?)";
    else
        link = "<span class=\"badge\">disabled</span>";
    h = R(std::move(h), "__SLIP_LINK_STATUS__", link);

    std::string halow;
    if (slip_on) {
        halow =
            "<div id=\"halow-live\" class=\"halow-live muted\">Loading modem status…</div>\n"
            "  <details class=\"neigh\" id=\"halow-neigh\" hidden>\n"
            "    <summary>Neighbours heard (<span id=\"halow-ncount\">0</span>)</summary>\n"
            "    <div class=\"body\"><table class=\"data\" id=\"halow-ntable\"></table></div>\n"
            "  </details>\n"
            "  <div class=\"halow-actions\">\n"
            "    <a class=\"btn\" href=\"" + base + "/modem\" target=\"_blank\" rel=\"noopener\">Open modem web UI &rarr;</a>\n"
            "    <form method=\"post\" action=\"" + base + "/reticulum/halow/add\" class=\"inline\" id=\"halow-add-form\" hidden>\n"
            "      <input type=\"hidden\" name=\"iface_name\" id=\"halow-add-name\" value=\"\">\n"
            "      <input type=\"hidden\" name=\"iface_port\" id=\"halow-add-port\" value=\"8001\">\n"
            "      <button type=\"submit\" class=\"btn-primary\">+ Add to rnsd config</button>\n"
            "    </form>\n"
            "  </div>\n"
            "  <p class=\"muted\" style=\"margin:6px 0 0\">Full stats, graphs &amp; config live in the modem's own web page.</p>";
    }
    h = R(std::move(h), "__HALOW_LIVE_BLOCK__", halow);

    bool halow_in_config = reticulum_has_tcp_client(sl.peer_ip.empty() ? "192.168.7.2" : sl.peer_ip, 8001);
    h = R(std::move(h), "__HALOW_IN_CONFIG__", halow_in_config ? "true" : "false");

    h = R(std::move(h), "__SLIP_ENABLED_CHECKED__", slip_on ? "checked" : "");
    h = R(std::move(h), "__SLIP_DEVICE__",   E(sl.device));
    h = R(std::move(h), "__SLIP_BAUD__",     E(sl.baud));
    h = R(std::move(h), "__SLIP_LOCAL_IP__", E(sl.local_ip));
    h = R(std::move(h), "__SLIP_PEER_IP__",  E(sl.peer_ip));
    h = R(std::move(h), "__CONFIG__",        E(cfg));

    res.body = render::layout(req, res, "Reticulum", "reticulum", h);
}

// ===================== AJAX GET JSON =====================

void reticulum_status(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    bool with_raw = (req.a("raw") == "1");   // slow path: cold-starts rnstatus (~5 s)
    // sysinfo::rnsd_status uses the pidfile + /proc-cmdline scan; a plain
    // `pidof rnsd` never matches (rnsd's comm is python3).
    sysinfo::Rnsd rn = sysinfo::rnsd_status(with_raw);
    store::SlipConfig sl = store::read_slip();
    std::string o = std::string("{\"running\":") + (rn.running ? "true" : "false") +
        ",\"pid\":" + (rn.pid.empty() ? "0" : rn.pid) +
        ",\"rss_mb\":" + std::to_string(rn.rss_mb) +
        ",\"restart_days\":" + std::to_string(store::read_rnsd_restart_days()) +
        ",\"slip\":{\"enabled\":\"" + http::json_escape(sl.enabled) +
        "\",\"device\":\"" + http::json_escape(sl.device) +
        "\",\"baud\":\"" + http::json_escape(sl.baud) +
        "\",\"local_ip\":\"" + http::json_escape(sl.local_ip) +
        "\",\"peer_ip\":\"" + http::json_escape(sl.peer_ip) + "\"}";
    if (with_raw)
        o += ",\"raw\":\"" + http::json_escape(rn.raw) + "\"";
    o += "}";
    res.content_type = "application/json";
    res.body = o;
}

// Best-effort live-modem endpoint. The real stats need the python halow_status()
// helper (HTTP-over-SLIP + JSON), which has no native C++ port yet — so this
// returns reachable:false and the page's JS renders the "unreachable / sl0 down"
// state faithfully. See NOTE.
void reticulum_halow(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    // Live modem status is HTTP-over-SLIP + JSON — delegated to the python helper
    // (fails soft). It only spawns on this async fetch, never on a page paint,
    // and only when the page renders the card (SLIP enabled).
    util::RunResult rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/halow.py"}, "", 10);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') {
        res.body = rr.out;
        return;
    }
    // helper missing/failed — minimal best-effort shape so the JS still renders.
    store::SlipConfig sl = store::read_slip();
    bool enabled = truthy(sl.enabled);
    bool sl0_up = sysinfo::slip_status().up;
    std::string peer = sl.peer_ip.empty() ? "192.168.7.2" : sl.peer_ip;
    res.body = std::string("{\"enabled\":") + (enabled ? "true" : "false") +
        ",\"peer\":\"" + http::json_escape(peer) + "\"" +
        ",\"local\":\"" + http::json_escape(sl.local_ip) + "\"" +
        ",\"sl0_up\":" + (sl0_up ? "true" : "false") +
        ",\"reachable\":false,\"neighbours\":[]}";
}

// ===================== PRG POST handlers =====================

void reticulum_config_save(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string err;
    if (!store::write_reticulum_config(req.f("config"), err)) {
        render::redirect_flash(res, ret_redirect(), "error", "Error: " + err);
        return;
    }
    // NB: matches old Flask — saving does NOT restart rnsd.
    render::redirect_flash(res, ret_redirect(), "success", "Config saved — restart rnsd to apply.");
}

void reticulum_restart(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    web::run_init("S82rnsd");
    render::redirect_flash(res, ret_redirect(), "success", "rnsd restart requested");
}

void reticulum_autorestart(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    int days = atoi(req.f("days").c_str());
    std::string err;
    if (!store::write_rnsd_restart_days(days, err)) {
        render::redirect_flash(res, ret_redirect(), "error", "Bad input: " + err);
        return;
    }
    web::run_init("S84cron", "reload");
    if (days == 0)
        render::redirect_flash(res, ret_redirect(), "success", "rnsd auto-restart disabled.");
    else
        render::redirect_flash(res, ret_redirect(), "success",
            "rnsd will auto-restart every " + std::to_string(days) + " day(s) at 04:00.");
}

void reticulum_slip(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::SlipConfig c;
    c.enabled  = req.f("enabled") == "on" ? "yes" : "no";
    c.device   = util::trim(req.f("device"));
    c.baud     = util::trim(req.f("baud"));
    c.local_ip = util::trim(req.f("local_ip"));
    c.peer_ip  = util::trim(req.f("peer_ip"));
    std::string err;
    if (!store::write_slip(c, err)) {
        render::redirect_flash(res, ret_redirect(), "error", "SLIP: " + err + ".");
        return;
    }
    web::run_init("S31slip", "reload");
    if (c.enabled == "yes")
        render::redirect_flash(res, ret_redirect(), "success",
            "SLIP link enabled on " + c.device + " @ " + c.baud + " baud. Once the modem is "
            "reachable, use “Add to rnsd config” on the HaLow card below to route "
            "Reticulum through it (TCPClientInterface → " + c.peer_ip + ":8001).");
    else
        render::redirect_flash(res, ret_redirect(), "success", "SLIP link disabled.");
}

void reticulum_halow_add(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::SlipConfig sl = store::read_slip();
    std::string peer = sl.peer_ip.empty() ? "192.168.7.2" : sl.peer_ip;
    std::string name = util::trim(req.f("iface_name"));
    int port = atoi(req.f("iface_port").c_str());
    if (port < 1 || port > 65535) port = 8001;
    // NOTE: the old view queried the modem for its hostname when the browser
    // didn't supply one (JS off). That path needs halow_status() (no C++ port);
    // store::add_halow_interface() falls back to "RNode-Halow" for a blank name.
    std::string msg;
    if (store::add_halow_interface(name, peer, port, msg)) {
        web::run_init("S82rnsd");
        render::redirect_flash(res, ret_redirect(), "success",
            "Added HaLow interface “" + msg + "” (TCPClientInterface → " +
            peer + ":" + std::to_string(port) + ") and restarted rnsd.");
    } else if (msg == "already") {
        render::redirect_flash(res, ret_redirect(), "success",
            "That HaLow interface is already in the Reticulum config.");
    } else {
        render::redirect_flash(res, ret_redirect(), "error",
            "Couldn't add the HaLow interface: " + msg + ".");
    }
}

void reticulum_autoupdate(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enabled = req.f("auto_update") == "on";
    std::string err;
    if (!store::write_update_check(enabled, err)) {   // NOTE: add to store (see notes)
        render::redirect_flash(res, ret_redirect(), "error", "Error: " + err);
        return;
    }
    web::run_init("S84cron", "reload");
    render::redirect_flash(res, ret_redirect(), "success",
        std::string("Automatic update checks ") + (enabled ? "enabled (daily)." : "disabled."));
}

// checkupdate / applyupdate — ASYNC JSON endpoints (the reticulum.html doUpdate()
// spinner calls these via fetch). They shell out to the python updatecheck helper
// and return {"ok":bool,"reload":bool,"message":"..."}. Auth failure is a JSON 401
// (require_auth json=true), so the page shows "session expired" instead of
// navigating to a login page mid-update.
static void update_json(Response& res, bool ok, bool reload, const std::string& message) {
    res.content_type = "application/json";
    res.body = std::string("{\"ok\":") + (ok ? "true" : "false") +
        ",\"reload\":" + (reload ? "true" : "false") +
        ",\"message\":\"" + http::json_escape(message) + "\"}";
}

void reticulum_checkupdate(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    auto rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/updatecheck.py", "check"}, "", 60);
    std::string j = rr.out;
    if (j.find("installed") == std::string::npos && j.find("error") == std::string::npos) {
        update_json(res, false, false, "Update check failed — the box may be offline. Last known latest: unknown.");
        return;
    }
    std::string uerror = json_str(j, "error");
    std::string latest = json_str(j, "latest");
    std::string installed = json_str(j, "installed");
    bool avail = json_true(j, "update_available");
    if (!uerror.empty())
        update_json(res, false, false,
            "Update check failed — the box may be offline. Last known latest: " +
            (latest.empty() ? "unknown" : latest) + ".");
    else if (avail)
        update_json(res, true, true, "Update available: rns " + latest + " (installed " + installed + ").");
    else
        update_json(res, true, true, "rnsd is up to date (" + (installed.empty() ? "unknown" : installed) + ").");
}

void reticulum_applyupdate(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    // Long: pip install + rnsd restart. uhttpd's script/network timeouts are
    // raised to 200s in S81router so this isn't killed mid-flight.
    auto rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/updatecheck.py", "apply"}, "", 190);
    std::string j = rr.out;
    bool ok = json_true(j, "ok");
    bool rolled = json_true(j, "rolled_back");
    std::string from = json_str(j, "from");
    std::string to = json_str(j, "to");
    std::string uerror = json_str(j, "error");
    if (ok)
        update_json(res, true, true, "rnsd updated " + from + " → " + to + " and restarted.");
    else if (rolled)
        update_json(res, false, false, "Update failed (" + uerror + ") — reverted to " + from + ".");
    else
        update_json(res, false, false, "Update not applied: " + (uerror.empty() ? "unknown error" : uerror) + ".");
}

}  // namespace routes