// routes_reticulum.cpp — Reticulum tab: server-rendered page + PRG forms +
// AJAX JSON endpoints (rnsd status / live HaLow modem). Faithful port of the
// old Flask reticulum() view + its POST/AJAX routes.
#include "routes.h"
#include "json.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "sysinfo.h"
#include "updatecheck.h"
#include "util.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <dirent.h>
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

static std::string rnode_fill(std::string h);
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

// Turn the helper's raw error into the one sentence an operator can act on.
static std::string update_error_hint(const std::string& e) {
    std::string low = e;
    for (char& c : low) c = (char)tolower((unsigned char)c);
    if (low.find("certificate_verify_failed") != std::string::npos ||
        low.find("certificate verify failed") != std::string::npos)
        return "TLS certificate verification failed — the image has no CA certificates";
    if (low.find("timed out") != std::string::npos ||
        low.find("timeout") != std::string::npos)
        return "the update source timed out (WAN blocked or source unreachable)";
    if (low.find("name or service not known") != std::string::npos ||
        low.find("getaddrinfo") != std::string::npos)
        return "DNS resolution failed";
    if (e.empty()) return "unknown error";
    return e.size() > 140 ? e.substr(0, 140) + "…" : e;
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

static bool rnsdmon_write(bool enabled, long limit, std::string& err) {
    bool known = false;
    for (long v : RNSDMON_LIMITS) if (v == limit) { known = true; break; }
    if (!known) { err = "unsupported down_limit"; return false; }
    std::string conf = "enabled=" + std::string(enabled ? "yes" : "no") +
                       "\ndown_limit=" + std::to_string(limit) + "\n";
    if (!util::write_file_atomic(RNSDMON_CONF, conf, 0644)) {
        err = "could not write the config";
        return false;
    }
    return true;
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
    // The marker alone is not proof of an outage: wanwatch's liveness test
    // is the pidfile's kill -0 plus `pidof -x rnsd` (busybox pidof has no
    // -x), so a stale pidfile can leave the marker counting down while
    // rnsd actually runs — and the portal, which finds rnsd with a /proc
    // cmdline scan, painted "DOWN n min" right above a green RUNNING
    // state row. Cross-check real liveness first and clear a stale marker
    // (best effort) so the contradiction self-heals instead of ending in
    // a needless reboot.
    std::string state;
    bool rnsd_up = sysinfo::rnsd_status().running;
    if (!c.enabled) {
        state = "<span class=\"badge\">off</span> rnsd outages never trigger a reboot";
    } else if (rnsd_up) {
        state = "<span class=\"badge ok\">watching</span> rnsd healthy";
        std::string since = util::trim(util::read_file("/run/rnsd-down-since", 32));
        if (!since.empty() && since.find_first_not_of("0123456789") == std::string::npos)
            ::unlink("/run/rnsd-down-since");   // stale: rnsd is alive
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

// ===================== Bluetooth (BLE transport) ==========================
// Stack state for the card + the status JSON: adapter (sysfs), system D-Bus
// (socket presence) and bluetoothd (/proc comm scan). All cheap — the page
// paint path runs these inline; no subprocesses.
struct BtState {
    bool stack = false;        // /sys/class/bluetooth exists at all
    bool hci = false;          // an hciN adapter is registered
    std::string hci_name;      // "hci0"
    bool dbus = false;         // the system bus socket is there
    bool daemon = false;       // bluetoothd is running
};

static BtState bt_status() {
    BtState s;
    if (DIR* d = opendir("/sys/class/bluetooth")) {
        s.stack = true;
        while (dirent* e = readdir(d)) {
            if (strncmp(e->d_name, "hci", 3) == 0) { s.hci = true; s.hci_name = e->d_name; break; }
        }
        closedir(d);
    }
    s.dbus = (::access("/run/dbus/system_bus_socket", F_OK) == 0);
    if (DIR* p = opendir("/proc")) {
        while (dirent* e = readdir(p)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            std::string c = util::trim(util::read_file("/proc/" + std::string(e->d_name) + "/comm", 64));
            if (c == "bluetoothd") { s.daemon = true; break; }
        }
        closedir(p);
    }
    return s;
}

static bool bt_installed() {
    // The ble_reticulum package ships the shim RNS exec()s; absent = the
    // image was built without the BLE stack (64 MB cv1800b RAM-boot board).
    return ::access("/etc/reticulum/interfaces/BLEInterface.py", F_OK) == 0;
}

static std::string bt_status_html(const BtState& b) {
    if (!b.stack)
        return "<span class=\"badge\">no Bluetooth stack in the kernel</span>";
    if (!b.hci)
        return "<span class=\"badge warn\">no adapter</span> <span class=\"muted\">(onboard BT off / no USB dongle yet — rnsd re-probes every 30 s)</span>";
    std::string o = "<span class=\"badge ok\">" + E(b.hci_name) + " present</span>";
    o += b.dbus ? " <span class=\"badge ok\">dbus</span>"
                : " <span class=\"badge err\">dbus down</span>";
    o += b.daemon ? " <span class=\"badge ok\">bluetoothd</span>"
                  : " <span class=\"badge err\">bluetoothd down</span>";
    return o;
}

// Rows of the TCP client interface editor: one editable line per
// [[...]] type = TCPClientInterface stanza, all inside the card's single form.
static std::string tcp_clients_rows(const std::string& base) {
    auto list = store::read_tcp_clients();
    std::string rows;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& t = list[i];
        std::string idx = std::to_string(i);
        rows +=
            "<tr>"
            "<td><input type=\"text\" name=\"name_" + idx + "\" value=\"" + E(t.name) +
            "\" maxlength=\"48\" title=\"the [[section]] name in the rnsd config\"></td>"
            "<td><input type=\"text\" name=\"host_" + idx + "\" value=\"" + E(t.host) +
            "\" maxlength=\"253\" required></td>"
            "<td><input type=\"number\" name=\"port_" + idx + "\" value=\"" + E(t.port) +
            "\" min=\"1\" max=\"65535\" required></td>"
            "<td class=\"tcp-actions\">"
            "<button type=\"submit\" class=\"btn-danger\" formaction=\"" + base +
            "/reticulum/tcpclient/delete\" name=\"idx\" value=\"" + idx +
            "\" formnovalidate>Delete</button>"
            "</td></tr>";
    }
    return rows;
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
    std::string downloaded = u.downloaded_version;
    long downloaded_at     = u.downloaded_at;
    bool wheel_exists = !downloaded.empty() &&
        ::access(("/var/lib/rnsbox/rns-" + downloaded + ".whl").c_str(), F_OK) == 0;
    bool download_ready = update_available && downloaded == latest && wheel_exists;

    bool auto_update  = store::read_update_check();   // default on
    bool auto_install = store::read_update_install(); // default off

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
        chk += " <span class=\"muted\">— last attempt failed: " +
               E(update_error_hint(uerror)) + "</span>";
    else if (!u.source.empty())
        chk += " <span class=\"muted\">— via " + E(u.source) + "</span>";
    h = R(std::move(h), "__UPDATE_CHECK_LINE__", chk);

    std::string dline = update_available ? "no update downloaded" : "—";
    if (!downloaded.empty()) {
        dline = "rns " + E(downloaded) + " downloaded";
        if (downloaded_at > 0) {
            char buf[64]; time_t t = (time_t)downloaded_at; struct tm g;
            gmtime_r(&t, &g);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &g);
            dline += " at " + std::string(buf);
        }
        if (!wheel_exists)           dline += " — wheel missing, download again";
        else if (download_ready)     dline += " — ready to install";
        else if (update_available)   dline += " (latest is " + E(latest) + ")";
        else                         dline += " (installed version is current)";
    }
    h = R(std::move(h), "__UPDATE_DOWNLOAD_LINE__", dline);

    // Manual update controls: check, download and install are separate
    // actions; install stays disabled until the matching wheel is on disk.
    std::string update_buttons;
    if (update_available) {
        std::string le = E(latest);
        std::string dl_label = download_ready ? "Download again" : ("Download rns " + le);
        update_buttons =
            "<button type=\"button\" class=\"btn\" onclick=\"doUpdate('downloadupdate', this, false)\">" +
            dl_label + "</button> " +
            "<button type=\"button\" class=\"btn-primary\"" +
            (download_ready ? std::string()
                            : " disabled title=\"Download the update first\"") +
            " onclick=\"doUpdate('installupdate', this, true)\">Install rns " + le + " now</button>";
    }
    h = R(std::move(h), "__UPDATE_BUTTONS__", update_buttons);

    h = R(std::move(h), "__AUTO_UPDATE_CHECKED__", auto_update ? "checked" : "");
    h = R(std::move(h), "__AUTO_INSTALL_CHECKED__", auto_install ? "checked" : "");

    std::string opts = "<option value=\"0\" " + std::string(days == 0 ? "selected" : "") + ">None (default)</option>";
    for (int d = 1; d <= 7; ++d)
        opts += "\n      <option value=\"" + std::to_string(d) + "\" " +
                (days == d ? "selected" : "") + ">every " + std::to_string(d) +
                " day" + (d > 1 ? "s" : "") + "</option>";
    h = R(std::move(h), "__RESTART_DAYS_OPTIONS__", opts);
    h = R(std::move(h), "__RNSD_RESTART_TIME__", E(store::read_rnsd_restart_time()));

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

    // Bluetooth (BLE transport) card. The form block is filled server-side
    // (like __HALOW_LIVE_BLOCK__): a BLE-less image (cv1800b RAM-boot) gets
    // just a note instead of controls that could never work.
    store::BleConfig bl = store::read_ble();
    BtState bt = bt_status();
    bool bt_on = truthy(bl.enabled);
    std::string bt_form;
    if (!bt_installed()) {
        bt_form = "<p class=\"muted\" style=\"margin:8px 0 0\">The BLE transport is not part of this image "
                  "(no BLE stack packages).</p>";
    } else {
        bt_form =
            "<form method=\"post\" action=\"" + base + "/reticulum/bluetooth\">\n"
            "    <label style=\"margin:8px 0 0\"><input type=\"checkbox\" name=\"enabled\" "
            "__BT_ENABLED_CHECKED__> Enable BLE transport (scans for peers AND accepts inbound "
            "connections over its own GATT server)</label>\n"
            "    <div class=\"row-form\" style=\"margin-top:10px\">\n"
            "      <label style=\"margin:0\">Advertised name\n"
            "        <input type=\"text\" name=\"device_name\" value=\"__BT_NAME__\" maxlength=\"16\" "
            "title=\"Prefilled with the box hostname, so hostname, echo bot and BLE advert share one base prefix. Keep it short: BLE advertisement payloads are 31 bytes total. Clear the field to advertise the service UUID only\"></label>\n"
            "      <label style=\"margin:0\">Advertising interval\n"
            "        <select name=\"advertise_interval_ms\" title=\"How often the BLE advertisement goes out. Fast = a scanning phone finds the box within a couple of scan windows.\">\n"
            "          <option value=\"100\" __BT_IV100__>100 ms — fast (default)</option>\n"
            "          <option value=\"200\" __BT_IV200__>200 ms</option>\n"
            "          <option value=\"500\" __BT_IV500__>500 ms</option>\n"
            "          <option value=\"1000\" __BT_IV1000__>1 s</option>\n"
            "          <option value=\"0\" __BT_IV0__>BlueZ default (slowest)</option>\n"
            "        </select></label>\n"
            "      <label style=\"margin:0\">Central mode\n"
            "        <select name=\"enable_central\" title=\"Central mode makes the box scan for other Reticulum BLE peers and connect to them. Phones connect TO the box (peripheral) without it; leaving it off avoids radio contention with the phone's own connection attempts.\">\n"
            "          <option value=\"no\" __BT_EC_NO__>No — peripheral only (recommended)</option>\n"
            "          <option value=\"yes\" __BT_EC_YES__>Yes — also connect to advertising peers</option>\n"
            "        </select></label>\n"
            "    </div>\n"
            "    <div class=\"form-actions\" style=\"margin-top:10px\">\n"
            "      <span class=\"muted\">saving restarts rnsd — active links reconnect on their own</span>\n"
            "      <button type=\"submit\" class=\"btn-primary\">Save &amp; apply</button>\n"
            "    </div>\n"
            "  </form>\n"
            "  <h4 style=\"margin-top:16px\">Connected devices</h4>\n"
            "  <table class=\"data\"><thead><tr><th>Name</th><th>Address</th></tr></thead>\n"
            "  <tbody id=\"bt-peers\"><tr><td colspan=\"2\" class=\"muted\">none</td></tr></tbody></table>\n";
    }
    h = R(std::move(h), "__BT_FORM_BLOCK__", bt_form);
    h = R(std::move(h), "__BT_STACK_STATUS__", bt_status_html(bt));
    h = R(std::move(h), "__BT_ENABLED_CHECKED__", (bt_on && bt_installed()) ? "checked" : "");
    // Advertised name defaults to the box hostname, so the node carries one
    // base prefix everywhere: the hostname itself (S10uuid reads
    // /boot/hostname, pinned to "rnsbox" by the build), the echo bot's
    // "<hostname> echo bot" and this BLE advert. Clearing the field still
    // means the UUID-only advertisement the driver defaults to.
    {
        std::string btn = util::trim(bl.device_name);
        if (btn.empty()) btn = util::trim(util::read_file("/proc/sys/kernel/hostname", 128));
        if (btn == "localhost") btn.clear();
        if (btn.size() > 16) btn = btn.substr(0, 16);   // write_ble's own bound
        h = R(std::move(h), "__BT_NAME__", E(btn));
    }
    // Advertising interval select marker (unknown values fall back to 100)
    {
        std::string iv = util::trim(bl.advertise_interval_ms);
        if (iv != "0" && iv != "200" && iv != "500" && iv != "1000") iv = "100";
        h = R(std::move(h), "__BT_IV100__",  iv == "100"  ? "selected" : "");
        h = R(std::move(h), "__BT_IV200__",  iv == "200"  ? "selected" : "");
        h = R(std::move(h), "__BT_IV500__",  iv == "500"  ? "selected" : "");
        h = R(std::move(h), "__BT_IV1000__", iv == "1000" ? "selected" : "");
        h = R(std::move(h), "__BT_IV0__",    iv == "0"    ? "selected" : "");
    }
    // Central-mode select marker (absent/unknown = the safe peripheral-only)
    {
        bool ec = util::trim(bl.enable_central) == "yes";
        h = R(std::move(h), "__BT_EC_NO__",  ec ? "" : "selected");
        h = R(std::move(h), "__BT_EC_YES__", ec ? "selected" : "");
    }
    h = rnode_fill(std::move(h));
    h = rnsh_fill(std::move(h));
    h = rnsdmon_fill(std::move(h));
    h = R(std::move(h), "__ECHOBOT_CARDS__", echobot_cards(base));
    h = R(std::move(h), "__RNSH_ACCESS_CARDS__", access_cards(base));
    h = R(std::move(h), "__TCP_CLIENTS_ROWS__", tcp_clients_rows(base));
    h = R(std::move(h), "__TCP_CLIENTS_COUNT__", std::to_string(store::read_tcp_clients().size()));
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
    {
        store::BleConfig bl = store::read_ble();
        BtState bt = bt_status();
        o += ",\"ble\":{\"installed\":" + std::string(bt_installed() ? "true" : "false") +
            ",\"enabled\":\"" + http::json_escape(bl.enabled) +
            "\",\"device_name\":\"" + http::json_escape(bl.device_name) +
            "\",\"hci\":" + std::string(bt.hci ? "true" : "false") +
            ",\"hci_name\":\"" + http::json_escape(bt.hci_name) +
            "\",\"dbus\":" + std::string(bt.dbus ? "true" : "false") +
            ",\"bluetoothd\":" + std::string(bt.daemon ? "true" : "false") + "}";
    }
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
    std::string time = util::trim(req.f("time"));
    if (time.empty()) time = store::read_rnsd_restart_time();
    std::string err;
    if (!store::write_rnsd_restart_days(days, time, err)) {
        render::redirect_flash(res, ret_redirect(), "error", "Bad input: " + err);
        return;
    }
    web::run_init("S84cron", "reload");
    if (days == 0)
        render::redirect_flash(res, ret_redirect(), "success", "rnsd auto-restart disabled.");
    else
        render::redirect_flash(res, ret_redirect(), "success",
            "rnsd will auto-restart every " + std::to_string(days) + " day(s) at " + time + ".");
}

void reticulum_rnsdwatchdog(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enabled = req.f("enabled") == "on";
    long limit = strtol(req.f("limit").c_str(), nullptr, 10);
    bool known = false;
    for (long v : RNSDMON_LIMITS) if (v == limit) { known = true; break; }
    if (!known) limit = 3600;
    std::string err;
    if (!rnsdmon_write(enabled, limit, err)) {
        render::redirect_flash(res, ret_redirect(), "error", "Watchdog: " + err + ".");
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

// ---- TCP client interfaces (the editor card) ------------------------------
// add/update/delete a [[...]] type = TCPClientInterface stanza in
// /etc/reticulum/config (store::), then restart rnsd — the interface only
// exists while rnsd parses its config.

static void tcp_client_apply_msg(Response& res, bool ok, const std::string& err,
                                 const char* what) {
    if (!ok) {
        render::redirect_flash(res, ret_redirect(), "error",
            std::string("TCP interface: ") + (err.empty() ? "could not write the config" : err) + ".");
        return;
    }
    web::run_init("S82rnsd");
    render::redirect_flash(res, ret_redirect(), "success",
        std::string(what) + " — rnsd restarted to apply.");
}

void reticulum_tcpclient_add(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string name = util::trim(req.f("new_name"));
    std::string host = util::trim(req.f("new_host"));
    std::string port = util::trim(req.f("new_port"));
    if (name.empty() && host.empty() && port.empty()) {
        name = util::trim(req.f("name"));
        host = util::trim(req.f("host"));
        port = util::trim(req.f("port"));
    }
    std::string err;
    bool ok = store::add_tcp_client(name, host, port, err);
    tcp_client_apply_msg(res, ok, err, "TCP client interface added");
}

void reticulum_tcpclient_update(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string err;
    bool ok = store::update_tcp_client(atoi(req.f("idx").c_str()), util::trim(req.f("name")),
                                       util::trim(req.f("host")), util::trim(req.f("port")), err);
    tcp_client_apply_msg(res, ok, err, "TCP client interface saved");
}

void reticulum_tcpclient_save(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    int n = atoi(req.f("count").c_str());
    std::vector<store::TcpClientIface> list;
    for (int i = 0; i < n; ++i) {
        std::string idx = std::to_string(i);
        list.push_back({req.f(("name_" + idx).c_str()),
                        req.f(("host_" + idx).c_str()),
                        req.f(("port_" + idx).c_str())});
    }
    std::string err;
    bool ok = store::save_tcp_clients(list, err);
    tcp_client_apply_msg(res, ok, err, "TCP client interfaces saved");
}

void reticulum_tcpclient_delete(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string err;
    bool ok = store::delete_tcp_client(atoi(req.f("idx").c_str()), err);
    tcp_client_apply_msg(res, ok, err, "TCP client interface deleted");
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

// One Save & apply for the whole Maintenance block: daily check/download,
// optional automatic install, rnsd auto-restart and the down-watchdog. The
// settings live in two files (cron.conf, rnsdmon.conf); cron.conf is written
// once so a cron tick never sees half of the schedule.
void reticulum_maintenance(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;

    bool auto_update = req.f("auto_update") == "on";
    bool auto_install = req.f("auto_install") == "on";
    if (!auto_update) auto_install = false;   // install needs the daily check
    int days = atoi(req.f("days").c_str());
    std::string time = util::trim(req.f("time"));
    if (time.empty()) time = store::read_rnsd_restart_time();
    std::string err;
    if (!store::write_cron_settings(days, time, auto_update, auto_install, err)) {
        render::redirect_flash(res, ret_redirect(), "error",
            "Maintenance settings: " + err + ".");
        return;
    }

    bool watchdog = req.f("enabled") == "on";
    long limit = strtol(req.f("limit").c_str(), nullptr, 10);
    bool known = false;
    for (long v : RNSDMON_LIMITS) if (v == limit) { known = true; break; }
    if (!known) limit = 3600;
    if (!rnsdmon_write(watchdog, limit, err)) {
        render::redirect_flash(res, ret_redirect(), "error", "Watchdog: " + err + ".");
        return;
    }

    web::run_init("S84cron", "reload");
    render::redirect_flash(res, ret_redirect(), "success",
        "Maintenance settings saved (check/download " +
        std::string(auto_update ? "on" : "off") + ", auto-install " +
        std::string(auto_install ? "on" : "off") + ", auto-restart, watchdog).");
}

// checkupdate / downloadupdate / installupdate — ASYNC JSON endpoints (the
// reticulum.html doUpdate() spinner calls these via fetch). They shell out to
// the python updatecheck helper and return {"ok":bool,"reload":bool,
// "message":"..."}. Auth failure is a JSON 401 (require_auth json=true), so the
// page shows "session expired" instead of navigating to a login page
// mid-update. Download and install are deliberately separate: the wheel is
// fetched/verified first, then installed with a restart+rollback.
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

void reticulum_downloadupdate(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    if (!clock_trusted()) {
        update_json(res, false, false, std::string("Can't download the update: ") + NO_CLOCK);
        return;
    }
    auto rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/updatecheck.py", "download"}, "", 150);
    std::string j = rr.out;
    if (j.find("version") == std::string::npos && j.find("error") == std::string::npos) {
        update_json(res, false, false, "Download failed — no answer from the update helper.");
        return;
    }
    bool ok = json_true(j, "ok");
    std::string ver = json_str(j, "version");
    std::string src = json_str(j, "source");
    std::string uerror = json_str(j, "error");
    if (ok)
        update_json(res, true, true,
            "rns " + ver + " downloaded" + (src.empty() ? "" : " from " + src) +
            " and verified — ready to install.");
    else
        update_json(res, false, false, "Download failed: " +
            (uerror.empty() ? "unknown error" : uerror) + ".");
}

void reticulum_installupdate(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    // No clock check: the wheel is already on disk and verified; installing it
    // and restarting rnsd does not need TLS or a correct date.
    // Long: extract the wheel, restart rnsd, wait up to two minutes for it to
    // come up, and roll back on failure. Budgets nest (see updatecheck.py):
    // the helper finishes inside 270 s + python start-up; we give it 285 s;
    // uhttpd's script/network timeouts are 300 s (S81router).
    auto rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/updatecheck.py", "install"}, "", 285);
    if (rr.timed_out) {
        update_json(res, false, false,
            "Install timed out — the helper may still be rolling back; check rnsd on the next load.");
        return;
    }
    std::string j = rr.out;
    bool ok = json_true(j, "ok");
    bool rolled = json_true(j, "rolled_back");
    std::string from = json_str(j, "from");
    std::string to = json_str(j, "to");
    std::string uerror = json_str(j, "error");
    if (ok)
        update_json(res, true, true, "rnsd updated " + from + " → " + to + " and restarted.");
    else if (rolled)
        update_json(res, false, false,
            "Install failed (" + uerror + ") — rolled back to " + from + ".");
    else
        update_json(res, false, false,
            "Update not installed: " + (uerror.empty() ? "unknown error" : uerror) + ".");
}

// Legacy endpoint used by older cached pages: install the already-downloaded
// wheel. The current page uses downloadupdate + installupdate separately.
void reticulum_applyupdate(const Request& req, Response& res) {
    reticulum_installupdate(req, res);
}



// ===================== Bluetooth (BLE transport) POST ===============
void reticulum_bluetooth(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::BleConfig c;
    c.enabled     = req.f("enabled") == "on" ? "yes" : "no";
    c.device_name = util::trim(req.f("device_name"));
    c.advertise_interval_ms = util::trim(req.f("advertise_interval_ms"));
    c.enable_central = req.f("enable_central") == "yes" ? "yes" : "no";
    std::string err;
    if (!store::write_ble(c, err)) {
        render::redirect_flash(res, ret_redirect(), "error", "Bluetooth: " + err + ".");
        return;
    }
    // The interface exists only while rnsd parses its config, so a toggle
    // (or a renamed advertisement) needs the restart — said so up front.
    web::run_init("S82rnsd");
    if (c.enabled == "yes")
        render::redirect_flash(res, ret_redirect(), "success",
            "BLE transport enabled" +
            (c.device_name.empty() ? std::string("") : " as “" + c.device_name + "”") +
            " and rnsd restarted to apply (active links reconnect on their own). "
            "Without an adapter it runs as a logged no-op and picks a USB dongle up within 30 s of it appearing.");
    else
        render::redirect_flash(res, ret_redirect(), "success",
            "BLE transport disabled and rnsd restarted to apply. The Bluetooth stack stays up "
            "(bluetoothd + dbus) — disable those under Network if you want the radio fully off.");
}

void reticulum_bluetooth_raise(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    // bluetoothd first (it re-scans adapters as they appear), then rnsd —
    // a fresh BLEInterface probe then sees whatever the stack found.
    web::run_init("S40bluetoothd", "restart");
    web::run_init("S82rnsd");
    render::redirect_flash(res, ret_redirect(), "success",
        "Bluetooth stack and rnsd restarted. Check the Adapter line above: hci0 present + dbus + bluetoothd "
        "all green is a healthy stack; a missing adapter means no onboard radio and no dongle.");
}

// ---- Bluetooth card: generic BLE discovery + pairing ----------------------
// The BLE transport itself scans/connects/accepts inside rnsd, and the
// bundled BLE Agent bonds peers automatically on connect (BlueZ JustWorks).
// These endpoints give the operator eyes (who is advertising?) and a manual
// pair as a troubleshooting fallback. Same helper as the RNode card's pair
// (a stack-level operation), a wider scan (scan-any).

// Short-TTL cache for rnode.py scan-any (a discovery cycle costs ~6 s of
// CPU+radio on the target); same pattern as the RNode scan cache.
constexpr const char* BT_SCAN_CACHE = "/run/rnsbox-btscan.json";
constexpr const char* BT_SCAN_LOCK  = "/run/rnsbox-btscan.lock";
constexpr int BT_SCAN_TTL = 15;   // seconds

static bool bt_scan_fresh(std::string& out) {
    struct stat st;
    if (::stat(BT_SCAN_CACHE, &st) != 0) return false;
    time_t now = ::time(nullptr);
    if (st.st_mtime > now || now - st.st_mtime >= BT_SCAN_TTL) return false;
    out = util::read_file(BT_SCAN_CACHE, 256 * 1024);
    std::string t = util::trim(out);
    return !t.empty() && t[0] == '{';
}

// GET /reticulum/bluetooth/scan — every advertising BLE device (rnode.py
// scan-any), fails soft with a JSON error the page renders.
void reticulum_bluetooth_scan(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string cached;
    if (bt_scan_fresh(cached)) { res.body = cached; return; }

    int lfd = ::open(BT_SCAN_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0) ::flock(lfd, LOCK_EX);
    struct LockGuard { int fd; ~LockGuard() { if (fd >= 0) ::close(fd); } } guard{lfd};
    if (bt_scan_fresh(cached)) { res.body = cached; return; }

    if (!store::rnode_ble_stack_present()) {
        res.body = "{\"error\":\"no BLE stack in this image (bluez/dbus + bleak needed)\",\"devices\":[]}";
        return;
    }
    util::RunResult rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/rnode.py", "scan-any"}, "", 20);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') {
        util::write_file_atomic(BT_SCAN_CACHE, rr.out, 0600);
        res.body = rr.out;
        return;
    }
    res.body = "{\"error\":\"scan failed\",\"devices\":[]}";
}

// POST /reticulum/bluetooth/pair — same generic bluetoothctl pair + trust as
// the RNode card's endpoint (pairing is a BlueZ-level operation, not an
// RNode-specific one); the strict address check comes with it.
void reticulum_bluetooth_pair(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    reticulum_rnode_pair(req, res);
}

// GET /reticulum/bluetooth/peers — devices currently connected to the BLE
// adapter. BlueZ is the authoritative ACL view: it lists our outbound
// (central) links too. The Bluetooth card polls this every few seconds.
//
// Reliability: bluetoothctl needs several seconds to come up and print when
// the box is busy, and on a timeout it exits with NO output — which used to
// render as an empty "none" flash mid-session. So: serve a /run snapshot
// while it is <6 s old (the poll cadence reuses it, bluetoothctl is not
// spawned per request), take a fresh one with a generous timeout otherwise,
// and fall back to the last good snapshot (up to 60 s) when bluetoothctl
// comes back empty-handed.
constexpr const char* BT_PEERS_CACHE = "/run/rnsbox-bt-peers.json";
constexpr int BT_PEERS_FRESH = 6;    // s a snapshot is served without re-query
constexpr int BT_PEERS_STALE = 60;   // s an empty result may still fall back

static bool bt_peers_cache_age(time_t& age) {
    struct stat st;
    if (::stat(BT_PEERS_CACHE, &st) != 0) return false;
    time_t now = ::time(nullptr);
    if (st.st_mtime > now) return false;
    age = now - st.st_mtime;
    return true;
}

// The BLE transport logs "received identity handshake from dev:AA:BB:..:
// <hash>" (rnsd.log on BSP images, syslog elsewhere) — an unnamed RPA shows
// up in the table as its Reticulum identity instead of a bare question mark.
static std::map<std::string, std::string> bt_identity_names() {
    std::map<std::string, std::string> m;
    const char* logs[] = {"/var/log/rnsd.log", "/var/log/messages"};
    for (const char* lg : logs) {
        std::string s = util::read_file(lg, 192 * 1024);
        if (s.empty()) continue;
        const std::string pat = "received identity handshake from dev:";
        size_t pos = 0;
        while ((pos = s.find(pat, pos)) != std::string::npos) {
            pos += pat.size();
            // "dev:78:37:EC:8A:11:8C: 35e53f845c9cbca3" — a fixed 17-char
            // address, then ": " and the 16-byte hash (newest entry wins).
            if (pos + 19 > s.size()) break;
            std::string addr = s.substr(pos, 17);
            if (s[pos + 17] != ':') continue;
            size_t e = s.find('\n', pos);
            std::string hash = util::trim(s.substr(pos + 19, e == std::string::npos ? std::string::npos : e - pos - 19));
            size_t c = hash.find_first_not_of("0123456789abcdef");
            if (c != std::string::npos) hash = hash.substr(0, c);
            if (addr.size() == 17 && hash.size() >= 8)
                m[addr] = "RNS " + hash.substr(0, 8);
        }
        if (!m.empty()) break;
    }
    return m;
}

void reticulum_bluetooth_peers(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";

    time_t age = -1;
    bool have = bt_peers_cache_age(age);
    if (have && age <= BT_PEERS_FRESH) {
        std::string cached = util::read_file(BT_PEERS_CACHE, 64 * 1024);
        if (!cached.empty()) { res.body = cached; return; }
    }

    util::RunResult r = util::run({"/usr/bin/bluetoothctl", "devices", "Connected"}, "", 8);
    std::vector<std::pair<std::string, std::string>> found;
    size_t pos = 0;
    const std::string& s = r.out;
    while (pos <= s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string ln = util::trim(s.substr(pos, e - pos));
        pos = e + 1;
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        // "Device AA:BB:CC:DD:EE:FF Name with spaces"
        if (ln.compare(0, 7, "Device ") != 0) continue;
        std::string rest = ln.substr(7);
        size_t sp = rest.find(' ');
        std::string addr = sp == std::string::npos ? rest : rest.substr(0, sp);
        std::string name = sp == std::string::npos ? std::string() : util::trim(rest.substr(sp + 1));
        found.push_back({addr, name});
    }
    if (found.empty() && have && age <= BT_PEERS_STALE) {
        // bluetoothctl silent/timeout — the last snapshot beats a false "none"
        std::string cached = util::read_file(BT_PEERS_CACHE, 64 * 1024);
        if (!cached.empty()) { res.body = cached; return; }
    }

    std::map<std::string, std::string> ident;
    if (!found.empty()) ident = bt_identity_names();

    std::string out = "{\"devices\":[";
    bool first = true;
    for (const auto& d : found) {
        std::string name = d.second;
        if (name.empty()) {
            auto it = ident.find(d.first);
            if (it != ident.end()) name = it->second;
        }
        if (!first) out += ",";
        first = false;
        out += "{\"address\":\"" + http::json_escape(d.first) +
               "\",\"name\":\"" + http::json_escape(name) + "\"}";
    }
    out += "]";
    std::string ttx, trx;
    if (rnstatus_interface_traffic("BLE", ttx, trx))
        out += ",\"traffic\":{\"tx\":\"" + http::json_escape(ttx) +
               "\",\"rx\":\"" + http::json_escape(trx) + "\"}";
    out += "}";
    util::write_file_atomic(BT_PEERS_CACHE, out, 0600);
    res.body = out;
}

// ===================== rnstatus (JSON, cached) =====================
// `rnstatus -a -j` costs a python cold start (~2-5 s on the C906), but its
// JSON is the only complete interface inventory: rnsd's configured interfaces
// plus every spawned peer/client/listener, several of which carry ':' in the
// name (TCP endpoints) and were silently dropped by the old text parser.
// The parsed result is cached in /run for RNSTATUS_TTL seconds with a flock
// making concurrent misses single-flight — the page polls it and gets an
// instant (at most TTL-old) snapshot instead of a console dump.
constexpr const char* RNSTATUS_CACHE = "/run/rnsbox-rnstatus-v2.json";
constexpr const char* RNSTATUS_LOCK  = "/run/rnsbox-rnstatus-v2.lock";
constexpr int RNSTATUS_TTL = 5;   // seconds
// A mesh node can run a lot of interfaces (every discovered AutoInterface
// peer is one); rnstatus's JSON is ~1-2 KB per interface, so allow 1 MB.
constexpr size_t RNSTATUS_MAX = 1024 * 1024;

struct IfStat {
    std::string name, details, type, full_name, hash, parent;
    std::string status, mode, rate, mtu;
    std::string clients, peers, serving;
    std::string tx, tx_rate, rx, rx_rate;
};

// Formatting mirrors RNS.prettysize()/prettyspeed() and rnstatus's
// speed_str(), so the web table reads exactly like the CLI.
static std::string pretty_size(double num, const char* suffix = "B") {
    if (std::strcmp(suffix, "b") == 0) num *= 8.0;
    static const char* units[] = {"", "K", "M", "G", "T", "P", "E", "Z", "Y"};
    int u = 0;
    while ((num >= 1000.0 || num <= -1000.0) && u < 8) { num /= 1000.0; ++u; }
    char buf[64];
    if (u == 0) snprintf(buf, sizeof(buf), "%.0f %s%s", num, units[u], suffix);
    else        snprintf(buf, sizeof(buf), "%.2f %s%s", num, units[u], suffix);
    return buf;
}

static std::string pretty_speed(double bits_per_sec) {
    return pretty_size(bits_per_sec / 8.0, "b") + "ps";
}

static std::string pretty_bitrate(double bits_per_sec) {
    static const char* units[] = {"", "k", "M", "G", "T", "P", "E", "Z", "Y"};
    int u = 0;
    while ((bits_per_sec >= 1000.0 || bits_per_sec <= -1000.0) && u < 8) {
        bits_per_sec /= 1000.0;
        ++u;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %sbps", bits_per_sec, units[u]);
    return buf;
}

// RNS.prettytime(), non-compact and without fractional seconds.
static std::string pretty_uptime(double seconds) {
    if (seconds < 0) seconds = 0;
    long long s = (long long)seconds;
    long long d = s / 86400; s %= 86400;
    long long h = s / 3600;  s %= 3600;
    long long m = s / 60;    s %= 60;
    std::string o;
    auto add = [&](long long v, const char* unit) {
        if (!v) return;
        if (!o.empty()) o += " ";
        o += std::to_string(v) + unit;
    };
    add(d, "d"); add(h, "h"); add(m, "m"); add(s, "s");
    return o.empty() ? "0s" : o;
}

// Names come from str(interface) and can carry NUL/control bytes
// (LocalClientInterface's socket path starts with \0). Drop them before the
// value reaches JSON/HTML; the class part of the full name is used instead.
static std::string clean_text(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s)
        if (c >= 0x20 && c != 0x7f) o.push_back((char)c);
    return o;
}

static std::string mode_name(long long m) {
    switch (m) {
        case 2:  return "Point-to-Point";
        case 3:  return "Access Point";
        case 4:  return "Roaming";
        case 5:  return "Boundary";
        case 6:  return "Gateway";
        case 7:  return "Internal";
        default: return "Full";
    }
}

static bool rnstatus_parse(const std::string& out, std::vector<IfStat>& ifs,
                           std::string& thash, std::string& tupt, std::string& tstate) {
    json::Value root;
    std::string err;
    bool parsed = false;
    // util::run() merges stderr into stdout, so skip any warning prefix.
    size_t start = out.find('{');
    while (start != std::string::npos) {
        if (json::parse(out.substr(start), root, err) && root.is_object() &&
            root.find("interfaces") != nullptr) {
            parsed = true;
            break;
        }
        start = out.find('{', start + 1);
    }
    const json::Value* arr = parsed ? root.find("interfaces") : nullptr;
    if (!arr || !arr->is_array()) return false;

    auto str_of = [](const json::Value& v, const char* key) {
        const json::Value* x = v.find(key);
        return x ? x->as_string() : std::string();
    };
    const json::Value* x = nullptr;

    for (const json::Value& v : arr->arr) {
        if (!v.is_object()) continue;
        IfStat f;
        f.full_name = clean_text(str_of(v, "name"));
        f.type      = clean_text(str_of(v, "type"));
        std::string raw_short = str_of(v, "short_name");
        std::string short_name = clean_text(raw_short);

        size_t ob = f.full_name.find('[');
        size_t cb = f.full_name.rfind(']');
        std::string bracket;
        if (ob != std::string::npos && cb != std::string::npos && cb > ob)
            bracket = f.full_name.substr(ob + 1, cb - ob - 1);

        // Spawned interfaces (AutoInterfacePeer, ...) have no name of their
        // own; LocalClientInterface's name embeds a NUL-prefixed socket path.
        // Fall back to the class part of the full form in both cases.
        if (short_name.empty() || short_name == "None" || short_name != raw_short)
            short_name = util::trim(f.full_name.substr(0, ob == std::string::npos
                                                            ? f.full_name.size() : ob));
        if (short_name.empty()) short_name = "?";

        // The daemon's local listener: "Shared Instance[/run/...socket]".
        bool shared = f.full_name.rfind("Shared Instance[", 0) == 0;
        f.name = shared ? "Shared instance" : short_name;

        if (!bracket.empty()) {
            std::string d = bracket;
            if (d == short_name) d.clear();
            else if (d.compare(0, short_name.size() + 1, short_name + "/") == 0)
                d = d.substr(short_name.size() + 1);
            f.details = clean_text(d);
        }

        x = v.find("status");
        f.status = (x && x->as_bool(false)) ? "Up" : "Down";
        if ((x = v.find("gravity")) && x->is_number() && x->num != 0)
            f.status += ", gravity " + std::to_string(x->as_int(0));

        // Every interface has a mode; when the config omits it Reticulum uses
        // the default full mode. rnstatus's text form hides it for a few
        // interface classes, but the portal shows it for all of them.
        x = v.find("mode");
        f.mode = mode_name(x ? x->as_int(1) : 1);

        if ((x = v.find("bitrate")) && x->is_number()) f.rate = pretty_bitrate(x->num);
        if ((x = v.find("mtu")) && x->is_number())     f.mtu  = std::to_string(x->as_int(0));

        long long clients = -1;
        if ((x = v.find("clients")) && x->is_number()) clients = x->as_int(0);
        if (shared) {
            // clients includes the rnstatus process asking right now.
            f.serving = std::to_string(clients > 0 ? clients - 1 : 0);
        } else if (clients >= 0) {
            f.clients = std::to_string(clients);
        }
        if ((x = v.find("peers")) && x->is_number()) f.peers = std::to_string(x->as_int(0));

        f.tx      = pretty_size(v.find("txb") ? v.find("txb")->as_number(0) : 0);
        f.tx_rate = pretty_speed(v.find("txs") ? v.find("txs")->as_number(0) : 0);
        f.rx      = pretty_size(v.find("rxb") ? v.find("rxb")->as_number(0) : 0);
        f.rx_rate = pretty_speed(v.find("rxs") ? v.find("rxs")->as_number(0) : 0);

        f.hash   = clean_text(str_of(v, "hash"));
        f.parent = clean_text(str_of(v, "parent_interface_name"));
        ifs.push_back(std::move(f));
    }

    if ((x = root.find("transport_id")) && x->is_string())      thash = x->str;
    if ((x = root.find("transport_uptime")) && x->is_number())  tupt  = pretty_uptime(x->num);
    if (!thash.empty()) tstate = "running";
    return !ifs.empty() || !thash.empty();
}

static bool rnstatus_fresh(std::string& out) {
    struct stat st;
    if (::stat(RNSTATUS_CACHE, &st) != 0) return false;
    time_t now = ::time(nullptr);
    if (st.st_mtime > now || now - st.st_mtime >= RNSTATUS_TTL) return false;
    out = util::read_file(RNSTATUS_CACHE, RNSTATUS_MAX);
    std::string t = util::trim(out);
    return !t.empty() && t[0] == '{';
}

// Cache age in seconds (-1: no cache file).
static long rnstatus_age() {
    struct stat st;
    if (::stat(RNSTATUS_CACHE, &st) != 0) return -1;
    time_t now = ::time(nullptr);
    if (st.st_mtime > now) return 0;
    return (long)(now - st.st_mtime);
}

// Run `rnstatus -a -j`, normalise, rewrite the cache. Shared by the
// synchronous path and the stale-while-revalidate child.
static std::string rnstatus_collect(bool* ok) {
    util::RunResult rr = util::run({"/usr/bin/rnstatus", "-a", "-j"}, "", 25, RNSTATUS_MAX);
    std::vector<IfStat> ifs;
    std::string thash, tupt, tstate;
    if (!rnstatus_parse(rr.out, ifs, thash, tupt, tstate)) {
        if (ok) *ok = false;
        return "{\"error\":\"rnstatus returned nothing (is rnsd running?)\"}";
    }
    if (ok) *ok = true;
    std::string o = "{\"v\":2,\"age\":" + std::to_string(::time(nullptr)) + ",\"interfaces\":[";
    for (size_t i = 0; i < ifs.size(); ++i) {
        const IfStat& f = ifs[i];
        if (i) o += ",";
        o += "{\"name\":\"" + http::json_escape(f.name) +
             "\",\"details\":\"" + http::json_escape(f.details) +
             "\",\"type\":\"" + http::json_escape(f.type) +
             "\",\"full_name\":\"" + http::json_escape(f.full_name) +
             "\",\"hash\":\"" + http::json_escape(f.hash) +
             "\",\"parent\":\"" + http::json_escape(f.parent) +
             "\",\"status\":\"" + http::json_escape(f.status) +
             "\",\"mode\":\"" + http::json_escape(f.mode) +
             "\",\"rate\":\"" + http::json_escape(f.rate) +
             "\",\"mtu\":\"" + http::json_escape(f.mtu) +
             "\",\"clients\":\"" + http::json_escape(f.clients) +
             "\",\"peers\":\"" + http::json_escape(f.peers) +
             "\",\"serving\":\"" + http::json_escape(f.serving) +
             "\",\"tx\":\"" + http::json_escape(f.tx) +
             "\",\"tx_rate\":\"" + http::json_escape(f.tx_rate) +
             "\",\"rx\":\"" + http::json_escape(f.rx) +
             "\",\"rx_rate\":\"" + http::json_escape(f.rx_rate) + "\"}";
    }
    o += "],\"transport\":{\"hash\":\"" + http::json_escape(thash) +
         "\",\"uptime\":\"" + http::json_escape(tupt) +
         "\",\"state\":\"" + http::json_escape(tstate) + "\"}}";
    util::write_file_atomic(RNSTATUS_CACHE, o, 0600);
    return o;
}

// GET /reticulum/rnstatus.json — the parsed interface table.
//
// rnstatus -a -j costs a python cold start (5-10 s on this CPU), so a request
// never waits for it: ANY parseable snapshot is served IMMEDIATELY, no
// matter how old (the table shows when it was taken), and a detached child
// refreshes it when it is older than RNSTATUS_TTL (non-blocking lock:
// concurrent refreshers collapse into one). Only a missing or corrupt
// cache takes the synchronous path.
void reticulum_rnstatus(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string cached;
    if (rnstatus_fresh(cached)) { res.body = cached; return; }

    long age = rnstatus_age();
    if (age >= 0) {
        std::string stale = util::read_file(RNSTATUS_CACHE, RNSTATUS_MAX);
        std::string t = util::trim(stale);
        if (!t.empty() && t[0] == '{') {
            if (age >= RNSTATUS_TTL) {
                pid_t pid = ::fork();
                if (pid == 0) {
                    // Detached refresher: new session; stdio moved to
                    // /dev/null. Closing 0-2 instead would let util::run's
                    // pipe() reuse those numbers, and its close(inpipe[1])
                    // would then close the child's own stdout pipe (empty
                    // rnstatus output, parse fails, cache never written).
                    ::setsid();
                    int nul = ::open("/dev/null", O_RDWR);
                    if (nul >= 0) {
                        ::dup2(nul, 0); ::dup2(nul, 1); ::dup2(nul, 2);
                        if (nul > 2) ::close(nul);
                    }
                    int lfd = ::open(RNSTATUS_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
                    if (lfd >= 0 && ::flock(lfd, LOCK_EX | LOCK_NB) == 0) {
                        if (rnstatus_age() >= RNSTATUS_TTL)
                            rnstatus_collect(nullptr);
                        ::close(lfd);
                    }
                    _exit(0);
                }
            }
            // Parent: serve the stale snapshot; the poll picks up the fresh
            // one a few seconds later.
            res.body = stale;
            return;
        }
    }

    int lfd = ::open(RNSTATUS_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0) ::flock(lfd, LOCK_EX);
    struct LockGuard { int fd; ~LockGuard() { if (fd >= 0) ::close(fd); } } guard{lfd};
    if (rnstatus_fresh(cached)) { res.body = cached; return; }
    res.body = rnstatus_collect(nullptr);
}

// Traffic (tx/rx, pretty strings) of one interface out of the parsed rnstatus
// snapshot, so other pages can show byte counters without running rnstatus
// themselves. Needle matches anywhere in the interface's JSON object (e.g.
// "BLE" hits name BLEInterface / details "BLE Interface").
bool rnstatus_interface_traffic(const char* needle, std::string& tx, std::string& rx) {
    std::string j = util::read_file(RNSTATUS_CACHE, RNSTATUS_MAX);
    size_t p = 0;
    while ((p = j.find("{\"name\":", p)) != std::string::npos) {
        size_t e = j.find('}', p);
        if (e == std::string::npos) break;
        std::string obj = j.substr(p, e - p);
        p = e;
        if (obj.find(needle) == std::string::npos) continue;
        size_t a = obj.find("\"tx\":\""), a2 = obj.find("\"rx\":\"");
        if (a == std::string::npos || a2 == std::string::npos) continue;
        a += 6;
        a2 += 6;
        tx = obj.substr(a, obj.find('"', a) - a);
        rx = obj.substr(a2, obj.find('"', a2) - a2);
        return true;
    }
    return false;
}

// ===================== RNode (USB serial / BLE) =====================
// /etc/rnsbox/rnode.conf is the single source of truth (store::read_rnode /
// write_rnode); S81rnode regenerates the managed [[RNode ...]] section of
// /etc/reticulum/config from it and leaves the resolved device state in
// /run/rnsbox-rnode.status. Save applies as `S81rnode reload` + an rnsd
// restart (both synchronous web::run_init calls, in that order). There is no
// daemon of our own to start: RNS >= 1.5.7 opens the serial port itself (and
// retries it every 5 s until the modem is plugged in) and speaks RNode-over-
// BLE natively (port = ble://..., bleak + BlueZ, peers bonded on connect by
// the bundled BLE Agent).

static std::string rnode_fill(std::string h) {
    store::RnodeConfig rc = store::read_rnode();
    store::RnodeStatus st = store::read_rnode_status();
    bool usb = rc.transport == "usb";
    bool ble = rc.transport == "ble";

    std::string status;
    if (rc.transport != "usb" && rc.transport != "ble") {
        status = "<span class=\"badge\">off</span>";
    } else if (st.state == "ok" && !st.port.empty()) {
        status = "<span class=\"badge ok\">ready</span> <span class=\"mono\">" + E(st.port) + "</span>";
    } else if (st.state == "waiting") {
        status = "<span class=\"badge warn\">waiting for device</span> " + E(st.message);
    } else if (st.state == "scanning") {
        status = "<span class=\"badge warn\">scanning</span> " + E(st.message);
    } else if (st.state == "no-pyserial") {
        status = "<span class=\"badge err\">pyserial missing</span> the image lacks the serial module RNS needs for USB RNodes";
    } else if (st.state == "no-ble-stack") {
        status = "<span class=\"badge err\">BLE stack missing</span> " + E(st.message);
    } else if (st.state == "invalid") {
        status = "<span class=\"badge err\">invalid config</span> " + E(st.message);
    } else if (st.state == "error") {
        status = "<span class=\"badge err\">error</span> " + E(st.message);
    } else {
        status = "<span class=\"badge\">not applied yet</span> (runs at boot, or Save &amp; apply below)";
    }
    h = R(std::move(h), "__RNODE_STATUS__", status);

    std::string sel_off = !usb && !ble ? " selected" : "";
    std::string sel_usb = usb ? " selected" : "";
    std::string sel_ble = ble ? " selected" : "";
    // Short labels only: board names / device paths live in the select's
    // title and the per-transport field notes, not in the option text.
    h = R(std::move(h), "__RNODE_TRANSPORT_OPTIONS__",
          "<option value=\"off\"" + sel_off + ">Off (default)</option>\n"
          "      <option value=\"usb\"" + sel_usb + ">USB serial</option>\n"
          "      <option value=\"ble\"" + sel_ble + ">BLE</option>");

    // Device field: a datalist over the USB serial devices present right now,
    // so "auto" and a pinned /dev/ttyACM0 are both one click away.
    std::string devs = "<option value=\"auto\">auto (first found)</option>";
    for (const auto& d : store::rnode_serial_devices())
        devs += "\n      <option value=\"" + E(d) + "\">" + E(d) + "</option>";
    h = R(std::move(h), "__RNODE_DEVICE_OPTIONS__", devs);

    h = R(std::move(h), "__RNODE_DEVICE__",     E(usb ? rc.device : "auto"));
    h = R(std::move(h), "__RNODE_BLE_ADDRESS__", E(ble ? rc.ble_address : ""));
    h = R(std::move(h), "__RNODE_BLE_NAME__",   E(ble ? rc.ble_name : ""));
    h = R(std::move(h), "__RNODE_NAME__",       E(rc.name));
    h = R(std::move(h), "__RNODE_FREQUENCY__",  E(rc.frequency));
    h = R(std::move(h), "__RNODE_BANDWIDTH__",  E(rc.bandwidth));
    h = R(std::move(h), "__RNODE_TXPOWER__",    E(rc.txpower));
    h = R(std::move(h), "__RNODE_SF__",         E(rc.spreadingfactor));
    h = R(std::move(h), "__RNODE_CR__",         E(rc.codingrate));
    h = R(std::move(h), "__RNODE_FLOW_CHECKED__", truthy(rc.flow_control) ? "checked" : "");

    std::string stack;
    if (!store::rnode_ble_stack_present())
        stack = "<p class=\"muted\" style=\"margin:6px 0 0\">This image has no BLE stack (bluez/dbus + bleak) — "
                "saving the BLE transport keeps the interface disabled until the stack is installed.</p>";
    h = R(std::move(h), "__RNODE_BLE_STACK_NOTE__", stack);
    return h;
}

void reticulum_rnode(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::RnodeConfig c;
    c.transport       = util::trim(req.f("transport"));
    c.device          = util::trim(req.f("device"));
    c.ble_address     = util::trim(req.f("ble_address"));
    c.ble_name        = util::trim(req.f("ble_name"));
    c.name            = util::trim(req.f("name"));
    c.frequency       = util::trim(req.f("frequency"));
    c.bandwidth       = util::trim(req.f("bandwidth"));
    c.txpower         = util::trim(req.f("txpower"));
    c.spreadingfactor = util::trim(req.f("spreadingfactor"));
    c.codingrate      = util::trim(req.f("codingrate"));
    c.flow_control    = req.f("flow_control") == "on" ? "yes" : "no";
    std::string err;
    if (!store::write_rnode(c, err)) {
        render::redirect_flash(res, ret_redirect(), "error", "RNode: " + err + ".");
        return;
    }
    // Regenerate the [[RNode ...]] section, then bounce rnsd so it opens the
    // (new) port. Both synchronous; S81rnode is fast, the restart is bounded
    // by S82rnsd's own 25 s lock wait.
    std::string apply = web::run_init("S81rnode", "reload") ? "" :
        " (warning: the config rewrite failed — see /var/log/messages)";
    web::run_init("S82rnsd");
    if (c.transport == "off")
        render::redirect_flash(res, ret_redirect(), "success", "RNode interface disabled." + apply);
    else if (c.transport == "usb")
        render::redirect_flash(res, ret_redirect(), "success",
            "RNode over USB saved" + apply + ". The modem may be plugged in at any time — rnsd attaches it "
            "within seconds (it retries the port every 5 s).");
    else
        render::redirect_flash(res, ret_redirect(), "success",
            "RNode over BLE saved" + apply + ". rnsd connects and bonds the peer automatically "
            "(JustWorks, fully automatic — no manual pairing needed).");
}

// Short-TTL cache for rnode.py scan (BLE discovery costs ~6 s of CPU+radio
// on the target); same pattern as the halow modem cache.
constexpr const char* RNODE_SCAN_CACHE = "/run/rnsbox-rnode-scan.json";
constexpr const char* RNODE_SCAN_LOCK  = "/run/rnsbox-rnode-scan.lock";
constexpr int RNODE_SCAN_TTL = 15;   // seconds

static bool rnode_scan_fresh(std::string& out) {
    struct stat st;
    if (::stat(RNODE_SCAN_CACHE, &st) != 0) return false;
    time_t now = ::time(nullptr);
    if (st.st_mtime > now || now - st.st_mtime >= RNODE_SCAN_TTL) return false;
    out = util::read_file(RNODE_SCAN_CACHE, 256 * 1024);
    std::string t = util::trim(out);
    return !t.empty() && t[0] == '{';
}

// GET /reticulum/rnode/status — cheap: reads S81rnode's status file and /dev,
// no python. The page polls this so a modem plugged in after load shows up.
void reticulum_rnode_status(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    store::RnodeStatus st = store::read_rnode_status();
    std::string o = std::string("{\"transport\":\"") + http::json_escape(st.transport) +
        "\",\"port\":\"" + http::json_escape(st.port) +
        "\",\"state\":\"" + http::json_escape(st.state) +
        "\",\"message\":\"" + http::json_escape(st.message) +
        "\",\"status_present\":" + (st.present ? "true" : "false") +
        ",\"ble_stack\":" + (store::rnode_ble_stack_present() ? "true" : "false") +
        ",\"devices\":[";
    bool first = true;
    for (const auto& d : store::rnode_serial_devices()) {
        if (!first) o += ",";
        first = false;
        o += "\"" + http::json_escape(d) + "\"";
    }
    o += "]}";
    res.content_type = "application/json";
    res.body = o;
}

// GET /reticulum/rnode/scan — BLE discovery via the rnode.py helper (needs
// the BLE stack; fails soft with a JSON error the page can render).
void reticulum_rnode_scan(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string cached;
    if (rnode_scan_fresh(cached)) { res.body = cached; return; }

    int lfd = ::open(RNODE_SCAN_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0) ::flock(lfd, LOCK_EX);
    struct LockGuard { int fd; ~LockGuard() { if (fd >= 0) ::close(fd); } } guard{lfd};
    if (rnode_scan_fresh(cached)) { res.body = cached; return; }

    if (!store::rnode_ble_stack_present()) {
        res.body = "{\"error\":\"no BLE stack in this image (bluez/dbus + bleak needed)\",\"devices\":[]}";
        return;
    }
    util::RunResult rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/rnode.py", "scan"}, "", 20);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') {
        util::write_file_atomic(RNODE_SCAN_CACHE, rr.out, 0600);
        res.body = rr.out;
        return;
    }
    res.body = "{\"error\":\"scan failed\",\"devices\":[]}";
}

// POST /reticulum/rnode/pair — manual bluetoothctl pair (+ trust) as a
// troubleshooting fallback; normal attach bonds automatically on connect.
// Best-effort: needs the BLE stack.
void reticulum_rnode_pair(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string addr = util::trim(req.f("address"));
    if (!store::rnode_ble_stack_present()) {
        update_json(res, false, false, "No BLE stack in this image (bluez/dbus + bleak needed).");
        return;
    }
    // Strict address check: it goes into a bluetoothctl argv (no shell), so
    // this is defense in depth, not the security boundary itself.
    if (addr.size() != 17) { update_json(res, false, false, "Bad address."); return; }
    for (size_t i = 0; i < addr.size(); ++i) {
        char c = addr[i];
        if (i % 3 == 2) { if (c != ':') { update_json(res, false, false, "Bad address."); return; } }
        else if (!isxdigit((unsigned char)c)) { update_json(res, false, false, "Bad address."); return; }
    }
    util::RunResult rr = util::run({"/usr/bin/python3", "/usr/lib/rnsbox/rnode.py", "pair", addr}, "", 40);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') { res.body = rr.out; return; }
    update_json(res, false, false, "Pairing failed (is bluetoothd running? is the RNode in pairing reach?)");
}

}  // namespace routes
