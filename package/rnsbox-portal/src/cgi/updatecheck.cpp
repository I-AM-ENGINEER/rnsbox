// updatecheck.cpp — see updatecheck.h.
#include "updatecheck.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <vector>

namespace {

// Leading numeric components of a version -> int vector ("1.5.2" -> {1,5,2}).
std::vector<long> vtuple(const std::string& v) {
    std::vector<long> out;
    size_t i = 0;
    while (i < v.size()) {
        if (!isdigit((unsigned char)v[i])) break;
        long n = 0;
        while (i < v.size() && isdigit((unsigned char)v[i])) { n = n * 10 + (v[i] - '0'); ++i; }
        out.push_back(n);
        if (i < v.size() && v[i] == '.') ++i; else break;
    }
    return out;
}

bool newer(const std::string& latest, const std::string& installed) {
    if (latest.empty() || installed.empty()) return false;
    return vtuple(latest) > vtuple(installed);
}

// `__version__ = "1.5.2"` out of a _version.py (either quote style); "" if absent.
std::string version_py(const std::string& text) {
    size_t p = text.find("__version__");
    if (p == std::string::npos) return "";
    p = text.find('=', p);
    if (p == std::string::npos) return "";
    ++p;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p >= text.size() || (text[p] != '"' && text[p] != '\'')) return "";
    char q = text[p++];
    size_t e = text.find(q, p);
    if (e == std::string::npos || e - p > 32) return "";
    std::string v = text.substr(p, e - p);
    for (char c : v) if (!(isalnum((unsigned char)c) || c == '.' || c == '+' || c == '-')) return "";
    return v;
}

// Installed rns version. The package's own RNS/_version.py is authoritative;
// only if it's unreadable do we fall back to the metadata dir names — and then
// take the HIGHEST version among ALL of them, because an incremental buildroot
// build leaves stale records behind (seen on a shipped image: rns-1.2.6
// egg-info next to rns-1.5.2.dist-info; readdir order made either one "win").
std::string installed_version() {
    DIR* d = opendir("/usr/lib");
    if (!d) return "";
    std::string pydir;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "python3.", 8) == 0) { pydir = e->d_name; break; }
    }
    closedir(d);
    if (pydir.empty()) return "";
    std::string sp = "/usr/lib/" + pydir + "/site-packages";
    std::string ver = version_py(util::read_file(sp + "/RNS/_version.py", 4096));
    if (!ver.empty()) return ver;
    DIR* s = opendir(sp.c_str());
    if (!s) return "";
    while (dirent* e = readdir(s)) {
        std::string n = e->d_name;
        // "rns-1.5.2.dist-info" | "rns-1.2.6-py3.11.egg-info"
        if (n.size() < 5 || strncasecmp(n.c_str(), "rns-", 4) != 0) continue;
        std::string rest;
        if (n.size() > 14 && n.compare(n.size() - 10, 10, ".dist-info") == 0)
            rest = n.substr(4, n.size() - 4 - 10);
        else if (n.size() > 13 && n.compare(n.size() - 9, 9, ".egg-info") == 0)
            rest = n.substr(4, n.size() - 4 - 9);
        else
            continue;
        std::string v = rest.substr(0, rest.find('-'));   // drop "-py3.11"
        if (!v.empty() && isdigit((unsigned char)v[0]) && (ver.empty() || vtuple(v) > vtuple(ver)))
            ver = v;
    }
    closedir(s);
    return ver;
}

// Minimal extractor for a "key":"value" string field in our own JSON state file.
std::string json_str(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < j.size() && isspace((unsigned char)j[p])) ++p;
    if (p >= j.size() || j[p] != '"') return "";   // not a string value (e.g. null)
    ++p;
    std::string out;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) ++p;
        out += j[p++];
    }
    return out;
}

long json_int(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return 0;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    return strtol(j.c_str() + p + 1, nullptr, 10);
}

}  // namespace

namespace updatecheck {

State display() {
    State s;
    s.installed = installed_version();
    std::string j = util::read_file("/var/lib/rnsbox/update.json", 8192);
    if (!j.empty()) {
        s.latest = json_str(j, "latest");
        s.error = json_str(j, "error");
        s.checked_at = json_int(j, "checked_at");
    }
    s.update_available = newer(s.latest, s.installed);
    return s;
}

}  // namespace updatecheck
