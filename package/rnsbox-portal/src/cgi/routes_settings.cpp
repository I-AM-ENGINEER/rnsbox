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
    if (src == "rns")
        return "<span class=\"badge ok\">Reticulum</span> <span class=\"muted\">stepped by rns-time over the mesh</span>";
    if (src == "rtc")
        return "<span class=\"badge ok\">RTC</span> <span class=\"muted\">read from the hardware clock at boot</span>";
    if (src == "browser")
        return "<span class=\"badge ok\">browser</span> <span class=\"muted\">set from a browser on this page</span>";
    return "<span class=\"badge warn\">none</span> <span class=\"muted\">no NTP, Reticulum time, RTC or browser time yet</span>";
}

// Network time (ordinary NTP or rns-time over the mesh) outranks the browser.
static bool clock_net_synced(const std::string& src) {
    return src == "ntp" || src == "rns";
}

// A pidfile with a live pid (used by the rns-time status line). /proc keeps
// this free of POSIX kill() so the Windows portal build shares the file.
static bool pidfile_running(const char* path) {
    std::string s = util::trim(util::read_file(path, 64));
    if (s.empty()) return false;
    char* end = nullptr;
    long pid = strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0' || pid <= 0) return false;
    return ::access(("/proc/" + std::to_string(pid)).c_str(), F_OK) == 0;
}

static std::string rnstime_status_html(const store::RnsTimeConfig& c) {
    if (!c.enabled) return "<span class=\"badge warn\">disabled</span>";
    std::string s = pidfile_running("/var/run/rnstime.pid")
        ? "<span class=\"badge ok\">client running</span>"
        : "<span class=\"badge err\">client stopped</span>";
    if (c.serve) {
        s += pidfile_running("/var/run/rnstime-server.pid")
            ? " <span class=\"badge ok\">server running</span>"
            : " <span class=\"badge err\">server stopped</span>";
        std::string dest = util::trim(util::read_file("/run/rnstime.destination", 64));
        if (!dest.empty())
            s += " <span class=\"muted\">destination " + E(dest) + "</span>";
    } else {
        s += " <span class=\"muted\">serving off</span>";
    }
    return s;
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

// ---- SSH card fragments ----

static bool ssh_running() {
    util::RunResult pid = util::run({"/bin/pidof", "sshd"}, "", 5, 4096);
    return pid.exit_code == 0 && !util::trim(pid.out).empty();
}

// Status line: running (sshd alive) / disabled / stopped (enabled=yes but not
// running — e.g. sshd crashed or was killed).
static std::string ssh_status_html() {
    store::SshConfig c = store::read_ssh();
    if (ssh_running()) return "<span class=\"badge ok\">running</span>";
    if (!c.enabled) return "<span class=\"badge err\">disabled</span>";
    return "<span class=\"badge warn\">stopped</span>";
}

// ---- USB port role card ----
// The switch exists only on OTG-capable boards. Capability and the live role
// come from /run/usbmode.state, which S07usbmode writes at boot and after
// every apply; when the file is missing we run the script's status once (it
// (re)writes the file), and if even that fails, detect by hand the same way
// the script does: CVITEK's /proc/cviusb/otg_role, else a Pi Zero 2 W model,
// with an optional /etc/rnsbox/board.conf usb_otg=yes|no pin winning.
struct UsbState {
    bool capable = false;
    std::string mode = "device";   // configured (usb.conf)
    std::string active;            // what the controller does now
    std::string platform;          // cvi | rpi | other
};

static std::string usb_pin() {
    return util::trim(util::conf_get("/etc/rnsbox/board.conf", "usb_otg"));
}

static bool usb_capable_fallback() {
    std::string pin = usb_pin();
    if (pin == "yes") return true;
    if (pin == "no") return false;
    // The CVITEK dwc2 glue always provides the proc role switch.
    if (!util::read_file("/proc/cviusb/otg_role", 16).empty()) return true;
    std::string model = util::read_file("/proc/device-tree/model", 128);
    return model.find("Zero 2") != std::string::npos;
}

static UsbState usb_state() {
    UsbState s;
    s.mode = store::read_usb().mode;
    std::string cap = util::trim(util::conf_get(store::USB_STATE_FILE, "capable"));
    if (cap.empty()) {
        util::run({"/etc/init.d/S07usbmode", "status"}, "", 5);
        cap = util::trim(util::conf_get(store::USB_STATE_FILE, "capable"));
    }
    s.capable = (cap == "yes") || (cap.empty() && usb_capable_fallback());
    s.active = util::trim(util::conf_get(store::USB_STATE_FILE, "active"));
    // S07usbmode writes the state file at boot, before the gadget NIC exists,
    // so "active" is stale for the rest of the session. Ask it again once the
    // link is up; usb0 with carrier is ground truth for device mode.
    if (s.active.empty() || s.active == "unknown") {
        util::run({"/etc/init.d/S07usbmode", "status"}, "", 5);
        s.active = util::trim(util::conf_get(store::USB_STATE_FILE, "active"));
    }
    if (s.active.empty() || s.active == "unknown") {
        if (util::trim(util::read_file("/sys/class/net/usb0/carrier", 8)) == "1")
            s.active = "device";
    }
    if (s.active.empty()) s.active = s.mode;
    s.platform = util::trim(util::conf_get(store::USB_STATE_FILE, "platform"));
    return s;
}

static std::string usb_mode_badge(const std::string& role) {
    if (role == "host")
        return "<span class=\"badge warn\">host</span> <span class=\"muted\">USB devices "
               "(OTG adapter); no USB-NCM link</span>";
    if (role == "device")
        return "<span class=\"badge ok\">device</span> <span class=\"muted\">CDC-NCM gadget "
               "&mdash; portal at 10.42.0.1 over USB-NCM</span>";
    return "<span class=\"badge\">unknown</span>";
}

static std::string usb_section_html(const UsbState& s) {
    if (!s.capable) return std::string();   // Pi 3/4/5, Orange Pi R1, ...: no switch at all
    bool host = s.mode == "host";
    std::string note;
    if (s.platform == "rpi")
        note = "Takes effect at the next reboot (the driver reads the role once at boot). "
               "In host mode the USB network link is gone &mdash; make sure the WiFi hotspot "
               "or another access path works first.";
    else
        note = "Applies as soon as you save (the controller switches roles at runtime). "
               "Switching to host cuts the USB network link &mdash; keep the WiFi hotspot or "
               "SSH reachable first.";
    std::string h;
    h += "<section class=\"card\">\n  <h3>USB port role</h3>\n";
    h += "  <table class=\"kv\">\n";
    h += "    <tr><th>Running now</th><td>" + usb_mode_badge(s.active) + "</td></tr>\n";
    h += "    <tr><th>Configured</th><td><strong>" + std::string(host ? "host" : "device (gadget)") + "</strong>" +
         std::string(s.active == s.mode ? "" : " <span class=\"badge warn\">pending</span>") +
         "</td></tr>\n";
    h += "  </table>\n";
    h += "  <form method=\"post\" action=\"" + web::base() + "/settings/usb\">\n";
    h += "    <label><input type=\"radio\" name=\"mode\" value=\"device\"" +
         std::string(host ? "" : " checked") + "> Device (gadget) &mdash; the port is the box's "
         "USB-NCM: <code>10.42.0.1</code> over the cable</label>\n";
    h += "    <label><input type=\"radio\" name=\"mode\" value=\"host\"" +
         std::string(host ? " checked" : "") + "> Host &mdash; the port drives USB devices "
         "(flash drives, Huawei modems, RNode) through an OTG adapter; the USB-NCM link goes away</label>\n";
    h += "    <button type=\"submit\" class=\"btn-primary\">Save &amp; apply</button>\n  </form>\n";
    h += "  <p class=\"muted\">" + note + "</p>\n</section>\n";
    return h;
}

// ---- UART card ----
// The controllers are enabled in the DTS; the pin names below come from the
// vendor header tables. The function is the operator's assignment: SLIP picks
// up the UART marked SLIP, RNode is configured in its own card, and the kernel
// console is fixed by the boot command line.
struct UartRow { std::string key, dev, pins; };

static std::string board_model() {
    std::string s = util::read_file("/proc/device-tree/model", 512);
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return util::trim(s);
}

static std::vector<UartRow> uart_rows() {
    const std::string m = board_model();
    std::vector<UartRow> v;
    if (m.find("Duo Module 01") != std::string::npos ||
        m.find("Duo S") != std::string::npos) {
        v = {
            {"uart0", "ttyS0", "XGPIOA16 TX / XGPIOA17 RX"},
            {"uart1", "ttyS1", "XGPIOA28 TX / XGPIOA29 RX"},
            {"uart2", "ttyS2", "XGPIOB11 TX / XGPIOB12 RX"},
            {"uart3", "ttyS3", "XGPIOB1 TX / XGPIOB2 RX"},
            {"uart4", "ttyS4", "not pinned out on this header"},
        };
    } else if (m.find("256M") != std::string::npos) {
        v = {
            {"uart0", "ttyS0", "XGPIOA16 TX / XGPIOA17 RX"},
            {"uart1", "ttyS1", "XGPIOA28 TX / XGPIOA29 RX"},
            {"uart2", "ttyS2", "PWR_GPIO19 TX / PWR_GPIO20 RX"},
            {"uart3", "ttyS3", "PWR_GPIO19 TX / PWR_GPIO20 RX (shared)"},
            {"uart4", "ttyS4", "not pinned out on this header"},
        };
    } else if (m.find("Milk-V Duo") != std::string::npos) {
        v = {
            {"uart0", "ttyS0", "XGPIOA16 TX / XGPIOA17 RX"},
            {"uart1", "ttyS1", "XGPIOA28 TX / XGPIOA29 RX"},
            {"uart2", "ttyS2", "PWR_GPIO19 TX / PWR_GPIO20 RX"},
            {"uart3", "ttyS3", "PWR_GPIO19 TX / PWR_GPIO20 RX (shared)"},
            {"uart4", "ttyS4", "PWR_GPIO26 TX / PWR_GPIO25 RX"},
        };
    } else if (m.find("Raspberry Pi") != std::string::npos) {
        // The 40-pin header has exactly one UART, but which controller sits
        // on it depends on the model and the overlays: Zero 2 W runs
        // miniuart-bt, so the PL011 (ttyAMA0) is on GPIO14/15; Pi 3/4
        // default to the mini-UART (ttyS0) there; Pi 5 uses ttyAMA0 and
        // keeps its debug UART (ttyAMA10) off the header. /dev/serial0 is
        // the firmware alias that follows the header — use it, not a
        // controller node, or the same card stops working on another model.
        v = {{"serial0", "serial0", "GPIO14 TX / GPIO15 RX (40-pin header)"}};
        if (::access("/dev/ttyAMA10", F_OK) == 0)
            v.push_back({"ttyAMA10", "ttyAMA10",
                         "debug UART (Pi 5 console; not on the header)"});
    } else if (m.find("Orange Pi R1") != std::string::npos) {
        v = {{"uart0", "ttyS0", "UART0 header pins (PA4 TX / PA5 RX)"}};
    } else {
        for (int i = 0; i < 4; ++i) {
            std::string d = "ttyS" + std::to_string(i);
            if (::access(("/dev/" + d).c_str(), F_OK) == 0)
                v.push_back({"uart" + std::to_string(i), d, "pins unknown on this board"});
        }
        if (::access("/dev/ttyAMA0", F_OK) == 0)
            v.push_back({"uart0", "ttyAMA0", "pins unknown on this board"});
    }
    return v;
}

static std::string kernel_console() {
    const std::string cmd = util::read_file("/proc/cmdline", 4096);
    size_t p = 0;
    while ((p = cmd.find("console=", p)) != std::string::npos) {
        size_t b = p + 8;
        size_t e = cmd.find_first_of(" \t\n", b);
        std::string t = cmd.substr(b, e == std::string::npos ? std::string::npos : e - b);
        size_t comma = t.find(',');
        if (comma != std::string::npos) t.resize(comma);
        if (t.rfind("tty", 0) == 0) return t;
        p = b;
    }
    return "";
}

static std::string uart_conf_value(const std::string& key) {
    std::string data = util::read_file("/etc/rnsbox/uart.conf", 16384);
    size_t i = 0;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        std::string ln = util::trim(data.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? data.size() : nl + 1;
        size_t eq = ln.find('=');
        if (eq != std::string::npos && util::trim(ln.substr(0, eq)) == key)
            return util::trim(ln.substr(eq + 1));
    }
    return "";
}

static bool uart_func_ok(const std::string& f) {
    return f == "console" || f == "slip" || f == "rnode" ||
           f == "custom" || f == "off";
}

static std::string uart_select(const std::string& key, const std::string& cur) {
    static const char* opts[][2] = {
        {"console", "Console"}, {"slip", "SLIP / HaLow"}, {"rnode", "RNode"},
        {"custom", "Custom"}, {"off", "Off"},
    };
    std::string s = "<select name=\"" + E(key) + "\">";
    for (auto& o : opts)
        s += "<option value=\"" + std::string(o[0]) + "\"" +
             (cur == o[0] ? " selected" : "") + ">" + o[1] + "</option>";
    return s + "</select>";
}

static std::string uart_section_html() {
    std::string base = web::base();
    std::string model = board_model();
    std::string cons = kernel_console();
    store::SlipConfig slip = store::read_slip();
    store::RnodeConfig rn = store::read_rnode();
    std::vector<UartRow> rows = uart_rows();

    std::string s = "<section class=\"card\">\n  <h3>UART</h3>\n";
    s += "  <p class=\"muted\" style=\"max-width:none\">Board: <strong>" + E(model) +
         "</strong>. ";
    if (!cons.empty())
        s += "The kernel console is on <code>/dev/" + E(cons) + "</code>. Before assigning "
             "that UART to SLIP, RNode or a custom service, remove <code>console=" +
             E(cons) + "</code> from the kernel command line in the boot files &mdash; "
             "otherwise the console and the service write to the same port.";
    else
        s += "No serial console is enabled in the kernel command line.";
    s += "</p>\n  <form method=\"post\" action=\"" + base + "/settings/uart\">\n";
    s += "  <table class=\"data\">\n    <thead><tr><th>UART</th><th>Pins</th>"
         "<th>Device</th><th>Function</th></tr></thead>\n    <tbody>\n";
    for (const auto& r : rows) {
        std::string fn = uart_conf_value(r.key);
        if (!uart_func_ok(fn)) {
            if (r.dev == cons) fn = "console";
            else if (("/dev/" + r.dev) == slip.device) fn = "slip";
            else if (rn.transport == "usb" && rn.device == "/dev/" + r.dev) fn = "rnode";
            else fn = "off";
        }
        bool present = ::access(("/dev/" + r.dev).c_str(), F_OK) == 0;
        bool conflict = !cons.empty() && r.dev == cons && fn != "console";
        std::string label = r.key.rfind("uart", 0) == 0
            ? "UART" + r.key.substr(4) : r.key;
        s += "      <tr><td>" + E(label) + "</td><td class=\"mono\">" + E(r.pins) +
             "</td><td class=\"mono\">/dev/" + E(r.dev) + "</td><td>" +
             uart_select(r.key, fn);
        if (!present) s += " <span class=\"badge err\">no device</span>";
        if (conflict) s += " <span class=\"badge err\" title=\"The kernel console still uses this UART\">console</span>";
        s += "</td></tr>\n";
    }
    s += "    </tbody>\n  </table>\n";
    s += "  <div class=\"form-actions\"><span class=\"muted\">SLIP uses the UART marked SLIP when enabled on the Reticulum tab; "
         "RNode is configured in its own card.</span>"
         "<button type=\"submit\" class=\"btn-primary\">Save UARTs</button></div>\n";
    s += "  </form>\n</section>\n";
    return s;
}

static void set_conf_key(const std::string& path, const std::string& key, const std::string& val) {
    std::string data = util::read_file(path);
    std::vector<std::string> lines;
    bool found = false;
    size_t i = 0;
    while (i < data.size()) {
        size_t nl = data.find('\n', i);
        std::string ln = data.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? data.size() : nl + 1;
        std::string t = util::trim(ln);
        if (t.compare(0, key.size() + 1, key + "=") == 0) {
            ln = key + "=" + val;
            found = true;
        }
        lines.push_back(ln);
    }
    if (!found) lines.push_back(key + "=" + val);
    std::string out;
    for (const auto& l : lines) out += l + "\n";
    util::write_file_atomic(path, out, 0644);
}

void settings_uart(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::vector<UartRow> rows = uart_rows();
    std::string cons = kernel_console();
    std::string out = "# UART function assignment (Settings -> UART).\n";
    std::string slip_dev;
    bool conflict = false;
    for (const auto& r : rows) {
        std::string v = util::trim(req.f(r.key.c_str()));
        if (!uart_func_ok(v)) v = "off";
        out += r.key + "=" + v + "\n";
        if (v == "slip" && slip_dev.empty()) slip_dev = "/dev/" + r.dev;
        if (!cons.empty() && r.dev == cons && v != "console") conflict = true;
    }
    util::write_file_atomic("/etc/rnsbox/uart.conf", out, 0644);
    if (!slip_dev.empty()) set_conf_key("/etc/rnsbox/slip.conf", "device", slip_dev);
    std::string msg = "UART assignment saved.";
    if (!slip_dev.empty()) msg += " SLIP device set to " + slip_dev + ".";
    if (conflict)
        msg += " Warning: the kernel console is still on /dev/" + cons +
               "; remove console=" + cons + " from the boot command line before using it.";
    render::redirect_flash(res, settings_redirect(), conflict ? "warn" : "success", msg);
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

    store::RnsTimeConfig rt = store::read_rnstime();
    std::string rt_servers = rt.servers;
    for (char& c : rt_servers) if (c == ',') c = '\n';

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
    // Network time (NTP or rns-time) outranks a browser (see time_browser),
    // so the button is moot once either has synced; the POST handler enforces
    // it regardless.
    h = R(std::move(h), "__BROWSER_BTN_ATTR__", clock_net_synced(src) ? " disabled" : "");
    h = R(std::move(h), "__BROWSER_NOTE__", clock_net_synced(src)
          ? "<span class=\"muted\">Not needed: " +
            std::string(src == "rns" ? "Reticulum time (rns-time)" : "NTP") +
            " has synced the clock, and it takes precedence.</span>"
          : "");
    h = R(std::move(h), "__NTP_SERVERS__", E(ntp_joined));
    h = R(std::move(h), "__RNSTIME_ENABLED_ATTR__", rt.enabled ? " checked" : "");
    h = R(std::move(h), "__RNSTIME_SERVE_ATTR__", rt.serve ? " checked" : "");
    h = R(std::move(h), "__RNSTIME_SERVERS__", E(rt_servers));
    h = R(std::move(h), "__RNSTIME_UPSTREAM__", E(rt.upstream));
    h = R(std::move(h), "__RNSTIME_INTERVAL__", std::to_string(rt.interval));
    h = R(std::move(h), "__RNSTIME_THRESHOLD__", E(rt.step_threshold));
    h = R(std::move(h), "__RNSTIME_STATUS__", rnstime_status_html(rt));
    h = R(std::move(h), "__HOSTNAME__", E(host));
    h = R(std::move(h), "__WAN__", E(wan));
    // USB role card (empty unless the board is OTG-capable — see usb_state()).
    h = R(std::move(h), "__USB_SECTION__", usb_section_html(usb_state()));
    h = R(std::move(h), "__UART_SECTION__", uart_section_html());

    // SSH card. The checkbox reflects reality: sshd may be running from an
    // unconditional init even when the gate file was missing.
    store::SshConfig ssh = store::read_ssh();
    h = R(std::move(h), "__SSH_STATUS__", ssh_status_html());
    h = R(std::move(h), "__SSH_ENABLED_ATTR__",
          (ssh.enabled || ssh_running()) ? " checked" : "");

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
        // The TTY console logs in as the unix `admin` user; keep its password
        // in step with the portal password. The factory admin/admin value is
        // seeded by S10consoleuser on first boot.
        util::run({"/usr/bin/passwd", "-a", "sha512", "admin"},
                  neu + "\n" + neu + "\n", 15);
        // sec: a session issued under the OLD password must not survive the
        // change (whoever held one — e.g. signed in with the factory default
        // just before the operator changed it — would stay logged in for up
        // to the 24 h TTL). Drop every session; the next request re-auths.
        session::revoke_all();
        res.set_cookie(session::clear(req.cookie(session::COOKIE_NAME)));   // expire this browser's cookie too
        render::redirect_flash(res, web::base() + "/login", "success",
            "Password changed — sign in again with the new password.");
    }
}

void ssh_set(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    bool enable = req.f("enabled") == "on";
    std::string err;
    if (!store::write_ssh(enable, err)) {
        render::redirect_flash(res, settings_redirect(), "error",
            "Could not write /etc/rnsbox/ssh.conf; SSH not changed.");
        return;
    }
    // The gate script starts or stops sshd; `restart` also covers the
    // boot-time argument convention.
    web::run_init("S50sshd", "restart");
    render::redirect_flash(res, settings_redirect(), "success",
        enable ? "SSH enabled: log in as admin with the web admin password."
               : "SSH disabled: sshd stopped and stays off across reboots.");
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

void rnstime_set(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::RnsTimeConfig c = store::read_rnstime();

    std::string raw = req.f("servers");
    for (unsigned char ch : raw) {
        if ((ch < 0x20 && ch != '\r' && ch != '\n' && ch != '\t') || ch == 0x7f) {
            render::redirect_flash(res, settings_redirect(), "error",
                "Server destination hashes must not contain control characters; nothing was saved.");
            return;
        }
    }
    size_t tokens = 0;
    for (size_t i = 0; i < raw.size();) {
        while (i < raw.size() && (isspace((unsigned char)raw[i]) ||
                                  raw[i] == ',' || raw[i] == ';')) ++i;
        size_t j = i;
        while (j < raw.size() && !(isspace((unsigned char)raw[j]) ||
                                   raw[j] == ',' || raw[j] == ';')) ++j;
        if (j > i) ++tokens;
        i = j;
    }
    std::vector<std::string> kept = store::clean_rnstime_servers(raw);
    std::string joined;
    for (size_t i = 0; i < kept.size(); ++i) { if (i) joined += ","; joined += kept[i]; }

    c.enabled = req.f("enabled") == "on";
    c.serve = req.f("serve") == "on";
    c.servers = joined;
    c.upstream = util::trim(req.f("upstream"));

    std::string iv = util::trim(req.f("interval"));
    if (!iv.empty()) {
        bool digits = true;
        for (char ch : iv) if (!isdigit((unsigned char)ch)) digits = false;
        long n = digits ? atol(iv.c_str()) : 0;
        if (!digits || n < 60 || n > 86400) {
            render::redirect_flash(res, settings_redirect(), "error",
                "Interval must be a whole number of seconds between 60 and 86400; nothing was saved.");
            return;
        }
        c.interval = (int)n;
    }
    std::string th = util::trim(req.f("step_threshold"));
    if (!th.empty()) c.step_threshold = th;

    std::string err;
    if (!store::write_rnstime(c, err)) {
        render::redirect_flash(res, settings_redirect(), "error",
            "rns-time not saved: " + err + ".");
        return;
    }

    // Restart always: it also starts the optional server and picks up a new
    // server list. Disabling stops both loops.
    web::run_init("S88rnstime", c.enabled ? "restart" : "stop");

    std::string msg = "Saved " + std::to_string(kept.size()) + " rns-time server" +
                      (kept.size() == 1 ? "" : "s") + ".";
    if (tokens > kept.size()) {
        size_t dropped = tokens - kept.size();
        msg += " " + std::to_string(dropped) + " invalid entr" +
               (dropped == 1 ? "y" : "ies") + " ignored (a destination hash is 32 hex characters).";
    }
    msg += c.enabled ? " rns-time restarted." : " rns-time stopped.";
    render::redirect_flash(res, settings_redirect(), "success", msg);
}

void rnstime_sync(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::RnsTimeConfig c = store::read_rnstime();
    if (!c.enabled) {
        render::redirect_flash(res, settings_redirect(), "error",
            "rns-time is disabled; enable it first.");
        return;
    }
    if (store::clean_rnstime_servers(c.servers).empty()) {
        render::redirect_flash(res, settings_redirect(), "error",
            "Add at least one rns-time server destination hash first.");
        return;
    }
    int rc = util::run({"/etc/init.d/S88rnstime", "sync"}, "", 10).exit_code;
    if (rc != 0) {
        render::redirect_flash(res, settings_redirect(), "error",
            "Could not start the sync (exit " + std::to_string(rc) +
            "); see /var/log/rns-time.log.");
        return;
    }
    render::redirect_flash(res, settings_redirect(), "success",
        "Sync started in the background: reload in a few seconds to see the clock source "
        "(log: /var/log/rns-time.log).");
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
    // /run/clock-source precedence is ntp/rns > browser > rtc: once network
    // time (ordinary NTP or rns-time over the mesh) has stepped the clock, a
    // browser time (at best off by the page's age) would only fight it — and
    // would be written into the RTC as well. Tested under the lock, so a
    // one-shot that has just finished is seen.
    std::string src_now = sysinfo::clock_source();
    if (clock_net_synced(src_now)) {
        render::redirect_flash(res, settings_redirect(), "error",
            src_now == "rns"
                ? "The clock is already synced by Reticulum time (rns-time), which takes precedence; browser time not applied."
                : "The clock is already synced by NTP, which takes precedence; browser time not applied.");
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
    // (a one-shot records ntp/rns under the same lock, so it shouldn't have
    // landed since the test above): network time stays the source.
    // rnsd/lxmd/nomadnet need nothing from us: they run with or without a clock.
    if (!clock_net_synced(sysinfo::clock_source()))
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

void usb_mode_set(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    // Whitelist the value; anything else (or a missing field) refuses the save.
    std::string mode = req.f("mode");
    if (mode != "device" && mode != "host") {
        render::redirect_flash(res, settings_redirect(), "error", "Invalid USB mode.");
        return;
    }
    UsbState s = usb_state();
    if (!s.capable) {
        render::redirect_flash(res, settings_redirect(), "error",
            "This board has no OTG USB port; the USB mode is fixed to device.");
        return;
    }
    store::UsbConfig c;
    c.mode = mode;
    std::string err;
    if (!store::write_usb(c, err)) {
        render::redirect_flash(res, settings_redirect(), "error", "Could not save the USB mode (" + err + ").");
        return;
    }
    // S07usbmode live: syncs the boot artifacts (cvi: /boot/usb.host|usb.dev,
    // rpi: flags + config.txt dr_mode) and on CVITEK applies the role right
    // away via S08usbdev. Stdout carries one verdict line — "applied <msg>" |
    // "reboot <msg>" | "error <msg>"; scan past any other chatter to find it.
    // 30 s: the device-mode path waits for the UDC.
    util::RunResult r = util::run({"/etc/init.d/S07usbmode", "live"}, "", 30);
    std::string verdict, vmsg;
    if (!r.timed_out) {
        std::string out = util::trim(r.out);
        size_t i = 0;
        while (i < out.size() && verdict.empty()) {
            size_t e = out.find('\n', i);
            if (e == std::string::npos) e = out.size();
            std::string ln = util::trim(out.substr(i, e - i));
            for (const char* v : {"applied", "reboot", "error"}) {
                if (ln.rfind(v, 0) == 0) {
                    verdict = v;
                    vmsg = ln.size() > verdict.size() + 1 ? ln.substr(verdict.size() + 1) : std::string();
                    break;
                }
            }
            i = e + 1;
        }
    }
    if (verdict == "applied") {
        render::redirect_flash(res, settings_redirect(), "success",
            "USB mode saved and applied:" + (vmsg.empty() ? std::string(" mode: ") : " " + vmsg + ";") +
            " mode: " + mode + ".");
    } else if (verdict == "reboot") {
        render::redirect_flash(res, settings_redirect(), "warn",
            "USB mode saved (" + mode + "). " +
            (vmsg.empty() ? std::string("Reboot to apply.") : vmsg));
    } else {
        std::string why = r.timed_out ? std::string("the apply timed out")
                                      : (verdict == "error" && !vmsg.empty() ? vmsg
                                         : std::string("no verdict from S07usbmode"));
        render::redirect_flash(res, settings_redirect(), "warn",
            "USB mode saved (" + mode + "), but applying it now failed (" + why +
            "); it will apply at the next reboot.");
    }
}

}  // namespace routes
