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
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }
static std::string ret_redirect() { return web::base() + "/reticulum"; }

// ===================== RNS Shell (rnsh) =====================

constexpr const char* RNSH_CONF = "/etc/rnsbox/rnsh.conf";

struct RnshConf {
    bool enabled = false;
    std::string announce = "600";
    std::string allowed;   // normalized "allowed_id=<hash>" lines
};

static RnshConf rnsh_read_conf() {
    RnshConf c;
    std::string conf = util::read_file(RNSH_CONF, 16384);
    bool have_en = false, have_an = false;
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
            c.enabled = (low == "yes" || low == "true" || low == "1" || low == "on");
            have_en = true;
        } else if (k == "announce" && !have_an) {
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
                c.announce = v;
                have_an = true;
            }
        } else if (k == "allowed_id") {
            if (v.size() >= 16 && v.size() <= 64 &&
                v.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                for (char& ch : v) ch = (char)tolower((unsigned char)ch);
                c.allowed += v + "\n";
            }
        }
    }
    return c;
}

static bool rnsh_running() {
    std::string ps = util::trim(util::read_file("/var/run/rnsh.pid", 32));
    if (ps.empty() || ps.find_first_not_of("0123456789") != std::string::npos) return false;
    std::string comm = util::trim(util::read_file("/proc/" + ps + "/comm", 64));
    return comm == "rnsh";
}

static std::string rnsh_fill(std::string h) {
    RnshConf rc = rnsh_read_conf();
    bool running = rnsh_running();
    std::string dest = util::trim(util::read_file("/run/rnsh-dest", 128));
    std::string status;
    if (running)
        status = "<span class=\"badge ok\">running</span>";
    else if (rc.enabled && util::trim(rc.allowed).empty())
        status = "<span class=\"badge warn\">not started</span> add your client's identity hash below";
    else
        status = "<span class=\"badge\">stopped</span>";
    h = R(std::move(h), "__RNSH_CHECKED__", rc.enabled ? "checked" : "");
    h = R(std::move(h), "__RNSH_ANNOUNCE__", E(rc.announce));
    h = R(std::move(h), "__RNSH_ALLOWED__", E(rc.allowed));
    h = R(std::move(h), "__RNSH_STATUS__", status);
    h = R(std::move(h), "__RNSH_DEST__",
          dest.empty() ? "<span class=\"muted\">not running</span>" : E(dest));
    return h;
}

// HTTPS to PyPI needs a real date. With no trusted clock (/run/clock-source
// absent: no NTP, RTC or browser sync yet) TLS fails "certificate is not yet
// valid", which updatecheck.py records only as "URLError" (reads as offline),
// and check() would persist a 1970/2000 checked_at. So don't run it at all.
// (rnsbox-update-check applies the same rule to the cron/boot checks.)
static bool clock_trusted() { return !sysinfo::clock_source().empty(); }
static const char NO_CLOCK[] =
    "the clock isn't set (no NTP, RTC or browser time yet) and HTTPS needs a "
    "correct date. Set it under Settings → Time & Clock.";

// store._truthy: falsey iff stripped/lowered value in {no,0,false,off,""}.
static bool truthy(const std::string& s) {
    std::string v = util::trim(s);
    for (char& c : v) c = (char)tolower((unsigned char)c);
    return !(v == "no" || v == "0" || v == "false" || v == "off" || v.empty());
}

// ===================== rnsd down-watchdog (S46wanwatch step 2a) ============
// The watchdog syncs+reboots when rnsd has been continuously absent for
// down_limit seconds despite wanwatch's 10 s kicks. Config is portal-editable
// and re-read by wanwatch every tick, so a save here takes effect at once and
// the reboot is never a surprise: this card shows the live outage state too.

constexpr const char* RNSDMON_CONF = "/etc/rnsbox/rnsdmon.conf";

struct RnsdMonConf {
    bool enabled = true;
    long down_limit = 3600;
};

static const long RNSDMON_LIMITS[] = { 1800, 3600, 7200, 21600, 43200 };

static RnsdMonConf rnsdmon_read_conf() {
    RnsdMonConf c;
    std::string conf = util::read_file(RNSDMON_CONF, 512);
    bool have_en = false, have_l = false;
    size_t i = 0;
    while (i <= conf.size()) {
        size_t nl = conf.find('\n', i);
        std::string ln = util::trim(conf.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? conf.size() + 1 : nl + 1;
        if (ln.empty() || ln[0] == '#') continue;
        size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::trim(ln.substr(0, eq)), v = util::trim(ln.substr(eq + 1));
        if (k == "enabled" && !have_en) {
            c.enabled = truthy(v);
            have_en = true;
        } else if (k == "down_limit" && !have_l) {
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) {
                long l = strtol(v.c_str(), nullptr, 10);
                if (l >= 300 && l <= 86400) { c.down_limit = l; have_l = true; }
            }
        }
    }
    return c;
}

static std::string rnsdmon_fill(std::string h) {
    RnsdMonConf c = rnsdmon_read_conf();
    h = R(std::move(h), "__RNSDMON_CHECKED__", c.enabled ? "checked" : "");

    std::string o;
    bool matched = false;
    for (long v : RNSDMON_LIMITS) {
        if (v == c.down_limit) matched = true;
        std::string lbl = v < 3600 ? std::to_string(v / 60) + " minutes"
                                   : std::to_string(v / 3600) + " hour" + (v >= 7200 ? "s" : "");
        o += "\n      <option value=\"" + std::to_string(v) + "\" " +
             (v == c.down_limit ? "selected" : "") + ">" + lbl + "</option>";
    }
    if (!matched)
        o += "\n      <option value=\"" + std::to_string(c.down_limit) +
             "\" selected>" + std::to_string(c.down_limit / 60) + " minutes (custom)</option>";
    h = R(std::move(h), "__RNSDMON_OPTIONS__", o);

    // Live outage state: /run/rnsd-down-since holds the uptime second at
    // which the current continuous outage began (tmpfs, wanwatch-owned).
    std::string state;
    if (!c.enabled) {
        state = "<span class=\"badge\">off</span> rnsd outages never trigger a reboot";
    } else {
        std::string since = util::trim(util::read_file("/run/rnsd-down-since", 32));
        if (since.empty() || since.find_first_not_of("0123456789") != std::string::npos) {
            state = "<span class=\"badge ok\">watching</span> rnsd healthy";
        } else {
            std::string up = util::read_file("/proc/uptime", 64);
            size_t sp = up.find(' ');
            if (sp != std::string::npos) up = up.substr(0, sp);
            up = util::trim(up.substr(0, up.find('.')));
            long down = -1;
            if (!up.empty() && up.find_first_not_of("0123456789") == std::string::npos) {
                down = strtol(up.c_str(), nullptr, 10) - strtol(since.c_str(), nullptr, 10);
                if (down < 0) down = 0;
            }
            if (down < 0)
                state = "<span class=\"badge ok\">watching</span> rnsd healthy";
            else
                state = "<span class=\"badge warn\">rnsd DOWN " +
                        std::to_string(down / 60) + " min</span> — reboots after " +
                        std::to_string(c.down_limit / 60) + " min of continuous outage";
        }
    }
    h = R(std::move(h), "__RNSDMON_STATE__", state);
    return h;
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
// port_i <= 0 matches any target_port — the HaLow card only cares that some
// TCPClientInterface points at the modem, whatever its TCP bridge port is.
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
        if (is_tcp && have_host && cur_host == host && (port_i <= 0 || (have_port && cur_port == port))) return true;
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
    if (!clock_trusted())
        chk += " <span class=\"muted\">— paused: the clock isn't set (HTTPS needs a correct date); "
               "set it under Settings &rarr; Time &amp; Clock</span>";
    else if (!uerror.empty())
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
            "      <input type=\"hidden\" name=\"iface_port\" id=\"halow-add-port\" value=\"4242\">\n"
            "      <button type=\"submit\" class=\"btn-primary\">+ Add to rnsd config</button>\n"
            "    </form>\n"
            "  </div>\n"
            "  <p class=\"muted\" style=\"margin:6px 0 0\">Full stats, graphs &amp; config live in the modem's own web page.</p>";
    }
    h = R(std::move(h), "__HALOW_LIVE_BLOCK__", halow);

    bool halow_in_config = reticulum_has_tcp_client(sl.peer_ip.empty() ? "192.168.7.2" : sl.peer_ip, 0);
    h = R(std::move(h), "__HALOW_IN_CONFIG__", halow_in_config ? "true" : "false");

    h = R(std::move(h), "__SLIP_ENABLED_CHECKED__", slip_on ? "checked" : "");
    h = R(std::move(h), "__SLIP_DEVICE__",   E(sl.device));
    h = R(std::move(h), "__SLIP_BAUD__",     E(sl.baud));
    h = R(std::move(h), "__SLIP_LOCAL_IP__", E(sl.local_ip));
    h = R(std::move(h), "__SLIP_PEER_IP__",  E(sl.peer_ip));
    h = rnsh_fill(std::move(h));
    h = rnsdmon_fill(std::move(h));
    h = R(std::move(h), "__CONFIG__",        E(cfg));

    res.body = render::layout(req, res, "Reticulum", "reticulum", h);
}

// ===================== AJAX GET JSON =====================

void reticulum_status(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    bool with_raw = (req.a("raw") == "1");   // slow path: cold-starts rnstatus (~5 s)
    // sysinfo::rnsd_status uses the pidfile (S82rnsd) + a /proc-cmdline scan
    // fallback — no subprocess on this path.
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

// Short-TTL cache for the halow.py result. One helper run costs ~1.5 s of CPU
// on the C906 (python start + 3 HTTP-over-SLIP calls), and the dashboard's
// auto-refresh (5 s option) plus a Reticulum tab would otherwise each spawn one
// per tick. Requests inside the TTL reuse the last good JSON; a flock makes
// concurrent misses single-flight (the waiter then finds the fresh cache).
constexpr const char* HALOW_CACHE = "/run/rnsbox-halow.json";
constexpr const char* HALOW_LOCK  = "/run/rnsbox-halow.lock";
constexpr int HALOW_TTL = 10;   // seconds

static bool halow_cache_fresh(std::string& out) {
    struct stat st;
    if (::stat(HALOW_CACHE, &st) != 0) return false;
    time_t now = ::time(nullptr);
    // A future mtime means the clock stepped backwards (browser/NTP sync on a
    // box that boots at 1970) — treat as stale rather than trust it for years.
    if (st.st_mtime > now || now - st.st_mtime >= HALOW_TTL) return false;
    out = util::read_file(HALOW_CACHE, 256 * 1024);
    std::string t = util::trim(out);
    return !t.empty() && t[0] == '{';
}

static void halow_cache_drop() { ::unlink(HALOW_CACHE); }

// Best-effort live-modem endpoint (JSON for the dashboard + Reticulum cards).
void reticulum_halow(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string cached;
    if (halow_cache_fresh(cached)) { res.body = cached; return; }

    // Single-flight: whoever holds the lock runs the helper; the rest block
    // here (bounded by the helper's 10 s timeout) and then reuse its result.
    // O_CLOEXEC keeps the lock fd out of the python child.
    int lfd = ::open(HALOW_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0) ::flock(lfd, LOCK_EX);
    struct LockGuard { int fd; ~LockGuard() { if (fd >= 0) ::close(fd); } } guard{lfd};   // close = unlock
    if (halow_cache_fresh(cached)) { res.body = cached; return; }

    // Live modem status is HTTP-over-SLIP + JSON — delegated to the python helper
    // (fails soft). It only spawns on this async fetch, never on a page paint,
    // and only when the page renders the card (SLIP enabled).
    util::RunResult rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/halow.py"}, "", 10);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') {
        util::write_file_atomic(HALOW_CACHE, rr.out, 0600);   // best-effort
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

void reticulum_rnsdwatchdog(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enabled = req.f("enabled") == "on";
    long limit = strtol(req.f("limit").c_str(), nullptr, 10);
    bool known = false;
    for (long v : RNSDMON_LIMITS) if (v == limit) { known = true; break; }
    if (!known) limit = 3600;
    std::string conf = "enabled=" + std::string(enabled ? "yes" : "no") +
                       "\ndown_limit=" + std::to_string(limit) + "\n";
    if (!util::write_file_atomic(RNSDMON_CONF, conf, 0644)) {
        render::redirect_flash(res, ret_redirect(), "error",
            "Watchdog: could not write the config.");
        return;
    }
    // No service reload: S46wanwatch re-reads the file on every 10 s tick.
    if (!enabled)
        render::redirect_flash(res, ret_redirect(), "success",
            "rnsd outage watchdog disabled — wanwatch still kicks rnsd, it just never reboots.");
    else
        render::redirect_flash(res, ret_redirect(), "success",
            "Watchdog armed: the box reboots if rnsd stays down for " +
            std::to_string(limit / 60) + " minutes despite the automatic restarts.");
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
    halow_cache_drop();   // link/peer changed: don't serve up to 10 s of the old modem state
    if (c.enabled == "yes")
        render::redirect_flash(res, ret_redirect(), "success",
            "SLIP link enabled on " + c.device + " @ " + c.baud + " baud. Once the modem is "
            "reachable, use “Add to rnsd config” on the HaLow card below to route "
            "Reticulum through it (a TCPClientInterface to " + c.peer_ip +
            " on the modem's TCP Radio Bridge port).");
    else
        render::redirect_flash(res, ret_redirect(), "success", "SLIP link disabled.");
}

void reticulum_rnsh_save(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enabled = req.f("enabled") == "on";
    std::string announce = util::trim(req.f("announce"));
    if (announce.empty() || announce.find_first_not_of("0123456789") != std::string::npos)
        announce = "600";
    long an = strtol(announce.c_str(), nullptr, 10);
    if (an < 0 || an > 86400) an = 600;
    announce = std::to_string(an);

    // The allow-list is the security model: normalize hard. One hash per
    // line, 16..64 lowercase hex, '#' comments stripped, deduped, max 64.
    std::string body = req.f("allowed"), o, seen[64];
    int n = 0;
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        std::string ln = util::trim(body.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        size_t hash = ln.find('#');
        if (hash != std::string::npos) ln = util::trim(ln.substr(0, hash));
        for (char& ch : ln) ch = (char)tolower((unsigned char)ch);
        if (ln.size() < 16 || ln.size() > 64) continue;
        if (ln.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (seen[k] == ln) { dup = true; break; }
        if (dup || n >= 64) continue;
        seen[n++] = ln;
        o += "allowed_id=" + ln + "\n";
    }
    std::string conf = "enabled=" + std::string(enabled ? "yes" : "no") +
                       "\nannounce=" + announce + "\n" + o;
    if (!util::write_file_atomic(RNSH_CONF, conf, 0600)) {
        render::redirect_flash(res, ret_redirect(), "error", "rnsh: could not write the config.");
        return;
    }
    web::run_init("S86rnsh", "restart");
    if (!enabled) {
        render::redirect_flash(res, ret_redirect(), "success", "RNS Shell disabled.");
        return;
    }
    std::string dest = util::trim(util::read_file("/run/rnsh-dest", 128));
    render::redirect_flash(res, ret_redirect(), "success",
        "RNS Shell saved. Destination: " +
        (dest.empty() ? std::string("(not started — see the status line)") : dest) + ".");
}

void reticulum_halow_add(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::SlipConfig sl = store::read_slip();
    std::string peer = sl.peer_ip.empty() ? "192.168.7.2" : sl.peer_ip;
    std::string name = util::trim(req.f("iface_name"));
    int port = atoi(req.f("iface_port").c_str());
    if (port < 1 || port > 65535) port = 4242;
    // NOTE: the old view queried the modem for its hostname when the browser
    // didn't supply one (JS off). That path needs halow_status() (no C++ port);
    // store::add_halow_interface() falls back to "RNode-Halow" for a blank name.
    std::string msg;
    if (store::add_halow_interface(name, peer, port, msg)) {
        web::run_init("S82rnsd");
        halow_cache_drop();   // rnsd_up / interface state just changed
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
    // Before python starts, so no bogus checked_at / error gets persisted.
    if (!clock_trusted()) {
        update_json(res, false, false, std::string("Can't check for updates: ") + NO_CLOCK);
        return;
    }
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
    if (!clock_trusted()) {
        update_json(res, false, false, std::string("Can't update: ") + NO_CLOCK);
        return;
    }
    // Long: pip install + rnsd restart, with a rollback on failure. Budgets nest
    // (arithmetic in updatecheck.py, above APPLY_BUDGET): the helper finishes
    // inside 270 s + python start-up, we give it 285 s, and uhttpd's script /
    // network timeouts are 300 s (S81router). A kill mid-pip would leave RNS
    // uninstalled, so keep them in step if any of the three changes.
    auto rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/updatecheck.py", "apply"}, "", 285);
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