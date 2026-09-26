// main.cpp — RNSBox portal CGI. One binary at /cgi-bin/portal/*, PATH_INFO = route.
// Pages are rendered server-side (render::layout wraps a .html content block from
// PAGES_DIR); form POSTs are POST-redirect-GET with a one-shot flash. A handful of
// endpoints stay JSON for the pages' own AJAX (rnstatus, wifi scan/status,
// dashboard auto-refresh, live HaLow modem). Dual-mode: `nftgen` CLI at boot.
#include "http.h"
#include "session.h"
#include "util.h"
#include "web.h"
#include "render.h"
#include "routes.h"
#include "nftgen.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// ---- auth routes ----
static void r_login(const http::Request& req, http::Response& res) {
    if (req.method == "POST") {
        std::string user = util::trim(req.f("username"));
        std::string pass = req.f("password");
        std::string next = req.f("next");
        if (session::verify_password(user, pass)) {
            res.set_cookie(session::start(user));
            std::string dest = web::base();   // no trailing slash (a trailing slash turns the CGI path into a dir lookup)
            if (next.size() > 1 && next[0] == '/' && next[1] != '/') dest = next;  // local only
            res.redirect(dest);
            return;
        }
        ::sleep(1);  // brute-force speed bump
        render::redirect_flash(res, web::base() + "/login", "error", "Invalid credentials");
        return;
    }
    std::string h = render::page_file("login");
    h = web::replace_all(h, "__ACTION__", web::base() + "/login");
    h = web::replace_all(h, "__NEXT__", http::html_escape(req.a("next")));
    res.body = render::login_layout(req, res, h);
}

static void r_logout(const http::Request&, http::Response& res) {
    res.set_cookie(session::clear());
    res.redirect(web::base() + "/login");
}

int main(int argc, char** argv) {
    // CLI mode: `rnsbox-portal nftgen --wan wlan0 --lan usb0,wlan1` (boot path).
    if (argc >= 2 && strcmp(argv[1], "nftgen") == 0)
        return nftgen::cli_main(argc, argv);

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

    const std::string& p = req.path;
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
    else if (p == "/reticulum/autoupdate")       routes::reticulum_autoupdate(req, res);
    else if (p == "/reticulum/checkupdate")      routes::reticulum_checkupdate(req, res);
    else if (p == "/reticulum/applyupdate")      routes::reticulum_applyupdate(req, res);
    else if (p == "/reticulum/slip")             routes::reticulum_slip(req, res);
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
