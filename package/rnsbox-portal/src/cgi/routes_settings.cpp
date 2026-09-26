// routes_settings.cpp — Settings tab: server-rendered page + POST-redirect-GET forms.
// Time/clock, NTP, admin password, hostname. Ported from the old Flask settings*
// routes (app.py) — matches their field names, validation order, and flash text.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "session.h"
#include "store.h"
#include "sysinfo.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>   // ::sleep for the wrong-password brute-force speed bump

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }
static std::string settings_redirect() { return web::base() + "/settings"; }

// ---- GET render ----

void settings_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string base = web::base();

    std::string host = sysinfo::hostname();
    std::string wan  = store::read_wan().interface;

    std::vector<std::string> ntp = store::read_ntp_servers();
    std::string ntp_joined;
    for (size_t i = 0; i < ntp.size(); ++i) { if (i) ntp_joined += "\n"; ntp_joined += ntp[i]; }

    time_t now = ::time(nullptr);
    char ts[40]; struct tm tmv; gmtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    bool synced = (now > 1600000000);   // > 2020-09 => the clock has been set

    std::string h = render::page_file("settings");
    h = R(std::move(h), "__BASE__", base);
    h = R(std::move(h), "__SYS_TIME__", E(std::string(ts)));
    h = R(std::move(h), "__TIME_BADGE__",
          synced ? "<span class=\"badge ok\">synced</span>"
                 : "<span class=\"badge warn\">not synced</span>");
    h = R(std::move(h), "__NTP_SERVERS__", E(ntp_joined));
    h = R(std::move(h), "__HOSTNAME__", E(host));
    h = R(std::move(h), "__WAN__", E(wan));

    res.body = render::layout(req, res, "Settings", "settings", h);
}

// ---- POST-redirect-GET handlers ----

void settings_password(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    session::Info s = session::check(req.cookie(session::COOKIE_NAME));
    std::string old     = req.f("old");
    std::string neu     = req.f("new");
    std::string confirm = req.f("confirm");
    if (neu != confirm) {
        render::redirect_flash(res, settings_redirect(), "error", "New password and confirmation do not match");
    } else if (!session::verify_password(s.user, old)) {
        ::sleep(1);   // brute-force speed bump (WAN admin is open — harden hard)
        render::redirect_flash(res, settings_redirect(), "error", "Current password is incorrect");
    } else if (neu.size() < 6) {
        render::redirect_flash(res, settings_redirect(), "error", "New password must be at least 6 characters");
    } else {
        session::set_password(s.user, neu);
        render::redirect_flash(res, settings_redirect(), "success", "Password changed");
    }
}

void hostname_set(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string h = util::trim(req.f("hostname"));
    bool ok = !h.empty() && h.size() <= 63 && h[0] != '-';
    for (char c : h) if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) ok = false;
    if (!ok) {
        render::redirect_flash(res, settings_redirect(), "error", "Invalid hostname (alphanumerics, -, _; max 63 chars)");
        return;
    }
    util::write_file_atomic("/etc/hostname", h + "\n", 0644);
    util::run({"/bin/hostname", h}, "", 5);
    render::redirect_flash(res, settings_redirect(), "success",
        "Hostname changed to '" + h + "'. Reboot to apply everywhere.");
}

void ntp_set(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string raw = req.f("servers");
    for (auto& c : raw) if (c == ',') c = ' ';
    std::vector<std::string> servers;
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && isspace((unsigned char)raw[i])) ++i;
        size_t j = i;
        while (j < raw.size() && !isspace((unsigned char)raw[j])) ++j;
        if (j > i) servers.push_back(raw.substr(i, j - i));
        i = j;
    }
    std::vector<std::string> kept = store::write_ntp_servers(servers);
    size_t saved = kept.size();
    size_t dropped = servers.size() - saved;
    std::string msg = "Saved " + std::to_string(saved) + " NTP server" + (saved == 1 ? "" : "s") + ".";
    if (dropped)
        msg += " " + std::to_string(dropped) + " invalid entr" + (dropped == 1 ? "y" : "ies") + " ignored.";
    msg += " Restart NTP to apply.";
    render::redirect_flash(res, settings_redirect(), "success", msg);
}

void ntp_restart(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    web::run_init("S49ntp");
    render::redirect_flash(res, settings_redirect(), "success",
        "ntpd restarted — it will step the clock once it reaches a server.");
}

void time_browser(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    // Old: epoch = int(float(request.form.get("epoch",""))); empty/non-numeric =>
    // "Could not read the browser time." The onsubmit JS always posts a positive
    // integer (Math.floor(Date.now()/1000)).
    std::string es = util::trim(req.f("epoch"));
    bool parseable = !es.empty();
    for (char c : es) if (!isdigit((unsigned char)c)) parseable = false;
    if (!parseable) {
        render::redirect_flash(res, settings_redirect(), "error", "Could not read the browser time.");
        return;
    }
    long epoch = atol(es.c_str());
    if (epoch < 1577836800L || epoch > 4102444800L) {   // 2020-01-01 .. 2100-01-01
        render::redirect_flash(res, settings_redirect(), "error", "Browser time looks implausible; clock not changed.");
        return;
    }
    time_t e = (time_t)epoch;
    char stamp[40]; struct tm tmv; gmtime_r(&e, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    util::run({"/bin/date", "-s", "@" + std::to_string(epoch)}, "", 5);
    render::redirect_flash(res, settings_redirect(), "success",
        "Clock set to " + std::string(stamp) + " UTC (from your browser). rnsd picks up the new time on its own.");
}

}  // namespace routes
