// routes_map.cpp — Map tab: receive-only view of nodes that announce
// coordinates (RNS interface discovery), rendered offline in the browser.
//
// rnsd persists each validated discovery announce under
// <storage>/discovery/interfaces as one msgpack map. The native reader below
// decodes those files directly: on the C906 a python3 start costs ~1.5 s for
// a few hundred bytes of data, while this is a directory walk plus a few
// hundred tiny reads. Windows keeps the Python helper (the portal there is a
// desktop test front end and has no /etc/reticulum anyway).
#include "routes.h"
#include "render.h"
#include "web.h"
#include "store.h"
#include "modem.h"
#include "sysinfo.h"
#include "util.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <utility>
#include <vector>

#ifdef _WIN32
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

using http::Request;
using http::Response;

namespace routes {

namespace {
constexpr const char* HALOW_STATE = "/run/rnsbox/telemetry/halow.json";

// Tiny readers for our own one-line JSON (no escapes except the name) and for
// the modem payload snapshot written by rnsbox-telemetry.
std::string tel_jstr(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return std::string();
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return std::string();
    p++;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
    if (p >= j.size() || j[p] != '"') return std::string();
    p++;
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            p++;
            if (j[p] == 'n') out += '\n';
            else if (j[p] == 't') out += '\t';
            else if (j[p] == 'r') out += '\r';
            else out += j[p];
            p++;
        } else out += j[p++];
    }
    return out;
}

bool tel_jnum(const std::string& j, const char* key, double& out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p++;
    char* end = nullptr;
    double v = std::strtod(j.c_str() + p, &end);
    if (end == j.c_str() + p) return false;
    out = v;
    return true;
}

std::string tel_utc(double epoch) {
    if (epoch <= 0) return std::string("—");
    time_t t = (time_t)epoch;
    struct tm g;
    gmtime_r(&t, &g);
    char b[32];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M UTC", &g);
    return b;
}

bool discovery_on() {
    std::string c = util::read_file("/etc/reticulum/config", 256 * 1024);
    size_t p = 0;
    while (p < c.size()) {
        size_t e = c.find('\n', p);
        if (e == std::string::npos) e = c.size();
        std::string line = util::trim(c.substr(p, e - p));
        if (line.compare(0, 21, "discover_interfaces =") == 0) {
            std::string v = util::trim(line.substr(21));
            return v == "yes" || v == "Yes" || v == "1" || v == "true";
        }
        p = e + 1;
    }
    return false;
}

std::string tel_status_html(const store::TelemetryConfig& tc) {
    std::string out;
    out += "<div><span class=\"tel-k\">Interface discovery</span>";
    out += discovery_on()
        ? "on — announces heard over the mesh are collected below."
        : "off — enable a discovery path to collect other nodes.";
    out += "</div>";

    std::string hj = util::read_file(HALOW_STATE, 4096);
    double recv = 0;
    if (!hj.empty() && tel_jnum(hj, "received", recv) && recv > 0) {
        std::string name = tel_jstr(hj, "name");
        std::string src = tel_jstr(hj, "source");
        double lat = 0, lon = 0;
        bool has = false;
        size_t hp = hj.find("\"has_loc\"");
        if (hp != std::string::npos) {
            hp = hj.find(':', hp);
            if (hp != std::string::npos) {
                hp++;
                while (hp < hj.size() && (hj[hp] == ' ' || hj[hp] == '\t')) hp++;
                has = hj.compare(hp, 4, "true") == 0;
            }
        }
        tel_jnum(hj, "lat", lat);
        tel_jnum(hj, "lon", lon);
        out += "<div><span class=\"tel-k\">HaLow telemetry</span>last packet " + tel_utc(recv);
        if (has) {
            char b[96];
            std::snprintf(b, sizeof(b), " — %.5f, %.5f", lat, lon);
            out += b;
        }
        if (!name.empty()) out += " (" + render::esc(name) + ")";
        if (!src.empty()) out += " from " + render::esc(src);
        out += ", forwarded to " + render::esc(tc.halow_upstream) + ".</div>";
    } else if (tc.halow_mqtt == "yes") {
        out += "<div><span class=\"tel-k\">HaLow telemetry</span>nothing received yet — the "
               "modem sends a packet every 12 hours once its config is pushed.</div>";
    }
    return out;
}

}  // namespace

static std::string ret_redirect_map() { return web::base() + "/map"; }

void map_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::TelemetryConfig tc = store::read_telemetry();
    store::SlipConfig sl = store::read_slip();
    store::RnodeConfig rc = store::read_rnode();

    std::string h = render::page_file("map");
    auto set = [&](const char* k, const std::string& v) {
        h = web::replace_all(std::move(h), k, v);
    };
    set("__BASE__", render::esc(web::base()));
    set("__TEL_LAT__", render::esc(tc.latitude));
    set("__TEL_LON__", render::esc(tc.longitude));
    set("__TEL_HEIGHT__", render::esc(tc.height));
    set("__TEL_NAME__", render::esc(tc.name));
    set("__TEL_INTERVAL__", render::esc(tc.announce_interval.empty() ? "360" : tc.announce_interval));
    set("__TEL_REACH__", render::esc(tc.reachable_on));
    set("__TEL_TCP_CHECKED__", tc.discover_tcp == "yes" ? "checked" : "");
    set("__TEL_STATUS__", tel_status_html(tc));

    // A path is only shown when the transport it publishes over exists. Hidden
    // paths keep their previous state in a hidden field so saving the card does
    // not silently switch them off (the RNode/HaLow link can be re-enabled
    // later and the path resumes).
    auto checked = [](const std::string& v) -> std::string {
        return v == "yes" ? "checked" : "";
    };
    auto hidden = [](const char* name, const std::string& v) {
        return std::string("<input type=\"hidden\" name=\"") + name + "\" value=\"" +
               (v == "yes" ? "on" : "") + "\">";
    };
    std::string lora_block, halow_block, keep;
    if (rc.transport != "off") {
        lora_block =
            "<div class=\"tel-path\">\n"
            "        <label class=\"check\"><input type=\"checkbox\" name=\"discover_lora\" " +
            checked(tc.discover_lora) + "> Reticulum discovery — LoRa RNode</label>\n"
            "        <p class=\"muted tel-note\">Published by rnsd on the " +
            render::esc(rc.name.empty() ? std::string("RNode") : rc.name) +
            " interface; frequency and bandwidth are included.</p>\n"
            "      </div>";
    } else {
        keep += hidden("discover_lora", tc.discover_lora);
    }
    if (sl.enabled == "yes") {
        halow_block =
            "<div class=\"tel-path\">\n"
            "        <label class=\"check\"><input type=\"checkbox\" name=\"halow_mqtt\" "
            "id=\"tel-halow-mqtt\" " + checked(tc.halow_mqtt) +
            "> HaLow modem — Reticulum interface + MQTT telemetry</label>\n"
            "        <p class=\"muted tel-note\">Coordinates, name and antenna direction are "
            "written into the modem on save; it then sends its telemetry to this box, which "
            "keeps the last packet in RAM for the map and forwards it to " +
            render::esc(tc.halow_upstream) + " as a proxy (nothing is written to flash). "
            "Height is published by the RNS discovery paths only — the modem's telemetry "
            "packet carries latitude/longitude.</p>\n"
            "        <div class=\"row-form tel-halow-extra\" id=\"tel-halow-extra\">\n"
            "          <label class=\"check\" style=\"margin:0\"><input type=\"checkbox\" "
            "name=\"halow_ext\" " + checked(tc.halow_ext) + "> Extended telemetry</label>\n"
            "          <label class=\"check\" style=\"margin:0\"><input type=\"checkbox\" "
            "name=\"halow_dir_en\" id=\"tel-dir-en\" " + checked(tc.halow_dir_en) +
            "> Antenna direction</label>\n"
            "          <label style=\"margin:0\">Degrees\n"
            "            <input type=\"number\" name=\"halow_direction\" id=\"tel-dir\" value=\"" +
            render::esc(tc.halow_direction.empty() ? "0" : tc.halow_direction) +
            "\" min=\"0\" max=\"359\"></label>\n"
            "        </div>\n"
            "      </div>\n"
            "      <div class=\"tel-path\">\n"
            "        <label class=\"check\"><input type=\"checkbox\" name=\"discover_halow\" " +
            checked(tc.discover_halow) +
            "> Reticulum discovery — HaLow interface</label>\n"
            "        <p class=\"muted tel-note\">rnsd announces the RNode-Halow TCP interface "
            "(frequency and bandwidth included), so the box also appears on the discovery map "
            "through the modem.</p>\n"
            "      </div>";
    } else {
        keep += hidden("halow_mqtt", tc.halow_mqtt);
        keep += hidden("discover_halow", tc.discover_halow);
        keep += hidden("halow_ext", tc.halow_ext);
        keep += hidden("halow_dir_en", tc.halow_dir_en);
        keep += "<input type=\"hidden\" name=\"halow_direction\" value=\"" +
                render::esc(tc.halow_direction.empty() ? "0" : tc.halow_direction) + "\">";
    }
    set("__TEL_LORA_BLOCK__", lora_block);
    set("__TEL_HALOW_BLOCK__", halow_block);
    set("__TEL_KEEP__", keep);
    res.body = render::layout(req, res, "Discovery", "map", h);
}

void map_telemetry(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    store::TelemetryConfig c;
    c.latitude = util::trim(req.f("latitude"));
    c.longitude = util::trim(req.f("longitude"));
    c.height = util::trim(req.f("height"));
    c.name = util::trim(req.f("name"));
    c.announce_interval = util::trim(req.f("announce_interval"));
    if (c.announce_interval.empty()) c.announce_interval = "360";
    c.reachable_on = util::trim(req.f("reachable_on"));
    c.discover_lora = req.f("discover_lora") == "on" ? "yes" : "no";
    c.discover_tcp = req.f("discover_tcp") == "on" ? "yes" : "no";
    c.discover_halow = req.f("discover_halow") == "on" ? "yes" : "no";
    c.halow_mqtt = req.f("halow_mqtt") == "on" ? "yes" : "no";
    c.halow_ext = req.f("halow_ext") == "on" ? "yes" : "no";
    c.halow_dir_en = req.f("halow_dir_en") == "on" ? "yes" : "no";
    c.halow_direction = util::trim(req.f("halow_direction"));
    if (c.halow_direction.empty()) c.halow_direction = "0";

    std::string err;
    if (!store::write_telemetry(c, err)) {
        render::redirect_flash(res, ret_redirect_map(), "error", "Telemetry: " + err + ".");
        return;
    }
    store::SlipConfig sl = store::read_slip();
    store::RnodeConfig rc = store::read_rnode();
    std::string warn;
    if (c.discover_lora == "yes" && rc.transport == "off")
        warn += " No RNode LoRa interface is configured, so that path stays idle.";
    if (c.discover_halow == "yes" && sl.enabled != "yes")
        warn += " The HaLow link is off, so its discovery path stays idle.";
    if (c.halow_mqtt == "yes" && sl.enabled != "yes")
        warn += " The HaLow link is off; enable it on the Reticulum tab to start telemetry.";

    web::run_init("S80telemetry", "reload");
    web::run_init("S81rnode", "reload");
    web::run_init("S82rnsd");

    std::string modem_msg;
    if (c.halow_mqtt == "yes" && sl.enabled == "yes") {
        std::string local = modem::local_ip_towards(sl);
        // The modem's telemetry runs on a fixed 12-hour cadence; the card's
        // announce interval is for rnsd discovery only. Everything else is
        // the modem's standard telemetry config (only the MQTT destination
        // and the coordinates/name from this form are substituted).
        const long period = 12 * 3600;
        std::string json =
            "{\"en\":true,\"host\":\"" + local + "\",\"port\":1883,\"prd\":" +
            std::to_string(period) + ",\"lat\":" + c.latitude + ",\"lon\":" + c.longitude +
            ",\"ext\":" + std::string(c.halow_ext == "yes" ? "true" : "false") +
            ",\"dir_en\":" + std::string(c.halow_dir_en == "yes" ? "true" : "false") +
            ",\"dir\":" + c.halow_direction +
            ",\"usr\":\"rnode-halow\",\"pwd\":\"rnode-halow\","
            "\"top\":\"rnode-halow/telemetry\"";
        if (!c.name.empty()) json += ",\"name\":\"" + c.name + "\"";
        // Harmless if the firmware ignores it; keeps the full location in the
        // modem request for a future telemetry-packet revision.
        if (!c.height.empty()) json += ",\"height\":" + c.height;
        json += "}";
        std::string resp;
        if (modem::post(sl.peer_ip, "telemetry_cfg", json, resp))
            warn += " The modem now sends telemetry to " + local + ":1883.";
        else
            warn += " The modem did not accept the telemetry config (is it powered and reachable?).";
        web::run_init("S83telemetry", "restart");
    } else {
        // Best effort: stop the modem publishing even if the proxy is off.
        if (sl.enabled == "yes") {
            std::string resp;
            modem::post(sl.peer_ip, "telemetry_cfg", "{\"en\":false}", resp);
        }
        web::run_init("S83telemetry", "stop");
    }

    if (warn.empty())
        render::redirect_flash(res, ret_redirect_map(), "success", "Location and publish paths saved.");
    else
        render::redirect_flash(res, ret_redirect_map(), "warn", "Saved." + warn);
}

#ifdef _WIN32

static std::string map_nodes_body() {
    util::RunResult rr = util::run(
        {"/usr/bin/python3", "/usr/lib/rnsbox/mapnodes.py"}, "", 15);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') return out;
    return "{\"nodes\":[],\"total\":0,\"error\":\"" +
           http::json_escape(out.empty() ? "map helper failed" : out) + "\"}";
}

#else

namespace {

// Discovery records are tiny; refuse absurd container lengths from a corrupt
// file before allocating for them.
constexpr size_t MAX_ITEMS = 100000;

// Minimal msgpack (RNS vendors umsgpack; discovery records are flat maps of
// str/int/float/bool/nil). Nested values are parsed generically so a future
// record shape cannot desynchronise the reader.
struct MP {
    enum Kind { NIL, BOOL, INT, UINT, DOUBLE, STR, BIN, ARRAY, MAP } kind = NIL;
    bool b = false;
    long long i = 0;
    unsigned long long u = 0;
    double d = 0;
    std::string s;
    std::vector<MP> arr;
    std::vector<std::pair<MP, MP>> map;
};

struct Reader {
    const unsigned char* p;
    size_t n;
    size_t at = 0;
    bool ok = true;

    explicit Reader(const std::string& src)
        : p(reinterpret_cast<const unsigned char*>(src.data())), n(src.size()) {}

    bool need(size_t k) {
        if (!ok || k > n - at) { ok = false; return false; }
        return true;
    }
    unsigned u8() {
        return need(1) ? p[at++] : 0;
    }
    unsigned long long be(size_t bytes) {
        unsigned long long v = 0;
        if (!need(bytes)) return 0;
        for (size_t k = 0; k < bytes; ++k) v = (v << 8) | p[at++];
        return v;
    }
    bool take(size_t len, std::string& out) {
        if (!need(len)) return false;
        out.assign(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return true;
    }
    bool value(MP& v, int depth);
};

bool Reader::value(MP& v, int depth) {
    if (depth > 32) { ok = false; return false; }
    unsigned m = u8();
    if (!ok) return false;

    if (m <= 0x7f) { v.kind = MP::UINT; v.u = m; return true; }
    if (m >= 0xe0) { v.kind = MP::INT; v.i = static_cast<signed char>(m); return true; }

    if ((m & 0xf0) == 0x80) {                       // fixmap
        v.kind = MP::MAP;
        size_t count = m & 0x0f;
        v.map.reserve(count);
        for (size_t k = 0; k < count; ++k) {
            MP key, val;
            if (!value(key, depth + 1) || !value(val, depth + 1)) return false;
            v.map.emplace_back(std::move(key), std::move(val));
        }
        return true;
    }
    if ((m & 0xf0) == 0x90) {                       // fixarray
        v.kind = MP::ARRAY;
        size_t count = m & 0x0f;
        v.arr.reserve(count);
        for (size_t k = 0; k < count; ++k) {
            MP item;
            if (!value(item, depth + 1)) return false;
            v.arr.push_back(std::move(item));
        }
        return true;
    }
    if ((m & 0xe0) == 0xa0) {                       // fixstr
        v.kind = MP::STR;
        return take(m & 0x1f, v.s);
    }

    switch (m) {
        case 0xc0: v.kind = MP::NIL; return true;
        case 0xc2: v.kind = MP::BOOL; v.b = false; return true;
        case 0xc3: v.kind = MP::BOOL; v.b = true;  return true;
        case 0xc4:                                  // bin8
        case 0xc5:                                  // bin16
        case 0xc6: {                                // bin32
            size_t bytes = m == 0xc4 ? 1 : (m == 0xc5 ? 2 : 4);
            size_t len = static_cast<size_t>(be(bytes));
            if (!ok) return false;
            v.kind = MP::BIN;
            return take(len, v.s);
        }
        case 0xc7: case 0xc8: case 0xc9: {          // ext 8/16/32
            size_t bytes = m == 0xc7 ? 1 : (m == 0xc8 ? 2 : 4);
            size_t len = static_cast<size_t>(be(bytes));
            if (!need(len + 1)) return false;
            at += len + 1;                          // type byte + payload
            v.kind = MP::NIL;
            return true;
        }
        case 0xca: {                                // float32
            unsigned long long bits = be(4);
            if (!ok) return false;
            uint32_t b32 = static_cast<uint32_t>(bits);
            float f;
            std::memcpy(&f, &b32, sizeof(f));
            v.kind = MP::DOUBLE;
            v.d = f;
            return true;
        }
        case 0xcb: {                                // float64
            unsigned long long bits = be(8);
            if (!ok) return false;
            std::memcpy(&v.d, &bits, sizeof(v.d));
            v.kind = MP::DOUBLE;
            return true;
        }
        case 0xcc: v.kind = MP::UINT; v.u = be(1); return ok;
        case 0xcd: v.kind = MP::UINT; v.u = be(2); return ok;
        case 0xce: v.kind = MP::UINT; v.u = be(4); return ok;
        case 0xcf: v.kind = MP::UINT; v.u = be(8); return ok;
        case 0xd0: v.kind = MP::INT; v.i = static_cast<int8_t>(be(1));  return ok;
        case 0xd1: v.kind = MP::INT; v.i = static_cast<int16_t>(be(2)); return ok;
        case 0xd2: v.kind = MP::INT; v.i = static_cast<int32_t>(be(4)); return ok;
        case 0xd3: v.kind = MP::INT; v.i = static_cast<int64_t>(be(8)); return ok;
        case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8: {
            size_t len = 1u << (m - 0xd4);          // 1,2,4,8,16
            if (!need(len + 1)) return false;
            at += len + 1;
            v.kind = MP::NIL;
            return true;
        }
        case 0xd9: case 0xda: case 0xdb: {          // str 8/16/32
            size_t bytes = m == 0xd9 ? 1 : (m == 0xda ? 2 : 4);
            size_t len = static_cast<size_t>(be(bytes));
            if (!ok) return false;
            v.kind = MP::STR;
            return take(len, v.s);
        }
        case 0xdc: case 0xdd: {                     // array 16/32
            size_t bytes = m == 0xdc ? 2 : 4;
            size_t count = static_cast<size_t>(be(bytes));
            if (!ok || count > MAX_ITEMS) { ok = false; return false; }
            v.kind = MP::ARRAY;
            for (size_t k = 0; k < count; ++k) {
                MP item;
                if (!value(item, depth + 1)) return false;
                v.arr.push_back(std::move(item));
            }
            return true;
        }
        case 0xde: case 0xdf: {                     // map 16/32
            size_t bytes = m == 0xde ? 2 : 4;
            size_t count = static_cast<size_t>(be(bytes));
            if (!ok || count > MAX_ITEMS / 2) { ok = false; return false; }
            v.kind = MP::MAP;
            for (size_t k = 0; k < count; ++k) {
                MP key, val;
                if (!value(key, depth + 1) || !value(val, depth + 1)) return false;
                v.map.emplace_back(std::move(key), std::move(val));
            }
            return true;
        }
        default: break;
    }
    ok = false;
    return false;
}

const MP* field(const MP& rec, const char* key) {
    if (rec.kind != MP::MAP) return nullptr;
    for (const auto& kv : rec.map)
        if (kv.first.kind == MP::STR && kv.first.s == key) return &kv.second;
    return nullptr;
}

bool as_number(const MP& v, double& out) {
    switch (v.kind) {
        case MP::INT:    out = static_cast<double>(v.i); return true;
        case MP::UINT:   out = static_cast<double>(v.u); return true;
        case MP::DOUBLE: out = v.d; return true;
        default: return false;
    }
}

std::string as_text(const MP& v) {
    if (v.kind == MP::STR) return v.s;
    if (v.kind == MP::BIN) return util::to_hex(v.s);
    return std::string();
}

std::string text_or(const MP& rec, const char* key, const std::string& dflt = "") {
    const MP* v = field(rec, key);
    std::string s = v ? as_text(*v) : std::string();
    return s.empty() ? dflt : s;
}

bool number_or(const MP& rec, const char* key, double& out) {
    const MP* v = field(rec, key);
    return v && as_number(*v, out);
}

// A scalar rendered the way json.dumps would: strings quoted, numbers bare,
// everything else null. Discovery uses these for modulation/channel/port.
struct Scalar {
    bool str = false, num = false;
    std::string s;
    double d = 0;
};

Scalar scalar_of(const MP& rec, const char* key) {
    Scalar out;
    const MP* v = field(rec, key);
    if (!v) return out;
    if (v->kind == MP::STR) { out.str = true; out.s = v->s; }
    else if (as_number(*v, out.d)) out.num = true;
    return out;
}

std::string scalar_json(const Scalar& v) {
    if (v.str) return "\"" + http::json_escape(v.s) + "\"";
    if (v.num) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.10g", v.d);
        return b;
    }
    return "null";
}

std::string number_json(bool has, double v) {
    if (!has) return "null";
    char b[64];
    std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}

struct Node {
    std::string id, name, type, impl, version, status;
    Scalar modulation, channel, reachable_on;
    bool has_lat = false, has_lon = false, has_height = false, has_hops = false;
    bool has_freq = false, has_bw = false, has_sf = false, has_cr = false;
    bool has_port = false;
    double lat = 0, lon = 0, height = 0, last_heard = 0, discovered = 0;
    double frequency = 0, bandwidth = 0;
    long long hops = 0, sf = 0, cr = 0, port = 0, heard_count = 0;
};

const char* const STORES[] = {
    "/etc/reticulum/storage/discovery/interfaces",
    "/root/.reticulum/storage/discovery/interfaces",
    "/var/lib/reticulum/discovery/interfaces",
    "/home/admin/.reticulum/storage/discovery/interfaces",
};

bool is_dir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string pick_store() {
    // Test/support hook: point the reader at a copy of the store.
    if (const char* env = ::getenv("RNSBOX_MAP_STORE")) {
        if (*env) return env;
    }
    for (const char* p : STORES)
        if (is_dir(p)) return p;
    const char* home = ::getenv("HOME");
    return std::string(home ? home : "/root") +
           "/.reticulum/storage/discovery/interfaces";
}

bool load_record(const std::string& path, MP& rec) {
    std::string data = util::read_file(path, 4 * 1024 * 1024);
    if (data.empty()) return false;
    Reader r(data);
    if (!r.value(rec, 0) || !r.ok || rec.kind != MP::MAP) return false;
    return true;
}

Node node_from(const MP& rec, const char* basename) {
    Node n;
    n.name = text_or(rec, "name", "Reticulum node");
    n.type = text_or(rec, "type");
    n.impl = text_or(rec, "impl_name");
    n.version = text_or(rec, "version");
    n.status = text_or(rec, "status");

    if (const MP* v = field(rec, "transport_id")) n.id = as_text(*v);
    if (n.id.empty())
        if (const MP* v = field(rec, "network_id")) n.id = as_text(*v);
    if (n.id.empty()) n.id = basename;

    n.has_lat = number_or(rec, "latitude", n.lat);
    n.has_lon = number_or(rec, "longitude", n.lon);
    n.has_height = number_or(rec, "height", n.height);

    double hops = 0;
    if (number_or(rec, "hops", hops)) { n.has_hops = true; n.hops = (long long)hops; }
    else n.has_hops = false;

    number_or(rec, "last_heard", n.last_heard);
    number_or(rec, "discovered", n.discovered);
    double hc = 0;
    if (number_or(rec, "heard_count", hc)) n.heard_count = (long long)hc;

    n.has_freq = number_or(rec, "frequency", n.frequency);
    n.has_bw = number_or(rec, "bandwidth", n.bandwidth);

    double sf = 0, cr = 0;
    if (number_or(rec, "spreading_factor", sf)) { n.has_sf = true; n.sf = (long long)sf; }
    else if (number_or(rec, "sf", sf))          { n.has_sf = true; n.sf = (long long)sf; }
    if (number_or(rec, "coding_rate", cr))      { n.has_cr = true; n.cr = (long long)cr; }
    else if (number_or(rec, "cr", cr))          { n.has_cr = true; n.cr = (long long)cr; }

    double port = 0;
    if (number_or(rec, "port", port)) { n.has_port = true; n.port = (long long)port; }

    n.modulation = scalar_of(rec, "modulation");
    n.channel = scalar_of(rec, "channel");
    n.reachable_on = scalar_of(rec, "reachable_on");
    return n;
}

void append_node(std::string& o, const Node& n) {
    o += "{\"id\":\"" + http::json_escape(n.id) +
         "\",\"name\":\"" + http::json_escape(n.name) +
         "\",\"type\":\"" + http::json_escape(n.type) +
         "\",\"impl\":\"" + http::json_escape(n.impl) +
         "\",\"version\":\"" + http::json_escape(n.version) +
         "\",\"lat\":" + number_json(n.has_lat, n.lat) +
         ",\"lon\":" + number_json(n.has_lon, n.lon) +
         ",\"height\":" + number_json(n.has_height, n.height) +
         ",\"hops\":" + number_json(n.has_hops, (double)n.hops) +
         ",\"last_heard\":" + number_json(true, n.last_heard) +
         ",\"discovered\":" + number_json(true, n.discovered) +
         ",\"heard_count\":" + number_json(true, (double)n.heard_count) +
         ",\"status\":\"" + http::json_escape(n.status) +
         "\",\"frequency\":" + number_json(n.has_freq, n.frequency) +
         ",\"bandwidth\":" + number_json(n.has_bw, n.bandwidth) +
         ",\"spreading_factor\":" + number_json(n.has_sf, (double)n.sf) +
         ",\"coding_rate\":" + number_json(n.has_cr, (double)n.cr) +
         ",\"modulation\":" + scalar_json(n.modulation) +
         ",\"channel\":" + scalar_json(n.channel) +
         ",\"reachable_on\":" + scalar_json(n.reachable_on) +
         ",\"port\":" + number_json(n.has_port, (double)n.port) + "}";
}

// Two non-RNS sources share the map:
//   - this box, when the Map tab enabled a discovery path (so the marker the
//     box publishes elsewhere is visible locally too);
//   - the HaLow modem's own telemetry snapshot, held in RAM by
//     rnsbox-telemetry (a proxy — nothing is written to flash). The modem's coordinates are the ones the Map tab
//     pushed into its telemetry config.
void append_telemetry_nodes(std::vector<Node>& out) {
    store::TelemetryConfig tc = store::read_telemetry();
    if (store::telemetry_any_path(tc)) {
        double lat = 0, lon = 0;
        char *e1 = nullptr, *e2 = nullptr;
        if (!tc.latitude.empty() && !tc.longitude.empty()) {
            lat = std::strtod(tc.latitude.c_str(), &e1);
            lon = std::strtod(tc.longitude.c_str(), &e2);
            if (e1 && !*e1 && e2 && !*e2 && lat >= -90 && lat <= 90 &&
                lon >= -180 && lon <= 180 && !(lat == 0 && lon == 0)) {
                Node n;
                n.id = "local:box";
                n.name = tc.name.empty() ? sysinfo::hostname() : tc.name;
                n.type = "This box";
                n.impl = "RNSBox";
                n.has_lat = n.has_lon = true;
                n.lat = lat; n.lon = lon;
                if (!tc.height.empty()) {
                    char *e = nullptr;
                    double h = std::strtod(tc.height.c_str(), &e);
                    if (e && !*e) { n.has_height = true; n.height = h; }
                }
                n.last_heard = (double)::time(nullptr);
                n.discovered = n.last_heard;
                if (!tc.reachable_on.empty()) {
                    n.reachable_on.str = true;
                    n.reachable_on.s = tc.reachable_on;
                }
                out.push_back(n);
            }
        }
    }

    std::string hj = util::read_file(HALOW_STATE, 4096);
    if (hj.empty()) return;
    double recv = 0, lat = 0, lon = 0, freq = 0, bw = 0;
    if (!tel_jnum(hj, "received", recv) || recv <= 0) return;
    size_t hp = hj.find("\"has_loc\"");
    bool has = false;
    if (hp != std::string::npos) {
        hp = hj.find(':', hp);
        if (hp != std::string::npos) {
            hp++;
            while (hp < hj.size() && (hj[hp] == ' ' || hj[hp] == '\t')) hp++;
            has = hj.compare(hp, 4, "true") == 0;
        }
    }
    if (!has) return;
    if (!tel_jnum(hj, "lat", lat) || !tel_jnum(hj, "lon", lon)) return;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || (lat == 0 && lon == 0)) return;

    Node n;
    std::string lxmf = tel_jstr(hj, "lxmf");
    std::string src = tel_jstr(hj, "source");
    n.id = lxmf.empty() ? ("halow:" + src) : ("halow:" + lxmf);
    n.name = tel_jstr(hj, "name");
    if (n.name.empty()) n.name = "HaLow modem";
    n.type = "HaLow modem";
    n.impl = "RNode";
    n.version = tel_jstr(hj, "fw");
    n.has_lat = n.has_lon = true;
    n.lat = lat; n.lon = lon;
    n.last_heard = recv;
    n.discovered = recv;
    if (tel_jnum(hj, "freq_hz", freq) && freq > 0) { n.has_freq = true; n.frequency = freq; }
    if (tel_jnum(hj, "bw_hz", bw) && bw > 0)       { n.has_bw = true; n.bandwidth = bw; }
    if (!src.empty()) { n.reachable_on.str = true; n.reachable_on.s = src; }
    out.push_back(n);
}

std::string file_sig(const char* path) {
    struct stat st;
    if (::stat(path, &st) != 0) return "-";
    char b[64];
    std::snprintf(b, sizeof(b), "%lld:%lld", (long long)st.st_mtim.tv_sec,
                  (long long)st.st_size);
    return b;
}

// The store changes only when an announce is validated or updated; cache the
// rendered JSON keyed on every record's (name, mtime, size). A directory walk
// plus 114 stat() calls is ~2 ms, while decoding plus re-rendering is ~100 ms
// on the C906. The cache lives in /run (tmpfs) and is rebuilt on any change.
constexpr const char* MAP_CACHE = "/run/rnsbox-map-nodes.json";
constexpr const char* MAP_STAMP = "/run/rnsbox-map-nodes.stamp";

void fnv_str(uint64_t& h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
}

std::string store_signature(const std::string& store,
                            const std::vector<std::string>& names) {
    uint64_t h = 1469598103934665603ULL;
    fnv_str(h, store);
    for (const std::string& name : names) {
        struct stat st;
        std::string path = store + "/" + name;
        if (::stat(path.c_str(), &st) != 0) ::memset(&st, 0, sizeof(st));
        fnv_str(h, name);
        char b[96];
        std::snprintf(b, sizeof(b), "|%lld:%lld:%lld",
                      (long long)st.st_mtim.tv_sec,
                      (long long)st.st_mtim.tv_nsec,
                      (long long)st.st_size);
        fnv_str(h, b);
    }
    char out[32];
    std::snprintf(out, sizeof(out), "%016llx", (unsigned long long)h);
    return out;
}

}  // namespace

// Reticulum's discovery store is a cache of recent announces: rnsd validates
// and prunes it on every start, so after a reboot the map would start empty
// again. Keep a copy of every valid record under /var/lib/rnsbox (outside the
// RNS store) and merge it back in; the map then accumulates nodes across
// reboots and updates them in place as fresh announces arrive.
constexpr const char* MAP_PERSIST = "/var/lib/rnsbox/map-store";

void map_persist_update(const std::string& store,
                        const std::vector<std::string>& names) {
    for (const std::string& n : names) {
        std::string src = store + "/" + n;
        std::string dst = std::string(MAP_PERSIST) + "/" + n;
        struct stat ss, ds;
        if (::stat(src.c_str(), &ss) != 0 || !S_ISREG(ss.st_mode)) continue;
        if (::stat(dst.c_str(), &ds) == 0 &&
            ds.st_mtim.tv_sec >= ss.st_mtim.tv_sec &&
            ds.st_size == ss.st_size)
            continue;
        MP rec;
        if (!load_record(src, rec)) continue;   // partial write; retry next time
        std::string data = util::read_file(src, 4 * 1024 * 1024);
        if (!data.empty()) util::write_file_atomic(dst, data, 0600);
    }
}

std::string map_nodes_body() {
    std::string store = pick_store();
    // RNSBOX_MAP_STORE is the diagnostics/test override; never mix its
    // output with the persistent accumulation.
    const bool use_cache = ::getenv("RNSBOX_MAP_STORE") == nullptr;

    std::vector<std::string> live;
    DIR* dir = ::opendir(store.c_str());
    if (dir) {
        while (struct dirent* e = ::readdir(dir)) {
            if (e->d_name[0] == '.') continue;
            live.push_back(e->d_name);
        }
        ::closedir(dir);
        std::sort(live.begin(), live.end());
    }

    std::vector<std::string> persist;
    if (use_cache) {
        ::mkdir("/var/lib/rnsbox", 0700);
        ::mkdir(MAP_PERSIST, 0700);
        map_persist_update(store, live);
        DIR* pd = ::opendir(MAP_PERSIST);
        if (pd) {
            while (struct dirent* e = ::readdir(pd)) {
                if (e->d_name[0] == '.') continue;
                persist.push_back(e->d_name);
            }
            ::closedir(pd);
            std::sort(persist.begin(), persist.end());
        }
    }

    // The map also carries the box's own marker and the HaLow modem's
    // telemetry snapshot, so an empty discovery store is no longer an early
    // return: both are valid sources on their own.
    std::vector<std::string> names = live;
    for (const std::string& n : persist)
        if (std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    std::sort(names.begin(), names.end());

    std::string signature;
    if (use_cache) {
        signature = store_signature(store, live) + ":" +
                    store_signature(MAP_PERSIST, persist) + ":t" +
                    file_sig(store::TELEMETRY_FILE) + ":h" + file_sig(HALOW_STATE);
        std::string stamp = util::trim(util::read_file(MAP_STAMP, 128));
        std::string cached = util::read_file(MAP_CACHE, 4 * 1024 * 1024);
        if (stamp == signature && !cached.empty() && cached[0] == '{')
            return cached;
    }

    std::vector<Node> all;
    std::string first_error;
    for (const std::string& name : names) {
        MP rec;
        bool ok = load_record(store + "/" + name, rec);
        if (!ok && use_cache)
            ok = load_record(std::string(MAP_PERSIST) + "/" + name, rec);
        if (!ok) {
            if (first_error.empty()) first_error = name + ": invalid record";
            continue;
        }
        all.push_back(node_from(rec, name.c_str()));
    }

    // One physical node announces each interface under the same transport
    // identity. Keep the entry with coordinates; among those, the most
    // recently heard one.
    std::vector<Node> kept;
    size_t total = 0;
    for (const Node& n : all) {
        auto it = std::find_if(kept.begin(), kept.end(),
                               [&](const Node& k) { return k.id == n.id; });
        if (it == kept.end()) {
            kept.push_back(n);
            ++total;
            continue;
        }
        bool old_has = it->has_lat && it->has_lon;
        bool new_has = n.has_lat && n.has_lon;
        if (old_has && (!new_has || it->last_heard >= n.last_heard)) continue;
        *it = n;
    }

    std::vector<Node> out;
    for (const Node& n : kept)
        if (n.has_lat && n.has_lon) out.push_back(n);
    append_telemetry_nodes(out);
    std::stable_sort(out.begin(), out.end(), [](const Node& a, const Node& b) {
        return a.last_heard > b.last_heard;
    });

    if (out.empty() && first_error.empty())
        first_error = "No nodes with coordinates yet — enables a publish path above, "
                      "or wait for interface-discovery announces.";

    std::string body = "{\"nodes\":[";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) body += ",";
        append_node(body, out[i]);
    }
    body += "],\"total\":" + std::to_string(total) +
            ",\"store\":\"" + http::json_escape(store) + "\",\"error\":\"";
    body += http::json_escape(out.empty() ? first_error : std::string());
    body += "\"}";
    if (use_cache) {
        util::write_file_atomic(MAP_CACHE, body, 0600);
        util::write_file_atomic(MAP_STAMP, signature, 0600);
    }
    return body;
}

#endif  // _WIN32

void map_nodes(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.body = map_nodes_body();
}

// `rnsbox-portal mapnodes` — same JSON on stdout, for field diagnostics
// (timing the native reader without an authenticated browser).
void map_nodes_cli() {
    std::string body = map_nodes_body();
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fputc('\n', stdout);
}

}  // namespace routes
