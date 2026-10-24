// test_screen.cpp — unit tests for the server-side VT model and diff encoder.
#include <cstdio>
#include <cstring>
#include <string>

#include "term/screen.h"

using term::Cell;
using term::COLOR_DEFAULT;
using term::COLOR_INDEXED;
using term::COLOR_RGB;
using term::ScreenModel;
using term::Snap;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++g_pass; }                                              \
        else {                                                               \
            ++g_fail;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                   \
    do {                                                                     \
        std::string _a = (a), _b = (b);                                      \
        if (_a == _b) { ++g_pass; }                                          \
        else {                                                               \
            ++g_fail;                                                        \
            std::printf("FAIL %s:%d: got [%s] want [%s]\n", __FILE__,        \
                        __LINE__, _a.c_str(), _b.c_str());                   \
        }                                                                    \
    } while (0)

static void F(ScreenModel& m, const char* s) { m.feed(s, std::strlen(s)); }
static void F(ScreenModel& m, const std::string& s) { m.feed(s.data(), s.size()); }

static void test_basic() {
    ScreenModel m(10, 3);
    F(m, "hello");
    CHECK_EQ_STR(m.row_text(0), "hello");
    CHECK(m.cursor().x == 5 && m.cursor().y == 0);
    F(m, "\r\nworld");
    CHECK_EQ_STR(m.row_text(1), "world");
    CHECK(m.cursor().x == 5 && m.cursor().y == 1);
    F(m, "\x1b[2;3H");
    CHECK(m.cursor().x == 2 && m.cursor().y == 1);
    F(m, "\x1b[2J");
    CHECK_EQ_STR(m.row_text(0), "");
    CHECK_EQ_STR(m.row_text(1), "");
    CHECK(m.cursor().y == 1);  // ED 2 does not move the cursor
}

static void test_sgr() {
    ScreenModel m(20, 2);
    F(m, "\x1b[31;1mX");
    Cell c = m.cell(0, 0);
    CHECK(c.ch == 'X');
    CHECK(c.fg == (COLOR_INDEXED | 1));
    CHECK((c.attr & term::A_BOLD) != 0);
    F(m, "\x1b[0mY");
    c = m.cell(1, 0);
    CHECK(c.fg == COLOR_DEFAULT);
    CHECK(c.attr == 0);
    F(m, "\x1b[38;2;1;2;3mZ");
    c = m.cell(2, 0);
    CHECK(c.fg == (COLOR_RGB | 0x010203));
    F(m, "\x1b[48;5;200mA");
    c = m.cell(3, 0);
    CHECK(c.bg == (COLOR_INDEXED | 200));
    F(m, "\x1b[38:2::4:5:6mB");
    c = m.cell(4, 0);
    CHECK(c.fg == (COLOR_RGB | 0x040506));
}

static void test_wide() {
    ScreenModel m(10, 2);
    F(m, "a");
    F(m, "\xE4\xB8\xAD");  // U+4E2D, width 2
    F(m, "b");
    CHECK_EQ_STR(m.row_text(0), "a中b");
    CHECK(m.cursor().x == 4);
    CHECK(m.cell(1, 0).width == 2);
    CHECK(m.cell(2, 0).width == 0);
    CHECK(m.cell(3, 0).ch == 'b');
    // Overwrite the left half: the continuation must be cleared.
    F(m, "\x1b[1;2HZ");
    CHECK(m.cell(1, 0).ch == 'Z');
    CHECK(m.cell(1, 0).width == 1);
    CHECK(m.cell(2, 0).ch == ' ');
    CHECK(m.cell(2, 0).width == 1);
}

static void test_utf8_split() {
    ScreenModel m(10, 2);
    const char s[] = "\xE4\xB8\xAD";
    for (int i = 0; i < 3; ++i) m.feed(&s[i], 1);
    CHECK_EQ_STR(m.row_text(0), "中");
    CHECK(m.cell(0, 0).ch == 0x4E2D);
    CHECK(m.cell(0, 0).width == 2);
}

static void test_alt() {
    ScreenModel m(20, 3);
    F(m, "main");
    F(m, "\x1b[?1049h");
    CHECK(m.alt());
    CHECK_EQ_STR(m.row_text(0), "");
    F(m, "ALT");
    CHECK_EQ_STR(m.row_text(0), "ALT");
    F(m, "\x1b[?1049l");
    CHECK(!m.alt());
    CHECK_EQ_STR(m.row_text(0), "main");
}

static void test_modes() {
    ScreenModel m(20, 3);
    F(m, "\x1b[?1000h\x1b[?1006h\x1b[?1h\x1b[?2004h\x1b[?1004h\x1b[?25l\x1b[3 q");
    const term::ModeState& md = m.modes();
    CHECK(md.mouse_mode == 1000);
    CHECK(md.mouse_enc == 1006);
    CHECK(md.app_cursor);
    CHECK(md.bracketed_paste);
    CHECK(md.focus_report);
    CHECK(!m.cursor().visible);
    CHECK(m.cursor().style == 3);
    std::string j = ScreenModel::modes_json(md, m.cursor());
    CHECK(j.find("\"mouse\":1000") != std::string::npos);
    CHECK(j.find("\"enc\":1006") != std::string::npos);
    CHECK(j.find("\"cur\":false") != std::string::npos);
}

static void test_title() {
    ScreenModel m(20, 3);
    F(m, "\x1b]0;hello\x07");
    CHECK_EQ_STR(m.title(), "hello");
    uint64_t e = m.title_epoch();
    F(m, "\x1b]2;world\x1b\\");
    CHECK_EQ_STR(m.title(), "world");
    CHECK(m.title_epoch() == e + 1);
}

static void test_replies() {
    ScreenModel m(80, 24);
    F(m, "\x1b[5;7H\x1b[6n");
    CHECK_EQ_STR(m.take_replies(), "\x1b[5;7R");
    F(m, "\x1b[>c");
    CHECK_EQ_STR(m.take_replies(), "\x1b[>1;10;0c");
    F(m, "\x1b[18t");
    CHECK_EQ_STR(m.take_replies(), "\x1b[8;24;80t");
    F(m, "\x1b[?6n");
    CHECK_EQ_STR(m.take_replies(), "\x1b[?5;7R");
}

static void test_insert_delete() {
    ScreenModel m(10, 4);
    F(m, "abcdef\x1b[1;2H\x1b[2@");
    CHECK_EQ_STR(m.row_text(0), "a  bcdef");
    F(m, "\x1b[1;2H\x1b[2P");
    CHECK_EQ_STR(m.row_text(0), "abcdef");
    F(m, "\x1b[1;1Hline1\r\nline2\r\nline3");
    F(m, "\x1b[2;1H\x1b[L");
    CHECK_EQ_STR(m.row_text(1), "");
    CHECK_EQ_STR(m.row_text(2), "line2");
    F(m, "\x1b[2;1H\x1b[M");
    CHECK_EQ_STR(m.row_text(1), "line2");
}

static void test_scroll_region() {
    ScreenModel m(10, 5);
    F(m, "\x1b[1;1Hone\r\ntwo\r\nthree\r\nfour\r\nfive");
    F(m, "\x1b[2;4r");          // region rows 2..4
    F(m, "\x1b[4;1H");          // bottom of the region
    F(m, "\n");                 // scrolls rows 2..4 only
    CHECK_EQ_STR(m.row_text(0), "one");
    CHECK_EQ_STR(m.row_text(1), "three");
    CHECK_EQ_STR(m.row_text(2), "four");
    CHECK_EQ_STR(m.row_text(3), "");
    CHECK_EQ_STR(m.row_text(4), "five");
}

static void test_scroll_region_survives_alt() {
    ScreenModel m(10, 5);
    F(m, "\x1b[1;1Hone\r\ntwo\r\nthree\r\nfour\r\nfive");
    F(m, "\x1b[2;4r");          // main region rows 2..4
    F(m, "\x1b[?1049h");
    F(m, "\x1b[1;5r");          // the alt app changes its own region
    F(m, "\x1b[?1049l");
    // The main region must be back: scrolling at row 4 moves rows 2..4.
    F(m, "\x1b[4;1H\n");
    CHECK_EQ_STR(m.row_text(0), "one");
    CHECK_EQ_STR(m.row_text(1), "three");
    CHECK_EQ_STR(m.row_text(2), "four");
    CHECK_EQ_STR(m.row_text(3), "");
    CHECK_EQ_STR(m.row_text(4), "five");
}

static void test_origin_mode_cha() {
    ScreenModel m(20, 10);
    F(m, "\x1b[3;5r");          // region rows 3..5
    F(m, "\x1b[?6h");           // origin on
    F(m, "\x1b[2;10H");         // relative row 2 -> absolute row 4 (0-based 3)
    CHECK(m.cursor().y == 3 && m.cursor().x == 9);
    F(m, "\x1b[11G");           // CHA must not re-apply origin to the row
    CHECK(m.cursor().y == 3 && m.cursor().x == 10);
    F(m, "\x1b[1d");            // VPA is relative to the region
    CHECK(m.cursor().y == 2);
}

static void test_resize() {
    ScreenModel m(10, 3);
    F(m, "abcdefghij");
    F(m, "\x1b[3;4H");
    m.resize(5, 2);
    CHECK_EQ_STR(m.row_text(0), "abcde");
    CHECK(m.cursor().x == 3 && m.cursor().y == 1);
    CHECK(m.cols() == 5 && m.rows() == 2);

    // A wide character cut by the new right edge must not leave a dangling
    // continuation cell.
    ScreenModel w(10, 2);
    F(w, "\x1b[1;4H");
    F(w, "\xE4\xB8\xAD");  // cells 3,4
    w.resize(4, 2);
    CHECK(w.cell(3, 0).width != 2);
    CHECK(w.cell(3, 0).width != 0);
}

static void test_snapshot_roundtrip() {
    ScreenModel a(40, 8);
    F(a, "\x1b[?1049h\x1b[2J\x1b[1;1H\x1b[31;1mhello\x1b[0m");
    F(a, "\x1b[2;5H\xE4\xB8\xAD\x1b[48;5;17m \x1b[0m");
    F(a, "\x1b[4;3H\x1b[38;2;9;8;7mX");
    F(a, "\x1b[?1000h\x1b[?1006h\x1b[?2004h\x1b[?25l\x1b[4 q");
    Snap s = a.snapshot();
    std::string ansi = ScreenModel::snapshot_ansi(s);

    ScreenModel b(40, 8);
    F(b, ansi);
    Snap bs = b.snapshot();
    CHECK(bs == s);
    CHECK_EQ_STR(b.text(), a.text());
    CHECK(b.cursor() == a.cursor());
    CHECK(b.modes() == a.modes());
}

static void test_diff_roundtrip() {
    ScreenModel a(40, 8);
    F(a, "\x1b[?1049h\x1b[2J\x1b[1;1Hbase line\x1b[2;1Hsecond");
    Snap base = a.snapshot();

    F(a, "\x1b[1;6HCHANGED");
    F(a, "\x1b[3;10H\x1b[32mgreen\x1b[0m");
    F(a, "\x1b[?1000h\x1b[?1006h");
    F(a, "\x1b[7;1Hbottom");
    Snap cur = a.snapshot();

    bool need = false;
    std::string d = ScreenModel::diff_ansi(base, cur, &need);
    CHECK(!need);
    CHECK(!d.empty());

    ScreenModel b(40, 8);
    F(b, ScreenModel::snapshot_ansi(base));
    F(b, d);
    Snap bs = b.snapshot();
    CHECK(bs == cur);
    CHECK_EQ_STR(b.text(), a.text());
    CHECK(b.cursor() == a.cursor());
    CHECK(b.modes() == a.modes());
}

static void test_diff_alt_switch_needs_snapshot() {
    ScreenModel a(20, 4);
    Snap base = a.snapshot();
    F(a, "\x1b[?1049h");
    Snap cur = a.snapshot();
    bool need = false;
    std::string d = ScreenModel::diff_ansi(base, cur, &need);
    CHECK(need);
    CHECK(d.empty());
}

static void test_diff_size() {
    ScreenModel a(80, 24);
    F(a, "\x1b[?1049h\x1b[2J");
    for (int y = 1; y <= 24; ++y) {
        F(a, "\x1b[" + std::to_string(y) + ";1H");
        std::string row;
        for (int i = 0; i < 80; ++i) row += static_cast<char>('0' + (i % 10));
        F(a, row);
    }
    Snap base = a.snapshot();
    F(a, "\x1b[12;40HZ");
    Snap cur = a.snapshot();
    bool need = false;
    std::string d = ScreenModel::diff_ansi(base, cur, &need);
    CHECK(!need);
    // One cursor move, one SGR reset and one character: well under 32 bytes.
    CHECK(d.size() < 32);
}

static void test_scroll_diff_roundtrip() {
    ScreenModel a(20, 5);
    F(a, "one\r\ntwo\r\nthree\r\nfour\r\nfive");
    Snap base = a.snapshot();
    F(a, "\r\n");  // scroll the whole screen by one line
    Snap cur = a.snapshot();
    bool need = false;
    std::string d = ScreenModel::diff_ansi(base, cur, &need);
    CHECK(!need);
    ScreenModel b(20, 5);
    F(b, ScreenModel::snapshot_ansi(base));
    F(b, d);
    CHECK(b.snapshot() == cur);
}

int main() {
    test_basic();
    test_sgr();
    test_wide();
    test_utf8_split();
    test_alt();
    test_modes();
    test_title();
    test_replies();
    test_insert_delete();
    test_scroll_region();
    test_scroll_region_survives_alt();
    test_origin_mode_cha();
    test_resize();
    test_snapshot_roundtrip();
    test_diff_roundtrip();
    test_diff_alt_switch_needs_snapshot();
    test_diff_size();
    test_scroll_diff_roundtrip();
    std::printf("screen: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
