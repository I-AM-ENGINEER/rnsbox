// routes_echobot.cpp — Echo bot card on the Reticulum tab: the box's local
// LXMF echo bot (/usr/bin/rnsbox-echobot, started by S87echobot). The cards
// render inside the Reticulum page (echobot_cards()); the old /echobot URL
// stays a working bookmark and redirects to the card. PRG forms, like every
// other card. The daemon keeps its own state in
// /var/lib/rnsbox-echobot/state.json (atomic rewrites); the pidfile/comm test
// mirrors S87echobot's own liveness check, so the badge never lies about a
// dead daemon whose pidfile was left behind.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }
// The card lives on the Reticulum tab (anchor #echobot); all PRG forms and
// the old tab URL land there.
static std::string eb_redirect() { return web::base() + "/reticulum#echobot"; }

constexpr const char* ECHOBOT_CONF  = "/etc/rnsbox/echobot.conf";
constexpr const char* ECHOBOT_STATE = "/var/lib/rnsbox-echobot/state.json";
constexpr const char* ECHOBOT_PID   = "/var/run/echobot.pid";
constexpr const char* ECHOBOT_PEERS = "/run/rnsbox-echobot-peers.tsv";

struct EchobotConf {
    bool enabled = false;
    long interval = 86400;
    std::string name;   // empty = the derived default (echobot_default_name)
};

// The box's own hostname, e.g. "rnsbox-57145f". S10uuid derives it from the
// persisted /boot/hostname; the portal shows it wherever the bot is named so
// one box is never mistaken for another.
static std::string echobot_hostname() {
    std::string h = util::trim(util::read_file("/proc/sys/kernel/hostname", 128));
    if (h.empty() || h == "localhost") h = "rnsbox";
    return h;
}

// The bot's default display name puts the service first and the box second:
// "echo bot <hostname>". Shipped configs may pin the legacy literals
// ("rnsbox echo bot", "<hostname> echo bot"); treat them as "not customised"
// so the announced name follows the box's hostname.
static std::string echobot_default_name() {
    std::string n = "echo bot " + echobot_hostname();
    if (n.size() > 48) n = n.substr(0, 48);
    return n;
}
static bool echobot_legacy_name(const std::string& n) {
    return n == "rnsbox echo bot" || n == echobot_hostname() + " echo bot";
}

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

static std::string fmt_bytes(long b) {
    char buf[32];
    if (b >= 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else if (b >= 1024)   snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else                  snprintf(buf, sizeof(buf), "%ld B", b);
    return buf;
}

// --- per-peer traffic table -------------------------------------------------
// The daemon snapshots its in-RAM peer counters to /run/rnsbox-echobot-peers
// .tsv (tmpfs — no SD writes), one line per peer heard via announce or echoed
// to (newest first):
//   hash \t name \t rx_msgs \t rx_bytes \t tx_msgs \t tx_bytes \t tx_ok \t tx_fail \t last_seen
// Names are daemon-sanitised (single line, no tabs), but stay defensive here:
// a malformed line just doesn't render.
static void split_tab(const std::string& line, std::string f[9]) {
    size_t i = 0;
    for (int n = 0; n < 9; ++n) {
        size_t t = line.find('\t', i);
        f[n] = (t == std::string::npos || n == 8) ? line.substr(i) : line.substr(i, t - i);
        if (t == std::string::npos) break;
        i = t + 1;
    }
}

static long pl(const std::string& s) {
    return s.find_first_not_of("0123456789") == std::string::npos && !s.empty()
               ? strtol(s.c_str(), nullptr, 10) : 0;
}

static std::string echobot_peers_table() {
    std::string data = util::read_file(ECHOBOT_PEERS, 262144);
    std::vector<std::array<std::string, 9>> rows;
    size_t i = 0;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        std::string line = data.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? data.size() : nl + 1;
        std::string f[9];
        split_tab(line, f);
        if (f[0].size() != 32 || f[0].find_first_not_of("0123456789abcdef") != std::string::npos)
            continue;   // not an LXMF destination hash line
        rows.push_back({f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]});
    }
    if (rows.empty())
        return "<p class=\"muted\">No peers yet — a row appears for every destination the bot hears an announce from, and its counters grow with each message echoed.</p>";

    std::string t = "<table class=\"data\">\n    <tr><th>Peer</th><th>Received</th><th>Sent</th><th>Delivered</th></tr>";
    for (const auto& r : rows) {
        long rxm = pl(r[2]), rxb = pl(r[3]), txm = pl(r[4]), txb = pl(r[5]);
        long ok = pl(r[6]), fail = pl(r[7]);
        std::string peer;
        if (r[1].empty())
            peer = "<code title=\"" + r[0] + "\">" + r[0].substr(0, 8) + "&hellip;</code>";
        else
            peer = E(r[1]) + " <code title=\"" + r[0] + "\">" + r[0].substr(0, 8) + "</code>";
        std::string delivered;
        long tried = ok + fail;
        if (tried > 0) {
            char pct[8];
            snprintf(pct, sizeof(pct), "%ld%%", ok * 100 / tried);
            delivered = std::string(pct) + " <span class=\"muted\">(" + std::to_string(ok) +
                        "/" + std::to_string(tried) + ")</span>";
        } else {
            delivered = "<span class=\"muted\">&mdash;</span>";
        }
        t += "\n    <tr><td>" + peer + "</td><td>" + std::to_string(rxm) +
             " <span class=\"muted\">msgs · " + fmt_bytes(rxb) + "</span></td><td>" +
             std::to_string(txm) + " <span class=\"muted\">msgs · " + fmt_bytes(txb) +
             "</span></td><td>" + delivered + "</td></tr>";
    }
    t += "\n  </table>";
    return t;
}

// ===================== GET render =====================

// The two cards (status + settings), rendered for embedding into the
// Reticulum page (routes_reticulum.cpp fills __ECHOBOT_CARDS__). The page
// shell lives in pages/echobot.html: exactly the card sections, no <h2>/nav
// of its own.
std::string echobot_cards(const std::string& base) {
    EchobotConf c = echobot_read_conf();
    if (c.name.empty() || echobot_legacy_name(c.name)) c.name = echobot_default_name();
    std::string state = util::read_file(ECHOBOT_STATE, 65536);
    bool running = echobot_running();

    std::string status = json_str(state, "status");
    std::string detail = json_str(state, "detail");
    std::string dest   = json_str(state, "dest_hash");
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
    if (!running && !status.empty() && status != "running" && status != "stopped")
        badge += " <span class=\"muted\">(last state: " + E(status) + (detail.empty() ? "" : ": " + detail) + ")</span>";

    std::string addr = dest.empty()
        ? "<span class=\"muted\">not created yet — the bot makes it on first start</span>"
        : "<code>" + E(dest) + "</code>";

    // The announced name is exactly the configured name — no auto suffix.
    std::string name = dname.empty() ? E(c.name) : E(dname);

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
    h = R(std::move(h), "__ECHOBOT_ANNOUNCES__", std::to_string(announces));
    h = R(std::move(h), "__ECHOBOT_LAST_ANNOUNCE__", last);
    h = R(std::move(h), "__ECHOBOT_ECHO_OK__", std::to_string(echo_ok));
    h = R(std::move(h), "__ECHOBOT_ECHO_FAIL__", std::to_string(echo_fail));
    h = R(std::move(h), "__ECHOBOT_CHECKED__", c.enabled ? "checked" : "");
    h = R(std::move(h), "__ECHOBOT_NAME_VALUE__", E(c.name));
    h = R(std::move(h), "__ECHOBOT_HOSTNAME__", E(echobot_hostname()));
    h = R(std::move(h), "__ECHOBOT_PEERS__", echobot_peers_table());
    h = R(std::move(h), "__ECHOBOT_INTERVAL_OPTS__", opts);
    return h;
}

// Compact Dashboard card — only when the bot is enabled in the config, per
// the "if enabled, show it on the Dashboard" rule. Same cheap file reads as
// the Reticulum card; no subprocess, no state change.
std::string echobot_dashboard_card(const std::string& base) {
    EchobotConf c = echobot_read_conf();
    if (!c.enabled) return std::string();

    std::string state = util::read_file(ECHOBOT_STATE, 65536);
    bool running = echobot_running();
    std::string dest   = json_str(state, "dest_hash");
    std::string dname  = json_str(state, "display_name");
    long last_announce = json_num(state, "last_announce");
    long announces     = json_num(state, "announce_count");
    long echo_ok       = json_num(state, "echo_ok");
    long echo_fail     = json_num(state, "echo_fail");

    std::string badge = running
        ? "<span class=\"badge ok\">running</span>"
        : "<span class=\"badge warn\">stopped</span>";

    std::string addr = dest.empty()
        ? "<span class=\"muted\">not created yet</span>"
        : "<code>" + E(dest) + "</code>";
    std::string name = dname.empty() ? c.name : dname;
    if (name.empty() || echobot_legacy_name(name)) name = echobot_default_name();

    std::string last = "never";
    if (last_announce > 0) last = E(fmt_utc(last_announce));

    return "<section class=\"card\" id=\"echobot-card\">\n"
           "    <h3>Echo Bot</h3>\n"
           "    <table class=\"kv\">\n"
           "      <tr><th>Status</th><td>" + badge + "</td></tr>\n"
           "      <tr><th>LXMF address</th><td>" + addr + "</td></tr>\n"
           "      <tr><th>Announced name</th><td>" + E(name) + "</td></tr>\n"
           "      <tr><th>Announces sent</th><td>" + std::to_string(announces) +
           " <span class=\"muted\">· last " + last + "</span></td></tr>\n"
           "      <tr><th>Messages echoed</th><td>" + std::to_string(echo_ok) +
           " ok / " + std::to_string(echo_fail) + " failed</td></tr>\n"
           "    </table>\n"
           "    <p class=\"cardfoot\"><a class=\"btn\" href=\"" + base +
           "/reticulum#echobot\">Manage &rarr;</a></p>\n"
           "  </section>";
}

// Old bookmark route: the echo bot is a card on the Reticulum tab now.
void echobot_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    res.redirect(eb_redirect());
}

// GET /echobot/peers — just the Traffic table fragment, polled by the card's
// JS every few seconds so counters move without an F5.
void echobot_peers(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "text/html; charset=utf-8";
    res.body = echobot_peers_table();
}

// ===================== PRG POST handlers =====================

void echobot_save(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enabled = req.f("enabled") == "on";

    std::string name = util::trim(req.f("name"));
    if (name.empty() || echobot_legacy_name(name) || !util::is_single_line_clean(name))
        name = echobot_default_name();
    if (name.size() > 48) name = name.substr(0, 48);

    long interval = strtol(req.f("interval").c_str(), nullptr, 10);
    bool known = false;
    for (long v : ECHO_INTERVALS) if (v == interval) { known = true; break; }
    if (!known) interval = 86400;

    // The portal owns enabled/interval/name; an operator's min_free_mb RAM
    // floor (S87echobot reads it) is preserved, not clobbered.
    long min_free_mb = 40;
    std::string mfb = util::conf_get(ECHOBOT_CONF, "min_free_mb");
    if (!mfb.empty() && mfb.find_first_not_of("0123456789") == std::string::npos)
        min_free_mb = strtol(mfb.c_str(), nullptr, 10);

    std::string conf =
        "# Echo bot config — edited by the portal's Echo bot tab; applies on\n"
        "# restart (saving here restarts the bot). See the file's own comments\n"
        "# and local_docs/echo-bot.md.\n"
        "enabled=" + std::string(enabled ? "yes" : "no") + "\n" +
        "interval=" + std::to_string(interval) + "\n" +
        "name=" + name + "\n" +
        "announce_at_start=yes\n" +
        "min_free_mb=" + std::to_string(min_free_mb) + "\n";
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

// POST /echobot/toggle — legacy Enable-switch endpoint, kept for pages cached
// before the switch became part of the Save & apply form. Only the enabled
// flag changes; name/interval/min_free_mb are preserved as-is.
void echobot_toggle(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enable = req.f("enabled") == "on";

    EchobotConf cur = echobot_read_conf();
    long min_free_mb = 40;
    std::string mfb = util::conf_get(ECHOBOT_CONF, "min_free_mb");
    if (!mfb.empty() && mfb.find_first_not_of("0123456789") == std::string::npos)
        min_free_mb = strtol(mfb.c_str(), nullptr, 10);
    std::string raw_name = util::conf_get(ECHOBOT_CONF, "name");

    std::string conf =
        "# Echo bot config — edited by the portal's Echo bot tab; applies on\n"
        "# restart (saving here restarts the bot). See the file's own comments\n"
        "# and local_docs/echo-bot.md.\n"
        "enabled=" + std::string(enable ? "yes" : "no") + "\n" +
        "interval=" + std::to_string(cur.interval) + "\n" +
        "name=" + raw_name + "\n" +
        "announce_at_start=yes\n" +
        "min_free_mb=" + std::to_string(min_free_mb) + "\n";
    if (!util::write_file_atomic(ECHOBOT_CONF, conf, 0644)) {
        render::redirect_flash(res, eb_redirect(), "error", "Echo bot: could not write the config.");
        return;
    }
    if (!web::run_init("S87echobot", enable ? "restart" : "stop")) {
        render::redirect_flash(res, eb_redirect(), "warn",
            std::string("Config saved, but the bot service didn't respond (free RAM below the floor? see /var/log/messages)."));
        return;
    }
    render::redirect_flash(res, eb_redirect(), "success",
        enable ? "Echo bot enabled." : "Echo bot disabled.");
}

void echobot_announce(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    if (!web::run_init("S87echobot", "announce")) {
        render::redirect_flash(res, eb_redirect(), "error",
            "Announce not sent — the bot is not running. Enable it first.");
        return;
    }
    render::redirect_flash(res, eb_redirect(), "success",
        "Announce sent — the last-announce time updates on the next refresh.");
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
