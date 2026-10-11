#ifndef SIMPLETEXT_H
#define SIMPLETEXT_H

/* The writing surface shared by SimpleWords and SimpleNote. Storage, saving,
 * and application commands belong to the caller; wrapping and movement do not. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <curses.h>

static inline void sst_put_glyph(WINDOW *window, int row, int col,
                                 const wchar_t *text, attr_t attr)
{
    cchar_t cell;
    int height, width;
    getmaxyx(window, height, width); (void)height;
    /* ncurses merges window attributes into a complex character. A glyph
     * leaving a selection must not inherit reverse video from a blank cell. */
    wattrset(window, attr);
    setcchar(&cell, text, attr, 0, NULL);
    if (width > 0 && col == width - 1) mvwins_wch(window, row, col, &cell);
    else mvwadd_wch(window, row, col, &cell);
}

static inline int sst_decode(const char *text, size_t available, wchar_t *wc, int *used)
{
    mbstate_t state = {0};
    size_t n = mbrtowc(wc, text, available, &state);
    if (!n || n == (size_t)-1 || n == (size_t)-2) {
        *wc = L'\xfffd'; *used = 1; return 1;
    }
    *used = (int)n;
    int width = wcwidth(*wc);
    return width < 1 ? 1 : width;
}

static inline int sst_char_width(const char *text, int available, int col, int *used)
{
    if (*text == '\t') { *used = 1; return 4 - col % 4; }
    wchar_t wc;
    return sst_decode(text, (size_t)available, &wc, used);
}

static inline int sst_visual_col(const char *text, int length, int start, int end)
{
    int col = 0;
    if (end > length) end = length;
    for (int i = start; i < end; ) {
        int used;
        int width = sst_char_width(text + i, length - i, col, &used);
        if (i + used > end) break;
        col += width; i += used;
    }
    return col;
}

static inline int sst_space(char c) { return c == ' ' || c == '\t'; }

/* Keep every typed separator visible, including runs of spaces at a wrap. */
static inline void sst_wrap(const char *text, int length, int start, int width,
                            int *end, int *visual_width)
{
    int col = 0, last_break = -1, break_col = 0, space_start = -1;
    if (width < 1) width = 1;
    for (int i = start; i < length; ) {
        int used;
        int w = sst_char_width(text + i, length - i, col, &used);
        if (col + w > width) {
            if (last_break > start && last_break < i) {
                if (!sst_space(text[i]) && space_start > start && i - space_start > 1) {
                    *end = space_start;
                    *visual_width = sst_visual_col(text, length, start, space_start);
                } else { *end = last_break; *visual_width = break_col; }
            } else {
                *end = i; *visual_width = col;
                if (i == start) { *end = i + used; *visual_width = w > width ? width : w; }
            }
            return;
        }
        col += w;
        if (sst_space(text[i])) {
            if (i == start || !sst_space(text[i - 1])) space_start = i;
            last_break = i + used; break_col = col;
        } else space_start = -1;
        i += used;
    }
    *end = length; *visual_width = col;
}

static inline size_t sst_previous(const char *text, size_t index)
{
    if (!index) return 0;
    size_t start = index - 1;
    while (start && ((unsigned char)text[start] & 0xc0u) == 0x80u) start--;
    wchar_t wc; int used;
    sst_decode(text + start, index - start, &wc, &used);
    return start + (size_t)used == index ? start : index - 1;
}

static inline size_t sst_next(const char *text, size_t length, size_t index)
{
    if (index >= length) return length;
    wchar_t wc; int used;
    sst_decode(text + index, length - index, &wc, &used);
    return index + (size_t)used > length ? index + 1 : index + (size_t)used;
}

typedef struct { int line; size_t index; } SstPos;
typedef struct { SstPos start, end; int number, width; } SstRow;
typedef struct {
    SstPos cursor, anchor, affinity;
    int selection_active, goal, affinity_row, affinity_col;
} SstView;
typedef struct {
    void *context;
    int (*row)(void *, int, SstRow *);
    int (*position_row)(void *, SstPos, SstRow *);
    SstPos (*point)(void *, const SstRow *, int);
    int (*column)(void *, const SstRow *, SstPos);
    SstPos (*step)(void *, SstPos, int);
    SstPos (*line_edge)(void *, SstPos, int);
} SstLayout;
typedef enum { SST_LEFT = 1, SST_RIGHT, SST_UP, SST_DOWN,
               SST_HOME, SST_END, SST_PAGE_UP, SST_PAGE_DOWN } SstMovement;

static inline int sst_modified_navigation(const char *sequence)
{
    int first, modifier; char final;
    if ((sscanf(sequence, "[1;%d%c", &modifier, &final) == 2 ||
         sscanf(sequence, "O1;%d%c", &modifier, &final) == 2) &&
        modifier > 0 && ((modifier - 1) & 1)) {
        if (final == 'A') return SST_UP;
        if (final == 'B') return SST_DOWN;
        if (final == 'C') return SST_RIGHT;
        if (final == 'D') return SST_LEFT;
    }
    if (sscanf(sequence, "[%d;%d%c", &first, &modifier, &final) == 3 &&
        modifier > 0 && ((modifier - 1) & 1)) {
        if ((final == '~' && first == 5) || (final == 'u' && first == 57354)) return SST_PAGE_UP;
        if ((final == '~' && first == 6) || (final == 'u' && first == 57355)) return SST_PAGE_DOWN;
        if (final == 'u' && first == 57352) return SST_UP;
        if (final == 'u' && first == 57353) return SST_DOWN;
    }
    return 0;
}

static inline int sst_equal(SstPos a, SstPos b)
{
    return a.line == b.line && a.index == b.index;
}

static inline void sst_clear_affinity(SstView *view)
{
    view->affinity_row = view->affinity_col = -1;
}

static inline void sst_set_affinity(SstView *view, int row, int col)
{
    view->affinity = view->cursor;
    view->affinity_row = row; view->affinity_col = col;
}

static inline int sst_cursor_row(const SstLayout *layout, const SstView *view,
                                 SstRow *row, int *col)
{
    if (view->affinity_row >= 0 && view->affinity_col >= 0 &&
        sst_equal(view->cursor, view->affinity) &&
        layout->row(layout->context, view->affinity_row, row) &&
        view->affinity_col <= row->width &&
        sst_equal(layout->point(layout->context, row, view->affinity_col), view->cursor)) {
        *col = view->affinity_col;
        return 1;
    }
    if (!layout->position_row(layout->context, view->cursor, row)) return 0;
    *col = layout->column(layout->context, row, view->cursor);
    return 1;
}

static inline void sst_move(const SstLayout *layout, SstView *view,
                            SstMovement movement, int extend, int page)
{
    SstRow row, target;
    int col;
    if (extend) {
        if (!view->selection_active) { view->anchor = view->cursor; view->selection_active = 1; }
    } else view->selection_active = 0;
    if (!sst_cursor_row(layout, view, &row, &col)) return;

    if (movement == SST_PAGE_UP || movement == SST_PAGE_DOWN) {
        for (int i = 0; i < (page > 0 ? page : 1); i++)
            sst_move(layout, view, movement == SST_PAGE_UP ? SST_UP : SST_DOWN, extend, 1);
        return;
    }
    if (movement == SST_UP || movement == SST_DOWN) {
        int direction = movement == SST_UP ? -1 : 1;
        if (view->goal < 0) view->goal = col;
        if (!layout->row(layout->context, row.number + direction, &target)) {
            if (extend) view->cursor = layout->line_edge(layout->context, view->cursor, direction > 0);
            return;
        }
        col = view->goal < target.width ? view->goal : target.width;
        if (col < 0) col = 0;
        view->cursor = layout->point(layout->context, &target, col);
        sst_set_affinity(view, target.number, col);
        return;
    }
    view->goal = -1;
    if (movement == SST_HOME || movement == SST_END) {
        view->cursor = movement == SST_HOME ? row.start : row.end;
        sst_set_affinity(view, row.number, movement == SST_HOME ? 0 : row.width);
        return;
    }
    if (movement == SST_LEFT && col == 0 && row.number > 0 &&
        layout->row(layout->context, row.number - 1, &target) &&
        sst_equal(target.end, view->cursor)) {
        sst_set_affinity(view, target.number, target.width);
        return;
    }
    if (movement == SST_RIGHT && view->affinity_row >= 0 &&
        sst_equal(view->cursor, view->affinity) && col == row.width &&
        layout->row(layout->context, row.number + 1, &target) &&
        sst_equal(row.end, view->cursor) && sst_equal(target.start, view->cursor)) {
        sst_set_affinity(view, target.number, 0);
        return;
    }
    sst_clear_affinity(view);
    view->cursor = layout->step(layout->context, view->cursor, movement == SST_LEFT ? -1 : 1);
}

#endif
