#define _XOPEN_SOURCE 700

/*
 * simplewords - a small terminal word processor.
 *
 * Build: cc simplewords.c -Wall -Wextra -O2 -lncursesw -o simplewords
 * Run:   ./simplewords [file]
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define MAX_LINES 10000
#define MAX_LINE  4096
#define TEXT_WIDTH 80
#define TAB_WIDTH 4
#define TOP_PAD 3
#define UNDO_DEPTH 256

/* Private key codes for modified navigation. KEY_SR/KEY_SF mean terminal
 * scroll commands, not Shift+Up/Down, despite their misleading names. */
#define KEY_EXTEND_UP       (KEY_MAX + 1)
#define KEY_EXTEND_DOWN     (KEY_MAX + 2)
#define KEY_EXTEND_PAGE_UP  (KEY_MAX + 3)
#define KEY_EXTEND_PAGE_DOWN (KEY_MAX + 4)

typedef struct {
    int line_count;
    int cy;
    int cx;
    int top;
    char **lines;
} UndoState;

typedef struct {
    int autosave_interval;
    int text_width;
    int top_pad;
} Config;

typedef struct {
    int left;
    int body_width;
    int top_pad;
    int bottom;
    int visible_rows;
} BodyGeometry;

typedef struct {
    int line;
    int render_start;
    int render_end;
    int next_start;
    int cursor_start;
    int cursor_end;
    int visual_width;
    int doc_row;
} WrapRow;

static Config config = {
    .autosave_interval = 1,
    .text_width = TEXT_WIDTH,
    .top_pad = TOP_PAD
};

static char *lines[MAX_LINES];
static int line_count = 1;
static int cy = 0;
static int cx = 0;
static int cursor_affinity_line = -1;
static int cursor_affinity_x = -1;
static int cursor_affinity_doc_row = -1;
static int cursor_affinity_col = -1;
static int top = 0;
static int goal_col = -1;
static char filename[512] = "";
static char last_open_file[512] = "";
static char last_open_directory[512] = "";
static char last_save_directory[512] = "";
static char untitled_name[128] = "";
static int dirty = 0;
static int distraction_free = 0;

static int selecting = 0;
static int sel_cy = 0;
static int sel_cx = 0;
static char *clip = NULL;

enum {
    CLIP_BACKEND_UNKNOWN = -1,
    CLIP_BACKEND_NONE = 0,
    CLIP_BACKEND_WL,
    CLIP_BACKEND_XCLIP,
    CLIP_BACKEND_XSEL
};

__attribute__((unused)) static int clip_backend = CLIP_BACKEND_UNKNOWN;
static int clip_warned = 0;

static char status_msg[512] = "";
static char last_find[256] = "";
static int find_mode = 0;
static int find_active = 0;
static int find_match_y = -1;
static int find_match_x = -1;
static int find_match_len = 0;
static time_t status_time = 0;
static time_t last_edit_time = 0;
static int autosave_dirty = 0;

#define LEGACY_SESSION_FILE ".simplewords-session"

static UndoState undo_stack[UNDO_DEPTH];
static int undo_count = 0;
static UndoState redo_stack[UNDO_DEPTH];
static int redo_count = 0;
static int suppress_undo = 0;
static time_t last_type_time = 0;
static int burst_chars = 0;
static int cursor_visibility = -1;

enum {
    SCREEN_ROW_UNUSED,
    SCREEN_ROW_TEXT,
    SCREEN_ROW_EMPTY
};

typedef struct {
    int kind;
    WrapRow wrap;
} ScreenRow;

enum {
    SCREEN_CELL_BLANK,
    SCREEN_CELL_GLYPH,
    SCREEN_CELL_CONTINUATION
};

typedef struct {
    wchar_t wc;
    attr_t attr;
    unsigned char kind;
} ScreenCell;

static ScreenRow *desired_rows = NULL;
static int screen_row_capacity = 0;
static ScreenCell *screen_cells = NULL;
static ScreenCell *desired_cells = NULL;
static size_t screen_cell_capacity = 0;
static int screen_cache_valid = 0;
static int screen_cache_lines = 0;
static int screen_cache_cols = 0;
static int screen_cache_left = 0;
static int screen_cache_text_width = 0;
static int screen_cache_top_pad = 0;
static int screen_cache_distraction_free = 0;
static char screen_cache_title[700] = "";
static char screen_cache_wc[64] = "";
static char screen_cache_status[512] = "";
static int screen_cache_top = 0;
static int center_lock_enabled = 0;
static int windowed_redraw_enabled = 0;
static int idle_cursor_enabled = 0;
static WINDOW *body_window = NULL;
static int body_window_height = 0;
static int body_window_width = 0;
static int body_window_top = 0;
static int body_window_left = 0;
static long long last_keypress_ms = 0;
static int idle_cursor_hidden = 0;
static const char help_text[] =
    "C-x C-f open  C-x b blank  C-x C-s save  C-x C-w save as  "
    "C-s find  C-x u undo  C-x r redo  C-x C-z focus  C-x C-c quit";

static void clamp_cursor(void);
static void clamp_top(void);
static void clear_cursor_affinity(void);
static int visual_to_pos_in_row(const WrapRow *row, int target_col);

/*
 * Terminal editors such as nvim and emacs -nw are easiest on the eyes when the
 * prose itself is just the terminal's normal face. The terminal owns the actual
 * font choice; simplewords only chooses attributes and spacing.
 */
static attr_t body_attr(void)
{
    return A_NORMAL;
}

static attr_t selection_attr(void)
{
    return A_REVERSE;
}

static attr_t chrome_attr(void)
{
    return A_REVERSE;
}

static int env_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && strcmp(value, "1") == 0;
}

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static void configure_settle_options(void)
{
    int settle = env_enabled("SW_SETTLE");

    center_lock_enabled = settle || env_enabled("SW_CENTER_LOCK");
    windowed_redraw_enabled = settle || env_enabled("SW_WINDOWED_REDRAW");
    idle_cursor_enabled = settle || env_enabled("SW_IDLE_CURSOR");
    if (settle)
        distraction_free = 1;
}

static void set_cursor_visibility(int visible)
{
    visible = visible ? 1 : 0;
    if (visible == cursor_visibility)
        return;

    curs_set(visible);
    cursor_visibility = visible;
}

static char *new_line(const char *s)
{
    char *p = calloc(MAX_LINE, 1);
    if (!p) {
        endwin();
        perror("calloc");
        exit(1);
    }

    if (s) {
        strncpy(p, s, MAX_LINE - 1);
        p[MAX_LINE - 1] = '\0';
    }

    return p;
}

static void set_status(const char *msg)
{
    strncpy(status_msg, msg, sizeof(status_msg) - 1);
    status_msg[sizeof(status_msg) - 1] = '\0';
    status_time = time(NULL);
}

static void clear_status(void)
{
    status_msg[0] = '\0';
}

static void make_untitled_name(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    snprintf(untitled_name, sizeof(untitled_name),
             "untitled-%04d%02d%02d-%02d%02d%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void display_name(char *out, size_t outsz)
{
    if (filename[0]) {
        const char *slash = strrchr(filename, '/');
        snprintf(out, outsz, "%s", slash ? slash + 1 : filename);
    } else {
        snprintf(out, outsz, "%s", untitled_name);
    }
}

static int pos_before(int ay, int ax, int by, int bx)
{
    return ay < by || (ay == by && ax < bx);
}

static void free_state(UndoState *s)
{
    if (!s->lines)
        return;

    for (int i = 0; i < s->line_count; i++)
        free(s->lines[i]);
    free(s->lines);
    memset(s, 0, sizeof(*s));
}

static UndoState capture_state(void)
{
    UndoState s;
    s.line_count = line_count;
    s.cy = cy;
    s.cx = cx;
    s.top = top;
    s.lines = calloc((size_t)line_count, sizeof(char *));
    if (!s.lines) {
        endwin();
        perror("calloc");
        exit(1);
    }

    for (int i = 0; i < line_count; i++)
        s.lines[i] = new_line(lines[i]);

    return s;
}

static void restore_state(UndoState *s)
{
    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    line_count = s->line_count;
    cy = s->cy;
    cx = s->cx;
    top = s->top;

    for (int i = 0; i < line_count; i++) {
        lines[i] = s->lines[i];
        s->lines[i] = NULL;
    }

    free_state(s);
    goal_col = -1;
    clear_cursor_affinity();
    clamp_cursor();
    clamp_top();
}

static void push_state(UndoState *stack, int *count, UndoState state)
{
    if (*count == UNDO_DEPTH) {
        free_state(&stack[0]);
        memmove(&stack[0], &stack[1], sizeof(stack[0]) * (UNDO_DEPTH - 1));
        *count = UNDO_DEPTH - 1;
    }

    stack[(*count)++] = state;
}

static void clear_stack(UndoState *stack, int *count)
{
    for (int i = 0; i < *count; i++)
        free_state(&stack[i]);
    *count = 0;
}

static void save_undo(void)
{
    if (suppress_undo)
        return;

    push_state(undo_stack, &undo_count, capture_state());
    clear_stack(redo_stack, &redo_count);
}

static void break_undo_burst(void)
{
    burst_chars = 0;
    last_type_time = 0;
}

static void maybe_save_typing_undo(void)
{
    time_t now = time(NULL);
    if (!burst_chars || now - last_type_time > 1 || burst_chars > 30) {
        save_undo();
        burst_chars = 0;
    }

    burst_chars++;
    last_type_time = now;
}

static void mark_edit(void)
{
    dirty = 1;
    autosave_dirty = 1;
    last_edit_time = time(NULL);
    goal_col = -1;
    clear_cursor_affinity();
    clamp_top();
}

static int utf8_char_width(const char *s, int *bytes_used)
{
    mbstate_t st;
    wchar_t wc;
    size_t n;
    int w;

    memset(&st, 0, sizeof(st));
    n = mbrtowc(&wc, s, MB_CUR_MAX, &st);

    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        if (bytes_used)
            *bytes_used = 1;
        return 1;
    }

    if (bytes_used)
        *bytes_used = (int)n;

    w = wcwidth(wc);
    return w < 1 ? 1 : w;
}

static int utf8_decode(const char *s, wchar_t *wc, int *bytes_used)
{
    mbstate_t st;
    size_t n;
    int w;

    memset(&st, 0, sizeof(st));
    n = mbrtowc(wc, s, MB_CUR_MAX, &st);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *wc = L'\xfffd';
        *bytes_used = 1;
        return 1;
    }

    *bytes_used = (int)n;
    w = wcwidth(*wc);
    return w < 1 ? 1 : w;
}

static int char_visual_width(int col, char c)
{
    if (c == '\t')
        return TAB_WIDTH - (col % TAB_WIDTH);
    return 1;
}

static BodyGeometry body_geometry(void)
{
    BodyGeometry geo;
    int cols = COLS > 0 ? COLS : config.text_width;
    int rows = LINES > 0 ? LINES : config.top_pad + 1;

    geo.left = (cols - config.text_width) / 2;
    if (geo.left < 0)
        geo.left = 0;

    geo.body_width = config.text_width;
    if (geo.body_width > cols - geo.left)
        geo.body_width = cols - geo.left;
    if (geo.body_width < 1)
        geo.body_width = 1;

    geo.top_pad = config.top_pad;
    if (geo.top_pad < 0)
        geo.top_pad = 0;

    geo.bottom = distraction_free ? rows : rows - 1;
    if (geo.bottom < 0)
        geo.bottom = 0;

    geo.visible_rows = geo.bottom - geo.top_pad;
    if (geo.visible_rows < 1)
        geo.visible_rows = 1;

    return geo;
}

static int layout_width(void)
{
    return body_geometry().body_width;
}

static void clear_cursor_affinity(void)
{
    cursor_affinity_line = -1;
    cursor_affinity_x = -1;
    cursor_affinity_doc_row = -1;
    cursor_affinity_col = -1;
}

static void set_cursor_affinity(int line, int index, int doc_row, int col)
{
    cursor_affinity_line = line;
    cursor_affinity_x = index;
    cursor_affinity_doc_row = doc_row;
    cursor_affinity_col = col;
}

static int active_cursor_affinity(int *doc_row, int *col)
{
    if (cursor_affinity_line != cy || cursor_affinity_x != cx ||
        cursor_affinity_doc_row < 0 || cursor_affinity_col < 0)
        return 0;

    *doc_row = cursor_affinity_doc_row;
    *col = cursor_affinity_col;
    return 1;
}

static int wrap_space(char c)
{
    return c == ' ' || c == '\t';
}

static void finish_wrap_row(WrapRow *row, int end, int visual_width)
{
    row->render_end = end;
    row->next_start = end;
    row->cursor_end = end;
    row->visual_width = visual_width;
}

static int char_width_at(const char *line, int index, int col, int *used)
{
    if (line[index] == '\t') {
        if (used)
            *used = 1;
        return char_visual_width(col, line[index]);
    }

    return utf8_char_width(line + index, used);
}

static int measure_visual_width(const char *line, int start, int upto)
{
    int col = 0;

    if (upto < start)
        upto = start;

    for (int i = start; line[i] && i < upto; ) {
        int used = 1;
        int w = char_width_at(line, i, col, &used);

        if (i + used > upto)
            break;
        col += w;
        i += used;
    }

    return col;
}

static void build_wrap_row_width(const char *line, int line_no, int start,
                                 int doc_row, int width, WrapRow *row)
{
    int len = (int)strlen(line);
    int visual_text_capacity;
    int col = 0;
    int last_break = -1;
    int last_break_col = 0;

    row->line = line_no;
    row->render_start = start;
    row->render_end = start;
    row->next_start = start;
    row->cursor_start = start;
    row->cursor_end = start;
    row->visual_width = 0;
    row->doc_row = doc_row;

    if (width < 1)
        width = 1;
    visual_text_capacity = width > 1 ? width - 1 : 1;

    if (len == 0 || start >= len) {
        row->render_start = 0;
        row->render_end = 0;
        row->next_start = 0;
        row->cursor_start = 0;
        row->cursor_end = 0;
        return;
    }

    for (int i = start; i < len; ) {
        int used = 1;
        int w = char_width_at(line, i, col, &used);

        if (col + w > visual_text_capacity) {
            if (last_break > start && last_break < i) {
                finish_wrap_row(row, last_break, last_break_col);
            } else {
                finish_wrap_row(row, i, col);
                if (row->render_end == start)
                    finish_wrap_row(row, i + used,
                                    w > visual_text_capacity ?
                                    visual_text_capacity : w);
            }
            return;
        }

        col += w;
        if (wrap_space(line[i])) {
            last_break = i + used;
            last_break_col = col;
        }
        i += used;
    }

    finish_wrap_row(row, len, col);
}

static int layout_rows_for_line_width(const char *line, int width)
{
    int len = (int)strlen(line);
    int rows = 0;
    int start = 0;

    if (!len)
        return 1;

    while (start < len) {
        WrapRow row;

        build_wrap_row_width(line, -1, start, rows, width, &row);
        rows++;
        if (row.next_start <= start)
            break;
        start = row.next_start;
    }

    return rows;
}

static int layout_document_visual_rows_width(int width)
{
    int rows = 0;

    for (int i = 0; i < line_count; i++)
        rows += layout_rows_for_line_width(lines[i], width);

    return rows;
}

static int layout_document_visual_rows(void)
{
    return layout_document_visual_rows_width(layout_width());
}

static int row_col_for_pos(const WrapRow *row, const char *line, int target)
{
    if (target <= row->render_start)
        return 0;
    if (target >= row->render_end)
        return row->visual_width;
    return measure_visual_width(line, row->render_start, target);
}

static int layout_row_for_line_position_width(const char *line, int line_no,
                                              int target, int doc_row_base,
                                              int width, WrapRow *out)
{
    int len = (int)strlen(line);
    int start = 0;
    int row_no = 0;

    if (target < 0)
        target = 0;
    if (target > len)
        target = len;

    if (!len) {
        build_wrap_row_width(line, line_no, 0, doc_row_base, width, out);
        return 1;
    }

    while (start < len) {
        WrapRow row;

        build_wrap_row_width(line, line_no, start, doc_row_base + row_no,
                             width, &row);
        if (target <= row.render_end) {
            *out = row;
            return 1;
        }
        if (row.next_start <= start)
            break;
        start = row.next_start;
        row_no++;
    }

    build_wrap_row_width(line, line_no, start, doc_row_base + row_no,
                         width, out);
    return 1;
}

static int layout_row_for_position(int line_no, int target, WrapRow *out)
{
    int width = layout_width();
    int doc_row = 0;

    if (line_no < 0)
        line_no = 0;
    if (line_no >= line_count)
        line_no = line_count - 1;

    for (int i = 0; i < line_no; i++)
        doc_row += layout_rows_for_line_width(lines[i], width);

    return layout_row_for_line_position_width(lines[line_no], line_no, target,
                                              doc_row, width, out);
}

static int layout_row_for_doc_row_width(int target_doc_row, int width,
                                        WrapRow *out)
{
    int doc_row = 0;

    if (target_doc_row < 0)
        target_doc_row = 0;

    for (int li = 0; li < line_count; li++) {
        int len = (int)strlen(lines[li]);
        int start = 0;
        int row_no = 0;

        if (!len) {
            if (doc_row == target_doc_row) {
                build_wrap_row_width(lines[li], li, 0, doc_row, width, out);
                return 1;
            }
            doc_row++;
            continue;
        }

        while (start < len) {
            WrapRow row;

            build_wrap_row_width(lines[li], li, start, doc_row + row_no,
                                 width, &row);
            if (doc_row + row_no == target_doc_row) {
                *out = row;
                return 1;
            }
            if (row.next_start <= start)
                break;
            start = row.next_start;
            row_no++;
        }
        doc_row += row_no;
    }

    return 0;
}

static int layout_row_for_doc_row(int target_doc_row, WrapRow *out)
{
    return layout_row_for_doc_row_width(target_doc_row, layout_width(), out);
}

static void pos_to_visual_with_affinity(int line_no, int index,
                                        int preferred_doc_row,
                                        int preferred_col,
                                        int *out_doc_row, int *out_col)
{
    WrapRow row;

    if (preferred_doc_row >= 0 && preferred_col >= 0 &&
        layout_row_for_doc_row(preferred_doc_row, &row) &&
        row.line == line_no &&
        preferred_col <= row.visual_width &&
        visual_to_pos_in_row(&row, preferred_col) == index) {
        *out_doc_row = row.doc_row;
        *out_col = preferred_col;
        return;
    }

    if (!layout_row_for_position(line_no, index, &row)) {
        *out_doc_row = 0;
        *out_col = 0;
        return;
    }

    *out_doc_row = row.doc_row;
    *out_col = row_col_for_pos(&row, lines[row.line], index);
}

static void pos_to_visual(int line_no, int index, int *out_doc_row, int *out_col)
{
    pos_to_visual_with_affinity(line_no, index, -1, -1,
                                out_doc_row, out_col);
}

static int visual_to_pos_in_row(const WrapRow *row, int target_col)
{
    const char *line = lines[row->line];
    int col = 0;

    if (target_col <= 0)
        return row->cursor_start;
    if (target_col >= row->visual_width)
        return row->cursor_end;

    for (int i = row->render_start; i < row->render_end; ) {
        int used = 1;
        int w = char_width_at(line, i, col, &used);

        if (col + w > target_col)
            return i;
        col += w;
        i += used;
    }

    return row->cursor_end;
}

static int visual_to_pos_with_affinity(int doc_row, int target_col,
                                       int *out_line, int *out_index,
                                       int *out_affinity_col)
{
    WrapRow row;
    int actual_col;

    if (!layout_row_for_doc_row(doc_row, &row))
        return 0;

    actual_col = target_col;
    if (actual_col < 0)
        actual_col = 0;
    if (actual_col > row.visual_width)
        actual_col = row.visual_width;

    *out_line = row.line;
    *out_index = visual_to_pos_in_row(&row, actual_col);
    *out_affinity_col = actual_col;
    return 1;
}

static int visual_to_pos(int doc_row, int target_col,
                         int *out_line, int *out_index)
{
    int affinity_col;

    return visual_to_pos_with_affinity(doc_row, target_col,
                                       out_line, out_index,
                                       &affinity_col);
}

static void cursor_visual_pos(int *out_doc_row, int *out_col)
{
    int affinity_doc_row;
    int affinity_col;

    if (active_cursor_affinity(&affinity_doc_row, &affinity_col)) {
        pos_to_visual_with_affinity(cy, cx, affinity_doc_row, affinity_col,
                                    out_doc_row, out_col);
        return;
    }

    pos_to_visual(cy, cx, out_doc_row, out_col);
}

static int layout_row_for_cursor(WrapRow *out)
{
    int affinity_doc_row;
    int affinity_col;

    if (active_cursor_affinity(&affinity_doc_row, &affinity_col) &&
        layout_row_for_doc_row(affinity_doc_row, out) &&
        out->line == cy &&
        affinity_col <= out->visual_width &&
        visual_to_pos_in_row(out, affinity_col) == cx)
        return 1;

    return layout_row_for_position(cy, cx, out);
}

static void clamp_top(void)
{
    BodyGeometry geo = body_geometry();
    int max_top = layout_document_visual_rows() - geo.visible_rows;

    if (max_top < 0)
        max_top = 0;
    if (top < 0)
        top = 0;
    if (top > max_top)
        top = max_top;
}

static void wrap_segment(const char *line, int start, int *end, int *next_start)
{
    WrapRow row;

    build_wrap_row_width(line, -1, start, 0, layout_width(), &row);
    *end = row.render_end;
    *next_start = row.next_start;
}

static int visual_col_range(const char *line, int start, int upto)
{
    return measure_visual_width(line, start, upto);
}

static int visual_rows_for_line(const char *line)
{
    return layout_rows_for_line_width(line, layout_width());
}

static void wrapped_pos_for_index(const char *line, int target, int *out_row, int *out_col)
{
    WrapRow row;

    layout_row_for_line_position_width(line, -1, target, 0,
                                       layout_width(), &row);
    *out_row = row.doc_row;
    *out_col = row_col_for_pos(&row, line, target);
}

static int logical_cursor_row(void)
{
    int row;
    int col;

    cursor_visual_pos(&row, &col);
    return row;
}

static int document_visual_rows(void)
{
    return layout_document_visual_rows();
}

static void keep_cursor_visible(void)
{
    BodyGeometry geo = body_geometry();
    int crow;

    crow = logical_cursor_row();
    if (center_lock_enabled) {
        int anchor = (geo.visible_rows * 45) / 100;
        int max_top = document_visual_rows() - geo.visible_rows;

        if (max_top < 0)
            max_top = 0;
        top = crow - anchor;
        if (top < 0)
            top = 0;
        if (top > max_top)
            top = max_top;
        return;
    }

    if (crow < top)
        top = crow;
    else if (crow >= top + geo.visible_rows)
        top = crow - geo.visible_rows + 1;

    if (top < 0)
        top = 0;
    clamp_top();
}

static void cursor_screen_pos(int *out_row, int *out_col)
{
    BodyGeometry geo = body_geometry();
    int doc_row;
    int wc;
    int min_row = geo.top_pad;
    int max_row = geo.bottom - 1;
    int max_col = geo.body_width - 1;

    if (max_row < 0)
        max_row = 0;
    if (min_row > max_row)
        min_row = max_row;
    if (max_col < 0)
        max_col = 0;

    cursor_visual_pos(&doc_row, &wc);
    *out_row = geo.top_pad + (doc_row - top);
    *out_col = wc;

    if (*out_row < min_row)
        *out_row = min_row;
    if (*out_row > max_row)
        *out_row = max_row;
    if (*out_col < 0)
        *out_col = 0;
    if (*out_col > max_col)
        *out_col = max_col;
}

static void begin_selection_if_needed(void)
{
    if (!selecting) {
        selecting = 1;
        sel_cy = cy;
        sel_cx = cx;
    }
}

static void clear_selection(void)
{
    selecting = 0;
}

static int selection_nonempty(void)
{
    return selecting && (sel_cy != cy || sel_cx != cx);
}

static int editor_cursor_visibility(void)
{
    if (idle_cursor_hidden)
        return 0;
    if (find_active && find_match_y >= 0 && find_match_len > 0)
        return 0;
    return selection_nonempty() ? 0 : 1;
}

static void ordered_selection(int *sy, int *sx, int *ey, int *ex)
{
    *sy = sel_cy;
    *sx = sel_cx;
    *ey = cy;
    *ex = cx;

    if (pos_before(*ey, *ex, *sy, *sx)) {
        *sy = cy;
        *sx = cx;
        *ey = sel_cy;
        *ex = sel_cx;
    }
}

static int char_selected(int y, int x)
{
    int sy;
    int sx;
    int ey;
    int ex;

    if (!selecting)
        return 0;

    ordered_selection(&sy, &sx, &ey, &ex);
    return !pos_before(y, x, sy, sx) && pos_before(y, x, ey, ex);
}

static int empty_row_selected(int y)
{
    int sy;
    int sx;
    int ey;
    int ex;

    if (!selecting)
        return 0;

    ordered_selection(&sy, &sx, &ey, &ex);

    if (!pos_before(y, 0, sy, sx) && pos_before(y, 0, ey, ex))
        return 1;

    return (sy != ey || sx != ex) &&
           y == ey &&
           ex == 0 &&
           y >= 0 &&
           y < line_count &&
           lines[y][0] == '\0';
}

static int char_find_highlight(int y, int x)
{
    return find_active &&
           y == find_match_y &&
           x >= find_match_x &&
           x < find_match_x + find_match_len;
}

static int word_count(void)
{
    int words = 0;
    int in_word = 0;

    for (int y = 0; y < line_count; y++) {
        for (unsigned char *p = (unsigned char *)lines[y]; *p; p++) {
            if (isspace(*p)) {
                in_word = 0;
            } else if (!in_word) {
                words++;
                in_word = 1;
            }
        }
        in_word = 0;
    }

    return words;
}

static void draw_text_clipped(int row, int col, const char *s, attr_t attr, int maxw)
{
    int used_width = 0;

    if (row < 0 || row >= LINES || col >= COLS || maxw <= 0)
        return;

    if (col < 0)
        return;
    if (maxw > COLS - col)
        maxw = COLS - col;

    while (*s && used_width < maxw) {
        wchar_t wc;
        wchar_t text[2];
        cchar_t cell;
        int bytes;
        int width = utf8_decode(s, &wc, &bytes);

        if (used_width + width > maxw)
            break;
        text[0] = wc;
        text[1] = L'\0';
        setcchar(&cell, text, attr, 0, NULL);
        mvadd_wch(row, col + used_width, &cell);
        used_width += width;
        s += bytes;
    }
}

static void fill_body_row(int row, int left, int used_width)
{
    int width = body_geometry().body_width;

    if (left >= COLS || row < 0 || row >= LINES)
        return;
    if (used_width < 0)
        used_width = 0;
    if (used_width < width) {
        attrset(body_attr());
        mvhline(row, left + used_width, ' ', width - used_width);
    }
}

static void draw_line_wrapped_from(int *rowp, int left, int li, const char *line,
                                   int skip_rows, int bottom)
{
    int row = *rowp;
    int len = (int)strlen(line);
    int start = 0;
    int wrapped_row = 0;
    int width = body_geometry().body_width;

    if (!len) {
        if (skip_rows <= 0 && row < bottom) {
            fill_body_row(row, left, 0);
            if (empty_row_selected(li))
                mvaddch(row, left, ' ' | selection_attr());
            row++;
        }
        *rowp = row;
        return;
    }

    while (start < len && row < bottom) {
        WrapRow wrap;
        int col = 0;
        int painted_width = 0;

        build_wrap_row_width(line, li, start, wrapped_row, width, &wrap);

        if (wrapped_row >= skip_rows) {
            for (int i = wrap.render_start; i < wrap.render_end && row < bottom; ) {
                attr_t attr = char_selected(li, i) || char_find_highlight(li, i) ? selection_attr() : body_attr();

                if (line[i] == '\t') {
                    int spaces = TAB_WIDTH - (col % TAB_WIDTH);
                    int visible_spaces = spaces;

                    if (visible_spaces > wrap.visual_width - col)
                        visible_spaces = wrap.visual_width - col;
                    if (visible_spaces > 0) {
                        attrset(attr);
                        mvhline(row, left + col, ' ', visible_spaces);
                        painted_width = col + visible_spaces;
                    }
                    col += spaces;
                    i++;
                } else {
                    wchar_t wc;
                    wchar_t text[2];
                    cchar_t cell;
                    int used;
                    int w = utf8_decode(line + i, &wc, &used);

                    if (col + w <= wrap.visual_width) {
                        text[0] = wc;
                        text[1] = L'\0';
                        setcchar(&cell, text, attr, 0, NULL);
                        mvadd_wch(row, left + col, &cell);
                        painted_width = col + w;
                    }

                    col += w;
                    i += used;
                }
            }
            fill_body_row(row, left, painted_width);
            row++;
        }

        wrapped_row++;
        start = wrap.next_start;
    }

    *rowp = row;
}

static void ensure_screen_storage(int width)
{
    ScreenRow *grown_rows;
    ScreenCell *grown_cells;
    size_t cell_count = (size_t)LINES * (size_t)width;

    if (screen_row_capacity < LINES) {
        grown_rows = realloc(desired_rows,
                             sizeof(*desired_rows) * (size_t)LINES);
        if (!grown_rows) {
            endwin();
            perror("realloc");
            exit(1);
        }
        desired_rows = grown_rows;
        screen_row_capacity = LINES;
        screen_cache_valid = 0;
    }

    if (screen_cell_capacity >= cell_count)
        return;

    grown_cells = realloc(screen_cells, sizeof(*screen_cells) * cell_count);
    if (!grown_cells) {
        endwin();
        perror("realloc");
        exit(1);
    }
    screen_cells = grown_cells;

    grown_cells = realloc(desired_cells, sizeof(*desired_cells) * cell_count);
    if (!grown_cells) {
        endwin();
        perror("realloc");
        exit(1);
    }
    desired_cells = grown_cells;
    screen_cell_capacity = cell_count;
    screen_cache_valid = 0;
}

static void describe_screen_row(ScreenRow *desc, int kind, const WrapRow *wrap)
{
    desc->kind = kind;
    desc->wrap = *wrap;
}

static void build_visible_screen_rows(ScreenRow *rows)
{
    BodyGeometry geo = body_geometry();
    int physical_row = geo.top_pad;
    int doc_row = 0;
    int width = geo.body_width;

    memset(rows, 0, sizeof(*rows) * (size_t)LINES);

    for (int li = 0; li < line_count && physical_row < geo.bottom; li++) {
        int len = (int)strlen(lines[li]);

        if (!len) {
            WrapRow wrap;

            build_wrap_row_width(lines[li], li, 0, doc_row, width, &wrap);
            if (doc_row >= top) {
                describe_screen_row(&rows[physical_row], SCREEN_ROW_EMPTY,
                                    &wrap);
                physical_row++;
            }
            doc_row++;
            continue;
        }

        for (int start = 0; start < len; ) {
            WrapRow wrap;

            build_wrap_row_width(lines[li], li, start, doc_row, width, &wrap);
            if (doc_row >= top && physical_row < geo.bottom) {
                describe_screen_row(&rows[physical_row], SCREEN_ROW_TEXT,
                                    &wrap);
                physical_row++;
            }
            doc_row++;
            start = wrap.next_start;
        }
    }
}

static void set_desired_blank(ScreenCell *cell, attr_t attr)
{
    cell->wc = L' ';
    cell->attr = attr;
    cell->kind = SCREEN_CELL_BLANK;
}

static void build_desired_body_cells(int width)
{
    BodyGeometry geo = body_geometry();
    size_t cell_count = (size_t)LINES * (size_t)width;

    for (size_t i = 0; i < cell_count; i++)
        set_desired_blank(&desired_cells[i], body_attr());

    build_visible_screen_rows(desired_rows);
    for (int row = geo.top_pad; row < geo.bottom; row++) {
        const ScreenRow *desc = &desired_rows[row];
        ScreenCell *cells = desired_cells + (size_t)row * (size_t)width;
        int col = 0;

        if (desc->kind == SCREEN_ROW_UNUSED)
            continue;

        if (desc->kind == SCREEN_ROW_EMPTY) {
            if (width > 0 && empty_row_selected(desc->wrap.line))
                set_desired_blank(&cells[0], selection_attr());
            continue;
        }

        for (int i = desc->wrap.render_start; i < desc->wrap.render_end; ) {
            attr_t attr = char_selected(desc->wrap.line, i) ||
                          char_find_highlight(desc->wrap.line, i) ?
                          selection_attr() : body_attr();

            if (lines[desc->wrap.line][i] == '\t') {
                int spaces = TAB_WIDTH - (col % TAB_WIDTH);

                for (int k = 0;
                     k < spaces && col + k < desc->wrap.visual_width;
                     k++)
                    set_desired_blank(&cells[col + k], attr);
                col += spaces;
                i++;
            } else {
                wchar_t wc;
                int used;
                int glyph_width = utf8_decode(
                    lines[desc->wrap.line] + i, &wc, &used);

                if (col + glyph_width <= desc->wrap.visual_width) {
                    cells[col].wc = wc;
                    cells[col].attr = attr;
                    cells[col].kind = SCREEN_CELL_GLYPH;
                    for (int k = 1; k < glyph_width; k++) {
                        cells[col + k].wc = L'\0';
                        cells[col + k].attr = attr;
                        cells[col + k].kind = SCREEN_CELL_CONTINUATION;
                    }
                }
                col += glyph_width;
                i += used;
            }
        }
    }
}

static int screen_cells_equal(const ScreenCell *a, const ScreenCell *b)
{
    return a->wc == b->wc && a->attr == b->attr && a->kind == b->kind;
}

static int body_cells_valid(const ScreenCell *cells, int width)
{
    for (int row = 0; row < LINES; row++) {
        const ScreenCell *line = cells + (size_t)row * (size_t)width;

        for (int col = 0; col < width; col++) {
            if (line[col].kind > SCREEN_CELL_CONTINUATION)
                return 0;
            if (line[col].kind == SCREEN_CELL_CONTINUATION &&
                (col == 0 || line[col - 1].kind == SCREEN_CELL_BLANK))
                return 0;
        }
    }
    return 1;
}

static void destroy_body_window(void)
{
    if (body_window)
        delwin(body_window);
    body_window = NULL;
    body_window_height = 0;
    body_window_width = 0;
    body_window_top = 0;
    body_window_left = 0;
}

static int ensure_body_window(int left, int width, int bottom)
{
    BodyGeometry geo = body_geometry();
    int height = bottom - geo.top_pad;

    if (!windowed_redraw_enabled || height < 1 || width < 1)
        return 0;
    if (body_window &&
        body_window_height == height && body_window_width == width &&
        body_window_top == geo.top_pad && body_window_left == left)
        return 1;

    destroy_body_window();
    body_window = newwin(height, width, geo.top_pad, left);
    if (!body_window) {
        windowed_redraw_enabled = 0;
        return 0;
    }

    body_window_height = height;
    body_window_width = width;
    body_window_top = geo.top_pad;
    body_window_left = left;
    wbkgdset(body_window, (chtype)' ' | body_attr());
    scrollok(body_window, FALSE);
    idlok(body_window, FALSE);
    leaveok(body_window, FALSE);
    screen_cache_valid = 0;
    return 1;
}

static void sync_body_window_from_stdscr(void)
{
    if (!body_window)
        return;
    copywin(stdscr, body_window,
            body_window_top, body_window_left, 0, 0,
            body_window_height - 1, body_window_width - 1, FALSE);
}

static int move_body_window_cursor(int screen_row, int col)
{
    int row = screen_row - body_window_top;

    if (!body_window || row < 0 || row >= body_window_height ||
        col < 0 || col >= body_window_width)
        return 0;
    wmove(body_window, row, col);
    return 1;
}

static void refresh_windowed_screen(int cursor_row, int cursor_col, int left)
{
    if (!body_window ||
        !move_body_window_cursor(cursor_row, cursor_col)) {
        move(cursor_row, left + cursor_col);
        refresh();
        return;
    }

    wnoutrefresh(stdscr);
    wnoutrefresh(body_window);
    doupdate();
}

static void mark_wide_group(const ScreenCell *cells, unsigned char *dirty,
                            int col, int width)
{
    int start = col;
    int end;

    while (start > 0 &&
           cells[start].kind == SCREEN_CELL_CONTINUATION)
        start--;
    end = start + 1;
    while (end < width &&
           cells[end].kind == SCREEN_CELL_CONTINUATION)
        end++;
    memset(dirty + start, 1, (size_t)(end - start));
}

static void emit_desired_run(WINDOW *window, int row, int left,
                             int start, int end,
                             const ScreenCell *cells)
{
    for (int col = start; col < end; ) {
        const ScreenCell *cell = &cells[col];

        if (cell->kind == SCREEN_CELL_BLANK) {
            int blank_end = col + 1;

            while (blank_end < end &&
                   cells[blank_end].kind == SCREEN_CELL_BLANK &&
                   cells[blank_end].attr == cell->attr)
                blank_end++;
            wattrset(window, cell->attr);
            mvwhline(window, row, left + col, ' ', blank_end - col);
            col = blank_end;
        } else if (cell->kind == SCREEN_CELL_GLYPH) {
            cchar_t output;
            wchar_t text[2] = {cell->wc, L'\0'};
            int glyph_end = col + 1;

            while (glyph_end < end &&
                   cells[glyph_end].kind == SCREEN_CELL_CONTINUATION)
                glyph_end++;
            setcchar(&output, text, cell->attr, 0, NULL);
            mvwadd_wch(window, row, left + col, &output);
            col = glyph_end;
        } else {
            col++;
        }
    }
}

static void repaint_changed_body_row(int row, int left, int width)
{
    WINDOW *window = body_window ? body_window : stdscr;
    int window_row = body_window ? row - body_window_top : row;
    int window_left = body_window ? 0 : left;
    ScreenCell *old = screen_cells + (size_t)row * (size_t)width;
    ScreenCell *desired = desired_cells + (size_t)row * (size_t)width;
    unsigned char dirty[width];

    memset(dirty, 0, sizeof(dirty));
    for (int col = 0; col < width; col++) {
        if (!screen_cells_equal(&old[col], &desired[col])) {
            mark_wide_group(old, dirty, col, width);
            mark_wide_group(desired, dirty, col, width);
        }
    }

    for (int col = 0; col < width; ) {
        int end;

        if (!dirty[col]) {
            col++;
            continue;
        }
        end = col + 1;
        while (end < width && dirty[end])
            end++;
        emit_desired_run(window, window_row, window_left,
                         col, end, desired);
        col = end;
    }

    memcpy(old, desired, sizeof(*old) * (size_t)width);
}

static void format_screen_chrome(char *title, size_t titlesz,
                                 char *wc, size_t wcsz,
                                 char *status, size_t statussz)
{
    char shown[512];

    snprintf(wc, wcsz, "%d words", word_count());
    display_name(shown, sizeof(shown));
    snprintf(title, titlesz, "%s%s", shown, dirty ? " *" : "");
    snprintf(status, statussz, "%s", status_msg[0] ? status_msg : help_text);
}

static void capture_screen_cache(int left)
{
    int width = body_geometry().body_width;
    size_t cell_count;

    ensure_screen_storage(width);
    build_desired_body_cells(width);
    cell_count = (size_t)LINES * (size_t)width;
    memcpy(screen_cells, desired_cells, sizeof(*screen_cells) * cell_count);
    format_screen_chrome(screen_cache_title, sizeof(screen_cache_title),
                         screen_cache_wc, sizeof(screen_cache_wc),
                         screen_cache_status, sizeof(screen_cache_status));
    screen_cache_lines = LINES;
    screen_cache_cols = COLS;
    screen_cache_left = left;
    screen_cache_text_width = config.text_width;
    screen_cache_top_pad = config.top_pad;
    screen_cache_distraction_free = distraction_free;
    screen_cache_top = top;
    screen_cache_valid = 1;
}

static int screen_cache_geometry_matches(int left)
{
    return screen_cache_valid &&
           screen_cache_lines == LINES &&
           screen_cache_cols == COLS &&
           screen_cache_left == left &&
           screen_cache_text_width == config.text_width &&
           screen_cache_top_pad == config.top_pad &&
           screen_cache_distraction_free == distraction_free;
}

static void draw_screen_impl(int update)
{
    BodyGeometry geo;

    if (update)
        set_cursor_visibility(0);

    char wc[64];
    char shown[512];
    char title[700];
    int row;
    int logical_row = 0;
    int cr;
    int cc;

    screen_cache_valid = 0;
    keep_cursor_visible();
    geo = body_geometry();
    erase();

    snprintf(wc, sizeof(wc), "%d words", word_count());
    display_name(shown, sizeof(shown));
    snprintf(title, sizeof(title), "%s%s", shown, dirty ? " *" : "");

    if (!distraction_free) {
        int wc_width = (int)strlen(wc);
        int title_width = COLS - wc_width - 3;

        attrset(body_attr());
        draw_text_clipped(0, 1, title, body_attr(), title_width);
        draw_text_clipped(0, COLS - wc_width - 1, wc,
                          body_attr(), wc_width);
    }

    row = geo.top_pad;
    for (int li = 0; li < line_count && row < geo.bottom; li++) {
        int rows = visual_rows_for_line(lines[li]);
        int skip_rows;

        if (logical_row + rows <= top) {
            logical_row += rows;
            continue;
        }

        skip_rows = top - logical_row;
        if (skip_rows < 0)
            skip_rows = 0;

        draw_line_wrapped_from(&row, geo.left, li, lines[li], skip_rows,
                               geo.bottom);
        logical_row += rows;
    }

    if (!distraction_free) {
        attrset(chrome_attr());
        mvhline(LINES - 1, 0, ' ', COLS);

        if (status_msg[0]) {
            draw_text_clipped(LINES - 1, 1, status_msg, chrome_attr(), COLS - 2);
        } else {
            draw_text_clipped(LINES - 1, 1, help_text,
                              chrome_attr(), COLS - 2);
        }
    }

    cursor_screen_pos(&cr, &cc);
    attrset(body_attr());
    move(cr, geo.left + cc);
    if (update) {
        refresh();
        /*
         * A terminal block cursor masks the reverse-video cell beneath it.
         * While extending a selection this made its active-end character look
         * unselected; a one-character selection looked like it had not begun
         * at all until keyboard repeat moved the cursor again.
         */
        set_cursor_visibility(editor_cursor_visibility());
    }
}

static void draw_screen(void)
{
    BodyGeometry geo;

    /*
     * Keep the terminal cursor hidden while repainting. Otherwise ncurses can
     * briefly expose it at intermediate draw positions during vertical scroll,
     * producing the side-blink / eyeblink jump.
     */
    set_cursor_visibility(0);

    char title[700];
    char wc[64];
    char status[512];
    int body_ready;
    int cr;
    int cc;

    keep_cursor_visible();
    geo = body_geometry();

    ensure_screen_storage(geo.body_width);
    body_ready = ensure_body_window(geo.left, geo.body_width, geo.bottom);
    if ((!distraction_free && LINES < 2) ||
        !screen_cache_geometry_matches(geo.left) ||
        !body_cells_valid(screen_cells, geo.body_width)) {
        draw_screen_impl(body_ready ? 0 : 1);
        capture_screen_cache(geo.left);
        if (body_ready) {
            sync_body_window_from_stdscr();
            cursor_screen_pos(&cr, &cc);
            refresh_windowed_screen(cr, cc, geo.left);
            set_cursor_visibility(editor_cursor_visibility());
        }
        return;
    }

    build_desired_body_cells(geo.body_width);
    for (int row = geo.top_pad; row < geo.bottom; row++)
        repaint_changed_body_row(row, geo.left, geo.body_width);

    format_screen_chrome(title, sizeof(title), wc, sizeof(wc),
                         status, sizeof(status));

    if (!distraction_free &&
        (strcmp(title, screen_cache_title) != 0 ||
         strcmp(wc, screen_cache_wc) != 0)) {
        int wc_width = (int)strlen(wc);
        int title_width = COLS - wc_width - 3;

        attrset(body_attr());
        mvhline(0, 0, ' ', COLS);
        draw_text_clipped(0, 1, title, body_attr(), title_width);
        draw_text_clipped(0, COLS - wc_width - 1, wc,
                          body_attr(), wc_width);
    }

    if (!distraction_free && strcmp(status, screen_cache_status) != 0) {
        attrset(chrome_attr());
        mvhline(LINES - 1, 0, ' ', COLS);
        draw_text_clipped(LINES - 1, 1, status,
                          chrome_attr(), COLS - 2);
    }

    snprintf(screen_cache_title, sizeof(screen_cache_title), "%s", title);
    snprintf(screen_cache_wc, sizeof(screen_cache_wc), "%s", wc);
    snprintf(screen_cache_status, sizeof(screen_cache_status), "%s", status);
    screen_cache_top = top;

    cursor_screen_pos(&cr, &cc);
    if (body_ready) {
        refresh_windowed_screen(cr, cc, geo.left);
    } else {
        attrset(body_attr());
        move(cr, geo.left + cc);
        refresh();
    }
    set_cursor_visibility(editor_cursor_visibility());
}

static void clamp_cursor(void)
{
    if (cy < 0)
        cy = 0;
    if (cy >= line_count)
        cy = line_count - 1;
    if (cx < 0)
        cx = 0;
    if (cx > (int)strlen(lines[cy]))
        cx = (int)strlen(lines[cy]);
}

static int document_cursor_index(void)
{
    int index = cx;

    for (int i = 0; i < cy; i++)
        index += (int)strlen(lines[i]) + 1;

    return index;
}

static void trace_space_insert(void)
{
    int render_y = 0;
    int render_x = 0;
    int wrapped_row = 0;
    WrapRow row;

    if (!env_enabled("SW_TRACE_SPACE"))
        return;

    cursor_visual_pos(&render_y, &render_x);
    if (layout_row_for_cursor(&row))
        wrapped_row = row.doc_row;
    else
        wrapped_row = render_y;

    fprintf(stderr,
            "cursor_index=%d cursor_line=%d cursor_column=%d "
            "render_x=%d render_y=%d desired_x=%d line_length=%d "
            "wrapped_row=%d\n",
            document_cursor_index(), cy, cx, render_x, render_y, goal_col,
            (int)strlen(lines[cy]), wrapped_row);
    fflush(stderr);
}

static void insert_char(int ch)
{
    char *line = lines[cy];
    int len = (int)strlen(line);

    maybe_save_typing_undo();

    if (len >= MAX_LINE - 2) {
        set_status("Line is full");
        return;
    }

    memmove(line + cx + 1, line + cx, (size_t)(len - cx + 1));
    line[cx++] = (char)ch;
    mark_edit();
    if (ch == ' ')
        trace_space_insert();
}

static void newline(void)
{
    char *line;
    char *tail;

    break_undo_burst();
    save_undo();

    if (line_count >= MAX_LINES) {
        set_status("Document is full");
        return;
    }

    line = lines[cy];
    tail = new_line(line + cx);
    line[cx] = '\0';

    memmove(&lines[cy + 2], &lines[cy + 1], sizeof(char *) * (size_t)(line_count - cy - 1));
    lines[cy + 1] = tail;
    line_count++;
    cy++;
    cx = 0;
    mark_edit();
}

static void delete_selection(void);

static void backspace(void)
{
    break_undo_burst();

    if (selecting) {
        delete_selection();
        return;
    }

    if (cx == 0 && cy == 0)
        return;

    save_undo();

    if (cx > 0) {
        char *line = lines[cy];
        memmove(line + cx - 1, line + cx, strlen(line + cx) + 1);
        cx--;
    } else {
        int prev_len = (int)strlen(lines[cy - 1]);

        if (prev_len + (int)strlen(lines[cy]) >= MAX_LINE - 1) {
            set_status("Joined line would be too long");
            return;
        }

        strncat(lines[cy - 1], lines[cy], MAX_LINE - strlen(lines[cy - 1]) - 1);
        free(lines[cy]);
        memmove(&lines[cy], &lines[cy + 1], sizeof(char *) * (size_t)(line_count - cy - 1));
        line_count--;
        cy--;
        cx = prev_len;
    }

    mark_edit();
}

static void delete_forward(void)
{
    break_undo_burst();

    if (selecting) {
        delete_selection();
        return;
    }

    if (cx == (int)strlen(lines[cy]) && cy == line_count - 1)
        return;

    save_undo();

    if (cx < (int)strlen(lines[cy])) {
        memmove(lines[cy] + cx, lines[cy] + cx + 1, strlen(lines[cy] + cx + 1) + 1);
    } else {
        if (strlen(lines[cy]) + strlen(lines[cy + 1]) >= MAX_LINE - 1) {
            set_status("Joined line would be too long");
            return;
        }

        strncat(lines[cy], lines[cy + 1], MAX_LINE - strlen(lines[cy]) - 1);
        free(lines[cy + 1]);
        memmove(&lines[cy + 1], &lines[cy + 2], sizeof(char *) * (size_t)(line_count - cy - 2));
        line_count--;
    }

    mark_edit();
}

static void delete_selection(void)
{
    int sy;
    int sx;
    int ey;
    int ex;

    if (!selecting)
        return;

    break_undo_burst();
    ordered_selection(&sy, &sx, &ey, &ex);

    if (sy == ey && sx == ex) {
        clear_selection();
        return;
    }

    save_undo();

    if (sy == ey) {
        memmove(lines[sy] + sx, lines[sy] + ex, strlen(lines[sy] + ex) + 1);
    } else {
        lines[sy][sx] = '\0';
        strncat(lines[sy], lines[ey] + ex, MAX_LINE - strlen(lines[sy]) - 1);

        for (int y = sy + 1; y <= ey; y++)
            free(lines[y]);
        memmove(&lines[sy + 1], &lines[ey + 1], sizeof(char *) * (size_t)(line_count - ey - 1));
        line_count -= ey - sy;
    }

    cy = sy;
    cx = sx;

    goal_col = -1;
    clear_cursor_affinity();
    clear_selection();
    clamp_cursor();
    keep_cursor_visible();
    mark_edit();
}


static int command_exists(const char *cmd)
{
    char probe[256];

    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", cmd);
    return system(probe) == 0;
}

static int detect_clipboard_backend(void)
{
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *x11 = getenv("DISPLAY");

    if (wayland && *wayland &&
        command_exists("wl-copy") && command_exists("wl-paste"))
        return CLIP_BACKEND_WL;

    if (x11 && *x11 && command_exists("xclip"))
        return CLIP_BACKEND_XCLIP;

    if (x11 && *x11 && command_exists("xsel"))
        return CLIP_BACKEND_XSEL;

    return CLIP_BACKEND_NONE;
}

static void warn_no_system_clipboard_once(void)
{
    if (!clip_warned) {
        set_status("No system clipboard tool. Install wl-clipboard, xclip, or xsel.");
        clip_warned = 1;
    }
}

static void write_system_clipboard(const char *text)
{
    char tmpname[] = "/tmp/simplewords-clip-XXXXXX";
    char cmd[PATH_MAX + 256];
    int fd;
    FILE *fp;

    if (!text)
        return;

    fd = mkstemp(tmpname);
    if (fd < 0) {
        warn_no_system_clipboard_once();
        return;
    }

    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmpname);
        warn_no_system_clipboard_once();
        return;
    }

    fputs(text, fp);
    fclose(fp);

    switch (detect_clipboard_backend()) {
    case CLIP_BACKEND_WL:
        snprintf(cmd, sizeof(cmd),
                 "/usr/bin/wl-copy --type text/plain < '%s' 2>/dev/null",
                 tmpname);
        break;
    case CLIP_BACKEND_XCLIP:
        snprintf(cmd, sizeof(cmd),
                 "xclip -selection clipboard < '%s' 2>/dev/null",
                 tmpname);
        break;
    case CLIP_BACKEND_XSEL:
        snprintf(cmd, sizeof(cmd),
                 "xsel --clipboard --input < '%s' 2>/dev/null",
                 tmpname);
        break;
    default:
        unlink(tmpname);
        warn_no_system_clipboard_once();
        return;
    }

    {
        int ignored = system(cmd);
        (void)ignored;
    }
    unlink(tmpname);
}

static char *read_system_clipboard(void)
{
    FILE *fp = NULL;
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    int ch;

    switch (detect_clipboard_backend()) {
    case CLIP_BACKEND_WL:
        fp = popen("/usr/bin/wl-paste --no-newline 2>/dev/null", "r");
        break;
    case CLIP_BACKEND_XCLIP:
        fp = popen("xclip -selection clipboard -o 2>/dev/null", "r");
        break;
    case CLIP_BACKEND_XSEL:
        fp = popen("xsel --clipboard --output 2>/dev/null", "r");
        break;
    default:
        warn_no_system_clipboard_once();
        return NULL;
    }

    if (!fp)
        return NULL;

    while ((ch = fgetc(fp)) != EOF) {
        if (len + 2 > cap) {
            size_t new_cap = cap ? cap * 2 : 4096;
            char *grown = realloc(buf, new_cap);
            if (!grown) {
                free(buf);
                pclose(fp);
                return NULL;
            }
            buf = grown;
            cap = new_cap;
        }
        buf[len++] = (char)ch;
    }

    pclose(fp);

    if (!buf)
        return NULL;

    buf[len] = '\0';
    return buf;
}


static void copy_selection(void)
{
    int sy;
    int sx;
    int ey;
    int ex;
    size_t cap = 1;

    if (!selecting)
        return;

    ordered_selection(&sy, &sx, &ey, &ex);
    for (int y = sy; y <= ey; y++)
        cap += strlen(lines[y]) + 1;

    free(clip);
    clip = calloc(cap, 1);
    if (!clip)
        exit(1);

    for (int y = sy; y <= ey; y++) {
        int start = y == sy ? sx : 0;
        int end = y == ey ? ex : (int)strlen(lines[y]);

        if (end > start)
            strncat(clip, lines[y] + start, (size_t)(end - start));
        if (y != ey)
            strcat(clip, "\n");
    }

    write_system_clipboard(clip);
    set_status("Copied");
}

static void cut_selection(void)
{
    if (!selecting)
        return;
    copy_selection();
    delete_selection();
    set_status("Cut");
}

static void paste_clipboard(void)
{
    char *system_clip = read_system_clipboard();
    const char *text = NULL;

    if (system_clip && system_clip[0])
        text = system_clip;
    else if (clip && clip[0])
        text = clip;

    if (!text) {
        if (detect_clipboard_backend() == CLIP_BACKEND_NONE)
            warn_no_system_clipboard_once();
        else
            set_status("Clipboard empty");
        free(system_clip);
        return;
    }

    break_undo_burst();
    save_undo();
    suppress_undo = 1;

    if (selecting)
        delete_selection();

    for (const char *p = text; *p; p++) {
        if (*p == '\n')
            newline();
        else
            insert_char((unsigned char)*p);
    }

    suppress_undo = 0;
    break_undo_burst();
    mark_edit();
    set_status("Pasted");

    free(system_clip);
}

static int index_for_visual_col(const char *line, int start, int end, int target_col)
{
    int col = 0;

    if (target_col <= 0)
        return start;

    for (int i = start; i < end; ) {
        int used = 1;
        int w;

        w = char_width_at(line, i, col, &used);

        if (col + w > target_col)
            return i;

        col += w;
        i += used;
    }

    return end;
}

static void move_left(int extend)
{
    int doc_row;
    int col;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    cursor_visual_pos(&doc_row, &col);
    if (col == 0 && doc_row > 0) {
        WrapRow previous;

        if (layout_row_for_doc_row(doc_row - 1, &previous) &&
            previous.line == cy && previous.cursor_end == cx) {
            set_cursor_affinity(cy, cx, previous.doc_row,
                                previous.visual_width);
            return;
        }
    }

    clear_cursor_affinity();
    if (cx > 0)
        cx--;
    else if (cy > 0) {
        cy--;
        cx = (int)strlen(lines[cy]);
    }
}

static void move_right(int extend)
{
    int affinity_doc_row;
    int affinity_col;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    if (active_cursor_affinity(&affinity_doc_row, &affinity_col)) {
        WrapRow current;
        WrapRow next;

        if (layout_row_for_doc_row(affinity_doc_row, &current) &&
            affinity_col == current.visual_width &&
            current.line == cy && current.cursor_end == cx &&
            layout_row_for_doc_row(affinity_doc_row + 1, &next) &&
            next.line == cy && next.cursor_start == cx) {
            set_cursor_affinity(cy, cx, next.doc_row, 0);
            return;
        }
    }

    clear_cursor_affinity();
    if (cx < (int)strlen(lines[cy]))
        cx++;
    else if (cy < line_count - 1) {
        cy++;
        cx = 0;
    }
}

static void move_visual_line(int dir, int extend)
{
    int doc_row;
    int col;
    int target_doc_row;
    int total_rows;
    int out_line;
    int out_index;
    int affinity_col;

    break_undo_burst();
    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    cursor_visual_pos(&doc_row, &col);
    if (goal_col < 0)
        goal_col = col;

    total_rows = document_visual_rows();
    target_doc_row = doc_row + dir;

    if (target_doc_row < 0) {
        if (extend)
            cx = 0;
        return;
    }
    if (target_doc_row >= total_rows) {
        if (extend)
            cx = (int)strlen(lines[cy]);
        return;
    }

    if (visual_to_pos_with_affinity(target_doc_row, goal_col,
                                    &out_line, &out_index, &affinity_col)) {
        cy = out_line;
        cx = out_index;
        set_cursor_affinity(cy, cx, target_doc_row, affinity_col);
    }
}

static void move_visual_home(int extend)
{
    WrapRow row;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    if (layout_row_for_cursor(&row)) {
        cy = row.line;
        cx = row.cursor_start;
        set_cursor_affinity(cy, cx, row.doc_row, 0);
    }
}

static void move_visual_end(int extend)
{
    WrapRow row;

    break_undo_burst();
    goal_col = -1;

    if (extend)
        begin_selection_if_needed();
    else
        clear_selection();

    if (layout_row_for_cursor(&row)) {
        cy = row.line;
        cx = row.cursor_end;
        set_cursor_affinity(cy, cx, row.doc_row, row.visual_width);
    }
}

static void move_page(int dir, int extend)
{
    int page = body_geometry().visible_rows;

    if (page < 1)
        page = 1;
    for (int i = 0; i < page; i++)
        move_visual_line(dir, extend);
}

typedef struct {
    int code;
    int action;
    const char *capability;
} KeyMapping;

static KeyMapping runtime_keys[16];
static int runtime_key_count = 0;

static void add_terminfo_key(const char *capability, int action)
{
    const char *sequence = tigetstr((char *)capability);
    int code;

    if (!sequence || sequence == (char *)-1 || !*sequence)
        return;
    code = key_defined(sequence);
    if (code <= 0)
        return;

    for (int i = 0; i < runtime_key_count; i++) {
        if (runtime_keys[i].code == code && runtime_keys[i].action == action)
            return;
    }
    if (runtime_key_count >= (int)(sizeof(runtime_keys) / sizeof(runtime_keys[0])))
        return;

    runtime_keys[runtime_key_count++] = (KeyMapping){code, action, capability};
}

static void discover_modified_navigation(void)
{
    runtime_key_count = 0;
    add_terminfo_key("kUP", KEY_EXTEND_UP);
    add_terminfo_key("kDN", KEY_EXTEND_DOWN);
    add_terminfo_key("kLFT", KEY_SLEFT);
    add_terminfo_key("kRIT", KEY_SRIGHT);
    add_terminfo_key("kPRV", KEY_EXTEND_PAGE_UP);
    add_terminfo_key("kNXT", KEY_EXTEND_PAGE_DOWN);

    /* Some descriptions expose shifted vertical arrows as the standardized
     * scroll-reverse/scroll-forward capabilities. */
    add_terminfo_key("kri", KEY_EXTEND_UP);
    add_terminfo_key("kind", KEY_EXTEND_DOWN);
}

static int normalize_terminfo_key(int ch)
{
    for (int i = 0; i < runtime_key_count; i++) {
        if (runtime_keys[i].code == ch)
            return runtime_keys[i].action;
    }
    return ch;
}

static int shifted_modifier(int modifier)
{
    return modifier > 0 && ((modifier - 1) & 1) != 0;
}

static int parse_modified_csi(const char *sequence)
{
    int first;
    int modifier;
    char final;

    if (sscanf(sequence, "[1;%d%c", &modifier, &final) == 2 &&
        shifted_modifier(modifier)) {
        if (final == 'A') return KEY_EXTEND_UP;
        if (final == 'B') return KEY_EXTEND_DOWN;
        if (final == 'C') return KEY_SRIGHT;
        if (final == 'D') return KEY_SLEFT;
    }
    if (sscanf(sequence, "O1;%d%c", &modifier, &final) == 2 &&
        shifted_modifier(modifier)) {
        if (final == 'A') return KEY_EXTEND_UP;
        if (final == 'B') return KEY_EXTEND_DOWN;
        if (final == 'C') return KEY_SRIGHT;
        if (final == 'D') return KEY_SLEFT;
    }
    if (sscanf(sequence, "[%d;%d%c", &first, &modifier, &final) == 3 &&
        shifted_modifier(modifier)) {
        if (final == '~' && first == 5) return KEY_EXTEND_PAGE_UP;
        if (final == '~' && first == 6) return KEY_EXTEND_PAGE_DOWN;
        if (final == 'u' && first == 57352) return KEY_EXTEND_UP;
        if (final == 'u' && first == 57353) return KEY_EXTEND_DOWN;
        if (final == 'u' && first == 57354) return KEY_EXTEND_PAGE_UP;
        if (final == 'u' && first == 57355) return KEY_EXTEND_PAGE_DOWN;
    }
    return 0;
}

static int read_editor_key(void)
{
    char sequence[32];
    int len = 0;
    int ch = getch();

    if (ch != 27)
        return normalize_terminfo_key(ch);

    timeout(25);
    ch = getch();
    if (ch != '[' && ch != 'O') {
        if (ch != ERR)
            ungetch(ch);
        timeout(250);
        return 27;
    }

    sequence[len++] = (char)ch;
    while (len < (int)sizeof(sequence) - 1) {
        ch = getch();
        if (ch == ERR)
            break;
        if (ch < 0 || ch > UCHAR_MAX)
            break;
        sequence[len++] = (char)ch;
        if ((ch >= '@' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '~')
            break;
    }
    sequence[len] = '\0';
    timeout(250);

    ch = parse_modified_csi(sequence);
    if (ch)
        return ch;

    set_status("Unknown terminal key sequence ignored");
    return ERR;
}

static unsigned long long path_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;

    while (s && *s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }

    return h;
}

static int snprintf_ok(int n, size_t size)
{
    return n >= 0 && (size_t)n < size;
}

static int ensure_dir(const char *path)
{
    struct stat st;

    if (!path || !*path)
        return 0;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (mkdir(path, 0700) == 0)
        return 1;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    size_t len;

    if (!path || !*path)
        return 0;
    if (!snprintf_ok(snprintf(tmp, sizeof(tmp), "%s", path), sizeof(tmp)))
        return 0;

    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!ensure_dir(tmp))
                return 0;
            *p = '/';
        }
    }

    return ensure_dir(tmp);
}

static int home_path(char *out, size_t outsz, const char *suffix)
{
    const char *home = getenv("HOME");

    if (!out || outsz == 0)
        return 0;
    out[0] = '\0';
    if (!home || !*home || !suffix || !*suffix)
        return 0;

    return snprintf_ok(snprintf(out, outsz, "%s/%s", home, suffix), outsz);
}

static int simplewords_state_dir(char *out, size_t outsz)
{
    if (!home_path(out, outsz, ".local/state/simplewords"))
        return 0;
    return mkdirs(out);
}

static int simplewords_autosave_dir(char *out, size_t outsz)
{
    char state[PATH_MAX];

    if (!simplewords_state_dir(state, sizeof(state)))
        return 0;
    if (!snprintf_ok(snprintf(out, outsz, "%s/autosave", state), outsz))
        return 0;
    return mkdirs(out);
}

static int regular_file(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int copy_file_for_migration(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[8192];
    size_t got;
    int ok = 1;

    if (!in)
        return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }

    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            ok = 0;
            break;
        }
    }

    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;

    if (!ok)
        unlink(dst);
    return ok;
}

static void migrate_file_if_safe(const char *src, const char *dst)
{
    if (!regular_file(src) || regular_file(dst))
        return;
    if (rename(src, dst) == 0)
        return;
    if (copy_file_for_migration(src, dst))
        unlink(src);
}

static void autosave_path_for(const char *docpath, char *out, size_t outsz)
{
    char dir[PATH_MAX];
    const char *base;

    if (!simplewords_autosave_dir(dir, sizeof(dir))) {
        out[0] = '\0';
        return;
    }

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;

    snprintf(out, outsz, "%s/%016llx-%s.autosave",
             dir, path_hash(docpath), base);
}

static void legacy_hashed_autosave_path_for(const char *docpath, char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    const char *base;

    if (!home) {
        out[0] = '\0';
        return;
    }

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;

    snprintf(out, outsz, "%s/writing/autosave/%016llx-%s.autosave",
             home, path_hash(docpath), base);
}

static void legacy_autosave_path_for(const char *docpath, char *out, size_t outsz)
{
    const char *home = getenv("HOME");
    const char *base;

    if (!home) {
        out[0] = '\0';
        return;
    }

    base = strrchr(docpath, '/');
    base = base ? base + 1 : docpath;

    snprintf(out, outsz, "%s/writing/autosave/%s.autosave", home, base);
}

static int file_mtime(const char *path, time_t *out)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return 0;

    *out = st.st_mtime;
    return 1;
}

static void clear_document(void)
{
    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    line_count = 0;
}

static void ensure_one_empty_line(void)
{
    if (line_count == 0)
        lines[line_count++] = new_line("");
}

static int read_document_into_buffer(const char *path)
{
    FILE *fp = fopen(path, "r");
    char buf[MAX_LINE];

    if (!fp)
        return 0;

    clear_document();

    while (line_count < MAX_LINES && fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        int complete_line = len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r');

        if (!complete_line && len == sizeof(buf) - 1) {
            fclose(fp);
            clear_document();
            ensure_one_empty_line();
            errno = EOVERFLOW;
            return 0;
        }

        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        lines[line_count++] = new_line(buf);
    }

    if (line_count >= MAX_LINES) {
        int ch = fgetc(fp);
        if (ch != EOF) {
            fclose(fp);
            clear_document();
            ensure_one_empty_line();
            errno = EOVERFLOW;
            return 0;
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        ensure_one_empty_line();
        return 0;
    }

    fclose(fp);
    ensure_one_empty_line();
    return 1;
}

static int load_autosave_if_newer(const char *docpath)
{
    char candidates[3][PATH_MAX];
    char best[PATH_MAX] = "";
    char new_path[PATH_MAX] = "";
    time_t doc_time = 0;
    time_t best_time = 0;

    autosave_path_for(docpath, candidates[0], sizeof(candidates[0]));
    legacy_hashed_autosave_path_for(docpath, candidates[1], sizeof(candidates[1]));
    legacy_autosave_path_for(docpath, candidates[2], sizeof(candidates[2]));

    if (candidates[0][0])
        snprintf(new_path, sizeof(new_path), "%s", candidates[0]);

    for (size_t i = 0; i < 3; i++) {
        time_t candidate_time = 0;

        if (!candidates[i][0] || !file_mtime(candidates[i], &candidate_time))
            continue;
        if (!best[0] || candidate_time > best_time) {
            snprintf(best, sizeof(best), "%s", candidates[i]);
            best_time = candidate_time;
        }
    }

    if (!best[0])
        return 0;

    if (new_path[0] && strcmp(best, new_path) != 0 && !regular_file(new_path)) {
        migrate_file_if_safe(best, new_path);
        if (file_mtime(new_path, &best_time))
            snprintf(best, sizeof(best), "%s", new_path);
    }

    if (file_mtime(docpath, &doc_time) && best_time <= doc_time)
        return 0;

    if (!read_document_into_buffer(best))
        return 0;

    dirty = 1;
    autosave_dirty = 0;
    set_status("Recovered newer autosave");
    return 1;
}

static void remember_directory(const char *path, char *directory, size_t directory_size);
static int containing_directory_exists(const char *path);

static void reset_edit_state_after_load(void)
{
    clear_selection();
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
    break_undo_burst();
    goal_col = -1;
    clear_cursor_affinity();
}

static void load_file_at_position(const char *path, int recover_autosave, int restore_pos,
                                  int restore_y, int restore_x, int restore_top)
{
    if (!read_document_into_buffer(path)) {
        int open_errno = errno;
        char msg[700];

        clear_document();
        lines[line_count++] = new_line("");
        cy = cx = top = 0;

        if (open_errno == ENOENT) {
            strncpy(filename, path, sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';
            snprintf(last_open_file, sizeof(last_open_file), "%s", filename);
            if (containing_directory_exists(filename)) {
                remember_directory(filename, last_open_directory,
                                   sizeof(last_open_directory));
                remember_directory(filename, last_save_directory,
                                   sizeof(last_save_directory));
            }
            dirty = 0;
            autosave_dirty = 0;
            last_edit_time = 0;
            reset_edit_state_after_load();
            set_status("New file");
            return;
        }

        snprintf(msg, sizeof(msg),
                 "Open failed: %s: %s",
                 path, strerror(open_errno));
        filename[0] = '\0';
        reset_edit_state_after_load();
        set_status(msg);
        return;
    }

    strncpy(filename, path, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    snprintf(last_open_file, sizeof(last_open_file), "%s", filename);
    if (containing_directory_exists(filename)) {
        remember_directory(filename, last_open_directory,
                           sizeof(last_open_directory));
        remember_directory(filename, last_save_directory,
                           sizeof(last_save_directory));
    }

    if (!recover_autosave || !load_autosave_if_newer(path)) {
        dirty = 0;
        autosave_dirty = 0;
    }

    if (restore_pos) {
        cy = restore_y;
        cx = restore_x;
        top = restore_top;
    } else {
        cy = 0;
        cx = 0;
        top = 0;
    }

    clamp_cursor();
    if (top < 0)
        top = 0;
    clamp_top();
    reset_edit_state_after_load();
}

static void load_file(const char *path)
{
    load_file_at_position(path, 1, 0, 0, 0, 0);
}

static int save_template_for(const char *path, char *tmp, size_t tmpsz)
{
    const char *slash = strrchr(path, '/');
    int written;

    if (!slash)
        written = snprintf(tmp, tmpsz, ".simplewords-save-XXXXXX");
    else if (slash == path)
        written = snprintf(tmp, tmpsz, "/.simplewords-save-XXXXXX");
    else
        written = snprintf(tmp, tmpsz, "%.*s/.simplewords-save-XXXXXX",
                           (int)(slash - path), path);

    if (written < 0 || (size_t)written >= tmpsz) {
        errno = ENAMETOOLONG;
        return 0;
    }

    return 1;
}

static int write_document(const char *path)
{
    char tmp[PATH_MAX];
    struct stat st;
    mode_t mode;

    if (!save_template_for(path, tmp, sizeof(tmp)))
        return 0;

    if (stat(path, &st) == 0) {
        mode = st.st_mode & 0777;
    } else {
        mode_t mask = umask(0);
        umask(mask);
        mode = 0666 & ~mask;
    }

    int fd = mkstemp(tmp);
    if (fd < 0)
        return 0;

    fchmod(fd, mode);

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < line_count; i++) {
        if (fputs(lines[i], fp) == EOF)
            ok = 0;
        if (i != line_count - 1 && fputc('\n', fp) == EOF)
            ok = 0;
    }

    if (fclose(fp) != 0)
        ok = 0;

    if (ok && rename(tmp, path) == 0)
        return 1;

    {
        int saved_errno = errno;
        unlink(tmp);
        errno = saved_errno;
        return 0;
    }
}

static void expand_user_path(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (strncmp(in, "$HOME", 5) == 0 && (in[5] == '\0' || in[5] == '/') && home) {
        snprintf(out, outsz, "%s%s", home, in + 5);
        return;
    }

    if (in[0] == '~' && in[1] == '/' && home) {
        snprintf(out, outsz, "%s/%s", home, in + 2);
    } else {
        snprintf(out, outsz, "%s", in);
    }
}

static void display_user_path(const char *in, char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (home && home[0]) {
        size_t home_len = strlen(home);
        if (strncmp(in, home, home_len) == 0 &&
            (in[home_len] == '/' || in[home_len] == '\0')) {
            snprintf(out, outsz, "~%s", in + home_len);
            return;
        }
    }
    snprintf(out, outsz, "%s", in);
}

static void default_open_prompt_path(char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (last_open_directory[0]) {
        display_user_path(last_open_directory, out, outsz);
        return;
    }

    if (last_save_directory[0]) {
        display_user_path(last_save_directory, out, outsz);
        return;
    }

    if (home && home[0]) {
        char fallback[PATH_MAX];
        snprintf(fallback, sizeof(fallback), "%s/writing/", home);
        display_user_path(fallback, out, outsz);
        return;
    }

    snprintf(out, outsz, "./");
}

static void default_save_prompt_path(char *out, size_t outsz)
{
    const char *home = getenv("HOME");

    if (filename[0]) {
        display_user_path(filename, out, outsz);
        return;
    }

    if (last_save_directory[0]) {
        display_user_path(last_save_directory, out, outsz);
        return;
    }

    if (home && home[0]) {
        char fallback[PATH_MAX];
        snprintf(fallback, sizeof(fallback), "%s/writing/", home);
        display_user_path(fallback, out, outsz);
        return;
    }

    snprintf(out, outsz, "./");
}

static void remember_directory(const char *path, char *directory, size_t directory_size)
{
    const char *slash = strrchr(path, '/');
    size_t dir_len;

    if (!slash) {
        snprintf(directory, directory_size, "./");
        return;
    }

    dir_len = (size_t)(slash - path + 1);
    if (dir_len >= directory_size)
        dir_len = directory_size - 1;
    memcpy(directory, path, dir_len);
    directory[dir_len] = '\0';
}

static int containing_directory_exists(const char *path)
{
    char directory[PATH_MAX];
    const char *slash = strrchr(path, '/');
    struct stat st;
    size_t dir_len;

    if (!slash)
        return stat(".", &st) == 0 && S_ISDIR(st.st_mode);

    dir_len = slash == path ? 1 : (size_t)(slash - path);
    if (dir_len >= sizeof(directory))
        return 0;
    memcpy(directory, path, dir_len);
    directory[dir_len] = '\0';
    return stat(directory, &st) == 0 && S_ISDIR(st.st_mode);
}

typedef struct {
    char *name;
    int is_dir;
} PathCompletion;

static int compare_path_completions(const void *a, const void *b)
{
    const PathCompletion *pa = a;
    const PathCompletion *pb = b;
    return strcmp(pa->name, pb->name);
}

static void free_path_completions(PathCompletion *items, int count)
{
    for (int i = 0; i < count; i++)
        free(items[i].name);
    free(items);
}

static PathCompletion *path_completions(const char *input, int *count_out,
                                        int *base_len_out, int *error_out)
{
    char dirpart[512];
    char dirpath[PATH_MAX];
    char fullpath[PATH_MAX];
    const char *slash = strrchr(input, '/');
    const char *base = slash ? slash + 1 : input;
    PathCompletion *items = NULL;
    int count = 0;
    int cap = 0;
    DIR *dir;
    struct dirent *entry;

    *error_out = 0;

    if (slash) {
        size_t n = (size_t)(slash - input + 1);
        if (n >= sizeof(dirpart)) {
            *error_out = ENAMETOOLONG;
            return NULL;
        }
        memcpy(dirpart, input, n);
        dirpart[n] = '\0';
        expand_user_path(dirpart, dirpath, sizeof(dirpath));
    } else {
        dirpart[0] = '\0';
        snprintf(dirpath, sizeof(dirpath), ".");
    }

    dir = opendir(dirpath);
    if (!dir) {
        *error_out = errno;
        return NULL;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        int is_dir;
        size_t base_len = strlen(base);

        if (strncmp(entry->d_name, base, base_len) != 0)
            continue;
        if (!base[0] && entry->d_name[0] == '.' &&
            strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0)
            continue;

        snprintf(fullpath, sizeof(fullpath), "%s%s%s", dirpath,
                 dirpath[0] && dirpath[strlen(dirpath) - 1] == '/' ? "" : "/",
                 entry->d_name);
        is_dir = stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode);

        if (count == cap) {
            int new_cap = cap ? cap * 2 : 32;
            PathCompletion *grown = realloc(items,
                                             (size_t)new_cap * sizeof(*items));
            if (!grown)
                break;
            items = grown;
            cap = new_cap;
        }

        items[count].name = strdup(entry->d_name);
        if (!items[count].name)
            break;
        items[count].is_dir = is_dir;
        count++;
    }

    closedir(dir);
    qsort(items, (size_t)count, sizeof(*items), compare_path_completions);
    *count_out = count;
    *base_len_out = (int)strlen(base);
    return items;
}

static int complete_path(char *out, size_t outsz, PathCompletion *items,
                         int count, int base_len)
{
    const char *slash = strrchr(out, '/');
    int prefix_len = slash ? (int)(slash - out + 1) : 0;
    int common_len;
    int old_len = (int)strlen(out);

    if (count == 0)
        return 0;

    common_len = (int)strlen(items[0].name);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < common_len && items[0].name[j] == items[i].name[j])
            j++;
        common_len = j;
    }

    if (common_len > base_len && prefix_len + common_len < (int)outsz) {
        memcpy(out + prefix_len, items[0].name, (size_t)common_len);
        out[prefix_len + common_len] = '\0';
    }

    if (count == 1 && items[0].is_dir) {
        int len = (int)strlen(out);
        if (len + 1 < (int)outsz && (len == 0 || out[len - 1] != '/')) {
            out[len] = '/';
            out[len + 1] = '\0';
        }
    }

    return (int)strlen(out) != old_len;
}

static void draw_path_completions(const char *prompt, const char *path,
                                  PathCompletion *items, int count)
{
    int widest = 1;

    for (int i = 0; i < count; i++) {
        int width = (int)strlen(items[i].name) + (items[i].is_dir ? 1 : 0);
        if (width > widest)
            widest = width;
    }
    widest += 2;

    int cols = COLS / widest;
    int total_rows;
    int visible_rows;
    int pane_start;
    int last;

    if (cols < 1)
        cols = 1;
    total_rows = (count + cols - 1) / cols;
    visible_rows = LINES / 2 - 1;
    if (visible_rows < 1)
        visible_rows = 1;
    if (visible_rows > total_rows)
        visible_rows = total_rows;
    pane_start = LINES - visible_rows - 2;
    if (pane_start < 0)
        pane_start = 0;
    last = visible_rows * cols;
    if (last > count)
        last = count;

    attrset(chrome_attr());
    mvhline(pane_start, 0, ' ', COLS);
    {
        char heading[128];
        snprintf(heading, sizeof(heading),
                 "%d completion%s  Esc returns to path; Esc again cancels%s",
                 count, count == 1 ? "" : "s",
                 last < count ? "  (more below)" : "");
        mvaddnstr(pane_start, 1, heading, COLS - 2);
    }

    attrset(body_attr());
    for (int row = pane_start + 1; row < LINES - 1; row++)
        mvhline(row, 0, ' ', COLS);
    for (int i = 0; i < last; i++) {
        int row = pane_start + 1 + i / cols;
        int col = (i % cols) * widest;
        char label[PATH_MAX];
        snprintf(label, sizeof(label), "%s%s", items[i].name,
                 items[i].is_dir ? "/" : "");
        mvaddnstr(row, col, label, widest - 1);
    }

    attrset(chrome_attr());
    mvhline(LINES - 1, 0, ' ', COLS);
    mvaddnstr(LINES - 1, 1, prompt, COLS - 2);
    mvaddnstr(LINES - 1, 1 + (int)strlen(prompt), path,
              COLS - 2 - (int)strlen(prompt));
    move(LINES - 1, 1 + (int)strlen(prompt) + (int)strlen(path));
}

static int is_prompt_tab(int ch)
{
    if (ch == '\t' || ch == 9)
        return 1;
#ifdef KEY_STAB
    if (ch == KEY_STAB)
        return 1;
#endif
#ifdef KEY_CTAB
    if (ch == KEY_CTAB)
        return 1;
#endif
#ifdef KEY_BTAB
    if (ch == KEY_BTAB)
        return 1;
#endif
    return 0;
}

static int prompt_path(const char *prompt, const char *initial,
                       char *out, size_t outsz)
{
    int pos;
    int ch;
    int tab_pending = 0;
    int pane_open = 0;
    PathCompletion *items = NULL;
    int count = 0;
    int base_len = 0;
    char completion_feedback[128] = "";

    snprintf(out, outsz, "%s", initial ? initial : "");
    pos = (int)strlen(out);
    set_cursor_visibility(1);

    while (1) {
        draw_screen_impl(0);
        if (pane_open) {
            draw_path_completions(prompt, out, items, count);
        } else {
            attrset(chrome_attr());
            mvhline(LINES - 1, 0, ' ', COLS);
            mvaddnstr(LINES - 1, 1, prompt, COLS - 2);
            mvaddnstr(LINES - 1, 1 + (int)strlen(prompt), out,
                      COLS - 2 - (int)strlen(prompt));
            if (completion_feedback[0]) {
                int feedback_col = 1 + (int)strlen(prompt) + (int)strlen(out) + 2;
                if (feedback_col < COLS - 1)
                    mvaddnstr(LINES - 1, feedback_col, completion_feedback,
                              COLS - 1 - feedback_col);
                else if (LINES > 1) {
                    mvhline(LINES - 2, 0, ' ', COLS);
                    mvaddnstr(LINES - 2, 1, completion_feedback, COLS - 2);
                }
            }
            move(LINES - 1, 1 + (int)strlen(prompt) + pos);
        }
        refresh();
        set_cursor_visibility(1);

        do {
            ch = getch();
        } while (ch == ERR);
        if (ch == 27) {
            if (pane_open) {
                pane_open = 0;
                tab_pending = 0;
                free_path_completions(items, count);
                items = NULL;
                count = 0;
                continue;
            }
            free_path_completions(items, count);
            return 0;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            int accepted = out[0] != '\0';
            free_path_completions(items, count);
            return accepted;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0)
                out[--pos] = '\0';
            completion_feedback[0] = '\0';
            tab_pending = 0;
        } else if (is_prompt_tab(ch)) {
            int changed;
            int completion_errno;

            free_path_completions(items, count);
            count = 0;
            items = path_completions(out, &count, &base_len,
                                     &completion_errno);
            if (completion_errno) {
                snprintf(completion_feedback, sizeof(completion_feedback),
                         "%s", completion_errno == ENOENT || completion_errno == ENOTDIR
                               ? "No such directory" : strerror(completion_errno));
                set_status(completion_feedback);
                pane_open = 0;
                tab_pending = 0;
                continue;
            }
            completion_feedback[0] = '\0';
            changed = complete_path(out, outsz, items, count, base_len);
            pos = (int)strlen(out);
            if (tab_pending && !changed && count > 0)
                pane_open = 1;
            tab_pending = 1;
        } else if (isprint((unsigned char)ch) && pos < (int)outsz - 1) {
            out[pos++] = (char)ch;
            out[pos] = '\0';
            completion_feedback[0] = '\0';
            tab_pending = 0;
        }
    }
}


static int prompt_string(const char *prompt, char *out, size_t outsz)
{
    int pos = 0;
    int ch;

    out[0] = '\0';
    set_cursor_visibility(1);

    while (1) {
        draw_screen_impl(0);
        attrset(chrome_attr());
        mvhline(LINES - 1, 0, ' ', COLS);
        mvaddnstr(LINES - 1, 1, prompt, COLS - 2);
        mvaddnstr(LINES - 1, 1 + (int)strlen(prompt), out,
                  COLS - 2 - (int)strlen(prompt));
        move(LINES - 1, 1 + (int)strlen(prompt) + pos);
        refresh();

        do {
            ch = getch();
        } while (ch == ERR);

        if (ch == 27)
            return 0;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
            return out[0] != '\0';
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0)
                out[--pos] = '\0';
        } else if (isprint((unsigned char)ch) && pos < (int)outsz - 1) {
            out[pos++] = (char)ch;
            out[pos] = '\0';
        }
    }
}

static int find_text_forward(const char *needle, int start_y, int start_x,
                             int *out_y, int *out_x)
{
    for (int pass = 0; pass < 2; pass++) {
        int first_y = pass == 0 ? start_y : 0;
        int last_y = pass == 0 ? line_count : start_y + 1;

        for (int y = first_y; y < last_y; y++) {
            int x = (pass == 0 && y == start_y) ? start_x : 0;
            char *hit;

            if (x < 0) x = 0;
            if (x > (int)strlen(lines[y])) x = (int)strlen(lines[y]);

            hit = strstr(lines[y] + x, needle);
            if (hit) {
                *out_y = y;
                *out_x = (int)(hit - lines[y]);
                return 1;
            }
        }
    }
    return 0;
}

static char *last_strstr_before(char *haystack, const char *needle, int limit)
{
    char *best = NULL;
    char *hit = haystack;

    while ((hit = strstr(hit, needle))) {
        if ((int)(hit - haystack) > limit)
            break;
        best = hit;
        hit++;
    }

    return best;
}

static int find_text_backward(const char *needle, int start_y, int start_x,
                              int *out_y, int *out_x)
{
    for (int pass = 0; pass < 2; pass++) {
        int first_y = pass == 0 ? start_y : line_count - 1;
        int last_y = pass == 0 ? -1 : start_y - 1;

        for (int y = first_y; y > last_y; y--) {
            int limit = (pass == 0 && y == start_y)
                        ? start_x
                        : (int)strlen(lines[y]);
            char *hit = last_strstr_before(lines[y], needle, limit);

            if (hit) {
                *out_y = y;
                *out_x = (int)(hit - lines[y]);
                return 1;
            }
        }
    }
    return 0;
}

static void repeat_find(int direction)
{
    int fy, fx;

    if (!last_find[0]) {
        set_status("No active find");
        find_mode = 0;
        find_active = 0;
        find_match_y = -1;
        find_match_x = -1;
        find_match_len = 0;
        screen_cache_valid = 0;
        return;
    }

    if (direction > 0) {
        if (!find_text_forward(last_find, cy, cx + 1, &fy, &fx)) {
            set_status("No next match");
            return;
        }
    } else {
        if (!find_text_backward(last_find, cy, cx - 1, &fy, &fx)) {
            set_status("No previous match");
            return;
        }
    }

    cy = fy;
    cx = fx;
    find_match_y = fy;
    find_match_x = fx;
    find_match_len = (int)strlen(last_find);
    goal_col = -1;
    clear_cursor_affinity();
    clear_selection();
    keep_cursor_visible();
    set_status(direction > 0 ? "Next match" : "Previous match");
    find_mode = 1;
    find_active = 1;
    screen_cache_valid = 0;
}

static void find_word_prompt(void)
{
    char needle[256];

    break_undo_burst();
    clear_selection();

    if (!prompt_string("Find: ", needle, sizeof(needle))) {
        set_status("Find cancelled");
        return;
    }

    snprintf(last_find, sizeof(last_find), "%s", needle);
    find_mode = 1;
    repeat_find(1);
}



static void save_session(void);
static int load_session(void);

static void save_file(int force_write)
{
    char path[512];
    char initial[512];

    break_undo_burst();
    if (!force_write && filename[0] && !dirty) {
        set_status("No changes to save");
        return;
    }
    if (!filename[0]) {
        default_save_prompt_path(initial, sizeof(initial));
        if (!prompt_path("Save as: ", initial, path, sizeof(path))) {
            set_status("Save cancelled");
            return;
        }
        expand_user_path(path, filename, sizeof(filename));
    }

    if (write_document(filename)) {
        remember_directory(filename, last_save_directory, sizeof(last_save_directory));
        dirty = 0;
        autosave_dirty = 0;
        save_session();
        set_status("Saved");
    } else {
        char msg[600];
        snprintf(msg, sizeof(msg), "Save failed: %s", strerror(errno));
        set_status(msg);
    }
}

static int document_is_empty(void)
{
    return line_count == 1 &&
           lines[0] &&
           lines[0][0] == '\0';
}

static void ensure_autosave_dir(void)
{
    char dir[PATH_MAX];

    simplewords_autosave_dir(dir, sizeof(dir));
}

static void autosave_file(void)
{
    char path[PATH_MAX];

    if (!autosave_dirty || !last_edit_time)
        return;
    if (time(NULL) - last_edit_time < config.autosave_interval)
        return;

    if (filename[0]) {
        autosave_path_for(filename, path, sizeof(path));
    } else {
        char dir[PATH_MAX];

        if (!simplewords_autosave_dir(dir, sizeof(dir)))
            return;

        snprintf(path, sizeof(path),
                 "%s/%s.autosave",
                 dir, untitled_name);
    }

    if (!path[0])
        return;

    ensure_autosave_dir();

    if (document_is_empty()) {
        unlink(path);
        autosave_dirty = 0;
        return;
    }

    if (write_document(path)) {
        autosave_dirty = 0;
        save_session();
    }
}

static void open_file_prompt(void)
{
    char path[512];
    char initial[512];

    break_undo_burst();
    default_open_prompt_path(initial, sizeof(initial));
    if (!prompt_path("Open: ", initial, path, sizeof(path))) {
        set_status("Open cancelled");
        return;
    }

    char expanded[512];
    expand_user_path(path, expanded, sizeof(expanded));

    load_file_at_position(expanded, 0, 0, 0, 0, 0);
    save_session();
    set_status("Opened disk file");
}

static int confirm_quit(void)
{
    int ch;
    int quit = 0;

    break_undo_burst();
    if (!dirty)
        return 1;

    timeout(-1);
    set_status("Unsaved changes. Quit anyway? y/N");

    while (1) {
        draw_screen();
        ch = read_editor_key();

        if (ch == 'y' || ch == 'Y') {
            quit = 1;
            break;
        }

        if (ch == 'n' || ch == 'N' || ch == 27 ||
            ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            quit = 0;
            break;
        }
    }

    timeout(250);
    clear_status();
    return quit;
}

static void new_blank_buffer(void)
{
    if (dirty && !confirm_quit())
        return;

    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    line_count = 1;
    lines[0] = new_line("");
    cy = 0;
    cx = 0;
    top = 0;
    goal_col = -1;
    clear_cursor_affinity();
    filename[0] = '\0';
    make_untitled_name();

    dirty = 0;
    autosave_dirty = 0;
    last_edit_time = 0;

    clear_selection();
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
    clamp_top();

    set_status("New blank buffer");
}

static void do_undo(void)
{
    UndoState current;

    break_undo_burst();
    if (!undo_count) {
        set_status("Nothing to undo");
        return;
    }

    current = capture_state();
    push_state(redo_stack, &redo_count, current);
    restore_state(&undo_stack[--undo_count]);
    dirty = 1;
    set_status("Undo");
}

static void do_redo(void)
{
    UndoState current;

    break_undo_burst();
    if (!redo_count) {
        set_status("Nothing to redo");
        return;
    }

    current = capture_state();
    push_state(undo_stack, &undo_count, current);
    restore_state(&redo_stack[--redo_count]);
    dirty = 1;
    set_status("Redo");
}



static int transient_mail_file(const char *path)
{
    const char *base;

    if (!path || !*path)
        return 0;

    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    return strncmp(base, "simplemail-compose-", 19) == 0 ||
           strncmp(base, "simplemail-reply-",   17) == 0;
}

static int session_path(char *out, size_t outsz)
{
    char dir[PATH_MAX];

    if (!simplewords_state_dir(dir, sizeof(dir)))
        return 0;
    return snprintf_ok(snprintf(out, outsz, "%s/session", dir), outsz);
}

static void legacy_session_path(char *out, size_t outsz)
{
    if (!home_path(out, outsz, LEGACY_SESSION_FILE) && out && outsz)
        out[0] = '\0';
}

static void migrate_legacy_session(const char *new_path)
{
    static int attempted = 0;
    char old_path[PATH_MAX];

    if (attempted)
        return;
    attempted = 1;

    legacy_session_path(old_path, sizeof(old_path));
    if (!old_path[0])
        return;

    migrate_file_if_safe(old_path, new_path);
}


static void save_session(void)
{
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    FILE *fp;
    int fd;

    if (!filename[0])
        return;

    if (transient_mail_file(filename))
        return;

    if (!session_path(path, sizeof(path)))
        return;

    if (!snprintf_ok(snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path), sizeof(tmp)))
        return;
    fd = mkstemp(tmp);
    if (fd < 0)
        return;

    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        return;
    }

    fprintf(fp, "%s\n%d\n%d\n%d\n", filename, cy, cx, top);
    if (fclose(fp) != 0) {
        unlink(tmp);
        return;
    }

    rename(tmp, path);
}

static int load_session(void)
{
    char path[PATH_MAX];
    char filebuf[PATH_MAX];
    int sy = 0;
    int sx = 0;
    int st = 0;
    FILE *fp;

    if (!session_path(path, sizeof(path)))
        return 0;

    migrate_legacy_session(path);

    fp = fopen(path, "r");
    if (!fp)
        return 0;

    if (!fgets(filebuf, sizeof(filebuf), fp)) {
        fclose(fp);
        return 0;
    }

    filebuf[strcspn(filebuf, "\r\n")] = 0;

    if (fscanf(fp, "%d\n%d\n%d", &sy, &sx, &st) != 3) {
        fclose(fp);
        return 0;
    }

    fclose(fp);

    if (!filebuf[0])
        return 0;

    if (filename[0] && strcmp(filename, filebuf) != 0)
        return 0;

    load_file_at_position(filebuf, 1, 1, sy, sx, st);
    set_status("Session restored");
    return 1;
}

static void init_colors(void)
{
    if (!has_colors())
        return;

    start_color();
    use_default_colors();
}

int main(int argc, char **argv)
{
    int prefix = 0;
    int needs_redraw = 1;

    setlocale(LC_ALL, "");
    make_untitled_name();
    lines[0] = new_line("");
    configure_settle_options();

    if (argc > 1) {
        load_file(argv[1]);
    } else {
        load_session();
    }

    use_extended_names(TRUE);
    initscr();
    set_escdelay(25);
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    discover_modified_navigation();
    timeout(250);
    intrflush(stdscr, FALSE);
    leaveok(stdscr, FALSE);
    scrollok(stdscr, FALSE);
    init_colors();
    wbkgdset(stdscr, (chtype)' ' | body_attr());
    set_cursor_visibility(1);
    last_keypress_ms = monotonic_ms();

    while (1) {
        int ch;

        if (needs_redraw) {
            draw_screen();
            needs_redraw = 0;
        }
        ch = read_editor_key();

        if (ch == ERR) {
            /*
             * If stdin/TTY gets weird or timeout polling returns immediately,
             * don't spin the editor at 100% CPU. Sleep a little on idle ticks.
             */
            autosave_file();
            if (status_msg[0] && status_time && time(NULL) - status_time > 4) {
                clear_status();
                needs_redraw = 1;
            }
            if (idle_cursor_enabled && !selecting &&
                !idle_cursor_hidden && last_keypress_ms > 0 &&
                monotonic_ms() - last_keypress_ms >= 750) {
                idle_cursor_hidden = 1;
                set_cursor_visibility(0);
            }

            napms(50);
            continue;
        }

        last_keypress_ms = monotonic_ms();
        idle_cursor_hidden = 0;
        needs_redraw = 1;

        if (status_msg[0] && ch != 24)
            clear_status();

        if (prefix) {
            if (ch == 19) {
                save_file(0);
            } else if (ch == 6) {
                open_file_prompt();
            } else if (ch == 'b' || ch == 'B') {
                new_blank_buffer();
            } else if (ch == 23) {
                char path[512];
                char initial[512];
                default_save_prompt_path(initial, sizeof(initial));
                if (prompt_path("Save as: ", initial, path, sizeof(path))) {
                    expand_user_path(path, filename, sizeof(filename));
                    save_file(1);
                } else {
                    set_status("Save cancelled");
                }
            } else if (ch == 3) {
                if (confirm_quit())
                    break;
            } else if (ch == 'u' || ch == 'U') {
                do_undo();
            } else if (ch == 'r' || ch == 'R' || ch == 18) {
                do_redo();
            } else if (ch == 26) {
                distraction_free = !distraction_free;
                clear_status();
                clamp_top();
                screen_cache_valid = 0;
            } else {
                set_status("Unknown C-x command");
            }
            prefix = 0;
            continue;
        }

        if (ch == 19) {
            find_word_prompt();
            prefix = 0;
            continue;
        }

        if (find_active && (ch == 'n' || ch == 'N')) {
            repeat_find(ch == 'N' ? -1 : 1);
            continue;
        }

        if (ch == 27 && find_active) {
            find_mode = 0;
            find_active = 0;
            find_match_y = -1;
            find_match_x = -1;
            find_match_len = 0;
            screen_cache_valid = 0;
            set_status("Find cleared");
            continue;
        }

        if (ch == KEY_RESIZE) {
            destroy_body_window();
            screen_cache_valid = 0;
            clamp_top();
            keep_cursor_visible();
        } else if (ch == 24) {
            prefix = 1;
        } else if (ch == KEY_HOME) {
            move_visual_home(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_END) {
            move_visual_end(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_UP) {
            move_visual_line(-1, 0);
            screen_cache_valid = 0;
        } else if (ch == KEY_DOWN) {
            move_visual_line(1, 0);
            screen_cache_valid = 0;
        } else if (ch == KEY_LEFT) {
            move_left(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_RIGHT) {
            move_right(0);
            screen_cache_valid = 0;
        } else if (ch == KEY_EXTEND_UP || ch == KEY_SR) {
            move_visual_line(-1, 1);
        } else if (ch == KEY_EXTEND_DOWN || ch == KEY_SF) {
            move_visual_line(1, 1);
        } else if (ch == KEY_SLEFT) {
            move_left(1);
        } else if (ch == KEY_SRIGHT) {
            move_right(1);
        } else if (ch == KEY_EXTEND_PAGE_UP || ch == KEY_SPREVIOUS) {
            move_page(-1, 1);
        } else if (ch == KEY_EXTEND_PAGE_DOWN || ch == KEY_SNEXT) {
            move_page(1, 1);
        } else if (ch == KEY_PPAGE) {
            move_page(-1, 0);
        } else if (ch == KEY_NPAGE) {
            move_page(1, 0);
        } else if (ch == 27) {
            /* Alt-w / Meta-w copies selection, Emacs-style. */
            timeout(25);
            ch = getch();
            timeout(250);
            if (ch == 'w' || ch == 'W') {
                copy_selection();
            } else if (ch != ERR) {
                ungetch(ch);
            }
        } else if (ch == 23) {
            cut_selection();
        } else if (ch == 25) {
            paste_clipboard();
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            backspace();
        } else if (ch == KEY_DC) {
            delete_forward();
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            set_cursor_visibility(0);
            if (selecting)
                delete_selection();
            newline();
        } else if (ch == '\t' || ch == 9) {
            if (selecting)
                delete_selection();
            insert_char('\t');
        } else if (isprint((unsigned char)ch)) {
            if (selecting)
                delete_selection();
            insert_char(ch);
        }

    }

    if (env_enabled("SIMPLEWORDS_AUTOSAVE_ON_EXIT") && filename[0] && dirty) {
        if (write_document(filename)) {
            dirty = 0;
            autosave_dirty = 0;
        }
    }

    autosave_file();
    save_session();
    destroy_body_window();
    endwin();

    for (int i = 0; i < line_count; i++)
        free(lines[i]);
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
    free(clip);
    free(desired_rows);
    free(screen_cells);
    free(desired_cells);

    return 0;
}
