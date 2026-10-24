// screen.h — server-side VT terminal model for the fast console path.
//
// The broker feeds the PTY's output into one ScreenModel per session.  A
// browser then receives a full snapshot once and per-frame diffs after that:
// only cells that actually changed, coalesced into at most one frame every few
// milliseconds.  The model is deliberately small (no scrollback tree): the
// fast path is for full-screen TUIs such as NomadNet, which live on the
// alternate screen.  The shell keeps the raw-VT path so its scrollback stays
// in xterm.js.
//
// The ANSI produced by snapshot_ansi()/diff_ansi() is fed straight to
// xterm.js.  Positions are always absolute and insert/origin modes are never
// forwarded, so the client terminal stays a dumb renderer.
#ifndef RNSBOX_TERM_SCREEN_H
#define RNSBOX_TERM_SCREEN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace term {

// Colours are packed into 32 bits:
//   0xFFFFFFFF                     default (39/49)
//   0x01____ __                    256-colour palette index (low 8 bits)
//   0x02____ __                    truecolour 0x00RRGGBB
constexpr uint32_t COLOR_DEFAULT = 0xFFFFFFFFu;
constexpr uint32_t COLOR_INDEXED = 0x01000000u;
constexpr uint32_t COLOR_RGB     = 0x02000000u;

enum : uint16_t {
    A_BOLD      = 1u << 0,
    A_DIM       = 1u << 1,
    A_ITALIC    = 1u << 2,
    A_UNDERLINE = 1u << 3,
    A_BLINK     = 1u << 4,
    A_REVERSE   = 1u << 5,
    A_HIDDEN    = 1u << 6,
    A_STRIKE    = 1u << 7,
};

struct Cell {
    uint32_t ch    = ' ';
    uint32_t comb1 = 0;   // first zero-width mark
    uint32_t comb2 = 0;   // second zero-width mark
    uint32_t fg    = COLOR_DEFAULT;
    uint32_t bg    = COLOR_DEFAULT;
    uint16_t attr  = 0;
    uint8_t  width = 1;   // 1, 2 (wide), or 0 (continuation of a wide cell)

    bool operator==(const Cell& o) const {
        return ch == o.ch && comb1 == o.comb1 && comb2 == o.comb2 &&
               fg == o.fg && bg == o.bg && attr == o.attr && width == o.width;
    }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

struct CursorState {
    int  x = 0;
    int  y = 0;
    int  style = 0;       // DECSCUSR 0..6
    bool visible = true;

    bool operator==(const CursorState& o) const {
        return x == o.x && y == o.y && style == o.style && visible == o.visible;
    }
    bool operator!=(const CursorState& o) const { return !(*this == o); }
};

struct ModeState {
    bool alt = false;
    bool origin = false;
    bool autowrap = true;
    bool insert = false;
    bool app_cursor = false;
    bool app_keypad = false;
    bool bracketed_paste = false;
    bool focus_report = false;
    bool cursor_blink = true;
    int  mouse_mode = 0;   // 0, 1000, 1002, 1003 (1009/1007 mapped to 1000)
    int  mouse_enc  = 0;   // 0 (X10), 1005, 1006 (SGR), 1015 (urxvt)

    bool operator==(const ModeState& o) const {
        return alt == o.alt && origin == o.origin && autowrap == o.autowrap &&
               insert == o.insert && app_cursor == o.app_cursor &&
               app_keypad == o.app_keypad &&
               bracketed_paste == o.bracketed_paste &&
               focus_report == o.focus_report &&
               cursor_blink == o.cursor_blink &&
               mouse_mode == o.mouse_mode && mouse_enc == o.mouse_enc;
    }
    bool operator!=(const ModeState& o) const { return !(*this == o); }
};

struct Snap {
    int cols = 0, rows = 0;
    std::vector<Cell> cells;
    CursorState cur;
    ModeState modes;

    bool operator==(const Snap& o) const {
        return cols == o.cols && rows == o.rows && cur == o.cur &&
               modes == o.modes && cells == o.cells;
    }
    bool operator!=(const Snap& o) const { return !(*this == o); }
};

class ScreenModel {
public:
    explicit ScreenModel(int cols = 80, int rows = 24);

    void resize(int cols, int rows);
    void reset();

    // Apply raw PTY bytes.  Replies the terminal must send back (DSR/DA) are
    // accumulated and returned by take_replies().
    void feed(const char* p, size_t n);
    std::string take_replies();

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    const Cell& cell(int x, int y) const { return cells_[idx(x, y)]; }
    CursorState cursor() const { return cur_; }
    const ModeState& modes() const { return modes_; }
    bool alt() const { return modes_.alt; }

    Snap snapshot() const;
    uint64_t mode_epoch() const { return mode_epoch_; }
    const std::string& title() const { return title_; }
    uint64_t title_epoch() const { return title_epoch_; }

    // ANSI for a full repaint (used on attach and after any discontinuity).
    static std::string snapshot_ansi(const Snap& s);
    // ANSI for the difference base -> cur.  Sets *need_snapshot when a diff
    // cannot express the change (size or alternate-screen switch).
    static std::string diff_ansi(const Snap& base, const Snap& cur,
                                 bool* need_snapshot);

    // Small JSON control message describing the modes the client needs to
    // know about (mouse wheel fallback, cursor shape).
    static std::string modes_json(const ModeState& m, const CursorState& c);

    std::string text() const;          // tests / debugging
    std::string row_text(int y) const;

private:
    size_t idx(int x, int y) const {
        return static_cast<size_t>(y) * static_cast<size_t>(cols_) +
               static_cast<size_t>(x);
    }
    Cell& at(int x, int y) { return cells_[idx(x, y)]; }
    const Cell& at(int x, int y) const { return cells_[idx(x, y)]; }

    Cell blank() const { return blank_with(pen_bg_); }
    Cell blank_with(uint32_t bg) const;
    void bump_modes() { ++mode_epoch_; }

    void ground_byte(uint8_t b);
    void control(uint8_t b);
    void esc_dispatch(uint8_t b);
    void csi_byte(uint8_t b);
    void csi_dispatch(uint8_t final);
    void osc_dispatch();
    void str_dispatch();
    void put_char(uint32_t cp);
    void clear_cell(int x, int y);
    void erase_range(int y, int x0, int x1);
    void linefeed();
    void reverse_index();
    void scroll_up(int n);
    void scroll_down(int n);
    void move_to(int y, int x);      // absolute (CSI H/f/d/G), origin-aware
    void move_rel(int dy, int dx);   // relative (CSI A/B/C/D/E/F)
    void fix_row(int y);
    void set_scroll_region(int top, int bottom);
    void erase_display(int mode);
    void erase_line(int mode);
    void insert_cells(int n);
    void delete_cells(int n);
    void insert_lines(int n);
    void delete_lines(int n);
    void erase_chars(int n);
    void save_cursor();
    void restore_cursor();
    void enter_alt(bool clear, bool save);
    void leave_alt(bool clear);
    void apply_sgr();
    void apply_modes(bool set);
    int  param(size_t i, int def) const;
    int  param_count() const { return static_cast<int>(params_.size()); }

    int cols_ = 80;
    int rows_ = 24;
    std::vector<Cell> cells_;
    CursorState cur_;
    ModeState modes_;
    int scroll_top_ = 0;
    int scroll_bottom_ = 0;
    std::vector<uint8_t> tabs_;      // 1 = tab stop
    bool wrap_pending_ = false;
    uint32_t pen_fg_ = COLOR_DEFAULT;
    uint32_t pen_bg_ = COLOR_DEFAULT;
    uint16_t pen_attr_ = 0;
    std::string replies_;
    std::string title_;
    uint64_t mode_epoch_ = 0;
    uint64_t title_epoch_ = 0;

    // parser
    enum State {
        S_GROUND = 0,
        S_ESC,
        S_ESC_INTER,
        S_CSI_ENTRY,
        S_CSI_PARAM,
        S_CSI_INTER,
        S_CSI_IGNORE,
        S_OSC,
        S_OSC_ESC,
        S_STRING,
        S_STRING_ESC,
    };
    State state_ = S_GROUND;
    std::vector<int> params_;
    int cur_param_ = -1;
    bool param_started_ = false;
    char priv_ = 0;              // '?', '>', '<', '='
    std::string inter_;          // CSI intermediates
    std::string str_;            // OSC / DCS / SOS payload
    int utf8_need_ = 0;
    uint32_t utf8_cp_ = 0;

    // alternate screen / saved cursor
    bool alt_active_ = false;
    std::vector<Cell> main_cells_;
    CursorState main_cur_;
    int main_cols_ = 0;
    int main_rows_ = 0;
    bool main_valid_ = false;
    int saved_scroll_top_ = 0;
    int saved_scroll_bottom_ = 0;
    bool saved_valid_ = false;
    CursorState saved_cur_;
    uint32_t saved_fg_ = COLOR_DEFAULT;
    uint32_t saved_bg_ = COLOR_DEFAULT;
    uint16_t saved_attr_ = 0;
};

}  // namespace term

#endif  // RNSBOX_TERM_SCREEN_H
