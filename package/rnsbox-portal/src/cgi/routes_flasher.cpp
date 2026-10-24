// routes_flasher.cpp — RNode HaLow flasher tab.
//
// Factory HaLow modems speak the HGIC protocol over raw Ethernet (ethertype
// 0x4847); already-flashed modems use the HTTP OTA path in the HaLow card.
// This tab discovers both kinds of device, and drives the raw flasher helper
// (halow_flasher.py) for factory devices: vendor preflash -> RNode image ->
// reboot -> optional LittleFS format + TFTP of the www/ files.
//
// The flasher helper runs detached and records progress in
// /run/rnsbox-halow-flasher.json; the page polls it. The firmware image is
// assembled by the existing OTA upload/fetch endpoints in
// /run/rnsbox-halow-ota.fw, so both tabs share one chunked-upload path.
#include "routes.h"
#include "render.h"
#include "web.h"
#include "util.h"
#include "http.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using http::Request;
using http::Response;

namespace routes {

#ifndef _WIN32

namespace {

constexpr const char* FW_HELPER = "/usr/lib/rnsbox/halow_flasher.py";
constexpr const char* FW_IMAGE  = "/run/rnsbox-halow-ota.fw";
constexpr const char* FW_STATE  = "/run/rnsbox-halow-flasher.json";
constexpr const char* FW_LOCK   = "/run/rnsbox-halow-flasher.lock";

bool iface_ok(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    for (unsigned char c : s)
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return s.find("..") == std::string::npos;
}

bool mac_ok(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i % 3 == 2) { if (s[i] != ':') return false; }
        else if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

std::string run_helper(const std::vector<std::string>& args, int timeout_s) {
    std::vector<std::string> argv = {"/usr/bin/python3", FW_HELPER};
    for (const auto& a : args) argv.push_back(a);
    util::RunResult rr = util::run(argv, "", timeout_s);
    std::string out = util::trim(rr.out);
    if (rr.exit_code != 0 || out.empty()) {
        return "{\"ok\":false,\"error\":\"" +
               http::json_escape(rr.timed_out ? "helper timed out" :
                                 (out.empty() ? "helper failed" : out)) + "\"}";
    }
    return out;
}

}  // namespace

void flasher_page(const Request& req, Response& res) {
    if (!web::require_auth(req, res, false)) return;
    std::string h = render::page_file("flasher");
    h = web::replace_all(std::move(h), "__BASE__", render::esc(web::base()));
    res.body = render::layout(req, res, "RNode HaLow flasher", "flasher", h);
}

void flasher_scan(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.body = run_helper({"scan"}, 15);
}

void flasher_status(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string s = util::trim(util::read_file(FW_STATE, 16384));
    res.body = (s.empty() || s[0] != '{') ? "{\"state\":\"idle\"}" : s;
}

void flasher_action(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string action = req.f("action");
    std::string iface = util::trim(req.f("iface"));
    std::string mac = util::trim(req.f("mac"));
    if (!iface_ok(iface) || !mac_ok(mac)) {
        res.status = "400 Bad Request";
        res.body = "{\"ok\":false,\"error\":\"bad interface or MAC\"}";
        return;
    }
    if (action == "ip" || action == "reboot" || action == "format") {
        res.body = run_helper({action, iface, mac}, action == "format" ? 70 : 15);
        return;
    }
    if (action != "flash") {
        res.status = "400 Bad Request";
        res.body = "{\"ok\":false,\"error\":\"unknown action\"}";
        return;
    }
    if (access(FW_LOCK, F_OK) == 0) {
        res.status = "409 Conflict";
        res.body = "{\"ok\":false,\"error\":\"an update is already running\"}";
        return;
    }
    struct stat st;
    if (stat(FW_IMAGE, &st) != 0 || st.st_size <= 0) {
        res.status = "400 Bad Request";
        res.body = "{\"ok\":false,\"error\":\"no firmware uploaded or downloaded\"}";
        return;
    }
    std::vector<std::string> args = {"flash", iface, mac, FW_IMAGE};
    if (req.f("preflash") == "on") args.push_back("--preflash");
    if (req.f("tftp") == "on") args.push_back("--tftp");

    util::write_file_atomic(FW_STATE,
        "{\"state\":\"starting\",\"stage\":\"queued\",\"done\":0,\"total\":0,"
        "\"pct\":0,\"error\":\"\",\"log\":[]}\n", 0644);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
        std::vector<char*> av;
        av.push_back(const_cast<char*>("/usr/bin/python3"));
        av.push_back(const_cast<char*>(FW_HELPER));
        for (const auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execv(av[0], av.data());
        _exit(127);
    }
    if (pid < 0) {
        res.status = "500 Internal Server Error";
        res.body = "{\"ok\":false,\"error\":\"could not start the flasher\"}";
        return;
    }
    res.status = "202 Accepted";
    res.body = "{\"ok\":true}";
}

void flasher_fetch(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    std::string tag = util::trim(req.f("tag"));
    if (tag.empty() || tag.size() > 40 ||
        tag.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
        res.status = "400 Bad Request";
        res.body = "{\"ok\":false,\"error\":\"bad release tag\"}";
        return;
    }
    util::RunResult rr = util::run(
        {"/usr/bin/python3", "/usr/lib/rnsbox/halow_fw.py", "fetch", tag, FW_IMAGE}, "", 120);
    struct stat st;
    if (rr.exit_code != 0 || stat(FW_IMAGE, &st) != 0 || st.st_size <= 0) {
        res.status = "502 Bad Gateway";
        std::string why = rr.timed_out ? "download timed out" : util::trim(rr.out);
        res.body = "{\"ok\":false,\"error\":\"" +
                   http::json_escape(why.empty() ? "download failed" : why) + "\"}";
        return;
    }
    res.body = "{\"ok\":true,\"bytes\":" + std::to_string((long long)st.st_size) + "}";
}

#else

void flasher_page(const Request& req, Response& res) {
    (void)req;
    res.status = "404 Not Found";
    res.body = "HaLow flasher is only available on the Linux appliance.";
}
void flasher_scan(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.body = "{\"devices\":[],\"error\":\"not available on this platform\"}";
}
void flasher_status(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.body = "{\"state\":\"idle\"}";
}
void flasher_action(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.status = "501 Not Implemented";
    res.body = "{\"ok\":false,\"error\":\"not available on this platform\"}";
}
void flasher_fetch(const Request& req, Response& res) {
    if (!web::require_auth(req, res, true)) return;
    res.content_type = "application/json";
    res.status = "501 Not Implemented";
    res.body = "{\"ok\":false,\"error\":\"not available on this platform\"}";
}

#endif

}  // namespace routes
