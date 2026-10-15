// routes_access.cpp — "Remote access (rnsh)" tab: issue/revoke downloadable
// rnsh client access files.
//
// The rnsh server itself (S86rnsh + /etc/rnsbox/rnsh.conf) ships with the
// images and is also manually editable on the Reticulum tab. This tab
// automates the client side: minting a FRESH Reticulum Identity per access,
// adding its public hash to the allow-list, and handing the user a one-time
// plain-text file with the private key + copy-paste connect instructions.
// Key model: the private key is never persisted on the box — it exists once,
// in the helper's memory and then in this HTTP response (see
// local_docs/rnsh-access.md). Lost file => revoke & re-issue.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using http::Request;
using http::Response;

namespace routes {

namespace {

constexpr const char* RNSH_CONF    = "/etc/rnsbox/rnsh.conf";       // shared with routes_reticulum.cpp
constexpr const char* ACCESS_LOG   = "/etc/rnsbox/rnsh_access.conf"; // issuance journal
constexpr const char* HELPER       = "/usr/lib/rnsbox/rnshaccess.py";
constexpr const char* PIDFILE      = "/var/run/rnsh.pid";
constexpr const char* HOSTNAME_SRC = "/proc/sys/kernel/hostname";
constexpr const char* MARK         = "RNSBOX-ACCESS/8";
constexpr const char* END_MARK     = "RNSBOX-ACCESS-END";
constexpr int  MAX_IDS  = 64;   // keep in sync with reticulum_rnsh_save
constexpr int  MAX_GRANTS = 256;

std::string R(std::string s, const std::string& a, const std::string& b) { return web::replace_all(std::move(s), a, b); }
std::string E(const std::string& s) { return render::esc(s); }
std::string access_redirect() { return web::base() + "/access"; }

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool is_hex(const std::string& s) {
    if (s.size() < 16 || s.size() > 64) return false;   // rnsh accepts 16..64 hex (S86rnsh conf parser)
    return s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// normalize a hash input line: strip comments/whitespace, lowercase; "" if invalid
std::string norm_hash(const std::string& in) {
    std::string ln = util::trim(in);
    size_t h = ln.find('#');
    if (h != std::string::npos) ln = util::trim(ln.substr(0, h));
    ln = lower(ln);
    if (!is_hex(ln)) return "";
    return ln;
}

// ---- /etc/rnsbox/rnsh.conf — the same file the Reticulum tab writes ----
struct RnshConf {
    bool enabled = false;
    std::string announce = "600";
    std::vector<std::string> ids;   // lowercase hex, in file order
};

RnshConf read_rnsh_conf() {
    RnshConf c;
    std::string body = util::read_file(RNSH_CONF, 16384);
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        std::string ln = util::trim(body.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        if (ln.empty()) continue;
        size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::trim(ln.substr(0, eq));
        std::string v = util::trim(ln.substr(eq + 1));
        if (k == "enabled") c.enabled = (lower(v) == "yes" || lower(v) == "true" || v == "1" || lower(v) == "on");
        else if (k == "announce") {
            if (!v.empty() && v.find_first_not_of("0123456789") == std::string::npos) c.announce = v;
        } else if (k == "allowed_id") {
            std::string h = norm_hash(v);
            if (!h.empty()) c.ids.push_back(h);
        }
    }
    return c;
}

bool write_rnsh_conf(const RnshConf& c) {
    std::string o = std::string("enabled=") + (c.enabled ? "yes" : "no") + "\nannounce=" + c.announce + "\n";
    for (const auto& h : c.ids) o += "allowed_id=" + h + "\n";
    return util::write_file_atomic(RNSH_CONF, o, 0600);
}

// ---- issuance journal: one `grant=<hash> issued=<epoch> revoked=<epoch|0>` per line ----
struct Grant {
    std::string hash;        // 32 lowercase hex
    long long issued = 0;
    long long revoked = 0;   // 0 = active
};

std::vector<Grant> read_grants() {
    std::vector<Grant> out;
    std::string body = util::read_file(ACCESS_LOG, 64 * 1024);
    size_t i = 0;
    while (i <= body.size() && out.size() < MAX_GRANTS) {
        size_t nl = body.find('\n', i);
        std::string ln = util::trim(body.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        if (ln.empty() || ln[0] == '#') continue;
        Grant g;
        size_t p = 0;
        while (p < ln.size()) {
            size_t sp = ln.find(' ', p);
            std::string tok = util::trim(ln.substr(p, sp == std::string::npos ? std::string::npos : sp - p));
            p = (sp == std::string::npos) ? ln.size() : sp + 1;
            size_t eq = tok.find('=');
            if (eq == std::string::npos) continue;
            std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
            if (k == "grant") g.hash = lower(v);
            else if (k == "issued") g.issued = strtoll(v.c_str(), nullptr, 10);
            else if (k == "revoked") g.revoked = strtoll(v.c_str(), nullptr, 10);
        }
        if (g.hash.size() == 32 && g.hash.find_first_not_of("0123456789abcdef") == std::string::npos)
            out.push_back(g);
    }
    return out;
}

bool write_grants(const std::vector<Grant>& gs) {
    std::string o = "# rnsh access grants — one grant=<identity hash> per line.\n"
                    "# Private keys are NOT stored here (see local_docs/rnsh-access.md).\n";
    for (const auto& g : gs)
        o += "grant=" + g.hash + " issued=" + std::to_string(g.issued) +
             " revoked=" + std::to_string(g.revoked) + "\n";
    return util::write_file_atomic(ACCESS_LOG, o, 0600);
}

bool rnsh_running() {
    std::string pid = util::trim(util::read_file(PIDFILE, 32));
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) return false;
    std::string comm = util::trim(util::read_file("/proc/" + pid + "/comm", 64));
    return comm == "rnsh" || comm.rfind("python", 0) == 0;
}

std::string utc_date(long long epoch) {
    char buf[32];
    time_t t = (time_t)epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv);
    return std::string(buf);
}

std::string node_name() { return util::trim(util::read_file(HOSTNAME_SRC, 128)); }

// ---- helper call: mint a fresh client identity ----
struct KeyMaterial {
    std::string hash, keyb64, dest;
    bool ok = false;
};

bool b64_ok(const std::string& s) {
    // 64 bytes => 44 chars, standard alphabet, "==" padding
    if (s.size() != 44 || s.substr(42) != "==") return false;
    return s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") == std::string::npos;
}

KeyMaterial run_issue_helper() {
    KeyMaterial km;
    util::RunResult rr = util::run({"/usr/bin/python3", HELPER, "issue"}, "", 30);
    size_t b = rr.out.find(MARK);
    if (b == std::string::npos) return km;
    b += strlen(MARK);
    size_t e = rr.out.find(END_MARK, b);
    if (e == std::string::npos) return km;
    std::string body = rr.out.substr(b, e - b);
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        std::string ln = util::trim(body.substr(i, nl == std::string::npos ? std::string::npos : nl - i));
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        size_t eq = ln.find('=');
        if (eq == std::string::npos) continue;
        std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
        if (k == "HASH") km.hash = lower(v);
        else if (k == "KEYB64") km.keyb64 = v;
        else if (k == "DEST") km.dest = lower(v);
    }
    km.ok = is_hex(km.hash) && km.hash.size() == 32 &&
            is_hex(km.dest) && km.dest.size() == 32 && b64_ok(km.keyb64);
    return km;
}

// ---- the downloadable file ----
std::string access_file_text(const KeyMaterial& km) {
    std::string o;
    o += "RNSBox — файл удалённого доступа (rnsh)\n";
    o += "========================================\n";
    o += "Нода:      " + node_name() + "\n";
    o += "Выдан:     " + utc_date(time(nullptr)) + "\n";
    o += "Хеш ключа: " + km.hash + "   (identity клиента, добавлен в allow-лист ноды)\n";
    o += "Адресат:   " + km.dest + "   (destination rnsh-сервера ноды)\n";
    o += "\n";
    o += "ВАЖНО. В файле единственная копия приватного ключа. Роутер ключ не хранит:\n";
    o += "повторно скачать этот файл нельзя. Сохрани его в надёжное место. Если файл\n";
    o += "потерян — отзови доступ и выдай новый («Revoke & re-issue» в веб-морде ноды),\n";
    o += "после чего этот файл перестаёт работать.\n";
    o += "\n";
    o += "1) Установка (любой Linux с python3):\n";
    o += "   pip install rns\n";
    o += "   (команда rnsh входит в пакет rns; версия протокола совпадает с сервером)\n";
    o += "\n";
    o += "2) Сохрани приватный ключ клиента в файл (base64 от 64-байтового ключа\n";
    o += "   Reticulum Identity). Путь можно любой — он используется в шагах 3-4:\n";
    o += "   umask 077\n";
    o += "   cat > \"$HOME/rnsbox-access.key\" <<'EOF'\n";
    o += km.keyb64 + "\n";
    o += "   EOF\n";
    o += "\n";
    o += "3) Проверка ключа (напечатанный хеш должен совпасть с «Хеш ключа»):\n";
    o += "   rnsh -i \"$HOME/rnsbox-access.key\" -p\n";
    o += "\n";
    o += "4) Подключение из любого места, где есть Reticulum-связность с нодой:\n";
    o += "   rnsh -i \"$HOME/rnsbox-access.key\" " + km.dest + "\n";
    o += "   Выход из сессии: точка-тильда (~.) на отдельной строке.\n";
    o += "\n";
    o += "Примечания:\n";
    o += "- Связность: нода должна быть видна по Reticulum — напрямую (LAN/WAN-интерфейс\n";
    o += "  ноды) либо через транспорт (свой/публичный RNS-сервер, RNode-мост и т.п.).\n";
    o += "  Первый поиск пути может занять до минуты — rnsh сам запросит path.\n";
    o += "- Если соединение не устанавливается: rnpath " + km.dest + " покажет, известен\n";
    o += "  ли маршрут; rnstatus — состояние интерфейсов.\n";
    o += "- Сервер доступа на ноде: /etc/init.d/S86rnsh, allow-лист: /etc/rnsbox/rnsh.conf.\n";
    return o;
}

void send_access_file(Response& res, const KeyMaterial& km) {
    res.status = "200 OK";
    res.content_type = "text/plain; charset=utf-8";
    res.headers.push_back("Content-Disposition: attachment; filename=\"rnsbox-access-" +
                          km.hash.substr(0, 8) + ".txt\"");
    res.headers.push_back("Cache-Control: no-store");
    res.body = access_file_text(km);
}

// Remove one hash from the conf allow-list. Returns true if it was there.
bool conf_drop_id(RnshConf& c, const std::string& hash) {
    bool had = false;
    std::vector<std::string> keep;
    for (const auto& h : c.ids) {
        if (h == hash) { had = true; continue; }
        keep.push_back(h);
    }
    c.ids = keep;
    return had;
}

// Issue one access: helper -> conf -> journal -> restart -> file response.
// Returns false (after a flash redirect) on failure.
bool issue_access(Response& res) {
    KeyMaterial km = run_issue_helper();
    if (!km.ok) {
        render::redirect_flash(res, access_redirect(), "error",
            "rnsh: key generation failed — is the rns package installed?");
        return false;
    }
    RnshConf c = read_rnsh_conf();
    bool had = conf_drop_id(c, km.hash);   // dedupe: a fresh key never reuses a hash, but stay safe
    c.ids.push_back(km.hash);
    if ((int)c.ids.size() > MAX_IDS) {
        render::redirect_flash(res, access_redirect(), "error",
            "rnsh: allow-list is full (64) — revoke an access first.");
        return false;
    }
    c.enabled = true;   // first issuance turns the server on
    if (!write_rnsh_conf(c)) {
        render::redirect_flash(res, access_redirect(), "error",
            "rnsh: could not write " + std::string(RNSH_CONF) + ".");
        return false;
    }
    std::vector<Grant> gs = read_grants();
    for (auto& g : gs) if (g.hash == km.hash) g.revoked = time(nullptr); // journal dedupe
    gs.push_back(Grant{km.hash, (long long)time(nullptr), 0});
    if (!write_grants(gs)) {
        // conf already has the hash — the shell works; journal is bookkeeping.
        // Still restart so the running server picks the new allow-list up.
        web::run_init("S86rnsh", "restart");
        render::redirect_flash(res, access_redirect(), "warn",
            "rnsh: access issued but the journal could not be written (file downloaded anyway?).");
        return false;
    }
    web::run_init("S86rnsh", "restart");
    (void)had;
    send_access_file(res, km);
    return true;
}

}  // namespace

void access_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    RnshConf c = read_rnsh_conf();
    std::vector<Grant> gs = read_grants();
    std::string dest = util::trim(util::read_file("/run/rnsh-dest", 128));

    // Status line
    std::string status;
    if (rnsh_running()) status = "<span class=\"badge ok\">running</span>";
    else if (c.enabled) status = "<span class=\"badge warn\">enabled, not running</span> "
                                 "<span class=\"muted\">(starts with rnsd; see the Reticulum tab)</span>";
    else status = "<span class=\"badge\">off</span> <span class=\"muted\">issue an access to enable it</span>";

    // Grants table rows (newest first)
    std::string rows, manual_rows;
    for (size_t gi = gs.size(); gi-- > 0;) {
        const Grant& g = gs[gi];
        bool active = g.revoked == 0;
        std::string acts;
        if (active) {
            acts += "<form class=\"inline\" method=\"post\" action=\"" + access_redirect() +
                    "/reissue\"><input type=\"hidden\" name=\"hash\" value=\"" + g.hash +
                    "\"><button type=\"submit\" class=\"btn\">Revoke &amp; re-issue</button></form> ";
            acts += "<form class=\"inline\" method=\"post\" action=\"" + access_redirect() +
                    "/revoke\"><input type=\"hidden\" name=\"hash\" value=\"" + g.hash +
                    "\"><button type=\"submit\" class=\"btn\">Revoke</button></form>";
        }
        rows += "<tr><td class=\"mono\">" + E(g.hash.substr(0, 16)) + "&hellip;</td>" +
                "<td>" + utc_date(g.issued) + "</td>" +
                "<td>" + (active ? "<span class=\"badge ok\">active</span>"
                                 : "<span class=\"badge\">revoked</span>") + "</td>" +
                "<td>" + acts + "</td></tr>";
    }
    if (rows.empty())
        rows = "<tr><td colspan=\"4\" class=\"muted\">No issued accesses yet — issue one below.</td></tr>";

    // Manual allow-list entries: in conf but never issued here (e.g. typed on
    // the Reticulum tab from the user's own `rnsh -p`).
    for (const auto& h : c.ids) {
        bool granted = false;
        for (const auto& g : gs) if (g.hash == h) { granted = true; break; }
        if (granted) continue;
        std::string shown = h.size() > 16 ? E(h.substr(0, 16)) + "&hellip;" : E(h);
        manual_rows += "<tr><td class=\"mono\">" + shown +
                       "</td><td>" +
                       "<form class=\"inline\" method=\"post\" action=\"" + access_redirect() +
                       "/revoke\"><input type=\"hidden\" name=\"hash\" value=\"" + h +
                       "\"><button type=\"submit\" class=\"btn\">Revoke</button></form>" +
                       "</td></tr>";
    }
    if (manual_rows.empty())
        manual_rows = "<tr><td class=\"muted\">none</td></tr>";

    std::string h = render::page_file("access");
    h = R(std::move(h), "__BASE__", web::base());
    h = R(std::move(h), "__A_STATUS__", status);
    h = R(std::move(h), "__A_DEST__", E(dest.empty() ? "(not started)" : dest));
    h = R(std::move(h), "__A_ANNOUNCE__", E(c.announce));
    h = R(std::move(h), "__A_GRANTS__", rows);
    h = R(std::move(h), "__A_MANUAL__", manual_rows);
    res.body = render::layout(req, res, "Remote access", "access", h);
}

void access_issue(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    issue_access(res);
}

void access_revoke(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string hash = norm_hash(req.f("hash"));
    if (hash.empty()) {
        render::redirect_flash(res, access_redirect(), "error", "rnsh: invalid hash.");
        return;
    }
    RnshConf c = read_rnsh_conf();
    bool had = conf_drop_id(c, hash);
    std::vector<Grant> gs = read_grants();
    bool marked = false;
    for (auto& g : gs) if (g.hash == hash && g.revoked == 0) { g.revoked = time(nullptr); marked = true; }
    if (!had && !marked) {
        render::redirect_flash(res, access_redirect(), "error",
            "rnsh: hash not found in the allow-list or journal.");
        return;
    }
    if (c.ids.empty()) c.enabled = false;   // S86rnsh refuses an empty allow-list; keep conf honest
    if (!write_rnsh_conf(c) || !write_grants(gs)) {
        render::redirect_flash(res, access_redirect(), "error", "rnsh: could not write the config.");
        return;
    }
    web::run_init("S86rnsh", "restart");
    std::string msg = had ? std::string("Access revoked — the downloaded file no longer works.")
                          : std::string("Journal entry revoked.");
    render::redirect_flash(res, access_redirect(), "success", msg);
}

void access_reissue(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string hash = norm_hash(req.f("hash"));
    if (hash.empty()) {
        render::redirect_flash(res, access_redirect(), "error", "rnsh: invalid hash.");
        return;
    }
    RnshConf c = read_rnsh_conf();
    bool had = conf_drop_id(c, hash);
    std::vector<Grant> gs = read_grants();
    bool known = had, was_active = false;
    for (auto& g : gs) {
        if (g.hash != hash) continue;
        known = true;
        if (g.revoked == 0) { g.revoked = time(nullptr); was_active = true; }
    }
    if (was_active && !write_grants(gs)) {
        render::redirect_flash(res, access_redirect(), "error", "rnsh: could not write the journal.");
        return;
    }
    if (!known) {
        render::redirect_flash(res, access_redirect(), "error",
            "rnsh: hash not found — issue a new access instead.");
        return;
    }
    if (!write_rnsh_conf(c)) {
        render::redirect_flash(res, access_redirect(), "error", "rnsh: could not write the config.");
        return;
    }
    issue_access(res);
}

}  // namespace routes
