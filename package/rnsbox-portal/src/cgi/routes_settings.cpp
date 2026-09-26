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
#include <cstdio>
#include <cstdlib>
#include <cstring>    // ::strerror (an unreadable RTC)
#include <ctime>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/file.h> // ::flock (S45ntpsync's clock-step lock)
#include <unistd.h>   // ::sleep (wrong-password speed bump), ::access (/dev/rtc0)

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }
static std::string settings_redirect() { return web::base() + "/settings"; }

// ---- Time & Clock card fragments ----

// /run/clock-source as a badge + one line of what it means.
static std::string clock_source_html(const std::string& src) {
    if (src == "ntp")
        return "<span class=\"badge ok\">NTP</span> <span class=\"muted\">stepped from a time server</span>";
    if (src == "rtc")
        return "<span class=\"badge ok\">RTC</span> <span class=\"muted\">read from the hardware clock at boot</span>";
    if (src == "browser")
        return "<span class=\"badge ok\">browser</span> <span class=\"muted\">set from a browser on this page</span>";
    return "<span class=\"badge warn\">none</span> <span class=\"muted\">no NTP, RTC or browser time yet</span>";
}

static std::string rtc_html(const sysinfo::Rtc& rtc) {
    if (!rtc.present)
        return "<span class=\"muted\">none fitted — the clock starts at 1970 on every boot "
               "until NTP or a browser sync sets it</span>";
    std::string chip = rtc.name.empty() ? std::string() : " <span class=\"muted\">(" + E(rtc.name) + ")</span>";
    if (rtc.valid)
        return "<span class=\"badge ok\">set</span> <span class=\"timeval\">" + E(rtc.time) +
               "</span> <span class=\"muted\">UTC</span>" + chip;
    // The driver refuses the time while the oscillator-stop flag is set (EINVAL).
    if (rtc.osf)
        return "<span class=\"badge warn\">not set</span>" + chip +
               "<br><span class=\"muted\">Fitted, but its oscillator has stopped since it was last set "
               "(never set, or its backup cell ran flat while unpowered), so it holds no valid time "
               "and was ignored at boot. The next NTP sync or a browser sync below sets it; replace "
               "the coin cell if this recurs.</span>";
    if (!rtc.time.empty())
        return "<span class=\"badge warn\">not set</span>" + chip +
               "<br><span class=\"muted\">It reads an implausible </span><span class=\"timeval\">" + E(rtc.time) +
               "</span> <span class=\"muted\">UTC (outside 2020–2100), so it was ignored at boot. "
               "The next NTP sync or a browser sync below sets it.</span>";
    std::string why = rtc.read_errno ? std::string(::strerror(rtc.read_errno)) : std::string("unexpected value");
    return "<span class=\"badge warn\">unreadable</span>" + chip +
           "<br><span class=\"muted\">Fitted, but reading it failed (" + E(why) + "), so its time "
           "is ignored. Check its wiring; the next NTP sync or a browser sync below writes it.</span>";
}

// Row only when the DS3231's hwmon sensor is there (millidegrees -> 0.25 °C steps).
static std::string rtc_temp_row(const sysinfo::Rtc& rtc) {
    if (!rtc.present || !rtc.has_temp) return std::string();
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", rtc.temp_mc / 1000.0);
    return "<tr><th>RTC temperature</th><td>" + E(buf) + " &deg;C</td></tr>";
}

// ---- GET render ----

void settings_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string base = web::base();

    std::string host = sysinfo::hostname();
    std::string wan  = sysinfo::wan_label(store::read_wan().interface, sysinfo::network_summary());

    std::vector<std::string> ntp = store::read_ntp_servers();
    std::string ntp_joined;
    for (size_t i = 0; i < ntp.size(); ++i) { if (i) ntp_joined += "\n"; ntp_joined += ntp[i]; }

    time_t now = ::time(nullptr);
    char ts[40]; struct tm tmv; gmtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    // Synced = a recorded source (the same rule as /run/clock-synced); the year
    // test only guards against a stale or mistaken source file.
    std::string src = sysinfo::clock_source();
    bool synced = !src.empty() && (long long)now >= sysinfo::CLOCK_FLOOR;
    sysinfo::Rtc rtc = sysinfo::rtc_status();

    // Fixed/system tokens first, operator-controlled values (NTP servers,
    // hostname, WAN) last, so a value that happens to spell a token is never
    // expanded again.
    std::string h = render::page_file("settings");
    h = R(std::move(h), "__BASE__", base);
    h = R(std::move(h), "__SYS_TIME__", E(std::string(ts)));
    h = R(std::move(h), "__TIME_BADGE__",
          synced ? "<span class=\"badge ok\">synced</span>"
                 : "<span class=\"badge warn\">not synced</span>");
    h = R(std::move(h), "__CLOCK_SOURCE__", clock_source_html(src));
    h = R(std::move(h), "__RTC_STATUS__", rtc_html(rtc));
    h = R(std::move(h), "__RTC_TEMP_ROW__", rtc_temp_row(rtc));
    // NTP outranks a browser (see time_browser), so the button is moot once it
    // has synced; the POST handler enforces it regardless.
    h = R(std::move(h), "__BROWSER_BTN_ATTR__", src == "ntp" ? " disabled" : "");
    h = R(std::move(h), "__BROWSER_NOTE__", src == "ntp"
          ? "<span class=\"muted\">Not needed: NTP has synced the clock, and it takes precedence.</span>"
          : "");
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
    // Strict charset (alnum, '-', '_') — this also rejects CR/LF/control chars
    // before anything reaches /etc/hostname.
    bool ok = !h.empty() && h.size() <= 63 && h[0] != '-';
    for (char c : h) if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) ok = false;
    if (!ok) {
        render::redirect_flash(res, settings_redirect(), "error", "Invalid hostname (alphanumerics, -, _; max 63 chars)");
        return;
    }
    util::write_file_atomic("/etc/hostname", h + "\n", 0644);
    util::run({"/bin/hostname", h}, "", 5);
    // 127.0.1.1 -> the new name, so dnsmasq and local lookups resolve it
    // (buildroot generates that line from the build-time hostname). Left
    // alone if it can't be read: rewriting from nothing would drop localhost.
    std::string hosts = util::read_file("/etc/hosts");
    if (!hosts.empty()) {
        std::string o;
        bool done = false;
        size_t i = 0;
        while (i < hosts.size()) {
            size_t e = hosts.find('\n', i);
            if (e == std::string::npos) e = hosts.size();
            std::string ln = hosts.substr(i, e - i);
            if (!done && ln.compare(0, 9, "127.0.1.1") == 0 && (ln.size() == 9 || isspace((unsigned char)ln[9]))) {
                ln = "127.0.1.1\t" + h;
                done = true;
            }
            o += ln + "\n";
            i = e + 1;
        }
        if (!done) o += "127.0.1.1\t" + h + "\n";
        util::write_file_atomic("/etc/hosts", o, 0644);
    }
    // S10uuid applies /boot/hostname (sd_gen ships "rnsbox") at every boot, so
    // without this the reboot would put the old name back.
    if (!util::write_file_atomic("/boot/hostname", h + "\n", 0644)) {
        render::redirect_flash(res, settings_redirect(), "error",
            "Hostname changed to '" + h + "' for now, but /boot/hostname could not be written: "
            "it reverts at the next reboot.");
        return;
    }
    render::redirect_flash(res, settings_redirect(), "success",
        "Hostname changed to '" + h + "'. Reboot to apply everywhere.");
}

void ntp_set(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string raw = req.f("servers");
    // One server per line is the textarea's format, so CR/LF/TAB are just
    // separators here; any OTHER control char means garbage — refuse the lot
    // (nothing written) rather than silently dropping pieces of it.
    for (unsigned char c : raw) {
        if ((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c == 0x7f) {
            render::redirect_flash(res, settings_redirect(), "error",
                "NTP servers must not contain control characters; nothing was saved.");
            return;
        }
    }
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
    // Plus S45ntpsync's one-shot, so an edited server list steps the clock
    // within seconds instead of after ntpd's poll backoff. `sync` only launches
    // it detached and returns (0 = started, 2 = one is already running, 3 = no
    // configured server is routable right now: no default route, and no
    // literal-address server on a connected subnet); it never re-classifies
    // the RTC, and on success it records source=ntp and writes the RTC itself.
    // Safe right after the restart: the one-shot stops ntpd for its step and
    // starts it again (never two steppers at once), and when no server is
    // routable it isn't launched at all, leaving the restarted ntpd on the new
    // list.
    int rc = util::run({"/etc/init.d/S45ntpsync", "sync"}, "", 10).exit_code;
    std::string msg = "ntpd restarted";
    if (rc == 0)
        msg += ", and a one-off NTP sync started in the background: reload in a few seconds "
               "to see the clock source (log: /var/log/ntp-step.log).";
    else if (rc == 2)
        msg += "; an NTP sync is already running (log: /var/log/ntp-step.log).";
    else if (rc == 3)
        msg += "; none of the NTP servers is reachable right now (no route to any of them) — "
               "ntpd keeps trying, and the clock is stepped once one is.";
    else
        msg += " — it will step the clock once it reaches a server.";
    render::redirect_flash(res, settings_redirect(), "success", msg);
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
    // > 12 digits is past 2100 anyway; capping it keeps atol from overflowing
    // (wrapping) back into the plausible window.
    long epoch = es.size() > 12 ? 0L : atol(es.c_str());
    if (epoch < 1577836800L || epoch > 4102444800L) {   // 2020-01-01 .. 2100-01-01
        render::redirect_flash(res, settings_redirect(), "error", "Browser time looks implausible; clock not changed.");
        return;
    }
    // One clock stepper at a time: S45ntpsync's NTP one-shot holds this lock
    // while ntpdate -b steps the clock relative to the current time, so a step
    // of ours inside its sampling window would be applied twice. Held (and
    // S49ntp start deferred) only for the date + hwclock below; O_CLOEXEC
    // keeps it out of those children.
    int lfd = ::open("/run/ntpsync.step.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lfd >= 0 && ::flock(lfd, LOCK_EX | LOCK_NB) != 0) {
        ::close(lfd);
        render::redirect_flash(res, settings_redirect(), "error",
            "An NTP sync is running right now (up to a minute when its servers are unreachable); "
            "browser time not applied. Try again shortly.");
        return;
    }
    struct LockGuard { int fd; ~LockGuard() { if (fd >= 0) ::close(fd); } } guard{lfd};   // close = unlock
    // /run/clock-source precedence is ntp > browser > rtc: once NTP has stepped
    // the clock, a browser time (at best off by the page's age) would only fight
    // the running ntpd — and would be written into the RTC as well. Tested
    // under the lock, so a one-shot that has just finished is seen.
    if (sysinfo::clock_source() == "ntp") {
        render::redirect_flash(res, settings_redirect(), "error",
            "The clock is already synced by NTP, which takes precedence; browser time not applied.");
        return;
    }
    time_t e = (time_t)epoch;
    char stamp[40]; struct tm tmv; gmtime_r(&e, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    if (util::run({"/bin/date", "-s", "@" + std::to_string(epoch)}, "", 5).exit_code != 0) {
        render::redirect_flash(res, settings_redirect(), "error", "Could not set the clock; nothing changed.");
        return;
    }
    // Trusted wall clock from here on. clock-synced is only the derived marker
    // (exists iff clock-source does), so the source goes first. Re-read anyway
    // (the one-shot records ntp under the same lock, so it shouldn't have
    // landed since the test above): ntp stays the source.
    // rnsd/lxmd/nomadnet need nothing from us: they run with or without a clock.
    if (sysinfo::clock_source() != "ntp")
        util::write_file_atomic("/run/clock-source", "browser\n", 0644);
    util::write_file_atomic("/run/clock-synced", "", 0644);

    std::string msg = "Clock set to " + std::string(stamp) + " UTC from your browser";
    const char* cat = "success";
    // Persist it in the RTC when one is fitted — even one that reads "not set":
    // writing it is exactly how it gets set (RNSBox's rtc-ds1307 clears a
    // DS3231's oscillator-stop flag on the write). Busybox hwclock, RTC in UTC.
    if (::access("/dev/rtc0", F_OK) == 0) {
        util::RunResult hw = util::run({"/sbin/hwclock", "-w", "-u", "-f", "/dev/rtc0"}, "", 10);
        if (hw.exit_code == 0) {
            msg += " and written to the hardware clock (RTC).";
        } else {
            std::string why = hw.timed_out ? std::string("timed out") : util::trim(hw.out.substr(0, hw.out.find('\n')));
            if (why.size() > 120) why.resize(120);
            if (why.empty()) why = "exit " + std::to_string(hw.exit_code);
            msg += ", but writing the hardware clock (RTC) failed (" + why + "): it will not survive a reboot.";
            cat = "warn";
        }
    } else {
        msg += ". No hardware clock (RTC) fitted, so it is lost at the next reboot.";
    }
    render::redirect_flash(res, settings_redirect(), cat, msg);
}

}  // namespace routes
