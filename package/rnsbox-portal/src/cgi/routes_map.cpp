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
#include "util.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

void map_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string h = render::page_file("map");
    h = web::replace_all(std::move(h), "__BASE__", render::esc(web::base()));
    res.body = render::layout(req, res, "Map", "map", h);
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

std::string map_nodes_body() {
    std::string store = pick_store();
    DIR* dir = ::opendir(store.c_str());
    if (!dir) {
        return "{\"nodes\":[],\"total\":0,\"store\":\"" +
               http::json_escape(store) +
               "\",\"error\":\"interface discovery has not collected anything yet\"}";
    }

    std::vector<std::string> names;
    while (struct dirent* e = ::readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        names.push_back(e->d_name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());

    // RNSBOX_MAP_STORE is the diagnostics/test override; never mix its
    // output with the cache built from the real store.
    const bool use_cache = ::getenv("RNSBOX_MAP_STORE") == nullptr;
    std::string signature;
    if (use_cache) {
        signature = store_signature(store, names);
        std::string stamp = util::trim(util::read_file(MAP_STAMP, 128));
        std::string cached = util::read_file(MAP_CACHE, 4 * 1024 * 1024);
        if (stamp == signature && !cached.empty() && cached[0] == '{')
            return cached;
    }

    std::vector<Node> all;
    std::string first_error;
    for (const std::string& name : names) {
        MP rec;
        if (!load_record(store + "/" + name, rec)) {
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
    std::stable_sort(out.begin(), out.end(), [](const Node& a, const Node& b) {
        return a.last_heard > b.last_heard;
    });

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
