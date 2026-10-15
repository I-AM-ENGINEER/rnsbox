// routes_updates.cpp — Updates tab: A/B whole-OS update status + controls.
//
// The page is a thin, passive view: the CGI reads /run/rnsbox-sysupdate.json
// (worker state), /etc/rnsbox/update.conf (config) and the boot slot files —
// no python spawn, no network on render. Actions spawn the flock-guarded
// worker detached (util::run + the worker's own double-fork; argv execve,
// never a shell) and redirect; the page's JS polls /updates/data until the
// phase settles. Everything mutating is POST + PRG like the other tabs.

#include "routes.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "util.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

using http::Request;
using http::Response;

namespace routes {

namespace {

std::string R(std::string s, const std::string& a, const std::string& b) {
    return web::replace_all(std::move(s), a, b);
}
std::string E(const std::string& s) { return render::esc(s); }
std::string upd_redirect() { return web::base() + "/updates"; }

std::string fmt_epoch(const std::string& epoch_str) {
    if (epoch_str.empty() || epoch_str == "0") return "never";
    char* end = nullptr;
    long long t = strtoll(epoch_str.c_str(), &end, 10);
    if (!end || *end || t <= 0) return "never";
    time_t tt = (time_t)t;
    struct tm tmv;
    gmtime_r(&tt, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv);
    return buf;
}

std::string fmt_epoch(long long t) { return fmt_epoch(std::to_string(t)); }

// Phase -> badge html. "busy" phases get an amber pulse (the page JS keeps
// polling while one is on).
std::string phase_badge(const std::string& phase) {
    if (phase == "applied" || phase == "uptodate")
        return "<span class=\"badge ok\">" + E(phase) + "</span>";
    if (phase == "error")
        return "<span class=\"badge err\">error</span>";
    if (phase == "rolled_back")
        return "<span class=\"badge err\">rolled back</span>";
    if (phase == "idle" || phase.empty())
        return "<span class=\"muted\">idle</span>";
    if (phase == "available")
        return "<span class=\"badge warn\">update available</span>";
    return "<span class=\"badge warn\">" + E(phase) + "&#8230;</span>";
}

std::string read_worker_state_json() {
    return util::read_file("/run/rnsbox-sysupdate.json", 64 * 1024);
}

// crude JSON string-field reader (the state file is written by us; values
// are escaped by json.dump) — good enough for display, never for writes
std::string jstr(const std::string& js, const std::string& key) {
    std::string pat = "\"" + key + "\": \"";
    size_t p = js.find(pat);
    if (p == std::string::npos) {
        pat = "\"" + key + "\":\"";
        p = js.find(pat);
        if (p == std::string::npos) return "";
    }
    p += pat.size();
    std::string out;
    for (size_t i = p; i < js.size() && js[i] != '"'; ++i) {
        if (js[i] == '\\' && i + 1 < js.size()) { out += js[++i]; continue; }
        out += js[i];
    }
    return out;
}

std::string jnum(const std::string& js, const std::string& key) {
    std::string pat = "\"" + key + "\": ";
    size_t p = js.find(pat);
    if (p == std::string::npos) return "0";
    p += pat.size();
    std::string out;
    while (p < js.size() && (isdigit((unsigned char)js[p]))) out += js[p++];
    return out;
}

std::string read_log_tail() {
    for (const std::string& dir : {"/data/rnsbox-update",
                                   "/var/lib/rnsbox-update"}) {
        std::string log = util::read_file(dir + "/update.log", 16 * 1024);
        if (!log.empty()) {
            if (log.size() >= 16 * 1024) {
                size_t nl = log.find('\n');
                if (nl != std::string::npos) log = log.substr(nl + 1);
            }
            return log;
        }
    }
    return "";
}

}  // namespace


// ---- GET /updates : the page ----
void updates_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::UpdateConfig c = store::read_update_config();
    std::string st = read_worker_state_json();
    std::string h = render::page_file("updates");

    // status card
    std::string running = util::read_file("/etc/rnsbox/image.ver", 128);
    running = util::trim(running);
    std::string phase = jstr(st, "phase");
    std::string err = jstr(st, "error");
    std::string card;
    card += "<div class=\"card\"><h2>Status</h2><table class=\"kv\">";
    card += "<tr><th>Running image</th><td>" +
            (running.empty() ? std::string("<span class=\"badge warn\">unknown</span> "
                             "<span class=\"muted\">— reflash once with a "
                             "current image to enable updates</span>")
                             : "<strong>" + E(running) + "</strong>") +
            "</td></tr>";
    // A/B layout probe (cheap: p3 node + /boot state)
    std::string layout;
    if (!util::read_file("/sys/class/block/mmcblk0p3/size", 32).empty() ||
        !util::read_file("/sys/class/block/mmcblk0p3/dev", 32).empty()) {
        std::string slot = util::trim(util::read_file("/boot/ab_slot", 8));
        std::string tries = util::trim(util::read_file("/boot/ab_tries", 8));
        if (slot != "a" && slot != "b") slot = "a";
        layout = "<span class=\"badge ok\">A/B</span> active slot <strong>" +
                 E(slot) + "</strong>" +
                 (tries.empty() ? "" : " <span class=\"muted\">(boot counter " +
                                       E(tries) + "/3)</span>");
    } else {
        layout = "<span class=\"badge warn\">not A/B</span> <span "
                 "class=\"muted\">this card has a single rootfs — flash a "
                 "current image once, then updates work over the air</span>";
    }
    card += "<tr><th>Layout</th><td>" + layout + "</td></tr>";
    card += "<tr><th>Last check</th><td>" +
            fmt_epoch(jnum(st, "last_check")) + "</td></tr>";
    std::string cand = jstr(st, "version");
    {
        // candidate is a nested object; the flat reader above won't find
        // "version" at top level — pull it out of the candidate JSON
        size_t p = st.find("\"candidate\"");
        if (p != std::string::npos)
            cand = jstr(st.substr(p), "version");
    }
    if (!cand.empty())
        card += "<tr><th>Available</th><td><strong>" + E(cand) + "</strong>" +
                (jstr(st, "prerelease") == "True" ||
                 jstr(st, "prerelease") == "true"
                     ? " <span class=\"badge warn\">prerelease</span>"
                     : "") +
                "</td></tr>";
    card += "<tr><th>Next auto-check</th><td>" + fmt_epoch(c.next_check) +
            " <span class=\"muted\">(7-14 day random schedule)</span></td></tr>";
    card += "<tr><th>State</th><td>" + phase_badge(phase) +
            (err.empty() ? ""
                         : " <span class=\"muted\">" + E(err) + "</span>") +
            "</td></tr>";
    std::string lr = jstr(st, "last_result");
    if (!lr.empty())
        card += "<tr><th>Last action</th><td><span class=\"muted\">" + E(lr) +
                "</span></td></tr>";
    card += "</table>";
    std::string progress = jstr(st, "progress");
    if (!progress.empty())
        card += "<p class=\"muted\">" + E(progress) + "</p>";
    card += "<div class=\"btnrow\">";
    card += "<form method=\"post\" action=\"__BASE__/updates/check\">"
            "<button type=\"submit\">Check now</button></form> ";
    card += "<form method=\"post\" action=\"__BASE__/updates/apply\">"
            "<button type=\"submit\" class=\"danger\">Apply update now"
            "</button></form>";
    if (c.enabled == "yes")
        card += "<form method=\"post\" action=\"__BASE__/updates/auto\">"
                "<input type=\"hidden\" name=\"enabled\" value=\"no\">"
                "<button type=\"submit\">Disable auto-check</button></form>";
    else
        card += "<form method=\"post\" action=\"__BASE__/updates/auto\">"
                "<input type=\"hidden\" name=\"enabled\" value=\"yes\">"
                "<button type=\"submit\">Enable auto-check</button></form>";
    card += "</div></div>";
    h = R(std::move(h), "__STATUS_CARD__", card);
    h = R(std::move(h), "__AUTO_BADGE__",
          c.enabled == "yes"
              ? std::string("<span class=\"badge ok\">auto-check on</span>")
              : std::string("<span class=\"badge warn\">auto-check "
                            "off</span>"));

    // config form
    h = R(std::move(h), "__CFG_ENABLED__", c.enabled == "yes" ? " checked" : "");
    std::string prov_opts;
    for (const char* p : {"github-releases", "generic"})
        prov_opts += std::string("<option value=\"") + p + "\"" +
                     (c.provider == p ? " selected" : "") + ">" + p + "</option>";
    h = R(std::move(h), "__CFG_PROVIDER__", prov_opts);
    h = R(std::move(h), "__CFG_REPO__", E(c.repo));
    h = R(std::move(h), "__CFG_URL__", E(c.url));
    std::string board_opts;
    for (const char* b : {"auto", "rpi-universal", "licheerv-nano"})
        board_opts += std::string("<option value=\"") + b + "\"" +
                      (c.board == b ? " selected" : "") + ">" + b + "</option>";
    h = R(std::move(h), "__CFG_BOARD__", board_opts);
    h = R(std::move(h), "__CFG_PRE__", c.allow_prerelease == "yes" ? " checked" : "");
    h = R(std::move(h), "__CFG_AUTOAPPLY__", c.auto_apply == "yes" ? " checked" : "");
    h = R(std::move(h), "__CFG_INSECURE__",
          c.allow_insecure_http == "yes" ? " checked" : "");
    h = R(std::move(h), "__CFG_MINDAYS__", E(c.interval_min_days));
    h = R(std::move(h), "__CFG_MAXDAYS__", E(c.interval_max_days));

    // log tail
    std::string log = read_log_tail();
    h = R(std::move(h), "__LOG_TAIL__",
          log.empty() ? std::string("<p class=\"muted\">no update activity "
                                   "recorded yet</p>")
                      : "<pre class=\"log\">" + E(log) + "</pre>");

    res.body = render::layout(req, res, "Updates", "updates", h);
}

// ---- GET /updates/data : JSON for the page's auto-refresh ----
void updates_data(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    store::UpdateConfig c = store::read_update_config();
    std::string st = read_worker_state_json();
    std::string out = "{";
    auto q = [](const std::string& s) {
        std::string o = "\"";
        for (char ch : s) {
            if (ch == '"' || ch == '\\') o += '\\';
            if ((unsigned char)ch < 0x20) continue;
            o += ch;
        }
        return o + "\"";
    };
    out += "\"running_version\":" +
           q(util::trim(util::read_file("/etc/rnsbox/image.ver", 128)));
    out += ",\"phase\":" + q(jstr(st, "phase"));
    out += ",\"error\":" + q(jstr(st, "error"));
    out += ",\"progress\":" + q(jstr(st, "progress"));
    out += ",\"last_result\":" + q(jstr(st, "last_result"));
    out += ",\"last_check\":" + jnum(st, "last_check");
    out += ",\"next_check\":" +
           q(c.next_check.empty() ? "0" : c.next_check);
    size_t p = st.find("\"candidate\"");
    out += ",\"candidate_version\":" +
           q(p == std::string::npos ? "" : jstr(st.substr(p), "version"));
    out += ",\"enabled\":" + q(c.enabled);
    out += ",\"log_tail\":" + q(read_log_tail());
    out += "}";
    res.headers.push_back("Content-Type: application/json");
    res.body = out;
}

// ---- POST /updates/config ----
void updates_config(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::UpdateConfig c = store::read_update_config();   // keep managed keys
    c.enabled = req.f("enabled").empty() ? "no" : "yes";
    c.provider = req.f("provider");
    c.repo = util::trim(req.f("repo"));
    c.url = util::trim(req.f("url"));
    c.board = req.f("board");
    c.allow_prerelease = req.f("allow_prerelease").empty() ? "no" : "yes";
    c.allow_insecure_http = req.f("allow_insecure_http").empty() ? "no" : "yes";
    c.auto_apply = req.f("auto_apply").empty() ? "no" : "yes";
    c.interval_min_days = util::trim(req.f("interval_min_days"));
    c.interval_max_days = util::trim(req.f("interval_max_days"));
    std::string err;
    if (store::write_update_config(c, err)) {
        render::redirect_flash(res, upd_redirect(), "success",
                               "Update settings saved.");
    } else {
        render::redirect_flash(res, upd_redirect(), "error",
                               "Not saved: " + err);
    }
}

// ---- POST /updates/check | /updates/apply | /updates/auto ----
void updates_check(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    util::RunResult r = util::run({"/usr/sbin/rnsbox-sysupdate", "check"},
                                  "", 15);
    render::redirect_flash(
        res, upd_redirect(),
        r.exit_code == 0 ? "success" : "error",
        r.exit_code == 0
            ? "Check started — the page refreshes with the result."
            : "Check failed to start (is the rnsbox-update package "
              "installed?)");
}

void updates_apply(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    util::RunResult r = util::run({"/usr/sbin/rnsbox-sysupdate", "apply"},
                                  "", 15);
    render::redirect_flash(
        res, upd_redirect(),
        r.exit_code == 0 ? "success" : "error",
        r.exit_code == 0
            ? "Update started: download, verify, stage — the box reboots "
              "on its own when the new slot is ready."
            : "Apply refused or failed — see the log below.");
}

void updates_auto(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool on = req.f("enabled") == "yes";
    util::RunResult r = util::run(
        {"/usr/sbin/rnsbox-sysupdate", on ? "enable" : "disable"}, "", 15);
    render::redirect_flash(
        res, upd_redirect(), r.exit_code == 0 ? "success" : "error",
        on ? "Auto-check enabled (7-14 day random schedule)."
           : "Auto-check disabled — manual checks still work.");
}

}  // namespace routes
