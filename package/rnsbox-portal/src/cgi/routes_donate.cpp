// routes_donate.cpp — Donate tab: static wallet cards + QR codes.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "donate.h"
#include <cctype>
#include <string>

using http::Request;
using http::Response;

namespace routes {

static std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

void donate_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string cards;
    for (int i = 0; i < donate::WALLET_COUNT; ++i) {
        const donate::Wallet& w = donate::WALLETS[i];
        std::string uri = std::string(w.uri) + ":" + w.address;
        std::string qr = "/qr/" + lower(w.ticker) + ".svg";
        cards +=
            "<section class=\"card wallet\">"
            "<div class=\"wallet-head\">"
            "<span class=\"coin-sym\" aria-hidden=\"true\">" + std::string(w.symbol) + "</span>"
            "<span class=\"coin-name\">" + render::esc(w.name) + "</span>"
            "<span class=\"badge tcp coin-ticker\">" + render::esc(w.ticker) + "</span></div>"
            "<a class=\"qr-link\" href=\"" + render::esc(uri) + "\" title=\"Open in a " + render::esc(w.name) + " wallet\">"
            "<img class=\"qr\" src=\"" + qr + "\" width=\"160\" height=\"160\" alt=\"" + render::esc(w.name) + " donation QR code\" loading=\"lazy\"></a>"
            "<code class=\"wallet-addr\" id=\"addr-" + render::esc(w.ticker) + "\">" + render::esc(w.address) + "</code>"
            "<div class=\"wallet-actions\">"
            "<button type=\"button\" class=\"btn copy-btn\" data-addr=\"" + render::esc(w.address) + "\" data-label=\"Copy address\">Copy address</button>"
            "<a class=\"btn\" href=\"" + render::esc(uri) + "\">Open in wallet</a></div></section>";
    }
    std::string h = render::page_file("donate");
    h = web::replace_all(std::move(h), "__WALLET_CARDS__", cards);
    res.body = render::layout(req, res, "Donate", "donate", h);
}

}  // namespace routes
