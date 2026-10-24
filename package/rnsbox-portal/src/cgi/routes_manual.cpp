// routes_manual.cpp — bundled Reticulum manual.
//
// Reading happens in the browser: /www/manual/ is a static page that fetches
// the Markdown sources and renders them with marked.js, so page views cost the
// box nothing.  This route is the small update/version card, and the two POST
// actions call /usr/lib/rnsbox/manual.py (rngit / HTTPS check + download).
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include "json.h"
#include <string>
#include <vector>

using http::Request;
using http::Response;

namespace routes {

static std::string E(const std::string& s) { return render::esc(s); }

static bool docs_run(const std::vector<std::string>& args, json::Value& out) {
    std::vector<std::string> cmd = {"/usr/bin/python3", "/usr/lib/rnsbox/manual.py"};
    for (const auto& a : args) cmd.push_back(a);
    util::RunResult r = util::run(cmd, "", 180);
    std::string err;
    return json::parse(r.out, out, err);
}

static std::string jstr(const json::Value& v, const char* key, const char* dflt = "") {
    const json::Value* p = v.find(key);
    return (p && p->is_string()) ? p->as_string() : std::string(dflt);
}

void manual_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string ver = util::trim(util::read_file("/www/manual/docs/VERSION", 128));
    if (ver.empty()) ver = "unknown";
    std::string update;
    std::string st = util::read_file("/run/rnsbox-docs.json", 4096);
    if (!st.empty()) {
        json::Value v;
        std::string err;
        if (json::parse(st, v, err)) update = jstr(v, "result");
    }
    std::string h = render::page_file("manual");
    h = web::replace_all(std::move(h), "__DOCS_VERSION__", E(ver));
    h = web::replace_all(std::move(h), "__DOCS_UPDATE__", E(update));
    res.body = render::layout(req, res, "Manual", "manual", h);
}

static void docs_action(const Request& req, Response& res, const char* cmd) {
    if (!web::require_auth(req, res, false)) return;
    json::Value v;
    bool parsed = docs_run({cmd}, v);
    std::string msg = parsed ? jstr(v, "message", "No result.")
                             : "The documentation helper failed.";
    bool ok = parsed && v.find("ok") && v.find("ok")->as_bool();
    render::redirect_flash(res, web::base() + "/manual",
                           ok ? "success" : "error", msg);
}

void manual_check(const Request& req, Response& res)    { docs_action(req, res, "check"); }
void manual_download(const Request& req, Response& res) { docs_action(req, res, "download"); }

}  // namespace routes
