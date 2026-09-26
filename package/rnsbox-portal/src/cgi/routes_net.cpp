// routes_net.cpp — Network tab: server-rendered page + POST-redirect-GET forms.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "sysinfo.h"
#include "wifi.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <string>

using http::Request;
using http::Response;

namespace routes {

static std::string upper(std::string s) { for (char& c : s) c = (char)toupper((unsigned char)c); return s; }
static std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
static std::string E(const std::string& s) { return render::esc(s); }

static std::string net_redirect() { return web::base() + "/network"; }

void network_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::WanConfig w = store::read_wan();
    sysinfo::Network net = sysinfo::network_summary();
    std::string base = web::base();

    std::string h = render::page_file("network");
    h = R(std::move(h), "__BASE__", base);
    h = R(std::move(h), "__WAN__", E(w.interface));
    h = R(std::move(h), "__GATEWAY__", net.default_gateway.empty() ? "(none)" : E(net.default_gateway));
    h = R(std::move(h), "__SEL_ETH0__",  w.interface == "eth0"  ? "selected" : "");
    h = R(std::move(h), "__SEL_WLAN0__", w.interface == "wlan0" ? "selected" : "");
    bool is_static = (w.mode == "static");
    h = R(std::move(h), "__SEL_DHCP__",   is_static ? "" : "selected");
    h = R(std::move(h), "__SEL_STATIC__", is_static ? "selected" : "");
    h = R(std::move(h), "__WAN_STATIC_DISPLAY__", is_static ? "" : "display:none");
    h = R(std::move(h), "__WAN_ADDRESS__", E(w.address));
    h = R(std::move(h), "__WAN_PREFIX__",  E(w.prefix.empty() ? "24" : w.prefix));
    h = R(std::move(h), "__WAN_GATEWAY__", E(w.gateway));
    h = R(std::move(h), "__WAN_DNS__",     E(w.dns));

    // Port-forward rows
    std::string rows;
    auto fs = store::read_portforwards();
    if (fs.empty()) {
        rows = "<tr><td colspan=\"5\" class=\"muted\">No port forwards configured.</td></tr>";
    } else {
        for (size_t i = 0; i < fs.size(); ++i) {
            const auto& f = fs[i];
            rows += "<tr><td><span class=\"badge " + E(f.proto) + "\">" + E(upper(f.proto)) + "</span></td>"
                    "<td>" + std::to_string(f.wan_port) + "</td>"
                    "<td>" + E(f.lan_ip) + ":" + std::to_string(f.lan_port) + "</td>"
                    "<td>" + E(f.comment) + "</td>"
                    "<td><form method=\"post\" action=\"" + base + "/network/portforward/delete\" class=\"inline\">"
                    "<input type=\"hidden\" name=\"idx\" value=\"" + std::to_string(i) + "\">"
                    "<button type=\"submit\" class=\"btn-danger\">Remove</button></form></td></tr>";
        }
    }
    h = R(std::move(h), "__FORWARDS_ROWS__", rows);

    // Open-port rows
    rows.clear();
    auto ops = store::read_openports();
    if (ops.empty()) {
        rows = "<tr><td colspan=\"4\" class=\"muted\">No open ports &mdash; WAN access is closed (the LAN/usb0 side stays open).</td></tr>";
    } else {
        for (size_t i = 0; i < ops.size(); ++i) {
            const auto& o = ops[i];
            rows += "<tr><td><span class=\"badge " + E(o.proto) + "\">" + E(upper(o.proto)) + "</span></td>"
                    "<td>" + std::to_string(o.port) + "</td>"
                    "<td>" + E(o.comment) + "</td>"
                    "<td><form method=\"post\" action=\"" + base + "/network/openport/delete\" class=\"inline\">"
                    "<input type=\"hidden\" name=\"idx\" value=\"" + std::to_string(i) + "\">"
                    "<button type=\"submit\" class=\"btn-danger\">Remove</button></form></td></tr>";
        }
    }
    h = R(std::move(h), "__OPENPORTS_ROWS__", rows);

    // DHCP leases
    rows.clear();
    auto leases = sysinfo::dnsmasq_leases();
    if (leases.empty()) {
        rows = "<tr><td colspan=\"4\" class=\"muted\">No active leases.</td></tr>";
    } else {
        for (const auto& l : leases) {
            rows += "<tr><td>" + E(l.ip) + "</td><td><code>" + E(l.mac) + "</code></td>"
                    "<td>" + E(l.name) + "</td><td>" + E(l.expires) + "</td></tr>";
        }
    }
    h = R(std::move(h), "__LEASES_ROWS__", rows);

    // All interfaces
    rows.clear();
    for (const auto& i : net.ifaces) {
        std::string addrs;
        for (size_t k = 0; k < i.addrs.size(); ++k) { if (k) addrs += ", "; addrs += E(i.addrs[k]); }
        std::string st = (i.state == "up")
            ? std::string("<span class=\"badge ok\">UP</span>")
            : "<span class=\"badge warn\">" + E(i.state.empty() ? "?" : i.state) + "</span>";
        rows += "<tr><td><strong>" + E(i.name) + "</strong></td><td>" + st + "</td>"
                "<td>" + addrs + "</td><td><code>" + E(i.mac) + "</code></td>"
                "<td>" + E(i.mtu) + "</td><td>" + E(i.rx) + " / " + E(i.tx) + "</td></tr>";
    }
    h = R(std::move(h), "__IFACES_ROWS__", rows);

    res.body = render::layout(req, res, "Network", "network", h);
}

// ---- POST-redirect-GET handlers ----

void wan_set(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string iface = util::trim(req.f("wan")); if (iface.empty()) iface = "eth0";
    std::string mode = util::trim(req.f("wan_mode")); if (mode != "static") mode = "dhcp";
    if (iface == "wlan0" && !wifi::present()) {
        render::redirect_flash(res, net_redirect(), "error", "No WiFi radio — wlan0 WAN needs the W variant.");
        return;
    }
    store::WanConfig c;
    c.interface = iface;
    c.mode = mode;
    if (mode == "static") {
        c.address = util::trim(req.f("wan_address"));
        c.prefix  = util::trim(req.f("wan_prefix"));
        c.gateway = util::trim(req.f("wan_gateway"));
        c.dns     = util::trim(req.f("wan_dns"));
    }
    std::string err;
    if (!store::write_wan(c, err)) {
        render::redirect_flash(res, net_redirect(), "error", "Bad input: " + err);
        return;
    }
    web::run_init("S30eth");
    web::run_init("S60routing");
    if (iface == "wlan0")
        render::redirect_flash(res, net_redirect(), "success",
            "WAN set to wlan0 (WiFi). Make sure WiFi is connected as a client (WiFi tab → mode sta or sta+ap).");
    else if (mode == "static")
        render::redirect_flash(res, net_redirect(), "success",
            "WAN set to eth0 static " + c.address + "/" + c.prefix + ".");
    else
        render::redirect_flash(res, net_redirect(), "success", "WAN set to eth0 (DHCP).");
}

void pf_add(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::PortForward pf;
    pf.proto    = util::trim(req.f("proto"));
    pf.wan_port = atoi(req.f("wan_port").c_str());
    pf.lan_ip   = util::trim(req.f("lan_ip"));
    pf.lan_port = atoi(req.f("lan_port").c_str());
    pf.comment  = req.f("comment");
    std::string err;
    if (!store::add_portforward(pf, err)) {
        render::redirect_flash(res, net_redirect(), "error", "Bad input: " + err);
        return;
    }
    web::run_init("S60routing");
    render::redirect_flash(res, net_redirect(), "success", "Port forward added");
}

void pf_delete(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    if (!store::delete_portforward(atoi(req.f("idx").c_str())))
        render::redirect_flash(res, net_redirect(), "error", "Bad input: invalid index");
    else {
        web::run_init("S60routing");
        render::redirect_flash(res, net_redirect(), "success", "Port forward removed");
    }
}

void op_add(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::OpenPort op;
    op.proto   = util::trim(req.f("proto"));
    op.port    = atoi(req.f("port").c_str());
    op.comment = req.f("comment");
    std::string err;
    if (!store::add_openport(op, err)) {
        render::redirect_flash(res, net_redirect(), "error", "Bad input: " + err);
        return;
    }
    web::run_init("S60routing");
    render::redirect_flash(res, net_redirect(), "success", "Open port added");
}

void op_delete(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    if (!store::delete_openport(atoi(req.f("idx").c_str())))
        render::redirect_flash(res, net_redirect(), "error", "Bad input: invalid index");
    else {
        web::run_init("S60routing");
        render::redirect_flash(res, net_redirect(), "success", "Open port removed");
    }
}

}  // namespace routes
