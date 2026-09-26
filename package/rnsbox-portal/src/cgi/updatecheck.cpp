// updatecheck.cpp — see updatecheck.h.
#include "updatecheck.h"
#include "util.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
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

// Installed rns version from the dist-info dir name (rns-X.Y.Z.dist-info).
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
    DIR* s = opendir(sp.c_str());
    if (!s) return "";
    std::string ver;
    while (dirent* e = readdir(s)) {
        std::string n = e->d_name;
        // "rns-1.5.2.dist-info"
        if (n.rfind("rns-", 0) == 0 && n.size() > 14 &&
            n.compare(n.size() - 10, 10, ".dist-info") == 0) {
            ver = n.substr(4, n.size() - 4 - 10);
            break;
        }
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
