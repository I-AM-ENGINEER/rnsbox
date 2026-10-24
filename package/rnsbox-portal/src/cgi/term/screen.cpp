// screen.cpp — VT parser, screen grid and diff/snapshot encoder.
#include "screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "width_tables.h"

namespace term {

namespace {

bool in_ranges(const Range* rs, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < rs[mid].lo) hi = mid;
        else if (cp > rs[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

int width_of(uint32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return 0;
    if (in_ranges(kZeroWidth, kZeroWidth_count, cp)) return 0;
    if (in_ranges(kWideWidth, kWideWidth_count, cp)) return 2;
    return 1;
}

void append_uint(std::string& out, long v) {
    char b[24];
    int n = snprintf(b, sizeof b, "%ld", v);
    if (n > 0) out.append(b, static_cast<size_t>(n));
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct Pen {
    uint32_t fg = COLOR_DEFAULT;
    uint32_t bg = COLOR_DEFAULT;
    uint16_t attr = 0;
    bool operator==(const Pen& o) const {
        return fg == o.fg && bg == o.bg && attr == o.attr;
    }
};

void append_pen(std::string& out, const Pen& p) {
    out += "\x1b[0";
    if (p.attr & A_BOLD) out += ";1";
    if (p.attr & A_DIM) out += ";2";
    if (p.attr & A_ITALIC) out += ";3";
    if (p.attr & A_UNDERLINE) out += ";4";
    if (p.attr & A_BLINK) out += ";5";
    if (p.attr & A_REVERSE) out += ";7";
    if (p.attr & A_HIDDEN) out += ";8";
    if (p.attr & A_STRIKE) out += ";9";
    if (p.fg != COLOR_DEFAULT) {
        if ((p.fg & 0xFF000000u) == COLOR_INDEXED) {
            out += ";38;5;";
            append_uint(out, static_cast<long>(p.fg & 0xFF));
        } else {
            out += ";38;2;";
            append_uint(out, static_cast<long>((p.fg >> 16) & 0xFF));
            out += ';';
            append_uint(out, static_cast<long>((p.fg >> 8) & 0xFF));
            out += ';';
            append_uint(out, static_cast<long>(p.fg & 0xFF));
        }
    }
    if (p.bg != COLOR_DEFAULT) {
        if ((p.bg & 0xFF000000u) == COLOR_INDEXED) {
            out += ";48;5;";
            append_uint(out, static_cast<long>(p.bg & 0xFF));
        } else {
            out += ";48;2;";
            append_uint(out, static_cast<long>((p.bg >> 16) & 0xFF));
            out += ';';
            append_uint(out, static_cast<long>((p.bg >> 8) & 0xFF));
            out += ';';
            append_uint(out, static_cast<long>(p.bg & 0xFF));
        }
    }
    out += 'm';
}

void append_mode_changes(std::string& out, const ModeState& a, const CursorState& ac,
                         const ModeState& b, const CursorState& bc, bool full) {
    if (full || a.mouse_mode != b.mouse_mode) {
        out += "\x1b[?1000l\x1b[?1002l\x1b[?1003l";
        if (b.mouse_mode) {
            out += "\x1b[?";
            append_uint(out, b.mouse_mode);
            out += 'h';
        }
    }
    if (full || a.mouse_enc != b.mouse_enc) {
        out += "\x1b[?1005l\x1b[?1006l\x1b[?1015l";
        if (b.mouse_enc) {
            out += "\x1b[?";
            append_uint(out, b.mouse_enc);
            out += 'h';
        }
    }
    if (full || a.focus_report != b.focus_report)
        out += b.focus_report ? "\x1b[?1004h" : "\x1b[?1004l";
    if (full || a.bracketed_paste != b.bracketed_paste)
        out += b.bracketed_paste ? "\x1b[?2004h" : "\x1b[?2004l";
    if (full || a.app_cursor != b.app_cursor)
        out += b.app_cursor ? "\x1b[?1h" : "\x1b[?1l";
    if (full || a.app_keypad != b.app_keypad)
        out += b.app_keypad ? "\x1b[?66h" : "\x1b[?66l";
    if (full || a.cursor_blink != b.cursor_blink)
        out += b.cursor_blink ? "\x1b[?12h" : "\x1b[?12l";
    if (full || ac.visible != bc.visible)
        out += bc.visible ? "\x1b[?25h" : "\x1b[?25l";
    if (full || ac.style != bc.style) {
        out += "\x1b[";
        append_uint(out, bc.style < 0 ? 0 : (bc.style > 6 ? 0 : bc.style));
        out += " q";
    }
    if (full) out += "\x1b[?7h";   // autowrap: the diff writer never overruns
}

void resize_cells(std::vector<Cell>& cells, int oldc, int oldr, int newc, int newr,
                  const Cell& fill) {
    std::vector<Cell> nc(static_cast<size_t>(newc) * static_cast<size_t>(newr), fill);
    int copy_rows = std::min(oldr, newr);
    int copy_cols = std::min(oldc, newc);
    for (int y = 0; y < copy_rows; ++y) {
        for (int x = 0; x < copy_cols; ++x)
            nc[static_cast<size_t>(y) * newc + x] =
                cells[static_cast<size_t>(y) * oldc + x];
        if (copy_cols > 0 && copy_cols == newc) {
            Cell& last = nc[static_cast<size_t>(y) * newc + (newc - 1)];
            if (last.width == 0)
                last = fill;
            else if (last.width == 2)
                last = fill;
        }
    }
    cells.swap(nc);
}

}  // namespace

Cell ScreenModel::blank_with(uint32_t bg) const {
    Cell c;
    c.ch = ' ';
    c.width = 1;
    c.fg = COLOR_DEFAULT;
    c.bg = bg;
    c.attr = 0;
    return c;
}

ScreenModel::ScreenModel(int cols, int rows) {
    cols_ = std::max(1, std::min(cols, 1000));
    rows_ = std::max(1, std::min(rows, 1000));
    cells_.assign(static_cast<size_t>(cols_) * rows_, blank());
    tabs_.assign(static_cast<size_t>(cols_), 0);
    for (int x = 0; x < cols_; x += 8) tabs_[x] = 1;
    scroll_bottom_ = rows_ - 1;
}

void ScreenModel::reset() {
    cells_.assign(static_cast<size_t>(cols_) * rows_, blank());
    cur_ = CursorState();
    modes_ = ModeState();
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    tabs_.assign(static_cast<size_t>(cols_), 0);
    for (int x = 0; x < cols_; x += 8) tabs_[x] = 1;
    wrap_pending_ = false;
    pen_fg_ = COLOR_DEFAULT;
    pen_bg_ = COLOR_DEFAULT;
    pen_attr_ = 0;
    replies_.clear();
    title_.clear();
    state_ = S_GROUND;
    params_.clear();
    cur_param_ = -1;
    param_started_ = false;
    priv_ = 0;
    inter_.clear();
    str_.clear();
    utf8_need_ = 0;
    utf8_cp_ = 0;
    alt_active_ = false;
    main_cells_.clear();
    main_cur_ = CursorState();
    main_cols_ = main_rows_ = 0;
    main_valid_ = false;
    saved_scroll_top_ = 0;
    saved_scroll_bottom_ = rows_ - 1;
    saved_valid_ = false;
    ++mode_epoch_;
}

void ScreenModel::resize(int cols, int rows) {
    cols = std::max(1, std::min(cols, 1000));
    rows = std::max(1, std::min(rows, 1000));
    if (cols == cols_ && rows == rows_) return;
    resize_cells(cells_, cols_, rows_, cols, rows, blank());
    if (alt_active_ && main_valid_) {
        resize_cells(main_cells_, main_cols_, main_rows_, cols, rows, blank());
        main_cols_ = cols;
        main_rows_ = rows;
        main_cur_.x = std::max(0, std::min(main_cur_.x, cols - 1));
        main_cur_.y = std::max(0, std::min(main_cur_.y, rows - 1));
    }
    cols_ = cols;
    rows_ = rows;
    cur_.x = std::max(0, std::min(cur_.x, cols_ - 1));
    cur_.y = std::max(0, std::min(cur_.y, rows_ - 1));
    tabs_.assign(static_cast<size_t>(cols_), 0);
    for (int x = 0; x < cols_; x += 8) tabs_[x] = 1;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    wrap_pending_ = false;
    ++mode_epoch_;
}

std::string ScreenModel::take_replies() {
    std::string r;
    r.swap(replies_);
    return r;
}

void ScreenModel::feed(const char* p, size_t n) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = b[i];
        switch (state_) {
        case S_GROUND:
            ground_byte(c);
            break;
        case S_ESC:
            esc_dispatch(c);
            break;
        case S_ESC_INTER:
            state_ = S_GROUND;
            break;
        case S_CSI_ENTRY:
        case S_CSI_PARAM:
        case S_CSI_INTER:
            csi_byte(c);
            break;
        case S_CSI_IGNORE:
            break;
        case S_OSC:
            if (c == 0x07) {
                osc_dispatch();
                state_ = S_GROUND;
            } else if (c == 0x1B) {
                state_ = S_OSC_ESC;
            } else if (c >= 0x20 && str_.size() < 1024) {
                str_ += static_cast<char>(c);
            }
            break;
        case S_OSC_ESC:
            if (c == '\\') {
                osc_dispatch();
                state_ = S_GROUND;
            } else {
                state_ = S_GROUND;
            }
            break;
        case S_STRING:
            if (c == 0x1B) state_ = S_STRING_ESC;
            else if (c == 0x07) state_ = S_GROUND;
            break;
        case S_STRING_ESC:
            state_ = (c == '\\') ? S_GROUND : S_STRING;
            break;
        }
    }
}

void ScreenModel::ground_byte(uint8_t b) {
    if (b < 0x20) {
        control(b);
        return;
    }
    if (b == 0x7F) return;
    if (utf8_need_ == 0) {
        if (b < 0x80) {
            put_char(b);
        } else if ((b & 0xE0) == 0xC0) {
            utf8_cp_ = b & 0x1F;
            utf8_need_ = 1;
        } else if ((b & 0xF0) == 0xE0) {
            utf8_cp_ = b & 0x0F;
            utf8_need_ = 2;
        } else if ((b & 0xF8) == 0xF0) {
            utf8_cp_ = b & 0x07;
            utf8_need_ = 3;
        } else {
            put_char(0xFFFD);
        }
        return;
    }
    if ((b & 0xC0) != 0x80) {
        utf8_need_ = 0;
        put_char(0xFFFD);
        ground_byte(b);
        return;
    }
    utf8_cp_ = (utf8_cp_ << 6) | (b & 0x3F);
    if (--utf8_need_ == 0) put_char(utf8_cp_);
}

void ScreenModel::control(uint8_t b) {
    switch (b) {
    case 0x08:   // BS
        wrap_pending_ = false;
        if (cur_.x > 0) --cur_.x;
        break;
    case 0x09: { // HT
        wrap_pending_ = false;
        int x = cur_.x + 1;
        while (x < cols_ && !tabs_[static_cast<size_t>(x)]) ++x;
        cur_.x = (x < cols_) ? x : cols_ - 1;
        break;
    }
    case 0x0A:   // LF
    case 0x0B:   // VT
    case 0x0C:   // FF
        linefeed();
        break;
    case 0x0D:   // CR
        cur_.x = 0;
        wrap_pending_ = false;
        break;
    case 0x18:
    case 0x1A:
        state_ = S_GROUND;
        params_.clear();
        cur_param_ = -1;
        param_started_ = false;
        break;
    case 0x1B:
        state_ = S_ESC;
        params_.clear();
        cur_param_ = -1;
        param_started_ = false;
        priv_ = 0;
        inter_.clear();
        break;
    default:
        break;
    }
}

void ScreenModel::esc_dispatch(uint8_t b) {
    switch (b) {
    case '[':
        state_ = S_CSI_ENTRY;
        params_.clear();
        cur_param_ = -1;
        param_started_ = false;
        priv_ = 0;
        inter_.clear();
        break;
    case ']':
        state_ = S_OSC;
        str_.clear();
        break;
    case 'P':
    case 'X':
    case '^':
    case '_':
        state_ = S_STRING;
        str_.clear();
        break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '-':
    case '.':
    case '/':
        state_ = S_ESC_INTER;
        break;
    case '7':
        save_cursor();
        state_ = S_GROUND;
        break;
    case '8':
        restore_cursor();
        state_ = S_GROUND;
        break;
    case 'D':   // IND
        linefeed();
        state_ = S_GROUND;
        break;
    case 'E':   // NEL
        cur_.x = 0;
        linefeed();
        state_ = S_GROUND;
        break;
    case 'M':   // RI
        reverse_index();
        state_ = S_GROUND;
        break;
    case 'H':   // HTS
        if (cur_.x >= 0 && cur_.x < cols_) tabs_[static_cast<size_t>(cur_.x)] = 1;
        state_ = S_GROUND;
        break;
    case 'c':   // RIS
        reset();
        state_ = S_GROUND;
        break;
    case '=':
        if (!modes_.app_keypad) { modes_.app_keypad = true; ++mode_epoch_; }
        state_ = S_GROUND;
        break;
    case '>':
        if (modes_.app_keypad) { modes_.app_keypad = false; ++mode_epoch_; }
        state_ = S_GROUND;
        break;
    default:
        state_ = S_GROUND;
        break;
    }
}

void ScreenModel::csi_byte(uint8_t b) {
    if (b == 0x18 || b == 0x1A) {
        state_ = S_GROUND;
        params_.clear();
        cur_param_ = -1;
        param_started_ = false;
        return;
    }
    if (b == 0x1B) {
        state_ = S_ESC;
        params_.clear();
        cur_param_ = -1;
        param_started_ = false;
        priv_ = 0;
        inter_.clear();
        return;
    }
    if (state_ == S_CSI_ENTRY) {
        if (b >= '0' && b <= '9') {
            cur_param_ = b - '0';
            param_started_ = true;
            state_ = S_CSI_PARAM;
        } else if (b == '?' || b == '>' || b == '<' || b == '=') {
            priv_ = static_cast<char>(b);
            state_ = S_CSI_PARAM;
        } else if (b == ';' || b == ':') {
            params_.push_back(0);
            state_ = S_CSI_PARAM;
        } else if (b >= 0x20 && b <= 0x2F) {
            inter_ += static_cast<char>(b);
            state_ = S_CSI_INTER;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(b);
            state_ = S_GROUND;
        }
        return;
    }
    if (state_ == S_CSI_PARAM) {
        if (b >= '0' && b <= '9') {
            cur_param_ = (cur_param_ < 0 ? 0 : cur_param_) * 10 + (b - '0');
            param_started_ = true;
        } else if (b == ';' || b == ':') {
            params_.push_back(param_started_ ? cur_param_ : 0);
            cur_param_ = -1;
            param_started_ = false;
        } else if (b >= 0x20 && b <= 0x2F) {
            inter_ += static_cast<char>(b);
            state_ = S_CSI_INTER;
        } else if (b >= 0x40 && b <= 0x7E) {
            if (param_started_) params_.push_back(cur_param_);
            csi_dispatch(b);
            state_ = S_GROUND;
        }
        return;
    }
    if (state_ == S_CSI_INTER) {
        if (b >= 0x20 && b <= 0x2F) {
            inter_ += static_cast<char>(b);
        } else if (b >= 0x40 && b <= 0x7E) {
            if (param_started_) params_.push_back(cur_param_);
            csi_dispatch(b);
            state_ = S_GROUND;
        }
    }
}

int ScreenModel::param(size_t i, int def) const {
    return i < params_.size() ? params_[i] : def;
}

void ScreenModel::csi_dispatch(uint8_t f) {
    int p0 = param(0, 0);
    int n = (p0 <= 0) ? 1 : p0;
    switch (f) {
    case 'A':
        move_rel(-n, 0);
        break;
    case 'B':
    case 'e':
        move_rel(n, 0);
        break;
    case 'C':
    case 'a':
        move_rel(0, n);
        break;
    case 'D':
        move_rel(0, -n);
        break;
    case 'E':
        move_rel(n, 0);
        cur_.x = 0;
        break;
    case 'F':
        move_rel(-n, 0);
        cur_.x = 0;
        break;
    case 'G':
    case '`':
        // CHA/HPA: column only.  Do not go through move_to(), which applies
        // origin mode to the row.
        wrap_pending_ = false;
        cur_.x = (n - 1 < 0) ? 0 : (n - 1 >= cols_ ? cols_ - 1 : n - 1);
        break;
    case 'H':
    case 'f':
        move_to(param(0, 1) - 1, param(1, 1) - 1);
        break;
    case 'd':
        move_to(n - 1, cur_.x);
        break;
    case 'J':
        erase_display(p0);
        break;
    case 'K':
        erase_line(p0);
        break;
    case 'L':
        insert_lines(n);
        break;
    case 'M':
        delete_lines(n);
        break;
    case 'P':
        delete_cells(n);
        break;
    case '@':
        insert_cells(n);
        break;
    case 'X':
        erase_chars(n);
        break;
    case 'S':
        scroll_up(n);
        break;
    case 'T':
        scroll_down(n);
        break;
    case 'm':
        apply_sgr();
        break;
    case 'r':
        set_scroll_region(param(0, 1), param(1, rows_));
        break;
    case 's':
        save_cursor();
        break;
    case 'u':
        restore_cursor();
        break;
    case 'g':
        if (p0 == 3) {
            tabs_.assign(static_cast<size_t>(cols_), 0);
        } else if (cur_.x >= 0 && cur_.x < cols_) {
            tabs_[static_cast<size_t>(cur_.x)] = 0;
        }
        break;
    case 'h':
        apply_modes(true);
        break;
    case 'l':
        apply_modes(false);
        break;
    case 'n':
        if (p0 == 5) {
            replies_ += "\x1b[0n";
        } else if (priv_ == '?' && p0 == 6) {
            replies_ += "\x1b[?";
            append_uint(replies_, cur_.y + 1);
            replies_ += ';';
            append_uint(replies_, cur_.x + 1);
            replies_ += 'R';
        } else if (p0 == 6) {
            replies_ += "\x1b[";
            append_uint(replies_, cur_.y + 1);
            replies_ += ';';
            append_uint(replies_, cur_.x + 1);
            replies_ += 'R';
        }
        break;
    case 'c':
        if (priv_ == '>') replies_ += "\x1b[>1;10;0c";
        else replies_ += "\x1b[?62;1;2;6;9;15;22c";
        break;
    case 't':
        if (p0 == 18) {
            replies_ += "\x1b[8;";
            append_uint(replies_, rows_);
            replies_ += ';';
            append_uint(replies_, cols_);
            replies_ += 't';
        } else if (p0 == 14) {
            replies_ += "\x1b[4;";
            append_uint(replies_, rows_);
            replies_ += ';';
            append_uint(replies_, cols_);
            replies_ += 't';
        }
        break;
    case 'q':
        if (inter_ == " " && cur_.style != p0) {
            cur_.style = p0;
            ++mode_epoch_;
        }
        break;
    case 'p':
        if (inter_ == "!") reset();
        break;
    default:
        break;
    }
    params_.clear();
    cur_param_ = -1;
    param_started_ = false;
    priv_ = 0;
    inter_.clear();
}

void ScreenModel::apply_sgr() {
    if (params_.empty()) {
        pen_fg_ = COLOR_DEFAULT;
        pen_bg_ = COLOR_DEFAULT;
        pen_attr_ = 0;
        return;
    }
    size_t i = 0;
    while (i < params_.size()) {
        int p = params_[i++];
        if (p == 0) {
            pen_fg_ = COLOR_DEFAULT;
            pen_bg_ = COLOR_DEFAULT;
            pen_attr_ = 0;
        } else if (p == 1) pen_attr_ |= A_BOLD;
        else if (p == 2) pen_attr_ |= A_DIM;
        else if (p == 3) pen_attr_ |= A_ITALIC;
        else if (p == 4 || p == 21) pen_attr_ |= A_UNDERLINE;
        else if (p == 5 || p == 6) pen_attr_ |= A_BLINK;
        else if (p == 7) pen_attr_ |= A_REVERSE;
        else if (p == 8) pen_attr_ |= A_HIDDEN;
        else if (p == 9) pen_attr_ |= A_STRIKE;
        else if (p == 22) pen_attr_ &= static_cast<uint16_t>(~(A_BOLD | A_DIM));
        else if (p == 23) pen_attr_ &= static_cast<uint16_t>(~A_ITALIC);
        else if (p == 24) pen_attr_ &= static_cast<uint16_t>(~A_UNDERLINE);
        else if (p == 25) pen_attr_ &= static_cast<uint16_t>(~A_BLINK);
        else if (p == 27) pen_attr_ &= static_cast<uint16_t>(~A_REVERSE);
        else if (p == 28) pen_attr_ &= static_cast<uint16_t>(~A_HIDDEN);
        else if (p == 29) pen_attr_ &= static_cast<uint16_t>(~A_STRIKE);
        else if (p >= 30 && p <= 37) pen_fg_ = COLOR_INDEXED | static_cast<uint32_t>(p - 30);
        else if (p == 39) pen_fg_ = COLOR_DEFAULT;
        else if (p >= 40 && p <= 47) pen_bg_ = COLOR_INDEXED | static_cast<uint32_t>(p - 40);
        else if (p == 49) pen_bg_ = COLOR_DEFAULT;
        else if (p >= 90 && p <= 97) pen_fg_ = COLOR_INDEXED | static_cast<uint32_t>(8 + p - 90);
        else if (p >= 100 && p <= 107) pen_bg_ = COLOR_INDEXED | static_cast<uint32_t>(8 + p - 100);
        else if (p == 38 || p == 48) {
            uint32_t* dst = (p == 38) ? &pen_fg_ : &pen_bg_;
            if (i < params_.size() && params_[i] == 5) {
                if (i + 1 < params_.size()) {
                    *dst = COLOR_INDEXED | static_cast<uint32_t>(params_[i + 1] & 0xFF);
                    i += 2;
                } else {
                    i = params_.size();
                }
            } else if (i < params_.size() && params_[i] == 2) {
                size_t rem = params_.size() - i;   // includes the leading 2
                size_t base = (rem >= 5) ? i + 2 : i + 1;
                if (base + 2 < params_.size()) {
                    *dst = COLOR_RGB |
                           (static_cast<uint32_t>(params_[base] & 0xFF) << 16) |
                           (static_cast<uint32_t>(params_[base + 1] & 0xFF) << 8) |
                           static_cast<uint32_t>(params_[base + 2] & 0xFF);
                    i = base + 3;
                } else {
                    i = params_.size();
                }
            } else {
                i = params_.size();
            }
        } else if (p == 58 || p == 59) {
            // underline colour: skip its arguments
            if (i < params_.size() && params_[i] == 5) i = std::min(i + 2, params_.size());
            else if (i < params_.size() && params_[i] == 2) i = std::min(i + 4, params_.size());
        }
    }
}

void ScreenModel::apply_modes(bool set) {
    for (int p : params_) {
        if (priv_ == '?') {
            switch (p) {
            case 1:
                if (modes_.app_cursor != set) { modes_.app_cursor = set; ++mode_epoch_; }
                break;
            case 6:
                if (modes_.origin != set) { modes_.origin = set; ++mode_epoch_; }
                move_to(0, 0);
                break;
            case 7:
                modes_.autowrap = set;
                break;
            case 12:
                if (modes_.cursor_blink != set) { modes_.cursor_blink = set; ++mode_epoch_; }
                break;
            case 25:
                if (cur_.visible != set) { cur_.visible = set; ++mode_epoch_; }
                break;
            case 47:
            case 1047:
            case 1049:
                if (set) enter_alt(p == 1049, false);
                else leave_alt(p == 1047);
                break;
            case 1048:
                if (set) save_cursor();
                else restore_cursor();
                break;
            case 9:
                modes_.mouse_mode = set ? 1000 : 0;
                ++mode_epoch_;
                break;
            case 1000:
            case 1002:
            case 1003:
            case 1009: {
                int m = (p == 1009) ? 1000 : p;
                if (set) modes_.mouse_mode = m;
                else if (modes_.mouse_mode == m) modes_.mouse_mode = 0;
                ++mode_epoch_;
                break;
            }
            case 1005:
            case 1006:
            case 1015:
                if (set) modes_.mouse_enc = p;
                else if (modes_.mouse_enc == p) modes_.mouse_enc = 0;
                ++mode_epoch_;
                break;
            case 1004:
                if (modes_.focus_report != set) { modes_.focus_report = set; ++mode_epoch_; }
                break;
            case 2004:
                if (modes_.bracketed_paste != set) { modes_.bracketed_paste = set; ++mode_epoch_; }
                break;
            case 66:
                if (modes_.app_keypad != set) { modes_.app_keypad = set; ++mode_epoch_; }
                break;
            default:
                break;
            }
        } else {
            if (p == 4) {
                if (modes_.insert != set) { modes_.insert = set; ++mode_epoch_; }
            }
        }
    }
}

void ScreenModel::osc_dispatch() {
    size_t semi = str_.find(';');
    if (semi != std::string::npos && semi > 0 && semi < 8) {
        int ps = 0;
        bool ok = true;
        for (size_t i = 0; i < semi; ++i) {
            if (str_[i] < '0' || str_[i] > '9') { ok = false; break; }
            ps = ps * 10 + (str_[i] - '0');
        }
        if (ok && (ps == 0 || ps == 2)) {
            std::string t = str_.substr(semi + 1);
            if (t.size() > 256) t.resize(256);
            if (t != title_) {
                title_.swap(t);
                ++title_epoch_;
            }
        }
    }
    str_.clear();
}

void ScreenModel::str_dispatch() {
    str_.clear();
}

void ScreenModel::put_char(uint32_t cp) {
    int w = width_of(cp);
    if (w == 0) {
        int px = wrap_pending_ ? cols_ - 1 : cur_.x - 1;
        while (px >= 0 && at(px, cur_.y).width == 0) --px;
        if (px >= 0) {
            Cell& c = at(px, cur_.y);
            if (c.comb1 == 0) c.comb1 = cp;
            else if (c.comb2 == 0) c.comb2 = cp;
        }
        return;
    }
    if (wrap_pending_) {
        wrap_pending_ = false;
        cur_.x = 0;
        linefeed();
    }
    if (w == 2 && cur_.x == cols_ - 1) {
        if (modes_.autowrap) {
            cur_.x = 0;
            linefeed();
        } else {
            w = 1;
            cp = ' ';
        }
    }
    if (modes_.insert) {
        insert_cells(w);
    } else {
        clear_cell(cur_.x, cur_.y);
        if (w == 2 && cur_.x + 1 < cols_) clear_cell(cur_.x + 1, cur_.y);
    }
    Cell c;
    c.ch = cp;
    c.width = static_cast<uint8_t>(w);
    c.fg = pen_fg_;
    c.bg = pen_bg_;
    c.attr = pen_attr_;
    at(cur_.x, cur_.y) = c;
    if (w == 2) {
        Cell cont;
        cont.ch = 0;
        cont.width = 0;
        cont.fg = pen_fg_;
        cont.bg = pen_bg_;
        cont.attr = pen_attr_;
        at(cur_.x + 1, cur_.y) = cont;
    }
    cur_.x += w;
    if (cur_.x >= cols_) {
        if (modes_.autowrap) {
            cur_.x = cols_ - 1;
            wrap_pending_ = true;
        } else {
            cur_.x = cols_ - 1;
        }
    }
}

void ScreenModel::clear_cell(int x, int y) {
    Cell& c = at(x, y);
    if (c.width == 0) {
        if (x > 0) at(x - 1, y) = blank();
        c = blank();
    } else if (c.width == 2) {
        if (x + 1 < cols_) at(x + 1, y) = blank();
        c = blank();
    } else {
        c = blank();
    }
}

void ScreenModel::fix_row(int y) {
    for (int x = 0; x < cols_;) {
        Cell& c = at(x, y);
        if (c.width == 2) {
            if (x + 1 >= cols_ || at(x + 1, y).width != 0) {
                c = blank_with(c.bg);
                ++x;
            } else {
                x += 2;
            }
        } else if (c.width == 0) {
            c = blank_with(c.bg);
            ++x;
        } else {
            ++x;
        }
    }
}

void ScreenModel::erase_range(int y, int x0, int x1) {
    if (y < 0 || y >= rows_) return;
    if (x0 < 0) x0 = 0;
    if (x1 > cols_) x1 = cols_;
    if (x0 >= x1) return;
    Cell b = blank();
    for (int x = x0; x < x1; ++x) at(x, y) = b;
    fix_row(y);
}

void ScreenModel::erase_display(int mode) {
    if (mode == 0) {
        erase_range(cur_.y, cur_.x, cols_);
        for (int y = cur_.y + 1; y < rows_; ++y) erase_range(y, 0, cols_);
    } else if (mode == 1) {
        for (int y = 0; y < cur_.y; ++y) erase_range(y, 0, cols_);
        erase_range(cur_.y, 0, cur_.x + 1);
    } else if (mode == 2) {
        for (int y = 0; y < rows_; ++y) erase_range(y, 0, cols_);
    }
}

void ScreenModel::erase_line(int mode) {
    if (mode == 0) erase_range(cur_.y, cur_.x, cols_);
    else if (mode == 1) erase_range(cur_.y, 0, cur_.x + 1);
    else if (mode == 2) erase_range(cur_.y, 0, cols_);
}

void ScreenModel::insert_cells(int n) {
    if (n <= 0 || cur_.x >= cols_) return;
    if (n > cols_ - cur_.x) n = cols_ - cur_.x;
    for (int x = cols_ - 1; x >= cur_.x + n; --x)
        at(x, cur_.y) = at(x - n, cur_.y);
    Cell b = blank();
    for (int x = cur_.x; x < cur_.x + n; ++x) at(x, cur_.y) = b;
    fix_row(cur_.y);
}

void ScreenModel::delete_cells(int n) {
    if (n <= 0 || cur_.x >= cols_) return;
    if (n > cols_ - cur_.x) n = cols_ - cur_.x;
    for (int x = cur_.x; x < cols_ - n; ++x)
        at(x, cur_.y) = at(x + n, cur_.y);
    Cell b = blank();
    for (int x = cols_ - n; x < cols_; ++x) at(x, cur_.y) = b;
    fix_row(cur_.y);
}

void ScreenModel::erase_chars(int n) {
    erase_range(cur_.y, cur_.x, cur_.x + std::max(0, n));
}

void ScreenModel::insert_lines(int n) {
    if (cur_.y < scroll_top_ || cur_.y > scroll_bottom_) return;
    if (n > scroll_bottom_ - cur_.y + 1) n = scroll_bottom_ - cur_.y + 1;
    for (int y = scroll_bottom_; y >= cur_.y + n; --y)
        for (int x = 0; x < cols_; ++x) at(x, y) = at(x, y - n);
    Cell b = blank();
    for (int y = cur_.y; y < cur_.y + n; ++y)
        for (int x = 0; x < cols_; ++x) at(x, y) = b;
}

void ScreenModel::delete_lines(int n) {
    if (cur_.y < scroll_top_ || cur_.y > scroll_bottom_) return;
    if (n > scroll_bottom_ - cur_.y + 1) n = scroll_bottom_ - cur_.y + 1;
    for (int y = cur_.y; y <= scroll_bottom_ - n; ++y)
        for (int x = 0; x < cols_; ++x) at(x, y) = at(x, y + n);
    Cell b = blank();
    for (int y = scroll_bottom_ - n + 1; y <= scroll_bottom_; ++y)
        for (int x = 0; x < cols_; ++x) at(x, y) = b;
}

void ScreenModel::scroll_up(int n) {
    int height = scroll_bottom_ - scroll_top_ + 1;
    if (n <= 0 || height <= 0) return;
    if (n > height) n = height;
    for (int y = scroll_top_; y <= scroll_bottom_ - n; ++y)
        for (int x = 0; x < cols_; ++x) at(x, y) = at(x, y + n);
    Cell b = blank();
    for (int y = scroll_bottom_ - n + 1; y <= scroll_bottom_; ++y)
        for (int x = 0; x < cols_; ++x) at(x, y) = b;
}

void ScreenModel::scroll_down(int n) {
    int height = scroll_bottom_ - scroll_top_ + 1;
    if (n <= 0 || height <= 0) return;
    if (n > height) n = height;
    for (int y = scroll_bottom_; y >= scroll_top_ + n; --y)
        for (int x = 0; x < cols_; ++x) at(x, y) = at(x, y - n);
    Cell b = blank();
    for (int y = scroll_top_; y < scroll_top_ + n; ++y)
        for (int x = 0; x < cols_; ++x) at(x, y) = b;
}

void ScreenModel::linefeed() {
    wrap_pending_ = false;
    if (cur_.y == scroll_bottom_) {
        scroll_up(1);
    } else if (cur_.y < rows_ - 1) {
        ++cur_.y;
    }
}

void ScreenModel::reverse_index() {
    wrap_pending_ = false;
    if (cur_.y == scroll_top_) scroll_down(1);
    else if (cur_.y > 0) --cur_.y;
}

void ScreenModel::move_to(int y, int x) {
    wrap_pending_ = false;
    int top = modes_.origin ? scroll_top_ : 0;
    int bot = modes_.origin ? scroll_bottom_ : rows_ - 1;
    if (modes_.origin) y += scroll_top_;
    if (y < top) y = top;
    if (y > bot) y = bot;
    if (x < 0) x = 0;
    if (x >= cols_) x = cols_ - 1;
    cur_.x = x;
    cur_.y = y;
}

void ScreenModel::move_rel(int dy, int dx) {
    wrap_pending_ = false;
    int top = modes_.origin ? scroll_top_ : 0;
    int bot = modes_.origin ? scroll_bottom_ : rows_ - 1;
    int y = cur_.y + dy;
    int x = cur_.x + dx;
    if (y < top) y = top;
    if (y > bot) y = bot;
    if (x < 0) x = 0;
    if (x >= cols_) x = cols_ - 1;
    cur_.x = x;
    cur_.y = y;
}

void ScreenModel::set_scroll_region(int top, int bottom) {
    if (top < 1) top = 1;
    if (bottom > rows_) bottom = rows_;
    if (top >= bottom) {
        top = 1;
        bottom = rows_;
    }
    scroll_top_ = top - 1;
    scroll_bottom_ = bottom - 1;
    move_to(0, 0);
}

void ScreenModel::save_cursor() {
    saved_cur_ = cur_;
    saved_fg_ = pen_fg_;
    saved_bg_ = pen_bg_;
    saved_attr_ = pen_attr_;
    saved_valid_ = true;
}

void ScreenModel::restore_cursor() {
    if (!saved_valid_) return;
    cur_ = saved_cur_;
    pen_fg_ = saved_fg_;
    pen_bg_ = saved_bg_;
    pen_attr_ = saved_attr_;
    if (cur_.x >= cols_) cur_.x = cols_ - 1;
    if (cur_.y >= rows_) cur_.y = rows_ - 1;
    wrap_pending_ = false;
}

void ScreenModel::enter_alt(bool clear, bool save) {
    (void)save;
    if (modes_.alt) return;
    main_cells_ = cells_;
    main_cur_ = cur_;
    main_cols_ = cols_;
    main_rows_ = rows_;
    main_valid_ = true;
    saved_scroll_top_ = scroll_top_;
    saved_scroll_bottom_ = scroll_bottom_;
    modes_.alt = true;
    alt_active_ = true;
    if (clear) {
        cells_.assign(static_cast<size_t>(cols_) * rows_, blank());
        cur_ = CursorState();
        wrap_pending_ = false;
    }
    ++mode_epoch_;
}

void ScreenModel::leave_alt(bool clear) {
    (void)clear;
    if (!modes_.alt) return;
    if (main_valid_) {
        cells_ = main_cells_;
        cur_ = main_cur_;
        if (cur_.x >= cols_) cur_.x = cols_ - 1;
        if (cur_.y >= rows_) cur_.y = rows_ - 1;
    }
    int top = saved_scroll_top_;
    int bot = saved_scroll_bottom_;
    if (top < 0) top = 0;
    if (top > rows_ - 1) top = rows_ - 1;
    if (bot < top) bot = rows_ - 1;
    if (bot > rows_ - 1) bot = rows_ - 1;
    scroll_top_ = top;
    scroll_bottom_ = bot;
    modes_.alt = false;
    alt_active_ = false;
    wrap_pending_ = false;
    ++mode_epoch_;
}

Snap ScreenModel::snapshot() const {
    Snap s;
    s.cols = cols_;
    s.rows = rows_;
    s.cells = cells_;
    s.cur = cur_;
    s.modes = modes_;
    return s;
}

std::string ScreenModel::snapshot_ansi(const Snap& s) {
    std::string out;
    if (s.modes.alt) out += "\x1b[?1049h";
    out += "\x1b[2J";
    out += "\x1b[4l";
    append_mode_changes(out, ModeState(), CursorState(), s.modes, s.cur, true);
    for (int y = 0; y < s.rows; ++y) {
        int last = -1;
        for (int x = s.cols - 1; x >= 0; --x) {
            const Cell& c = s.cells[static_cast<size_t>(y) * s.cols + x];
            if (!(c.ch == ' ' && c.comb1 == 0 && c.comb2 == 0 &&
                  c.fg == COLOR_DEFAULT && c.bg == COLOR_DEFAULT &&
                  c.attr == 0 && c.width == 1)) {
                last = x;
                break;
            }
        }
        if (last < 0) continue;
        out += "\x1b[";
        append_uint(out, y + 1);
        out += ";1H";
        Pen pen;
        bool have = false;
        for (int x = 0; x <= last; ++x) {
            const Cell& c = s.cells[static_cast<size_t>(y) * s.cols + x];
            if (c.width == 0) continue;
            Pen p{c.fg, c.bg, c.attr};
            if (!have || !(p == pen)) {
                append_pen(out, p);
                pen = p;
                have = true;
            }
            append_utf8(out, c.ch);
            if (c.comb1) append_utf8(out, c.comb1);
            if (c.comb2) append_utf8(out, c.comb2);
        }
    }
    out += "\x1b[";
    append_uint(out, s.cur.y + 1);
    out += ';';
    append_uint(out, s.cur.x + 1);
    out += 'H';
    return out;
}

namespace {
bool cell_differs(const Snap& a, const Snap& b, int x, int y) {
    size_t i = static_cast<size_t>(y) * b.cols + x;
    const Cell& ca = a.cells[i];
    const Cell& cb = b.cells[i];
    if (ca != cb) return true;
    if (cb.width == 2 && x + 1 < b.cols) {
        size_t j = i + 1;
        if (a.cells[j] != b.cells[j]) return true;
    }
    return false;
}
}  // namespace

std::string ScreenModel::diff_ansi(const Snap& base, const Snap& cur,
                                   bool* need_snapshot) {
    if (need_snapshot) *need_snapshot = false;
    if (base.cols != cur.cols || base.rows != cur.rows ||
        base.modes.alt != cur.modes.alt) {
        if (need_snapshot) *need_snapshot = true;
        return std::string();
    }
    std::string out;
    append_mode_changes(out, base.modes, base.cur, cur.modes, cur.cur, false);
    Pen prev;
    bool have_pen = false;
    for (int y = 0; y < cur.rows; ++y) {
        int x = 0;
        while (x < cur.cols) {
            if (!cell_differs(base, cur, x, y)) {
                ++x;
                continue;
            }
            int x0 = x;
            size_t i0 = static_cast<size_t>(y) * cur.cols + x0;
            if (cur.cells[i0].width == 0 && x0 > 0) --x0;
            int x1 = x0;
            while (x1 < cur.cols) {
                if (cell_differs(base, cur, x1, y)) {
                    ++x1;
                    continue;
                }
                int look = x1;
                while (look < cur.cols && look - x1 < 2 &&
                       !cell_differs(base, cur, look, y))
                    ++look;
                if (look < cur.cols && cell_differs(base, cur, look, y)) {
                    x1 = look;
                    continue;
                }
                break;
            }
            if (x1 <= x0) x1 = x0 + 1;
            size_t ilast = static_cast<size_t>(y) * cur.cols + (x1 - 1);
            if (cur.cells[ilast].width == 2 && x1 < cur.cols) ++x1;
            out += "\x1b[";
            append_uint(out, y + 1);
            out += ';';
            append_uint(out, x0 + 1);
            out += 'H';
            for (int xx = x0; xx < x1 && xx < cur.cols; ++xx) {
                const Cell& c = cur.cells[static_cast<size_t>(y) * cur.cols + xx];
                if (c.width == 0) continue;
                Pen p{c.fg, c.bg, c.attr};
                if (!have_pen || !(p == prev)) {
                    append_pen(out, p);
                    prev = p;
                    have_pen = true;
                }
                append_utf8(out, c.ch);
                if (c.comb1) append_utf8(out, c.comb1);
                if (c.comb2) append_utf8(out, c.comb2);
            }
            x = x1;
        }
    }
    if (cur.cur != base.cur || !out.empty()) {
        out += "\x1b[";
        append_uint(out, cur.cur.y + 1);
        out += ';';
        append_uint(out, cur.cur.x + 1);
        out += 'H';
    }
    return out;
}

std::string ScreenModel::modes_json(const ModeState& m, const CursorState& c) {
    char b[320];
    int n = snprintf(
        b, sizeof b,
        "{\"t\":\"m\",\"alt\":%s,\"mouse\":%d,\"enc\":%d,\"cur\":%s,"
        "\"blink\":%s,\"style\":%d,\"paste\":%s,\"focus\":%s,"
        "\"app\":%s,\"pad\":%s}",
        m.alt ? "true" : "false", m.mouse_mode, m.mouse_enc,
        c.visible ? "true" : "false",
        m.cursor_blink ? "true" : "false", c.style,
        m.bracketed_paste ? "true" : "false",
        m.focus_report ? "true" : "false",
        m.app_cursor ? "true" : "false",
        m.app_keypad ? "true" : "false");
    return std::string(b, n > 0 ? static_cast<size_t>(n) : 0);
}

std::string ScreenModel::row_text(int y) const {
    if (y < 0 || y >= rows_) return std::string();
    std::string s;
    for (int x = 0; x < cols_; ++x) {
        const Cell& c = at(x, y);
        if (c.width == 0) continue;
        append_utf8(s, c.ch);
        if (c.comb1) append_utf8(s, c.comb1);
        if (c.comb2) append_utf8(s, c.comb2);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string ScreenModel::text() const {
    std::string s;
    for (int y = 0; y < rows_; ++y) {
        if (y) s += '\n';
        s += row_text(y);
    }
    return s;
}

}  // namespace term
