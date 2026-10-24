// routes_map.cpp — Map tab: receive-only view of nodes that announce
// coordinates (RNS interface discovery), rendered offline in the browser.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include <string>

using http::Request;
using http::Response;

namespace routes {

void map_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string h = render::page_file("map");
    h = web::replace_all(std::move(h), "__BASE__", render::esc(web::base()));
    res.body = render::layout(req, res, "Map", "map", h);
}

void map_nodes(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    util::RunResult rr = util::run(
        {"/usr/bin/python3", "/usr/lib/rnsbox/mapnodes.py"}, "", 15);
    std::string out = util::trim(rr.out);
    if (rr.exit_code == 0 && !out.empty() && out[0] == '{') {
        res.body = out;
        return;
    }
    res.body = "{\"nodes\":[],\"total\":0,\"error\":\"" +
               http::json_escape(out.empty() ? "map helper failed" : out) + "\"}";
}

}  // namespace routes
