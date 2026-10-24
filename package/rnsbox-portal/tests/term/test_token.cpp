// test_token.cpp — HMAC token minting/validation.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "term/token.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (c) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

int main() {
    char dir[] = "/tmp/rnsbox-token-XXXXXX";
    char* d = mkdtemp(dir);
    if (!d) { std::printf("mkdtemp failed\n"); return 1; }
    std::string p = std::string(d) + "/secret";
    unlink(p.c_str());

    CHECK(!term::check_token(p, "nomadnet", "1.2.3"));
    CHECK(term::make_token(p, "nomadnet", 60).empty());

    CHECK(term::ensure_secret(p));
    struct stat st;
    CHECK(stat(p.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    std::string t = term::make_token(p, "nomadnet", 60);
    CHECK(!t.empty());
    CHECK(term::check_token(p, "nomadnet", t));
    CHECK(!term::check_token(p, "shell", t));

    std::string bad = t;
    bad[bad.size() - 1] = (bad[bad.size() - 1] == '0') ? '1' : '0';
    CHECK(!term::check_token(p, "nomadnet", bad));

    CHECK(!term::check_token(p, "nomadnet", "not-a-token"));
    CHECK(!term::check_token(p, "nomadnet", "1.2"));

    std::string expired = term::make_token(p, "nomadnet", -10000);
    CHECK(!expired.empty());
    CHECK(!term::check_token(p, "nomadnet", expired));

    unlink(p.c_str());
    rmdir(d);
    std::printf("token: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
