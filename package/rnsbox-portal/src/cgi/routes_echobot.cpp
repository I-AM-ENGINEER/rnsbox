// routes_echobot.cpp — Echo bot tab: the box's local LXMF echo bot
// (/usr/bin/rnsbox-echobot, started by S87echobot). Server-rendered page +
// PRG forms, like every other tab. The daemon keeps its own state in
// /var/lib/rnsbox-echobot/state.json (atomic rewrites); the pidfile/comm test
// mirrors S87echobot's own liveness check, so the badge never lies about a
// dead daemon whose pidfile was left behind.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }
static std::string eb_redirect() { return web::base() + "/echobot"; }

constexpr const char* ECHOBOT_CONF  = "/etc/rnsbox/echobot.conf";
constexpr const char* ECHOBOT_STATE = "/var/lib/rnsbox-echobot/state.json";
constexpr const char* ECHOBOT_PID   = "/var/run/echobot.pid";

struct EchobotConf {
    bool enabled = true;
    long interval = 86400;
    std::string name = "rnsbox echo bot";
};

// Same parsing rules as the daemon's read_conf() and S87echobot's read_conf():
// first `key=value` wins, '#' comments out nothing (values are single-line
// clean by construction — the daemon tolerates quotes, we don't write any).
static EchobotConf echobot_read_conf() {
    EchobotConf c;
    std::string conf = util::read_file(ECHOBOT_CONF, 16384);
    bool have_en = false, have_in = false, have_nm = false;
    size_t i = 0;
    while (i <= conf.size()) {
        size_t nl = conf.find('\n', i);
        std::string ln = util::trim(conf.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? conf.size() + 1 : nl + 1;
        if (ln.empty() || ln[0] == '#') continue;
        size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::trim(ln.substr(0, eq)), v = util::trim(ln.substr(eq + 1));
        for (char& ch : k) ch = (char)tolower((unsigned char)ch);
        std::string low = v;
        for (char& ch : low) ch = (char)tolower((unsigned char)ch);
        if (k == "enabled" && !have_en) {
            c.enabled = !(low == "no" || low == "0" || low == "false" || low == "off" || low.empty());
            have_en = true;
        } else if (k == "interval" && !have_in) {
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
                c.interval = strtol(v.c_str(), nullptr, 10);
                have_in = true;
            }
        } else if (k == "name" && !have_nm && !v.empty()) {
            c.name = v;
            have_nm = true;
        }
    }
    return c;
}

// The daemon's own bounds (read_conf clamps to these too).
static const long ECHO_INTERVALS[] = { 3600, 21600, 43200, 86400, 172800, 604800 };

static std::string interval_label(long v) {
    if (v % 86400 == 0) {
        long d = v / 86400;
        return std::to_string(d) + " day" + (d > 1 ? "s" : "");
    }
    if (v % 3600 == 0)
        return std::to_string(v / 3600) + " hour" + (v >= 7200 ? "s" : "");
    return std::to_string(v / 60) + " minutes";
}

// Live daemon check: the pidfile must name a live process whose comm is the
// daemon (a shebang script's comm is the script name; 14 chars, under the
// 15-char comm limit). Same test S87echobot's is_echobot makes.
static bool echobot_running() {
    std::string ps = util::trim(util::read_file(ECHOBOT_PID, 32));
    if (ps.empty() || ps.find_first_not_of("0123456789") != std::string::npos) return false;
    std::string comm = util::trim(util::read_file("/proc/" + ps + "/comm", 64));
    return comm == "rnsbox-echobot";
}

// --- tiny best-effort JSON field readers for state.json (same shape as the
// ones in routes_reticulum.cpp; kept file-local so shared helpers stay put) --
static std::string json_str(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size() || j[p] != '"') return "";
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
static long json_num(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return 0;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    return strtol(j.c_str() + p + 1, nullptr, 10);
}

static std::string fmt_utc(long epoch) {
    char buf[64];
    time_t t = (time_t)epoch;
    struct tm g;
    gmtime_r(&t, &g);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &g);
    return std::string(buf);
}

// ===================== GET render =====================

void echobot_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string base = web::base();

    EchobotConf c = echobot_read_conf();
    std::string state = util::read_file(ECHOBOT_STATE, 65536);
    bool running = echobot_running();

    std::string status = json_str(state, "status");
    std::string detail = json_str(state, "detail");
    std::string dest   = json_str(state, "dest_hash");
    std::string suffix = json_str(state, "suffix");
    std::string dname  = json_str(state, "display_name");
    long last_announce = json_num(state, "last_announce");
    long announces     = json_num(state, "announce_count");
    long echo_ok       = json_num(state, "echo_ok");
    long echo_fail     = json_num(state, "echo_fail");

    // Status badge. The daemon's own status word is trusted only while it is
    // actually running; "stopped" wins otherwise (a stale state.json from a
    // previous run must not read as live).
    std::string badge;
    if (running)
        badge = "<span class=\"badge ok\">running</span>";
    else if (!c.enabled)
        badge = "<span class=\"badge\">disabled</span>";
    else
        badge = "<span class=\"badge warn\">stopped</span>";
    if (running && !detail.empty())
        badge += " <span class=\"muted\">" + E(detail) + "</span>";
    else if (!running && c.enabled)
        badge += " <span class=\"muted\">start it from here (Save) or <code>/etc/init.d/S87echobot start</code></span>";
    if (!running && !status.empty() && status != "running" && status != "stopped")
        badge += " <span class=\"muted\">(last state: " + E(status) + (detail.empty() ? "" : ": " + detail) + ")</span>";

    std::string addr = dest.empty()
        ? "<span class=\"muted\">not created yet — the bot makes it on first start</span>"
        : "<code>" + E(dest) + "</code>";

    std::string name = dname.empty()
        ? E(c.name) + (suffix.empty() ? std::string() : " <span class=\"muted\">+ " + E(suffix) + " on start</span>")
        : E(dname);

    std::string last = "never";
    if (last_announce > 0) last = E(fmt_utc(last_announce));

    std::string opts;
    bool matched = false;
    for (long v : ECHO_INTERVALS) {
        if (v == c.interval) matched = true;
        opts += "\n      <option value=\"" + std::to_string(v) + "\" " +
                (v == c.interval ? "selected" : "") + ">" + interval_label(v) + "</option>";
    }
    if (!matched && c.interval > 0)
        opts += "\n      <option value=\"" + std::to_string(c.interval) +
                "\" selected>" + E(interval_label(c.interval)) + " (custom)</option>";

    std::string h = render::page_file("echobot");
    h = R(std::move(h), "__BASE__", base);
    h = R(std::move(h), "__ECHOBOT_STATUS__", badge);
    h = R(std::move(h), "__ECHOBOT_ADDR__", addr);
    h = R(std::move(h), "__ECHOBOT_NAME__", name);
    h = R(std::move(h), "__ECHOBOT_SUFFIX__", suffix.empty() ? "&mdash;" : E(suffix));
    h = R(std::move(h), "__ECHOBOT_ANNOUNCES__", std::to_string(announces));
    h = R(std::move(h), "__ECHOBOT_LAST_ANNOUNCE__", last);
    h = R(std::move(h), "__ECHOBOT_ECHO_OK__", std::to_string(echo_ok));
    h = R(std::move(h), "__ECHOBOT_ECHO_FAIL__", std::to_string(echo_fail));
    h = R(std::move(h), "__ECHOBOT_CHECKED__", c.enabled ? "checked" : "");
    h = R(std::move(h), "__ECHOBOT_NAME_VALUE__", E(c.name));
    h = R(std::move(h), "__ECHOBOT_INTERVAL_OPTS__", opts);

    res.body = render::layout(req, res, "Echo bot", "echobot", h);
}

// ===================== PRG POST handlers =====================

void echobot_save(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enabled = req.f("enabled") == "on";

    std::string name = util::trim(req.f("name"));
    if (name.empty() || !util::is_single_line_clean(name)) name = "rnsbox echo bot";
    if (name.size() > 48) name = name.substr(0, 48);

    long interval = strtol(req.f("interval").c_str(), nullptr, 10);
    bool known = false;
    for (long v : ECHO_INTERVALS) if (v == interval) { known = true; break; }
    if (!known) interval = 86400;

    std::string conf =
        "# Echo bot config — edited by the portal's Echo bot tab; applies on\n"
        "# restart (saving here restarts the bot). See the file's own comments\n"
        "# and local_docs/echo-bot.md.\n"
        "enabled=" + std::string(enabled ? "yes" : "no") + "\n" +
        "interval=" + std::to_string(interval) + "\n" +
        "name=" + name + "\n" +
        "announce_at_start=yes\n" +
        "min_free_mb=40\n";
    if (!util::write_file_atomic(ECHOBOT_CONF, conf, 0644)) {
        render::redirect_flash(res, eb_redirect(), "error", "Echo bot: could not write the config.");
        return;
    }
    // S87echobot restart is a safe no-op when disabled; a start is needed when
    // enabling on a live box (rcS starts it at boot only).
    if (!web::run_init("S87echobot", enabled ? "restart" : "stop")) {
        render::redirect_flash(res, eb_redirect(), "warn",
            std::string("Config saved, but the bot service didn't respond (free RAM below the floor? see /var/log/messages)."));
        return;
    }
    if (enabled)
        render::redirect_flash(res, eb_redirect(), "success", "Echo bot settings saved — announcing every " + interval_label(interval) + ".");
    else
        render::redirect_flash(res, eb_redirect(), "success", "Echo bot disabled.");
}

void echobot_announce(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    if (!web::run_init("S87echobot", "announce")) {
        render::redirect_flash(res, eb_redirect(), "error",
            "Announce not sent — the bot is not running. Enable it and Save first.");
        return;
    }
    render::redirect_flash(res, eb_redirect(), "success",
        "Announce sent — see it in the wild with `logread | grep echobot`, or refresh: the last-announce time updated.");
}

void echobot_restart(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    if (!web::run_init("S87echobot", "restart")) {
        render::redirect_flash(res, eb_redirect(), "error",
            "Restart failed — see /var/log/messages (RAM below the min_free_mb floor refuses starts).");
        return;
    }
    render::redirect_flash(res, eb_redirect(), "success", "Echo bot restarted.");
}

}  // namespace routes
