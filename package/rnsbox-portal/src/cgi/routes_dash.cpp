// routes_dash.cpp — Dashboard tab (the login landing page): server-rendered
// summary + a compact JSON endpoint (/dashboard/data) for the client-side
// auto-refresh. Ports the old Flask dashboard()/dashboard_data() faithfully.
//
// The HaLow card + its live-status script are only emitted when SLIP is
// enabled (matching the old {% if slip_on %}). They refresh over their own
// fetch to /reticulum/halow (a JSON endpoint owned by the Reticulum tab).
#include "routes.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "sysinfo.h"
#include "updatecheck.h"
#include "util.h"
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

using http::Request;
using http::Response;

namespace routes {

static std::string R(std::string s, const std::string& a, const std::string& b) {
    return web::replace_all(std::move(s), a, b);
}
static std::string E(const std::string& s) { return render::esc(s); }
static std::string JE(const std::string& s) { return http::json_escape(s); }

// _truthy() from store.py: enabled unless it's one of the "off" spellings.
static bool slip_enabled(const store::SlipConfig& sl) {
    std::string v = util::trim(sl.enabled);
    for (char& c : v) c = (char)tolower((unsigned char)c);
    return !(v == "no" || v == "0" || v == "false" || v == "off" || v.empty());
}

static const sysinfo::Iface* find_iface(const sysinfo::Network& net, const std::string& name) {
    for (const auto& i : net.ifaces) if (i.name == name) return &i;
    return nullptr;
}

static std::string join_esc(const std::vector<std::string>& a) {
    std::string o;
    for (size_t k = 0; k < a.size(); ++k) { if (k) o += ", "; o += E(a[k]); }
    return o;
}

static std::string fmt_pct(double p) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f", p); return buf;
}

// --- JSON helpers for /dashboard/data ---
static std::string str_array_json(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t k = 0; k < v.size(); ++k) { if (k) o += ","; o += "\"" + JE(v[k]) + "\""; }
    o += "]";
    return o;
}
static std::string iface_json(const sysinfo::Iface* i) {
    if (!i) return "{}";   // old returned {} for a missing iface; the JS copes
    return std::string("{\"name\":\"") + JE(i->name) +
           "\",\"state\":\"" + JE(i->state) +
           "\",\"mac\":\"" + JE(i->mac) +
           "\",\"mtu\":\"" + JE(i->mtu) +
           "\",\"rx\":\"" + JE(i->rx) +
           "\",\"tx\":\"" + JE(i->tx) +
           "\",\"addrs\":" + str_array_json(i->addrs) + "}";
}

// --- HaLow card + live-status script (verbatim port of the old template) ---
static std::string halow_card_html(const std::string& base) {
    // href must go through the portal (SCRIPT_NAME base), not the server root —
    // a literal /modem/ hits uhttpd's docroot (404), not this CGI.
    std::string s = R"HTML(<section class="card card-wide" id="halow-card">
    <div class="cardhead"><h3>HaLow modem</h3><span class="badge" id="halow-badge">…</span></div>
    <div id="halow-body"><p class="muted">Loading modem status…</p></div>
    <p class="cardfoot"><a class="btn" href="__BASE__/modem" target="_blank" rel="noopener">Open modem web UI →</a></p>
  </section>)HTML";
    return web::replace_all(std::move(s), "__BASE__", base);
}
static std::string halow_script_html(const std::string& base) {
    std::string s = R"JS(<script>
(function(){
  function esc(s){ return (''+(s==null?'':s)).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];}); }
  function num(v){ if(v==null) return null; var m=(''+v).match(/-?\d+(\.\d+)?/); return m?parseFloat(m[0]):null; }
  function unit(v,u){ if(v==null||v==='') return '?'; return /^-?\d+(\.\d+)?$/.test(''+v)? v+' '+u : esc(v); }
  function sig(v,u,g,a){ if(v==null) return '<span class="muted">—</span>'; var c=v>=g?'sig-g':(v>=a?'sig-a':'sig-r'); return '<span class="'+c+'">'+v+' '+u+'</span>'; }
  function noise(v){ var n=num(v); if(n==null) return '<span class="muted">—</span>'; var c=n<=-80?'sig-g':(n<=-70?'sig-a':'sig-r'); return '<span class="'+c+'">'+n+' dBm</span>'; }
  function tile(k,v){ return '<div class="hstat"><div class="hstat-k">'+k+'</div><div class="hstat-v">'+v+'</div></div>'; }
  function load(){
    var badge=document.getElementById('halow-badge'), body=document.getElementById('halow-body');
    if(!badge) return;
    return fetch('__BASE__/reticulum/halow').then(function(r){return r.json();}).then(function(d){
      if(!d.enabled){ var c=document.getElementById('halow-card'); if(c) c.hidden=true; return; }
      if(!d.reachable){
        badge.className='badge '+(d.sl0_up?'err':'warn');
        badge.textContent=d.sl0_up?'unreachable':'sl0 down';
        body.innerHTML='<p class="muted">'+(d.sl0_up?'Modem not answering over SLIP.':'SLIP link (sl0) is down.')+'</p>';
        return;
      }
      badge.className='badge ok'; badge.textContent='link up';
      var rd=d.radio||{}, st=d.stats||{}, dv=d.device||{}, b=d.best, ns=d.neighbours||[];
      var name=(dv.hostname||'RNode-Halow'), port=(d.tcp_port||8001);
      var air=num(st.airtime);
      var airbar = air!=null ? '<span class="hbar"><span style="width:'+Math.max(0,Math.min(100,air))+'%"></span></span> ' : '';
      var mcs = (rd.mcs==null||rd.mcs==='') ? 'MCS?' : 'MCS'+esc((''+rd.mcs).replace(/^mcs\s*/i,''));
      var fw = (dv.fw ? esc((''+dv.fw).split(' ')[0]) : '?');
      var tiles=[
        tile('Link (sl0)','<span class="badge ok">UP</span> <span class="val">'+esc(d.local)+' &rarr; '+esc(d.peer)+'</span>'),
        tile('rnsd interface', d.rnsd_up
          ? '<span class="badge ok">UP</span> <span class="val">'+esc(name)+' &rarr; :'+esc(port)+'</span>'
          : '<span class="badge warn">DOWN</span> <span class="val">'+esc(name)+'</span>'),
        tile('Radio','<span class="val">'+unit(rd.freq,'MHz')+' · '+unit(rd.bw,'MHz')+' · '+mcs+'</span>'),
        tile('TX power','<span class="val">'+unit(rd.power,'dBm')+'</span>'),
        tile('Airtime', airbar+'<span class="val">'+esc(st.airtime||'?')+'</span>'),
        tile('Ch. util','<span class="val">'+esc(st.ch_util||'?')+'</span>'),
        tile('Noise floor', noise(st.noise)),
        tile('Throughput','<span class="val">&darr; '+esc(st.rx_speed||'?')+' · &uarr; '+esc(st.tx_speed||'?')+'</span>'),
        tile('Traffic','<span class="val">&darr; '+esc(st.rx_bytes||'?')+' · &uarr; '+esc(st.tx_bytes||'?')+'</span>'),
        tile('Best peer', b?(sig(num(b.rssi),'dBm',-75,-88)+' · '+sig(num(b.snr),'dB',10,5)):'<span class="muted">none heard</span>'),
        tile('Neighbours','<span class="val">'+ns.length+'</span>'),
        tile('Modem','<span class="val">'+esc(dv.temp||'?')+' · fw '+fw+' · up '+esc(dv.uptime||'?')+'</span>'),
      ];
      body.innerHTML='<div class="hstats">'+tiles.join('')+'</div>';
    }).catch(function(){ badge.className='badge warn'; badge.textContent='error';
      body.innerHTML='<p class="muted">Could not read modem status.</p>'; });
  }
  window.__loadHalow = load;
  load();
})();
</script>)JS";
    return web::replace_all(std::move(s), "__BASE__", base);
}

// ---- GET / : server-rendered dashboard ----
void dashboard_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string base = web::base();

    sysinfo::Summary sys = sysinfo::summary();
    sysinfo::Rnsd    rnsd = sysinfo::rnsd_status();      // cheap: no with_raw
    sysinfo::Network net  = sysinfo::network_summary();
    store::WanConfig  wan  = store::read_wan();
    bool slip_on = slip_enabled(store::read_slip());

    const sysinfo::Iface* wif = find_iface(net, wan.interface);
    const sysinfo::Iface* lif = find_iface(net, "usb0");

    std::string h = render::page_file("dashboard");
    h = R(std::move(h), "__BASE__", base);

    // System card
    h = R(std::move(h), "__HOSTNAME__", E(sysinfo::hostname()));
    h = R(std::move(h), "__KERNEL__",   E(sys.kernel));
    h = R(std::move(h), "__UPTIME__",   E(sys.uptime));
    h = R(std::move(h), "__LOAD__",     E(sys.load[0]) + " / " + E(sys.load[1]) + " / " + E(sys.load[2]));

    // RAM meter
    h = R(std::move(h), "__RAM_USED__",      std::to_string(sys.mem.used_mb));
    h = R(std::move(h), "__RAM_TOTAL__",     std::to_string(sys.mem.total_mb));
    h = R(std::move(h), "__RAM_AVAIL__",     std::to_string(sys.mem.avail_mb));
    h = R(std::move(h), "__RAM_BUFFCACHE__", std::to_string(sys.mem.buffcache_mb));
    double pct = sys.mem.used_pct;
    const char* rclass = pct >= 90 ? "crit" : pct >= 75 ? "warn" : "ok";
    h = R(std::move(h), "__RAM_FILL_CLASS__", rclass);
    h = R(std::move(h), "__RAM_PCT__", fmt_pct(pct));

    // WAN card
    h = R(std::move(h), "__WAN__", E(wan.interface));
    std::string wstate = (wif && wif->state == "up")
        ? std::string("<span class=\"badge ok\">UP</span>")
        : "<span class=\"badge err\">" + E((wif && !wif->state.empty()) ? wif->state : std::string("DOWN")) + "</span>";
    h = R(std::move(h), "__WAN_STATE__", wstate);
    h = R(std::move(h), "__WAN_ADDR__", (wif && !wif->addrs.empty()) ? join_esc(wif->addrs) : "(none)");
    h = R(std::move(h), "__WAN_GW__",   net.default_gateway.empty() ? "(none)" : E(net.default_gateway));
    h = R(std::move(h), "__WAN_DNS__",  net.dns.empty() ? "(none)" : join_esc(net.dns));
    {
        std::string rx = (wif && !wif->rx.empty()) ? E(wif->rx) : "0";
        std::string tx = (wif && !wif->tx.empty()) ? E(wif->tx) : "0";
        h = R(std::move(h), "__WAN_RXTX__", rx + " / " + tx);
    }

    // LAN card — only when usb0 is actually up (a USB host is tethered)
    std::string lan;
    if (lif && lif->state == "up") {
        std::string addr = lif->addrs.empty() ? std::string("(none)") : join_esc(lif->addrs);
        std::string rx = lif->rx.empty() ? std::string("0") : E(lif->rx);
        std::string tx = lif->tx.empty() ? std::string("0") : E(lif->tx);
        lan =
            "<section class=\"card\">\n"
            "    <h3>LAN (usb0)</h3>\n"
            "    <table class=\"kv\">\n"
            "      <tr><th>State</th><td id=\"d-lan-state\"><span class=\"badge ok\">UP</span></td></tr>\n"
            "      <tr><th>Address</th><td id=\"d-lan-addr\">" + addr + "</td></tr>\n"
            "      <tr><th>MAC</th><td>" + E(lif->mac) + "</td></tr>\n"
            "      <tr><th>RX / TX</th><td id=\"d-lan-rxtx\">" + rx + " / " + tx + "</td></tr>\n"
            "    </table>\n"
            "  </section>";
    }
    h = R(std::move(h), "__LAN_CARD__", lan);

    // Reticulum card
    std::string rstat;
    if (rnsd.running) {
        rstat = "<span class=\"badge ok\">RUNNING</span> (pid " + E(rnsd.pid) + ")";
        if (rnsd.rss_mb > 0) rstat += " &middot; " + std::to_string(rnsd.rss_mb) + " MB RSS";
    } else {
        rstat = "<span class=\"badge err\">STOPPED</span>";
    }
    h = R(std::move(h), "__RNSD_STATUS__", rstat);
    // Installed version (cheap dist-info read) + optional update badge — mirrors
    // the reticulum card and the /dashboard/data JS.
    updatecheck::State u = updatecheck::display();
    std::string ver = "Reticulum (rnsd) <strong>" + E(u.installed.empty() ? "unknown" : u.installed) + "</strong>";
    if (u.update_available)
        ver += " <span class=\"badge warn\">update available: " + E(u.latest) + "</span>";
    else if (!u.latest.empty() && u.error.empty())
        ver += " <span class=\"badge ok\">up to date</span>";
    h = R(std::move(h), "__RNSD_VERSION__", ver);

    // HaLow card + script — present only when SLIP is enabled
    h = R(std::move(h), "__HALOW_CARD__",   slip_on ? halow_card_html(base)  : std::string());
    h = R(std::move(h), "__HALOW_SCRIPT__", slip_on ? halow_script_html(base) : std::string());

    res.body = render::layout(req, res, "Dashboard", "dashboard", h);
}

// ---- GET /dashboard/data : compact JSON for the client-side auto-refresh ----
void dashboard_data(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;

    sysinfo::Summary sys = sysinfo::summary();
    sysinfo::Rnsd    rnsd = sysinfo::rnsd_status();
    sysinfo::Network net  = sysinfo::network_summary();
    store::WanConfig  wan  = store::read_wan();
    const sysinfo::Iface* wif = find_iface(net, wan.interface);
    const sysinfo::Iface* lif = find_iface(net, "usb0");

    std::string J = "{\"sys\":{";
    J += "\"uptime\":\"" + JE(sys.uptime) + "\",";
    J += "\"load\":[\"" + JE(sys.load[0]) + "\",\"" + JE(sys.load[1]) + "\",\"" + JE(sys.load[2]) + "\"],";
    J += "\"kernel\":\"" + JE(sys.kernel) + "\",";
    J += "\"mem\":{";
    if (sys.mem.ok) {
        J += "\"total_mb\":" + std::to_string(sys.mem.total_mb) + ",";
        J += "\"avail_mb\":" + std::to_string(sys.mem.avail_mb) + ",";
        J += "\"used_mb\":" + std::to_string(sys.mem.used_mb) + ",";
        J += "\"buffcache_mb\":" + std::to_string(sys.mem.buffcache_mb) + ",";
        J += "\"used_pct\":" + fmt_pct(sys.mem.used_pct);
    }
    J += "}},";
    J += "\"rnsd\":{\"running\":" + std::string(rnsd.running ? "true" : "false");
    J += ",\"pid\":"    + (rnsd.pid.empty()  ? std::string("null") : rnsd.pid);
    J += ",\"rss_mb\":" + (rnsd.rss_mb >= 0  ? std::to_string(rnsd.rss_mb) : std::string("null"));
    J += "},";
    J += "\"wan_if\":" + iface_json(wif) + ",";
    J += "\"lan_if\":" + iface_json(lif) + ",";
    J += "\"gateway\":\"" + JE(net.default_gateway) + "\",";
    J += "\"dns\":" + str_array_json(net.dns) + ",";
    updatecheck::State u = updatecheck::display();
    J += "\"update\":{\"installed\":" + (u.installed.empty() ? std::string("null") : "\"" + JE(u.installed) + "\"");
    J += ",\"update_available\":" + std::string(u.update_available ? "true" : "false");
    J += ",\"latest\":" + (u.latest.empty() ? std::string("null") : "\"" + JE(u.latest) + "\"");
    J += ",\"error\":" + (u.error.empty() ? std::string("null") : "\"" + JE(u.error) + "\"") + "}";
    J += "}";

    res.content_type = "application/json";
    res.body = J;
}

}  // namespace routes
