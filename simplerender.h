#ifndef SIMPLERENDER_H
#define SIMPLERENDER_H

#include <curses.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef SIMPLERENDER_TAB_WIDTH
#define SIMPLERENDER_TAB_WIDTH 4
#endif

enum {
    SSR_ROW_UNUSED,
    SSR_ROW_TEXT,
    SSR_ROW_EMPTY
};

typedef struct {
    int kind;
    const char *line;
    int len;
    int start;
    int end;
    int screen_width;
} SsrRow;

enum {
    SSR_CELL_BLANK,
    SSR_CELL_GLYPH,
    SSR_CELL_CONTINUATION
};

typedef struct {
    wchar_t text[CCHARW_MAX];
    attr_t attr;
    unsigned char kind;
} SsrCell;

typedef struct {
    SsrRow *desired_rows;
    int row_capacity;
    SsrCell *screen_cells;
    SsrCell *desired_cells;
    size_t cell_capacity;

    int cache_valid;
    int cache_lines;
    int cache_cols;
    int cache_top;
    int cache_left;
    int cache_height;
    int cache_width;
    int cache_scroll;
    unsigned long long cache_body_hash;

    WINDOW *body_window;
    int body_window_top;
    int body_window_left;
    int body_window_height;
    int body_window_width;

    int windowed_redraw_enabled;
    int scroll_window_enabled;
} SsrRenderer;

static void ssr_init(SsrRenderer *r)
{
    memset(r, 0, sizeof(*r));

    /*
     * Match SimpleWords' calmer default posture. The renderer can still grow
     * faster paths later, but reading surfaces should not opt into physical
     * terminal scrolling by default.
     */
    r->windowed_redraw_enabled = 0;
    r->scroll_window_enabled = 0;
}

static void ssr_destroy_body_window(SsrRenderer *r)
{
    if (r->body_window)
        delwin(r->body_window);
    r->body_window = NULL;
    r->body_window_top = 0;
    r->body_window_left = 0;
    r->body_window_height = 0;
    r->body_window_width = 0;
}

static void ssr_invalidate(SsrRenderer *r)
{
    r->cache_valid = 0;
}

static void ssr_deactivate(SsrRenderer *r)
{
    ssr_destroy_body_window(r);
    ssr_invalidate(r);
}

static void ssr_destroy(SsrRenderer *r)
{
    ssr_destroy_body_window(r);
    free(r->desired_rows);
    free(r->screen_cells);
    free(r->desired_cells);
    memset(r, 0, sizeof(*r));
}

static unsigned long long ssr_text_hash(const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    unsigned long long h = 1469598103934665603ULL;

    while (*p) {
        h ^= (unsigned long long)*p++;
        h *= 1099511628211ULL;
    }

    return h;
}

static int ssr_utf8_decode_n(const char *s, int avail, wchar_t *wc, int *bytes_used)
{
    mbstate_t st;
    size_t n;
    int w;

    memset(&st, 0, sizeof(st));
    n = mbrtowc(wc, s, avail > 0 ? (size_t)avail : 0, &st);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *wc = L'\xfffd';
        *bytes_used = 1;
        return 1;
    }

    *bytes_used = (int)n;
    w = wcwidth(*wc);
    if (*wc >= 0x1F3FB && *wc <= 0x1F3FF)
        return 0; /* emoji skin-tone modifiers extend the prior glyph */
    return w < 0 ? 1 : w;
}

static int ssr_char_visual_width(int col, char c)
{
    if (c == '\t')
        return SIMPLERENDER_TAB_WIDTH - (col % SIMPLERENDER_TAB_WIDTH);
    return 1;
}

static int ssr_is_emoji_base(wchar_t wc)
{
    return (wc >= 0x1F000 && wc <= 0x1FAFF) ||
           (wc >= 0x2600 && wc <= 0x27BF);
}

static void ssr_wrap_segment(const char *line, int len, int start, int width,
                             int *end, int *next_start)
{
    int col = 0;
    int last_space = -1;
    int join_next = 0;

    if (width < 1)
        width = 1;

    if (start >= len) {
        *end = len;
        *next_start = len;
        return;
    }

    for (int i = start; i < len; ) {
        int used = 1;
        int w;

        if (line[i] == '\t') {
            w = ssr_char_visual_width(col, line[i]);
            join_next = 0;
        } else {
            wchar_t wc;
            w = ssr_utf8_decode_n(line + i, len - i, &wc, &used);
            if (join_next && w > 0 && ssr_is_emoji_base(wc))
                w = 0;
            join_next = wc == 0x200D;
        }

        if (col + w > width) {
            if (last_space > start) {
                *end = last_space;
                *next_start = last_space + 1;
            } else {
                *end = i;
                *next_start = i;
                if (*end == start) {
                    *end = i + used;
                    *next_start = i + used;
                }
            }
            return;
        }

        col += w;
        if (line[i] == ' ' || line[i] == '\t')
            last_space = i;
        i += used;
    }

    *end = len;
    *next_start = len;
}

static int ssr_visual_col_range(const char *line, int len, int start, int upto)
{
    int col = 0;
    int join_next = 0;

    if (upto > len)
        upto = len;

    for (int i = start; i < upto; ) {
        if (line[i] == '\t') {
            col += ssr_char_visual_width(col, line[i]);
            join_next = 0;
            i++;
        } else {
            int used = 1;
            wchar_t wc;
            int w = ssr_utf8_decode_n(line + i, len - i, &wc, &used);
            if (i + used > upto)
                break;
            if (join_next && w > 0 && ssr_is_emoji_base(wc))
                w = 0;
            join_next = wc == 0x200D;
            col += w;
            i += used;
        }
    }

    return col;
}

static int ssr_visual_rows_for_line(const char *line, int len, int width)
{
    int rows = 0;
    int start = 0;

    if (len <= 0)
        return 1;

    while (start < len) {
        int end;
        int next_start;
        ssr_wrap_segment(line, len, start, width, &end, &next_start);
        rows++;
        start = next_start;
    }

    return rows;
}

static int ssr_visual_rows(const char *text, int width)
{
    const char *p = text ? text : "";
    int rows = 0;

    if (width < 1)
        width = 1;
    if (!*p)
        return 1;

    while (*p) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        rows += ssr_visual_rows_for_line(p, len, width);
        if (!e)
            break;
        p = e + 1;
        if (!*p)
            rows++;
    }

    return rows > 0 ? rows : 1;
}

static int ssr_ensure_storage(SsrRenderer *r, int height, int width)
{
    size_t cell_count = (size_t)height * (size_t)width;

    if (height < 1 || width < 1)
        return 0;

    if (r->row_capacity < height) {
        SsrRow *rows = realloc(r->desired_rows, sizeof(*rows) * (size_t)height);
        if (!rows)
            return 0;
        r->desired_rows = rows;
        r->row_capacity = height;
        r->cache_valid = 0;
    }

    if (r->cell_capacity < cell_count) {
        SsrCell *cells = realloc(r->screen_cells, sizeof(*cells) * cell_count);
        if (!cells)
            return 0;
        r->screen_cells = cells;

        cells = realloc(r->desired_cells, sizeof(*cells) * cell_count);
        if (!cells)
            return 0;
        r->desired_cells = cells;
        r->cell_capacity = cell_count;
        r->cache_valid = 0;
    }

    return 1;
}

static void ssr_describe_row(SsrRow *row, int kind, const char *line, int len,
                             int start, int end, int screen_width)
{
    row->kind = kind;
    row->line = line;
    row->len = len;
    row->start = start;
    row->end = end;
    row->screen_width = screen_width;
}

static void ssr_build_visible_rows(SsrRenderer *r, const char *text,
                                   int scroll, int height, int width)
{
    const char *p = text ? text : "";
    int physical_row = 0;
    int visual_row = 0;

    memset(r->desired_rows, 0, sizeof(*r->desired_rows) * (size_t)height);

    if (!*p) {
        if (scroll <= 0 && height > 0)
            ssr_describe_row(&r->desired_rows[0], SSR_ROW_EMPTY, p, 0, 0, 0, 0);
        return;
    }

    while (*p && physical_row < height) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);

        if (len == 0) {
            if (visual_row >= scroll) {
                ssr_describe_row(&r->desired_rows[physical_row],
                                 SSR_ROW_EMPTY, p, 0, 0, 0, 0);
                physical_row++;
            }
            visual_row++;
        } else {
            for (int start = 0; start < len && physical_row < height; ) {
                int end;
                int next_start;

                ssr_wrap_segment(p, len, start, width, &end, &next_start);
                if (visual_row >= scroll) {
                    ssr_describe_row(&r->desired_rows[physical_row],
                                     SSR_ROW_TEXT, p, len, start, end,
                                     ssr_visual_col_range(p, len, start, end));
                    physical_row++;
                }
                visual_row++;
                start = next_start;
            }
        }

        if (!e)
            break;
        p = e + 1;
        if (!*p && physical_row < height && visual_row >= scroll)
            ssr_describe_row(&r->desired_rows[physical_row],
                             SSR_ROW_EMPTY, p, 0, 0, 0, 0);
    }
}

static void ssr_set_blank(SsrCell *cell, attr_t attr)
{
    memset(cell->text, 0, sizeof(cell->text));
    cell->text[0] = L' ';
    cell->attr = attr;
    cell->kind = SSR_CELL_BLANK;
}

static void ssr_build_desired_cells(SsrRenderer *r, const char *text,
                                    int scroll, int height, int width,
                                    attr_t attr)
{
    size_t cell_count = (size_t)height * (size_t)width;

    for (size_t i = 0; i < cell_count; i++)
        ssr_set_blank(&r->desired_cells[i], attr);

    ssr_build_visible_rows(r, text, scroll, height, width);

    for (int row = 0; row < height; row++) {
        SsrRow *desc = &r->desired_rows[row];
        SsrCell *cells = r->desired_cells + (size_t)row * (size_t)width;
        int col = 0;
        int last_glyph_col = -1;
        int join_next = 0;

        if (desc->kind != SSR_ROW_TEXT)
            continue;

        for (int i = desc->start; i < desc->end; ) {
            if (desc->line[i] == '\t') {
                int spaces = SIMPLERENDER_TAB_WIDTH - (col % SIMPLERENDER_TAB_WIDTH);

                for (int k = 0; k < spaces && col + k < width; k++)
                    ssr_set_blank(&cells[col + k], attr);
                col += spaces;
                last_glyph_col = -1;
                join_next = 0;
                i++;
            } else {
                wchar_t wc;
                int used;
                int glyph_width = ssr_utf8_decode_n(desc->line + i,
                                                     desc->len - i,
                                                     &wc, &used);
                int joins_previous = join_next && glyph_width > 0 &&
                                     ssr_is_emoji_base(wc);
                int attach = glyph_width == 0 || joins_previous;

                if (attach && last_glyph_col >= 0) {
                    SsrCell *base = &cells[last_glyph_col];
                    int slot = 1;

                    while (slot < CCHARW_MAX && base->text[slot])
                        slot++;
                    if (slot + 1 < CCHARW_MAX) {
                        base->text[slot] = wc;
                        base->text[slot + 1] = L'\0';
                        join_next = wc == 0x200D;
                        i += used;
                        continue;
                    }
                    if (glyph_width == 0) {
                        join_next = wc == 0x200D;
                        i += used;
                        continue;
                    }
                }

                if (glyph_width == 0) {
                    /* Give an orphaned combining mark a visible base. */
                    glyph_width = 1;
                    if (col + glyph_width <= width) {
                        memset(cells[col].text, 0, sizeof(cells[col].text));
                        cells[col].text[0] = 0x25CC;
                        if (CCHARW_MAX > 2)
                            cells[col].text[1] = wc;
                        cells[col].attr = attr;
                        cells[col].kind = SSR_CELL_GLYPH;
                        last_glyph_col = col;
                        join_next = wc == 0x200D;
                        col += glyph_width;
                        i += used;
                        continue;
                    }
                }

                if (col + glyph_width <= width) {
                    memset(cells[col].text, 0, sizeof(cells[col].text));
                    cells[col].text[0] = wc;
                    cells[col].attr = attr;
                    cells[col].kind = SSR_CELL_GLYPH;
                    last_glyph_col = col;
                    for (int k = 1; k < glyph_width; k++) {
                        memset(cells[col + k].text, 0,
                               sizeof(cells[col + k].text));
                        cells[col + k].attr = attr;
                        cells[col + k].kind = SSR_CELL_CONTINUATION;
                    }
                }

                join_next = wc == 0x200D;
                col += glyph_width;
                i += used;
            }
        }
    }
}

static int ssr_cells_equal(const SsrCell *a, const SsrCell *b)
{
    return !memcmp(a->text, b->text, sizeof(a->text)) &&
           a->attr == b->attr && a->kind == b->kind;
}

static int ssr_cells_valid(const SsrRenderer *r, int height, int width)
{
    for (int row = 0; row < height; row++) {
        const SsrCell *line = r->screen_cells + (size_t)row * (size_t)width;

        for (int col = 0; col < width; col++) {
            if (line[col].kind > SSR_CELL_CONTINUATION)
                return 0;
            if (line[col].kind == SSR_CELL_CONTINUATION &&
                (col == 0 || line[col - 1].kind == SSR_CELL_BLANK))
                return 0;
        }
    }

    return 1;
}

static int ssr_ensure_body_window(SsrRenderer *r, int top, int left,
                                  int height, int width, attr_t attr)
{
    if (!r->windowed_redraw_enabled || height < 1 || width < 1)
        return 0;

    if (r->body_window &&
        r->body_window_top == top &&
        r->body_window_left == left &&
        r->body_window_height == height &&
        r->body_window_width == width)
        return 1;

    ssr_destroy_body_window(r);
    r->body_window = newwin(height, width, top, left);
    if (!r->body_window) {
        r->windowed_redraw_enabled = 0;
        r->scroll_window_enabled = 0;
        return 0;
    }

    r->body_window_top = top;
    r->body_window_left = left;
    r->body_window_height = height;
    r->body_window_width = width;
    wbkgdset(r->body_window, (chtype)' ' | attr);
    scrollok(r->body_window, r->scroll_window_enabled ? TRUE : FALSE);
    idlok(r->body_window, r->scroll_window_enabled ? TRUE : FALSE);
    leaveok(r->body_window, FALSE);
    r->cache_valid = 0;
    return 1;
}

static void ssr_mark_wide_group(const SsrCell *cells, unsigned char *dirty,
                                int col, int width)
{
    int start = col;
    int end;

    while (start > 0 && cells[start].kind == SSR_CELL_CONTINUATION)
        start--;
    end = start + 1;
    while (end < width && cells[end].kind == SSR_CELL_CONTINUATION)
        end++;
    memset(dirty + start, 1, (size_t)(end - start));
}

static void ssr_emit_run(WINDOW *window, int row, int left,
                         int start, int end, const SsrCell *cells)
{
    for (int col = start; col < end; ) {
        const SsrCell *cell = &cells[col];

        if (cell->kind == SSR_CELL_BLANK) {
            int blank_end = col + 1;

            while (blank_end < end &&
                   cells[blank_end].kind == SSR_CELL_BLANK &&
                   cells[blank_end].attr == cell->attr)
                blank_end++;
            wattrset(window, cell->attr);
            mvwhline(window, row, left + col, ' ', blank_end - col);
            col = blank_end;
        } else if (cell->kind == SSR_CELL_GLYPH) {
            cchar_t output;
            int glyph_end = col + 1;

            while (glyph_end < end &&
                   cells[glyph_end].kind == SSR_CELL_CONTINUATION)
                glyph_end++;
            setcchar(&output, cell->text, cell->attr, 0, NULL);
            mvwadd_wch(window, row, left + col, &output);
            col = glyph_end;
        } else {
            col++;
        }
    }
}

static int ssr_row_needs_raw_unicode(const SsrRow *row)
{
    if (!row || row->kind != SSR_ROW_TEXT)
        return 0;

    for (int i = row->start; i < row->end; ) {
        wchar_t wc;
        int used = 1;
        int width;

        if (row->line[i] == '\t') {
            i++;
            continue;
        }
        width = ssr_utf8_decode_n(row->line + i, row->len - i, &wc, &used);
        if (width == 0 || wc == 0x200D)
            return 1;
        i += used;
    }
    return 0;
}

/* ncurses' complex-character API discards spacing members of emoji ZWJ
 * clusters.  Feeding the original UTF-8 row in one operation lets the
 * terminal shape the grapheme while the normal cell cache still handles
 * ordinary rows efficiently. */
static void ssr_emit_raw_unicode_row(WINDOW *window, int row, int left,
                                     int width, const SsrRow *desc,
                                     attr_t attr)
{
    size_t source_len;
    size_t cap;
    size_t used = 0;
    int visual_col = 0;
    int join_next = 0;
    char *expanded;

    wattrset(window, attr);
    mvwhline(window, row, left, ' ', width);
    if (!desc || desc->kind != SSR_ROW_TEXT || desc->end <= desc->start)
        return;

    source_len = (size_t)(desc->end - desc->start);
    cap = source_len > ((size_t)-1 - 1) / SIMPLERENDER_TAB_WIDTH
              ? source_len + 1
              : source_len * SIMPLERENDER_TAB_WIDTH + 1;
    expanded = malloc(cap);
    if (!expanded) {
        mvwaddnstr(window, row, left, desc->line + desc->start,
                   desc->end - desc->start);
        return;
    }

    for (int i = desc->start; i < desc->end; ) {
        if (desc->line[i] == '\t') {
            int spaces = SIMPLERENDER_TAB_WIDTH -
                         (visual_col % SIMPLERENDER_TAB_WIDTH);
            while (spaces-- > 0 && used + 1 < cap) {
                expanded[used++] = ' ';
                visual_col++;
            }
            join_next = 0;
            i++;
        } else {
            wchar_t wc;
            int bytes = 1;
            int glyph_width = ssr_utf8_decode_n(desc->line + i,
                                                 desc->len - i,
                                                 &wc, &bytes);

            if (used + (size_t)bytes >= cap)
                break;
            memcpy(expanded + used, desc->line + i, (size_t)bytes);
            used += (size_t)bytes;
            if (!(join_next && glyph_width > 0 && ssr_is_emoji_base(wc)))
                visual_col += glyph_width;
            join_next = wc == 0x200D;
            i += bytes;
        }
    }
    expanded[used] = '\0';
    mvwaddnstr(window, row, left, expanded, (int)used);
    free(expanded);
}

static void ssr_repaint_changed_row(SsrRenderer *r, int row,
                                    int top, int left, int width)
{
    WINDOW *window = r->body_window ? r->body_window : stdscr;
    int window_row = r->body_window ? row : top + row;
    int window_left = r->body_window ? 0 : left;
    SsrCell *old = r->screen_cells + (size_t)row * (size_t)width;
    SsrCell *desired = r->desired_cells + (size_t)row * (size_t)width;
    unsigned char dirty[width];

    if (ssr_row_needs_raw_unicode(&r->desired_rows[row])) {
        ssr_emit_raw_unicode_row(window, window_row, window_left, width,
                                 &r->desired_rows[row], desired[0].attr);
        memcpy(old, desired, sizeof(*old) * (size_t)width);
        return;
    }

    memset(dirty, 0, sizeof(dirty));
    for (int col = 0; col < width; col++) {
        if (!ssr_cells_equal(&old[col], &desired[col])) {
            ssr_mark_wide_group(old, dirty, col, width);
            ssr_mark_wide_group(desired, dirty, col, width);
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
        ssr_emit_run(window, window_row, window_left, col, end, desired);
        col = end;
    }

    memcpy(old, desired, sizeof(*old) * (size_t)width);
}

static void ssr_invalidate_cell_row(SsrRenderer *r, int row,
                                    int width, attr_t attr)
{
    SsrCell *cells = r->screen_cells + (size_t)row * (size_t)width;

    for (int col = 0; col < width; col++)
        ssr_set_blank(&cells[col], attr);
}

static int ssr_shifted_cells_match(SsrRenderer *r, int delta,
                                   int height, int width)
{
    for (int row = 0; row < height; row++) {
        int old_row = row + delta;

        if (old_row < 0 || old_row >= height)
            continue;
        for (int col = 0; col < width; col++) {
            const SsrCell *old = r->screen_cells +
                (size_t)old_row * (size_t)width + (size_t)col;
            const SsrCell *desired = r->desired_cells +
                (size_t)row * (size_t)width + (size_t)col;

            if (!ssr_cells_equal(old, desired))
                return 0;
        }
    }

    return 1;
}

static void ssr_shift_cached_cells(SsrRenderer *r, int delta,
                                   int height, int width, attr_t attr)
{
    size_t row_size = sizeof(*r->screen_cells) * (size_t)width;

    if (delta > 0) {
        memmove(r->screen_cells,
                r->screen_cells + (size_t)delta * (size_t)width,
                row_size * (size_t)(height - delta));
        for (int row = height - delta; row < height; row++)
            ssr_invalidate_cell_row(r, row, width, attr);
    } else {
        int amount = -delta;

        memmove(r->screen_cells + (size_t)amount * (size_t)width,
                r->screen_cells,
                row_size * (size_t)(height - amount));
        for (int row = 0; row < amount; row++)
            ssr_invalidate_cell_row(r, row, width, attr);
    }
}

static int ssr_try_body_scroll(SsrRenderer *r, int delta,
                               int height, int width,
                               unsigned long long body_hash,
                               attr_t attr)
{
    if (!r->scroll_window_enabled || !r->body_window || delta == 0 ||
        height < 2 || delta <= -height || delta >= height ||
        delta < -5 || delta > 5 ||
        r->cache_body_hash != body_hash ||
        !ssr_shifted_cells_match(r, delta, height, width))
        return 0;
    if (wscrl(r->body_window, delta) == ERR)
        return 0;

    ssr_shift_cached_cells(r, delta, height, width, attr);
    return 1;
}

static int ssr_geometry_matches(const SsrRenderer *r, int top, int left,
                                int height, int width)
{
    return r->cache_valid &&
           r->cache_lines == LINES &&
           r->cache_cols == COLS &&
           r->cache_top == top &&
           r->cache_left == left &&
           r->cache_height == height &&
           r->cache_width == width;
}

static void ssr_capture_cache(SsrRenderer *r, int top, int left, int height,
                              int width, int scroll,
                              unsigned long long body_hash)
{
    size_t cell_count = (size_t)height * (size_t)width;

    memcpy(r->screen_cells, r->desired_cells, sizeof(*r->screen_cells) * cell_count);
    r->cache_lines = LINES;
    r->cache_cols = COLS;
    r->cache_top = top;
    r->cache_left = left;
    r->cache_height = height;
    r->cache_width = width;
    r->cache_scroll = scroll;
    r->cache_body_hash = body_hash;
    r->cache_valid = 1;
}

static void ssr_sync_body_window_from_stdscr(SsrRenderer *r)
{
    if (!r->body_window)
        return;
    copywin(stdscr, r->body_window,
            r->body_window_top, r->body_window_left, 0, 0,
            r->body_window_height - 1, r->body_window_width - 1, FALSE);
}

static void ssr_full_repaint_to(SsrRenderer *r, WINDOW *window,
                                int top, int left, int height, int width)
{
    int window_is_body = window && window == r->body_window;
    int window_left = window_is_body ? 0 : left;

    for (int row = 0; row < height; row++) {
        if (ssr_row_needs_raw_unicode(&r->desired_rows[row]))
            ssr_emit_raw_unicode_row(
                window, window_is_body ? row : top + row, window_left, width,
                &r->desired_rows[row],
                r->desired_cells[(size_t)row * (size_t)width].attr);
        else
            ssr_emit_run(window, window_is_body ? row : top + row,
                         window_left, 0, width,
                         r->desired_cells + (size_t)row * (size_t)width);
    }
}

static void ssr_present(SsrRenderer *r, int top, int left)
{
    if (r->body_window) {
        wmove(r->body_window, 0, 0);
        wnoutrefresh(stdscr);
        wnoutrefresh(r->body_window);
        doupdate();
    } else {
        move(top, left);
        refresh();
    }
}

static int ssr_render_text(SsrRenderer *r, const char *text, int scroll,
                           int top, int left, int height, int width,
                           attr_t attr)
{
    unsigned long long body_hash;
    int body_ready;

    if (!r->desired_rows && !r->screen_cells && !r->desired_cells) {
        int keep_windowed_redraw_enabled = r->windowed_redraw_enabled;
        int keep_scroll_window_enabled = r->scroll_window_enabled;

        ssr_init(r);
        r->windowed_redraw_enabled = keep_windowed_redraw_enabled;
        r->scroll_window_enabled = keep_scroll_window_enabled;
    }

    if (top < 0) {
        height += top;
        top = 0;
    }
    if (left < 0) {
        width += left;
        left = 0;
    }
    if (top >= LINES || left >= COLS || height < 1 || width < 1)
        return 0;
    if (height > LINES - top)
        height = LINES - top;
    if (width > COLS - left)
        width = COLS - left;
    if (height < 1 || width < 1)
        return 0;
    if (scroll < 0)
        scroll = 0;

    curs_set(0);
    body_hash = ssr_text_hash(text);

    if (!ssr_ensure_storage(r, height, width))
        return 0;

    body_ready = ssr_ensure_body_window(r, top, left, height, width, attr);
    if (!ssr_geometry_matches(r, top, left, height, width) ||
        !ssr_cells_valid(r, height, width)) {
        ssr_build_desired_cells(r, text, scroll, height, width, attr);
        ssr_full_repaint_to(r, stdscr, top, left, height, width);
        ssr_capture_cache(r, top, left, height, width, scroll, body_hash);
        if (body_ready)
            ssr_sync_body_window_from_stdscr(r);
        ssr_present(r, top, left);
        return 1;
    }

    ssr_build_desired_cells(r, text, scroll, height, width, attr);
    if (body_ready)
        ssr_try_body_scroll(r, scroll - r->cache_scroll,
                            height, width, body_hash, attr);
    for (int row = 0; row < height; row++)
        ssr_repaint_changed_row(r, row, top, left, width);

    r->cache_scroll = scroll;
    r->cache_body_hash = body_hash;
    r->cache_valid = 1;
    ssr_present(r, top, left);
    return 1;
}

#endif
