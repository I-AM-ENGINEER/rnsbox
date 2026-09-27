// main.cpp — RNSBox portal CGI. One binary at /cgi-bin/portal/*, PATH_INFO = route.
// Pages are rendered server-side (render::layout wraps a .html content block from
// PAGES_DIR); form POSTs are POST-redirect-GET with a one-shot flash. A handful of
// endpoints stay JSON for the pages' own AJAX (rnstatus, wifi scan/status,
// dashboard auto-refresh, live HaLow modem). Mutating routes are POST-only and
// every POST is same-origin checked, both here before dispatch (POST_ONLY below).
// Before either: the Host must be an IP literal or exactly "localhost"
// (host_is_literal; the dnsmasq aliases such as setup.lan only redirect to the
// IP), and the modem UI lives alone on its own port/origin (MODEM_PORT).
// Dual-mode: `nftgen` / `ctflush` CLI for the init scripts.
#include "http.h"
#include "session.h"
#include "util.h"
#include "web.h"
#include "render.h"
#include "routes.h"
#include "nftgen.h"
#include "conntrack.h"
#include "store.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    "/reticulum/config",
    "/reticulum/restart",
    "/reticulum/autorestart",
    "/reticulum/rnsdwatchdog",
    "/reticulum/autoupdate",
    "/reticulum/checkupdate",
    "/reticulum/applyupdate",
    "/reticulum/slip",
    "/reticulum/rnsh",
    "/reticulum/halow/add",
    "/settings/password",
    "/settings/hostname",
    "/settings/time/browser",
    "/settings/ntp",
    "/settings/ntp/restart",
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
    // CLI mode: `rnsbox-portal nftgen --wan eth0 --lan br-lan` (boot path).
    if (argc >= 2 && strcmp(argv[1], "nftgen") == 0)
        return nftgen::cli_main(argc, argv);
    // `rnsbox-portal ctflush` (S60routing uplink: WAN=auto switched uplinks).
    if (argc >= 2 && strcmp(argv[1], "ctflush") == 0)
        return conntrack::flush();

    // otherwise: CGI mode (invoked by uhttpd, request in the environment)
    session::ensure_default_auth();
    http::Request req = http::read_request();
    http::Response res;

    if (req.too_large) {
        res.status = "413 Payload Too Large";
        res.content_type = "application/json";
        res.body = "{\"error\":\"request too large\"}";
        res.send();
        return 0;
    }

    // Host not an IP literal / "localhost" (DNS rebinding): refuse everything, reads included.
    if (!host_is_literal(req.host)) {
        if ((req.method == "GET" || req.method == "HEAD") && host_is_alias(req.host)) {
            // Friendly name: bounce to the IP origin. Never serve content or set
            // a cookie on a name. Safe even if an attacker claims the name: for
            // a rebinding page this is a cross-origin redirect (unreadable, and
            // no SameSite=Lax cookie for the IP on a cross-site subresource).
            const char* a = getenv("SERVER_ADDR");
            std::string ip = (a && *a) ? a : "10.42.0.1";
            if (ip.find(':') != std::string::npos) ip = "[" + ip + "]";   // IPv6 listener
            const char* sp = getenv("SERVER_PORT");
            std::string port = (sp && *sp && strcmp(sp, "80") != 0) ? std::string(":") + sp : std::string();
            std::string to = "http://" + ip + port + web::base() + req.path;
            if (!req.query.empty()) to += "?" + req.query;
            res.redirect(to);
        } else {
            res.status = "403 Forbidden";
            res.content_type = "text/plain";
            res.body = "Unknown host name. Open the portal by IP: http://10.42.0.1/ "
                       "(through an SSH tunnel: http://127.0.0.1:<local port>/).\n";
        }
        res.send();
        return 0;
    }

    const std::string& p = req.path;

    // Port gate: modem routes only on MODEM_PORT, everything else only off it.
    // Must run before the 405 + same-origin gates: a POST on :8081 to e.g.
    // /reticulum/config would pass same_origin() against its own Host.
    const char* sp = getenv("SERVER_PORT");               // uhttpd proc.c: local port, decimal
    bool on_modem_port = sp && strcmp(sp, MODEM_PORT) == 0;
    if (on_modem_port != is_modem_route(p)) {
        const std::string hp = host_port(req.host);
        if (req.method == "GET" && on_modem_port) {         // e.g. require_auth's /login redirect -> portal port
            if ((!req.host.empty() && (hp.empty() || hp == "80")) || (!from_lan(req) && tcp_forwarded(80)) ||
                host_is_mapped(req.host, hp, MODEM_PORT)) {
                // The browser already asked for port 80 but a WAN port forward
                // delivered it to :8081; redirecting again would loop forever.
                // (No Host at all: origin_on_port falls back to SERVER_ADDR.)
                // Or WAN tcp 80 is forwarded elsewhere: the redirect would hand
                // that host the session cookie (see tcp_forwarded). Or the Host
                // is a tunnel/mapping (host_is_mapped): its :80 isn't ours.
                res.status = "404 Not Found";
                res.content_type = "text/plain";
                res.body = "Port 8081 only serves the HaLow modem page; the portal is on port 80. "
                           "From the WAN, open tcp 80 (Network -> Open ports) and remove any WAN tcp 80 port forward. "
                           "Through an SSH tunnel, open the portal on your tunnel for the box's port 80 "
                           "(e.g. http://127.0.0.1:<local port>/).\n";
            } else {
                std::string to = origin_on_port(req, nullptr) + web::base() + p;
                if (!req.query.empty()) to += "?" + req.query;
                res.redirect(to);
            }
        } else if (req.method == "GET" && (p == "/modem" || p == "/modem/")) {
            if (hp == MODEM_PORT) {
                // The browser already asked for :8081 but a port forward (ours,
                // left from an older version, or the upstream router's) delivered
                // it to :80; redirecting again would loop forever. WAN tcp 8081
                // can't be port-forwarded (store::add_portforward reserves it), and
                // our DNAT runs before the INPUT accept, so an existing one must go.
                res.status = "404 Not Found";
                res.content_type = "text/plain";
                res.body = std::string("The HaLow modem page is served on port 8081. From the WAN, ") +
                           (tcp_forwarded(atoi(MODEM_PORT))
                                ? "delete the WAN tcp 8081 port forward (Network -> Port forwards), then open "
                                : "open ") +
                           "tcp 8081 or tcp 80 (Network -> Open ports); a port forward to port 80 can't carry it.\n";
            } else if (!from_lan(req) && tcp_forwarded(atoi(MODEM_PORT))) {
                res.status = "404 Not Found";
                res.content_type = "text/plain";
                res.body = "WAN tcp 8081 is port-forwarded (the portal reserves it for the modem page); "
                           "remove that forward (Network -> Port forwards) to use the modem page.\n";
            } else if (host_is_mapped(req.host, hp, "80")) {
                res.content_type = "text/html; charset=utf-8";   // 200: let the user choose where the cookie goes
                std::string url = http::html_escape(origin_on_port(req, MODEM_PORT) + web::base() + "/modem/");
                res.body = "<!doctype html><title>HaLow modem page</title><p>The HaLow modem page is served on "
                           "port 8081 of the box, which this address (an SSH tunnel or port mapping) doesn't reach. "
                           "Through SSH, also forward it: <code>ssh -L 8081:127.0.0.1:8081 root@&lt;box&gt;</code>, "
                           "then open <a href=\"" + url + "\">" + url + "</a> (adjust the port if you mapped it elsewhere). "
                           "From the WAN, open tcp 8081 (Network -&gt; Open ports) on the address that reaches the box's port 8081.</p>\n";
            } else {
                res.redirect(origin_on_port(req, MODEM_PORT) + web::base() + "/modem/");
            }
        } else {
            res.status = "404 Not Found";
            res.content_type = "application/json";
            res.body = "{\"error\":\"not found\"}";
        }
        res.send();
        return 0;
    }

    // Mutating route, wrong method: refuse before any handler runs.
    if (is_post_only(p) && req.method != "POST") {
        res.status = "405 Method Not Allowed";
        res.headers.push_back("Allow: POST");
        res.content_type = "application/json";
        res.body = "{\"error\":\"method not allowed\"}";
        res.send();
        return 0;
    }

    // CSRF: every request that may change state (POST incl. /login and the
    // /modem/api/* proxy; also any other non-GET/HEAD method) must come from
    // the portal's own origin. See http::same_origin for the exact rule. Host
    // was checked to be an IP literal or "localhost" above (host_is_literal):
    // Origin == Host alone is satisfied by a DNS-rebinding page.
    if (req.method != "GET" && req.method != "HEAD" && !http::same_origin(req)) {
        res.status = "403 Forbidden";
        res.content_type = "application/json";
        res.body = "{\"error\":\"cross-origin request refused\"}";
        res.send();
        return 0;
    }

    if      (p == "/login")                      r_login(req, res);
    else if (p == "/logout")                     r_logout(req, res);
    else if (p == "/")                           routes::dashboard_page(req, res);
    else if (p == "/dashboard/data")             routes::dashboard_data(req, res);
    else if (p == "/network")                    routes::network_page(req, res);
    else if (p == "/network/wan")                routes::wan_set(req, res);
    else if (p == "/network/portforward/add")    routes::pf_add(req, res);
    else if (p == "/network/portforward/delete") routes::pf_delete(req, res);
    else if (p == "/network/openport/add")       routes::op_add(req, res);
    else if (p == "/network/openport/delete")    routes::op_delete(req, res);
    else if (p == "/wifi")                       routes::wifi_page(req, res);
    else if (p == "/wifi/scan")                  routes::wifi_scan(req, res);
    else if (p == "/wifi/status")                routes::wifi_status(req, res);
    else if (p == "/wifi/connect")               routes::wifi_connect(req, res);
    else if (p == "/wifi/ap")                    routes::wifi_ap(req, res);
    else if (p == "/wifi/mode")                  routes::wifi_mode(req, res);
    else if (p == "/wifi/txpower")               routes::wifi_txpower(req, res);
    else if (p == "/reticulum")                  routes::reticulum_page(req, res);
    else if (p == "/reticulum/status")           routes::reticulum_status(req, res);
    else if (p == "/reticulum/halow")            routes::reticulum_halow(req, res);
    else if (p == "/reticulum/config")           routes::reticulum_config_save(req, res);
    else if (p == "/reticulum/restart")          routes::reticulum_restart(req, res);
    else if (p == "/reticulum/autorestart")      routes::reticulum_autorestart(req, res);
    else if (p == "/reticulum/rnsdwatchdog")     routes::reticulum_rnsdwatchdog(req, res);
    else if (p == "/reticulum/autoupdate")       routes::reticulum_autoupdate(req, res);
    else if (p == "/reticulum/checkupdate")      routes::reticulum_checkupdate(req, res);
    else if (p == "/reticulum/applyupdate")      routes::reticulum_applyupdate(req, res);
    else if (p == "/reticulum/slip")             routes::reticulum_slip(req, res);
    else if (p == "/reticulum/rnsh")             routes::reticulum_rnsh_save(req, res);
    else if (p == "/reticulum/halow/add")        routes::reticulum_halow_add(req, res);
    else if (p == "/settings")                   routes::settings_page(req, res);
    else if (p == "/settings/password")          routes::settings_password(req, res);
    else if (p == "/settings/hostname")          routes::hostname_set(req, res);
    else if (p == "/settings/time/browser")      routes::time_browser(req, res);
    else if (p == "/settings/ntp")               routes::ntp_set(req, res);
    else if (p == "/settings/ntp/restart")       routes::ntp_restart(req, res);
    else if (p == "/donate")                     routes::donate_page(req, res);
    else if (p == "/modem" || p == "/modem/")    routes::modem_ui(req, res);
    else if (p.rfind("/modem/api/", 0) == 0)     routes::modem_api(req, res);
    else if (p == "/health") { res.content_type = "application/json"; res.body = "{\"ok\":true}"; }
    else { res.status = "404 Not Found"; res.content_type = "application/json"; res.body = "{\"error\":\"not found\"}"; }

    res.send();
    return 0;
}
