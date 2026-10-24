// fake_tui.cpp — a tiny full-screen stand-in for NomadNet's urwid TUI.
//
// It sets up exactly the parts of a real TUI the fast path must handle:
// alternate screen, mouse reporting (SGR), Unicode box drawing, colours, a
// small periodic update (so diffs are exercised) and visible reactions to
// keyboard, arrow keys and mouse events.
//
// FAKE_TUI_FULL=1 makes every tick repaint the whole screen. That is the
// worst-case raw stream (a naive TUI) the benchmark compares against the
// broker's coalesced cell diffs.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

static void w(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(1, s.data() + off, s.size() - off);
        if (n <= 0) _exit(0);
        off += static_cast<size_t>(n);
    }
}
static void w(const char* s) { w(std::string(s)); }
static void at(int y, int x) {
    char b[32];
    snprintf(b, sizeof b, "\x1b[%d;%dH", y, x);
    w(b);
}

int main() {
    struct termios tio;
    if (tcgetattr(0, &tio) == 0) {
        tio.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
        tio.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &tio);
    }

    const bool full_redraw = getenv("FAKE_TUI_FULL") != nullptr;
    FILE* flog = nullptr;
    if (const char* lp = getenv("FAKE_TUI_LOG")) flog = fopen(lp, "a");
    std::string key;
    std::string arrow = "none";
    bool mouse = false;
    bool burst_done = false;
    unsigned long tick = 0;

    auto draw_tick = [&]() {
        at(11, 1);
        w("\x1b[33mtick ");
        char b[32];
        snprintf(b, sizeof b, "%lu\x1b[0m", tick);
        w(b);
    };

    auto draw_all = [&]() {
        w("\x1b[2J\x1b[H");
        w("\x1b[1;1H\x1b[1;32mRNSBox fast terminal test\x1b[0m");
        w("\x1b[3;1Hkey: ");
        w(key.empty() ? std::string("  ") : key);
        w("\x1b[4;1Hmouse: ");
        w(mouse ? "yes" : "no ");
        w("\x1b[5;1Harrow: ");
        w(arrow);
        w("      ");

        w("\x1b[7;1H\x1b[36m");
        w("\xE2\x95\xAD");                              // ╭
        for (int i = 0; i < 20; ++i) w("\xE2\x94\x80"); // ─
        w("\xE2\x95\xAE");                              // ╮
        w("\x1b[8;1H\xE2\x94\x82");                     // │
        for (int i = 0; i < 20; ++i) w(" ");
        w("\xE2\x94\x82");
        w("\x1b[9;1H\xE2\x95\xB0");                     // ╰
        for (int i = 0; i < 20; ++i) w("\xE2\x94\x80");
        w("\xE2\x95\xAF\x1b[0m");                       // ╯

        draw_tick();
        at(13, 1);
        w("\x1b[35mstatus: alive\x1b[0m");
        if (burst_done) {
            at(14, 1);
            w("\x1b[36mBURST done\x1b[0m");
        }
    };

    w("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l");
    // The Node client test needs an alt screen with mouse reporting off, so
    // its wheel-to-arrows fallback can be observed.
    if (!getenv("FAKE_TUI_NO_MOUSE")) w("\x1b[?1000h\x1b[?1006h");
    draw_all();

    std::string in;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        int rc = select(1, &rf, nullptr, nullptr, &tv);
        if (rc > 0) {
            char buf[4096];
            ssize_t n = read(0, buf, sizeof buf);
            if (n > 0) {
                in.append(buf, static_cast<size_t>(n));
                if (flog) {
                    fprintf(flog, "read %zd:", n);
                    for (ssize_t i = 0; i < n; ++i)
                        fprintf(flog, " %02x", static_cast<unsigned char>(buf[i]));
                    fprintf(flog, "\n");
                    fflush(flog);
                }
                if (in.find('q') != std::string::npos) {
                    w("\x1b[?1000l\x1b[?1006l\x1b[?1049l");
                    return 0;
                }
                // 100 back-to-back full repaints (~200 KB): the broker must
                // coalesce them, not queue 100 frames for the browser.
                size_t bp = in.find('b');
                if (bp != std::string::npos) {
                    in.erase(bp, 1);
                    for (int i = 0; i < 100; ++i) {
                        ++tick;
                        draw_all();
                    }
                    burst_done = true;
                    draw_all();
                }
                bool changed = false;
                // Mouse: CSI < ...
                for (;;) {
                    size_t p = in.find("\x1b[<");
                    if (p == std::string::npos) break;
                    mouse = true;
                    changed = true;
                    in.erase(p, 3);
                }
                // Arrows (normal and application cursor mode).
                struct Arrow { const char* seq; const char* name; };
                static const Arrow arrows[] = {
                    {"\x1b[A", "UP"},    {"\x1b[B", "DOWN"},
                    {"\x1b[C", "RIGHT"}, {"\x1b[D", "LEFT"},
                    {"\x1bOA", "UP"},    {"\x1bOB", "DOWN"},
                    {"\x1bOC", "RIGHT"}, {"\x1bOD", "LEFT"},
                };
                for (const Arrow& a : arrows) {
                    size_t p = in.find(a.seq);
                    if (p != std::string::npos) {
                        arrow = a.name;
                        changed = true;
                        in.erase(p, std::strlen(a.seq));
                    }
                }
                // Last printable byte becomes the visible key.
                for (char c : in) {
                    if (c >= 0x20 && c < 0x7F) {
                        key = std::string(1, c);
                        changed = true;
                    }
                }
                in.clear();
                if (flog) {
                    fprintf(flog, "changed=%d key=[%s]\n", changed ? 1 : 0, key.c_str());
                    fflush(flog);
                }
                if (changed) draw_all();
            }
        }
        ++tick;
        if (full_redraw) draw_all();
        else draw_tick();
        if (!full_redraw && tick % 20 == 0) {
            at(13, 1);
            w("\x1b[35mstatus: alive\x1b[0m");
        }
    }
}
