// main.cpp — RNSBox portal CGI. One binary at /cgi-bin/portal/*, PATH_INFO = route.
// Pages are rendered server-side (render::layout wraps a .html content block from
// PAGES_DIR); form POSTs are POST-redirect-GET with a one-shot flash. A handful of
// endpoints stay JSON for the pages' own AJAX (rnstatus, wifi scan/status,
// dashboard auto-refresh, live HaLow modem). Mutating routes are POST-only and
// every POST is same-origin checked, both here before dispatch (POST_ONLY below).
// Before either: the Host must be an IP literal or exactly "localhost"
// (host_is_literal; the dnsmasq aliases such as setup.lan only redirect to the
// IP), and the modem UI lives alone on its own port/origin (MODEM_PORT).
// Multi-mode: CGI, `nftgen` / `ctflush` CLI for the init scripts, and
// `console-broker` (the long-lived PTY behind the Console tab).
#include "http.h"
#include "dispatch.h"
#include "session.h"
#include "util.h"
#include "web.h"
#include "render.h"
#include "routes.h"
#include "nftgen.h"
#include "conntrack.h"
#include "store.h"
#include "console.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>

// ---- method policy ----
// Every state-changing route. They are POST-only: any other method gets 405
// before the handler runs (a GET used to run them with empty fields — e.g.
// /wifi/mode -> "off" — and SameSite=Lax still sends the cookie on a top-level
// cross-site GET, so a plain link was a CSRF). Keep in sync with the dispatch
// table below; everything not listed here is a read.
static const char* const POST_ONLY[] = {
    "/logout",
    "/network/wan",
    "/network/portforward/add",
    "/network/portforward/delete",
    "/network/openport/add",
    "/network/openport/delete",
    "/wifi/connect",
    "/wifi/ap",
    "/wifi/mode",
    "/wifi/txpower",
    "/wifi/net/add",
    "/wifi/net/delete",
    "/wifi/net/connect",
    "/reticulum/config",
    "/reticulum/restart",
    "/reticulum/autorestart",
    "/reticulum/rnsdwatchdog",
    "/reticulum/autoupdate",
    "/reticulum/maintenance",
    "/reticulum/checkupdate",
    "/reticulum/downloadupdate",
    "/reticulum/installupdate",
    "/reticulum/applyupdate",
    "/reticulum/slip",
    "/reticulum/rnsh",
    "/access/issue", "/access/revoke", "/access/reissue",
    "/reticulum/halow/add",
    "/reticulum/tcpclient/add",
    "/reticulum/tcpclient/update",
    "/reticulum/tcpclient/save",
    "/reticulum/tcpclient/delete",
    "/reticulum/bluetooth",
    "/reticulum/bluetooth/raise",
    "/reticulum/bluetooth/pair",
    "/reticulum/rnode",
    "/reticulum/rnode/pair",
    "/settings/password",
    "/settings/hostname",
    "/settings/time/browser",
    "/settings/ntp",
    "/settings/ntp/restart",
    "/settings/ssh",
    "/settings/usb",
    "/updates/config",
    "/updates/check",
    "/updates/apply",
    "/updates/auto",
    "/echobot/toggle",
    "/echobot/save",
    "/echobot/announce",
    "/echobot/restart",
    "/console/input",
    "/console/resize",
    "/nomadnet/input",
    "/nomadnet/resize",
};

static bool is_post_only(const std::string& p) {
    for (const char* r : POST_ONLY)
        if (p == r) return true;
    return false;
}

// ---- Host allowlist (DNS-rebinding guard) ----
// same_origin() compares Origin with Host, and a DNS-rebinding page controls both
// (evil.example re-resolved to 10.42.0.1 -> Origin and Host are both evil.example,
// and the default admin/admin gets it a session). So only an IPv4/IPv6 literal
// (no DNS involved) or exactly "localhost" (never resolved via DNS by a browser;
// SSH tunnels) is an origin. No Host = non-browser client. No other name is: the
// box has no mDNS/LLMNR responder, so rnsbox.local or a bare hostname is answered
// by whoever claims it on any network the client is on. The dnsmasq.conf portal
// aliases are redirect-only entry points (GET/HEAD -> the same path on the IP);
// every other name (rnsbox.fritz.box, a DDNS name) is refused: open the portal
// by IP then.
static std::string lc(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// IP literal (no DNS involved), exactly "localhost", or no Host at all
// (non-browser client).
static bool host_is_literal(const std::string& host_hdr) {
    if (host_hdr.empty()) return true;
    std::string h = lc(host_hdr);
    if (h[0] == '[') {                                  // IPv6 literal "[fe80::1]:80"
        size_t e = h.find(']');
        if (e == std::string::npos || e == 1) return false;
        for (size_t i = 1; i < e; ++i)
            if (!(isxdigit((unsigned char)h[i]) || h[i] == ':' || h[i] == '.')) return false;
        return true;
    }
    size_t colon = h.rfind(':');
    if (colon != std::string::npos) h.erase(colon);     // any port
    // "localhost" never goes to DNS in a browser (RFC 6761: Chromium/Firefox hardcode
    // it to loopback, Safari reads /etc/hosts), so a rebinding page can't hold that
    // origin; it is how an SSH tunnel is opened (ssh -L 8080:127.0.0.1:80 ->
    // http://localhost:8080/). Exact name only, not *.localhost: WebKit/system
    // resolvers may send those to (possibly hostile) DNS.
    if (h == "localhost") return true;
    return store::valid_ipv4(h);
}

// dnsmasq.conf address= names: redirect-only entry points, never an origin.
static bool host_is_alias(const std::string& host_hdr) {
    std::string h = lc(host_hdr);
    size_t colon = h.rfind(':');
    if (colon != std::string::npos) h.erase(colon);
    if (!h.empty() && h.back() == '.') h.pop_back();    // "setup.lan."
    static const char* const ALIASES[] = {              // keep in sync with dnsmasq.conf address=
        "rnsbox.local", "setup.rnsbox", "setup.lan", "router.rnsbox",
    };
    for (const char* a : ALIASES) if (h == a) return true;
    return false;
}

// ---- modem UI origin ----
// The proxied modem web UI is the modem's own HTML + inline scripts. Served from
// the portal's origin, any script in it (a hostile modem, or a stored XSS in data
// the modem heard over the air) would ride the admin session and pass
// same_origin() on every mutating route. So the modem routes live only on a
// second uhttpd listener (S81router: -p 0.0.0.0:8081), a separate origin: a
// modem-page POST to the portal port carries Origin http://host:8081 != Host and
// gets 403, and the browser won't let it read portal responses. The session
// cookie is not port-scoped (and is HttpOnly), so a logged-in admin stays logged
// in there. Each port serves only its own routes; a GET for the other side's is
// redirected, anything else is 404.
static const char MODEM_PORT[] = "8081";

static bool is_modem_route(const std::string& p) {
    return p == "/modem" || p == "/modem/" || p.rfind("/modem/api/", 0) == 0;
}

// "http://<host as the browser addressed it>[:port]" for a cross-port redirect.
// Host was checked to be an IP literal (or "localhost") already; still keep only
// host-name bytes so nothing odd reaches the Location header. No usable Host ->
// the address uhttpd accepted on.
static std::string origin_on_port(const http::Request& req, const char* port) {
    std::string h = req.host;
    if (!h.empty() && h[0] == '[') {
        size_t e = h.find(']');
        h = (e == std::string::npos) ? std::string() : h.substr(0, e + 1);
    } else {
        size_t c = h.find(':');
        if (c != std::string::npos) h.resize(c);
    }
    for (unsigned char c : h)
        if (!(isalnum(c) || c == '.' || c == '-' || c == '[' || c == ']' || c == ':')) { h.clear(); break; }
    if (h.empty()) {
        const char* a = getenv("SERVER_ADDR");
        h = (a && *a) ? a : "10.42.0.1";
        if (h.find(':') != std::string::npos) h = "[" + h + "]";   // IPv6 listener
    }
    return std::string("http://") + h + (port ? std::string(":") + port : std::string());
}

// Port the browser addressed, from Host ("" = scheme default).
static std::string host_port(const std::string& h) {
    size_t from = 0;
    if (!h.empty() && h[0] == '[') { from = h.find(']'); if (from == std::string::npos) return std::string(); }
    size_t c = h.find(':', from);
    return c == std::string::npos ? std::string() : h.substr(c + 1);
}

// Host as the browser addressed it vs. the port a direct hit on this listener carries
// ("" = scheme default). A loopback name (the box is never localhost/127.x to the
// browser: an SSH tunnel) or any other port (a tunnel, or an upstream NAT on another
// public port) means a cross-port redirect would land on the admin's own machine or
// the NAT's same-numbered port, not on us, and hand it the session cookie.
static bool host_is_mapped(const std::string& host_hdr, const std::string& hp, const char* direct_port) {
    if (host_hdr.empty()) return false;              // non-browser: origin_on_port uses SERVER_ADDR
    std::string h = lc(host_hdr);
    if (h[0] == '[') { size_t e = h.find(']'); h = e == std::string::npos ? h : h.substr(0, e + 1); }
    else { size_t c = h.find(':'); if (c != std::string::npos) h.resize(c); }
    if (h == "localhost" || h.rfind("127.", 0) == 0 || h == "[::1]") return true;
    if (!strcmp(direct_port, "80")) return !(hp.empty() || hp == "80");
    return hp != direct_port;
}

// A WAN port forward of tcp <wan_port> (DNAT runs before INPUT) means a browser
// on the WAN that we redirect to that port reaches the forward's target, not
// us, and hands it the session cookie (cookies aren't port-scoped).
// store::add_portforward refuses new ones; this catches forwards left from an
// older version or added by hand. Only matters for WAN-origin requests: callers
// skip it for from_lan() ones, which our DNAT never touches.
static bool tcp_forwarded(int wan_port) {
    for (const auto& f : store::read_portforwards())
        if (f.proto == "tcp" && f.wan_port == wan_port) return true;
    return false;
}

// Request from the LAN or the box itself? Our prerouting DNAT matches only
// iifname @wan_ifaces (nftgen) and keeps the source address, so for these clients a
// redirect to another port of the same Host reaches us, never a forward's target.
// The LAN is one subnet: 10.42.0.1/24 on /run/lan-iface (S30gadget_nic) —
// br-lan, which bridges the USB-C host and the WiFi hotspot's clients (one
// dnsmasq pool), or usb0 alone if the bridge couldn't be made. uhttpd listens
// on IPv4 only (S81router).
static bool from_lan(const http::Request& req) {
    const std::string& ra = req.remote_addr;
    if (!store::valid_ipv4(ra)) return false;
    return ra.rfind("127.", 0) == 0 || ra.rfind("10.42.0.", 0) == 0;
}

// ---- auth routes ----
static void r_login(const http::Request& req, http::Response& res) {
    if (req.method == "POST") {
        std::string user = util::trim(req.f("username"));
        std::string pass = req.f("password");
        std::string next = req.f("next");
        bool next_ok = web::is_local_next(next);   // local portal path only (no open redirect)
        if (session::verify_password(user, pass)) {
            // One password for the box: SSH logs in as the unix `admin` user,
            // so keep its hash in step with the portal password the operator
            // just typed. (Changing the password does the same.)
            util::run({"/usr/bin/passwd", "-a", "sha512", "admin"},
                      pass + "\n" + pass + "\n", 15);
            res.set_cookie(session::start(user));
            // default: the dashboard. base() and base()+"/" both reach it (an empty
            // PATH_INFO is read as "/"; the nav's Dashboard link uses the slash form)
            res.redirect(next_ok ? next : web::base());
            return;
        }
        ::sleep(1);  // brute-force speed bump
        std::string again = web::base() + "/login";
        if (next_ok) again += "?next=" + http::url_encode(next);   // keep the deep link across a typo
        render::redirect_flash(res, again, "error", "Invalid credentials");
        return;
    }
    std::string next = req.a("next");
    std::string h = render::page_file("login");
    h = web::replace_all(h, "__ACTION__", web::base() + "/login");
    h = web::replace_all(h, "__NEXT__", web::is_local_next(next) ? http::html_escape(next) : std::string());
    res.body = render::login_layout(req, res, h);
}

// POST-only (see POST_ONLY). Ends the session only for the holder of the live
// token (session::clear checks it) — a cookieless request just gets bounced.
static void r_logout(const http::Request& req, http::Response& res) {
    res.set_cookie(session::clear(req.cookie(session::COOKIE_NAME)));
    res.redirect(web::base() + "/login");
}

int main(int argc, char** argv) {
    // The console broker writes to a PTY and a Unix socket; a browser that
    // navigates away closes its stream, and the CGI's next stdout write must
    // fail with EPIPE instead of killing the process.
    signal(SIGPIPE, SIG_IGN);

    // `rnsbox-portal console-broker [-f] [--ws-port N] [--ws-bind ADDR]`
    // — PTY session owner (S11console).
    if (argc >= 2 && strcmp(argv[1], "console-broker") == 0) {
        bool fg = false;
        int ws_port = -1;
        const char* ws_bind = nullptr;
        for (int i = 2; i < argc; ++i) {
            if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--foreground")) {
                fg = true;
            } else if (!strcmp(argv[i], "--ws-port") && i + 1 < argc) {
                ws_port = atoi(argv[++i]);
            } else if (!strcmp(argv[i], "--ws-bind") && i + 1 < argc) {
                ws_bind = argv[++i];
            }
        }
        return console::broker_main(fg, ws_port, ws_bind);
    }
    // CLI mode: `rnsbox-portal nftgen --wan eth0 --lan br-lan` (boot path).
    if (argc >= 2 && strcmp(argv[1], "nftgen") == 0)
        return nftgen::cli_main(argc, argv);
    // `rnsbox-portal ctflush` (S60routing uplink: WAN=auto switched uplinks).
    if (argc >= 2 && strcmp(argv[1], "ctflush") == 0)
        return conntrack::flush();
    // `rnsbox-portal rnstatus-refresh` (S83mon: warm the Interfaces
    // cache after rnsd is up, so the first page load does not block on it).
    if (argc >= 2 && strcmp(argv[1], "rnstatus-refresh") == 0)
        return routes::rnstatus_prewarm() ? 0 : 1;
    // `rnsbox-portal bt-peers-refresh` (rnsbox-mon / the peers endpoint):
    // update the connected-device cache in the background.
    if (argc >= 2 && strcmp(argv[1], "bt-peers-refresh") == 0)
        return routes::bt_peers_refresh() ? 0 : 1;

    // otherwise: CGI mode (invoked by uhttpd, request in the environment).
    // The request policy and the route table live in dispatch.cpp, shared with
    // the native Windows front end (win/main_win.cpp).
    session::ensure_default_auth();
    http::Request req = http::read_request();
    http::Response res;
    dispatch::handle(req, res);
    if (!res.streamed) res.send();
    return 0;
}
