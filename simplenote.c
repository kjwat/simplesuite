#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1

/* A blank page first, then a journal shelf. No workspace or document engine. */
#include <ctype.h>
#include <locale.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <wchar.h>
#include <wctype.h>
#include <curses.h>
#include "simpleui.h"
#include "simplerender.h"
#include "simpleproc.h"
#include "simplenote-store.h"

#define SN_VERSION "1.0.0"
#define CTRL(k) ((k) & 31)
#define SN_PASTE (KEY_MAX + 1)
#define SN_UNDO_COUNT 64
#define SN_UNDO_BYTES (8u * 1024u * 1024u)
#define SN_DRAFT_INTERVAL_MS 2000
#define SN_SAVE_INTERVAL_MS 5000

typedef struct { size_t start, end, next; int last; } NoteRow;
typedef struct { char *text; size_t cursor; } NoteUndo;
typedef struct { int left, top, width, height, pane_left; } NoteGeometry;
typedef struct {
    int enabled, dragging, moved, press_x, press_y, clicks;
    int64_t clicked_at;
    size_t anchor_start, anchor_end, start, end;
    char note_id[33];
} NoteMouse;
typedef enum { COMPOSE, BROWSE, HELP } NoteView;
typedef struct {
    SnStore store;
    SnNote edit;
    SnNote *original;
    size_t cursor, anchor;
    int selection, dirty, draft_dirty, scroll, preferred_col;
    int64_t changed_at, dirty_since, draft_since, autosave_retry_at;
    int64_t status_until, last_rollover;
    NoteUndo undo[SN_UNDO_COUNT];
    size_t undo_count, undo_bytes;
    char *kill;
    NoteRow *rows;
    size_t row_count, row_capacity;
    SnNote **visible;
    size_t visible_count, selected;
    int list_top, read_scroll, year_filter, trash, focus, sidebar_row, sidebar_top;
    int *years;
    size_t year_count;
    char search[256], status[512];
    NoteView view, before_help;
    int full_redraw, control_x, colors;
    NoteMouse mouse;
    SsrRenderer renderer;
} NoteApp;

static volatile sig_atomic_t note_stop;
static void note_signal(int sig) { note_stop = sig; }

static int current_year(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year + 1900;
}

static void message(NoteApp *a, const char *text)
{
    snprintf(a->status, sizeof a->status, "%s", text);
    a->status_until = sui_monotonic_ms() + 5000;
}

static size_t scalar_next(const char *text, size_t pos)
{
    if (!text[pos]) return pos;
    int used; wchar_t wc;
    ssr_utf8_decode_n(text + pos, (int)strnlen(text + pos, MB_LEN_MAX), &wc, &used);
    return pos + (size_t)used;
}

static size_t char_next(const char *text, size_t pos)
{
    size_t next = scalar_next(text, pos);
    if (next == pos || text[pos] == '\n') return next;
    int joined = 0;
    while (text[next] && text[next] != '\n') {
        int used; wchar_t wc;
        int width = ssr_utf8_decode_n(text + next, (int)strnlen(text + next, MB_LEN_MAX), &wc, &used);
        if (width > 0 && !joined) break;
        joined = wc == 0x200d;
        next += (size_t)used;
    }
    return next;
}

static size_t char_prev(const char *text, size_t pos)
{
    /* Walk just this logical line to keep combining and joined glyphs intact. */
    if (!pos) return 0;
    if (text[pos - 1] == '\n') return pos - 1;
    size_t start = pos;
    while (start && text[start - 1] != '\n') start--;
    size_t previous = start;
    while (start < pos) {
        previous = start;
        size_t next = char_next(text, start);
        if (next <= start || next >= pos) break;
        start = next;
    }
    return previous;
}

static void undo_clear(NoteApp *a)
{
    for (size_t i = 0; i < a->undo_count; i++) free(a->undo[i].text);
    a->undo_count = a->undo_bytes = 0;
}

static void undo_push(NoteApp *a)
{
    size_t bytes = a->edit.len + 1;
    while (a->undo_count && (a->undo_count == SN_UNDO_COUNT || a->undo_bytes + bytes > SN_UNDO_BYTES)) {
        a->undo_bytes -= strlen(a->undo[0].text) + 1;
        free(a->undo[0].text);
        memmove(a->undo, a->undo + 1, (--a->undo_count) * sizeof a->undo[0]);
    }
    if (bytes > SN_UNDO_BYTES) return;
    char *copy = strdup(a->edit.text);
    if (!copy) return;
    a->undo[a->undo_count++] = (NoteUndo){copy, a->cursor};
    a->undo_bytes += bytes;
}

static void changed(NoteApp *a)
{
    int64_t now = sui_monotonic_ms();
    if (!a->edit.id[0]) { sn_new_id(a->edit.id); sn_timestamp(a->edit.created); }
    sn_timestamp(a->edit.updated);
    if (!a->dirty) a->dirty_since = now;
    if (!a->draft_dirty) a->draft_since = now;
    a->dirty = a->draft_dirty = 1;
    a->changed_at = now;
    a->preferred_col = -1;
}

static int replace_range(NoteApp *a, size_t start, size_t end, const char *text, size_t len)
{
    if (start > end || end > a->edit.len || len > SN_TEXT_LIMIT - (a->edit.len - (end - start))) {
        message(a, "Note exceeds 16 MiB"); return 0;
    }
    size_t total = a->edit.len - (end - start) + len;
    char *copy = malloc(total + 1);
    if (!copy) { message(a, "Out of memory"); return 0; }
    memcpy(copy, a->edit.text, start);
    memcpy(copy + start, text, len);
    memcpy(copy + start + len, a->edit.text + end, a->edit.len - end + 1);
    undo_push(a);
    free(a->edit.text); a->edit.text = copy; a->edit.len = total;
    a->cursor = start + len; a->selection = 0;
    changed(a);
    return 1;
}

static int insert_text(NoteApp *a, const char *text, size_t len)
{
    size_t start = a->cursor, end = a->cursor;
    if (a->selection) {
        start = a->anchor < a->cursor ? a->anchor : a->cursor;
        end = a->anchor > a->cursor ? a->anchor : a->cursor;
    }
    return replace_range(a, start, end, text, len);
}

static void undo_edit(NoteApp *a)
{
    if (!a->undo_count) { message(a, "No earlier edit"); return; }
    NoteUndo u = a->undo[--a->undo_count];
    a->undo_bytes -= strlen(u.text) + 1;
    free(a->edit.text); a->edit.text = u.text; a->edit.len = strlen(u.text);
    a->cursor = u.cursor; a->selection = 0;
    changed(a);
}

static int save_edit(NoteApp *a)
{
    if (!a->dirty) return 1;
    if (!a->edit.id[0]) return 1;
    if (!a->edit.len && !sn_find(&a->store, a->edit.id)) {
        if (!sn_clear_draft(&a->store)) { message(a, a->store.error); return 0; }
        a->edit.id[0] = a->edit.created[0] = a->edit.updated[0] = 0;
        a->dirty = a->draft_dirty = 0;
        return 1;
    }
    SnNote *saved = NULL;
    if (!sn_save_edit_draft(&a->store, &a->edit, a->original) ||
        !sn_rollover(&a->store, current_year()) ||
        !sn_store_put(&a->store, a->edit.id, a->edit.created, a->edit.updated,
                      a->edit.text, 0, &saved)) {
        message(a, a->store.error); return 0;
    }
    a->edit.batch = saved->batch;
    a->dirty = a->draft_dirty = 0;
    a->autosave_retry_at = 0;
    return 1;
}

static int autosave(NoteApp *a, int64_t now)
{
    if (!a->dirty || now < a->autosave_retry_at) return 0;
    if (a->draft_dirty && (now - a->changed_at >= 250 ||
                          now - a->draft_since >= SN_DRAFT_INTERVAL_MS)) {
        if (!sn_save_edit_draft(&a->store, &a->edit, a->original)) {
            message(a, a->store.error); a->autosave_retry_at = now + 1000;
            return 1;
        }
        a->draft_dirty = 0;
    }
    if ((now - a->changed_at >= 1000 || now - a->dirty_since >= SN_SAVE_INTERVAL_MS) &&
        !save_edit(a)) {
        a->autosave_retry_at = now + 1000;
        return 1;
    }
    return 0;
}

static int note_compare(const void *left, const void *right)
{
    const SnNote *a = *(SnNote *const *)left, *b = *(SnNote *const *)right;
    int rc = strcmp(b->created, a->created);
    if (rc) return rc;
    if (a->batch->sequence != b->batch->sequence)
        return a->batch->sequence < b->batch->sequence ? 1 : -1;
    size_t a_index = 0, b_index = 0;
    for (size_t i = 0; i < a->batch->count; i++) {
        if (a->batch->notes[i] == a) a_index = i;
        if (a->batch->notes[i] == b) b_index = i;
    }
    return a_index < b_index ? 1 : (a_index > b_index ? -1 : 0);
}

static int year_compare(const void *left, const void *right)
{
    return *(const int *)right - *(const int *)left;
}

static int text_matches(const char *text, const char *query)
{
    if (!query[0]) return 1;
    size_t length = strlen(text), qlen = strlen(query);
    for (size_t i = 0; i <= length && qlen <= length - i; i++) {
        size_t j;
        for (j = 0; j < qlen; j++)
            if (tolower((unsigned char)text[i + j]) != tolower((unsigned char)query[j])) break;
        if (j == qlen) return 1;
    }
    return 0;
}

static int rebuild_list(NoteApp *a, const char *select_id)
{
    size_t total = 0;
    for (size_t i = 0; i < a->store.count; i++) total += a->store.batches[i]->count;
    SnNote **list = malloc((total + 1) * sizeof *list);
    int *years = malloc((a->store.count + 1) * sizeof *years);
    if (!list || !years) { free(list); free(years); message(a, "Out of memory"); return 0; }
    size_t count = 0, year_count = 0;
    for (size_t i = 0; i < a->store.count; i++) {
        SnBatch *batch = a->store.batches[i];
        int present = 0;
        for (size_t j = 0; j < year_count; j++) if (years[j] == batch->year) present = 1;
        if (!present && batch->count) years[year_count++] = batch->year;
        for (size_t j = 0; j < batch->count; j++) {
            SnNote *note = batch->notes[j];
            if (note->deleted == a->trash && (!a->year_filter || batch->year == a->year_filter) &&
                text_matches(note->text, a->search)) list[count++] = note;
        }
    }
    qsort(list, count, sizeof *list, note_compare);
    qsort(years, year_count, sizeof *years, year_compare);
    free(a->visible); free(a->years);
    a->visible = list; a->visible_count = count; a->years = years; a->year_count = year_count;
    if (a->selected >= count) a->selected = count ? count - 1 : 0;
    if (select_id && select_id[0])
        for (size_t i = 0; i < count; i++) if (!strcmp(list[i]->id, select_id)) a->selected = i;
    a->read_scroll = 0;
    a->mouse.dragging = 0; a->mouse.note_id[0] = 0; a->mouse.clicked_at = 0;
    return 1;
}

static void show_browser(NoteApp *a)
{
    if (!save_edit(a)) return;
    if (!sn_clear_draft(&a->store)) { message(a, a->store.error); return; }
    sn_free_note(a->original); a->original = NULL;
    a->view = BROWSE; a->full_redraw = 1; a->focus = 1;
    if (!a->trash) {
        SnNote *selected = sn_find(&a->store, a->edit.id);
        if (selected && (a->year_filter && selected->batch->year != a->year_filter)) a->year_filter = 0;
        if (selected && !text_matches(selected->text, a->search)) a->search[0] = 0;
    }
    rebuild_list(a, a->edit.id);
}

static void discard_edit(NoteApp *a)
{
    char select_id[33] = "";
    char *empty = strdup("");
    if (!empty) { message(a, "Out of memory"); return; }
    int ok = 1;
    /* Autosave may already have replaced the saved note. Keep its original in
     * recovery until restoring/removing the record has durably completed. */
    if (a->original) {
        SnNote *n = a->original;
        strcpy(select_id, n->id);
        ok = sn_store_put(&a->store, n->id, n->created, n->updated, n->text, n->deleted, NULL);
    } else if (a->edit.id[0]) ok = sn_store_remove(&a->store, a->edit.id);
    if (ok) ok = sn_clear_draft(&a->store);
    if (!ok) {
        free(empty); message(a, a->store.error);
        /* Retain both versions and permit retrying a failed discard. */
        if (a->edit.id[0]) {
            changed(a);
        }
        return;
    }
    sn_free_note(a->original); a->original = NULL;
    free(a->edit.text); memset(&a->edit, 0, sizeof a->edit); a->edit.text = empty;
    undo_clear(a); free(a->kill); a->kill = NULL;
    a->cursor = a->anchor = 0; a->selection = a->dirty = a->draft_dirty = 0;
    /* A recovered new note may have appeared in the old list before removal. */
    free(a->visible); a->visible = NULL; a->visible_count = 0;
    rebuild_list(a, select_id);
    a->view = BROWSE; a->full_redraw = 1; a->focus = 1;
    message(a, select_id[0] ? "Edits discarded; saved note restored" : "Note discarded");
}

static SnNote *copy_note(const SnNote *note)
{
    SnNote *copy = malloc(sizeof *copy);
    if (!copy) return NULL;
    *copy = *note; copy->text = strdup(note->text);
    if (!copy->text) { free(copy); return NULL; }
    return copy;
}

static int open_editor(NoteApp *a, SnNote *note)
{
    char *text = strdup(note ? note->text : "");
    SnNote *original = note ? copy_note(note) : NULL;
    if (!text || (note && !original)) {
        free(text); sn_free_note(original); message(a, "Out of memory"); return 0;
    }
    sn_free_note(a->original); a->original = original;
    free(a->edit.text); memset(&a->edit, 0, sizeof a->edit);
    if (note) a->edit = *note;
    a->edit.text = text; a->edit.len = strlen(text);
    a->cursor = a->edit.len; a->anchor = 0; a->selection = 0;
    a->dirty = a->draft_dirty = 0; a->scroll = 0; a->preferred_col = -1;
    a->autosave_retry_at = 0;
    undo_clear(a); a->view = COMPOSE; a->full_redraw = 1;
    a->status[0] = 0;
    return 1;
}

static int append_row(NoteApp *a, NoteRow row)
{
    if (a->row_count == a->row_capacity) {
        size_t capacity = a->row_capacity ? a->row_capacity * 2 : 64;
        NoteRow *p = realloc(a->rows, capacity * sizeof *p);
        if (!p) return 0;
        a->rows = p; a->row_capacity = capacity;
    }
    a->rows[a->row_count++] = row;
    return 1;
}

static int text_rows(NoteApp *a, const char *text, size_t length, int width)
{
    a->row_count = 0;
    size_t base = 0;
    for (;;) {
        const char *newline = strchr(text + base, '\n');
        int len = (int)(newline ? (size_t)(newline - text) - base : length - base);
        if (!len) {
            if (!append_row(a, (NoteRow){base, base, base, 1})) return 0;
        } else for (int start = 0; start < len; ) {
            int end, next;
            ssr_wrap_segment(text + base, len, start, width, &end, &next);
            if (!append_row(a, (NoteRow){base + (size_t)start, base + (size_t)end,
                                       base + (size_t)next, next == len})) return 0;
            start = next;
        }
        if (!newline) break;
        base = (size_t)(newline - text) + 1;
    }
    return 1;
}

static int editor_rows(NoteApp *a, int width)
{
    return text_rows(a, a->edit.text, a->edit.len, width);
}

static size_t cursor_row(NoteApp *a, int *col)
{
    for (size_t i = 0; i < a->row_count; i++) {
        NoteRow *r = &a->rows[i];
        if (a->cursor >= r->start && (a->cursor < r->next || (r->last && a->cursor <= r->end))) {
            *col = ssr_visual_col_range(a->edit.text + r->start, (int)(r->end - r->start),
                                        0, (int)(a->cursor - r->start));
            return i;
        }
    }
    *col = 0;
    return a->row_count ? a->row_count - 1 : 0;
}

static int editor_width(void) { return COLS > 82 ? 80 : (COLS > 2 ? COLS - 2 : 1); }
static int editor_top(void) { return LINES > 6 ? 3 : 0; }
static int editor_height(void) { int h = LINES - editor_top() - 1; return h > 0 ? h : 1; }

static void cursor_vertical(NoteApp *a, int delta)
{
    if (!editor_rows(a, editor_width())) return;
    int col;
    long row = (long)cursor_row(a, &col);
    if (a->preferred_col < 0) a->preferred_col = col;
    row += delta;
    if (row < 0) row = 0;
    if ((size_t)row >= a->row_count) row = (long)a->row_count - 1;
    NoteRow r = a->rows[row];
    size_t pos = r.start;
    while (pos < r.end) {
        size_t next = char_next(a->edit.text, pos);
        int width = ssr_visual_col_range(a->edit.text + r.start, (int)(r.end - r.start), 0,
                                         (int)(next - r.start));
        if (width > a->preferred_col) break;
        pos = next;
    }
    a->cursor = pos;
}

static void clear_line(int y, int x, int width, attr_t attr)
{
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || width < 1) return;
    if (width > COLS - x) width = COLS - x;
    attrset(attr); mvhline(y, x, ' ', width);
}

static void clipped(int y, int x, const char *text, int width, attr_t attr)
{
    if (width < 1 || y < 0 || y >= LINES || x < 0 || x >= COLS) return;
    int col = 0;
    attrset(attr); move(y, x);
    for (size_t i = 0, len = strlen(text); i < len; ) {
        int used; wchar_t wc;
        int w = ssr_utf8_decode_n(text + i, (int)(len - i), &wc, &used);
        if (wc == '\n' || wc == '\r') break;
        if (wc == '\t') { wc = ' '; w = 1; }
        if (iswcntrl(wc)) { wc = L'\xfffd'; w = 1; }
        if (col + w > width || x + col + w > COLS) break;
        wchar_t glyph[2] = {wc, 0};
        addnwstr(glyph, 1); col += w; i += (size_t)used;
    }
    attrset(A_NORMAL);
}

static void preview(const char *text, char *out, size_t size)
{
    size_t i = 0, j = 0;
    while (text[i] && isspace((unsigned char)text[i])) i++;
    int space = 0;
    for (; text[i] && j + 1 < size; i++) {
        if (isspace((unsigned char)text[i])) { space = 1; continue; }
        if (space && j && j + 2 < size) out[j++] = ' ';
        space = 0; out[j++] = text[i];
    }
    /* Do not leave a truncated UTF-8 character at the end of a preview. */
    if (text[i] && j) {
        while (j && ((unsigned char)out[j - 1] & 0xc0) == 0x80) j--;
        if (j && (unsigned char)out[j - 1] >= 0xc0) j--;
    }
    out[j] = 0;
    if (!j) snprintf(out, size, "Empty note");
}

static size_t year_count(NoteApp *a, int year)
{
    size_t count = 0;
    for (size_t i = 0; i < a->store.count; i++)
        if (!year || a->store.batches[i]->year == year)
            for (size_t j = 0; j < a->store.batches[i]->count; j++)
                if (a->store.batches[i]->notes[j]->deleted == a->trash) count++;
    return count;
}

static attr_t highlight(NoteApp *a) { return a->colors ? COLOR_PAIR(1) : A_REVERSE; }

static void draw_editor(NoteApp *a)
{
    int width = editor_width(), left = (COLS - width) / 2, height = editor_height(), col = 0;
    if (!editor_rows(a, width)) { message(a, "Out of memory"); return; }
    int row = (int)cursor_row(a, &col);
    if (row < a->scroll) a->scroll = row;
    if (row >= a->scroll + height) a->scroll = row - height + 1;
    if (a->selection && a->cursor != a->anchor) {
        SsrSpan span = {a->cursor < a->anchor ? a->cursor : a->anchor,
                       a->cursor > a->anchor ? a->cursor : a->anchor, A_REVERSE};
        ssr_render_text_spans(&a->renderer, a->edit.text, a->scroll, editor_top(), left,
                              height, width, A_NORMAL, &span, 1);
    } else ssr_render_text(&a->renderer, a->edit.text, a->scroll, editor_top(), left,
                           height, width, A_NORMAL);
    clear_line(LINES - 1, 0, COLS, A_NORMAL);
    if (a->status[0]) clipped(LINES - 1, 1, a->status, COLS - 2, A_NORMAL);
    if (a->control_x) clipped(LINES - 1, 1, "C-x", COLS - 2, A_NORMAL);
    int cursor_y = editor_top() + row - a->scroll;
    int cursor_x = left + col;
    if (cursor_y >= LINES) cursor_y = LINES - 1;
    if (cursor_x >= COLS) cursor_x = COLS - 1;
    move(cursor_y, cursor_x); refresh(); curs_set(1);
}

static int sidebar_width(void) { return COLS >= 110 ? 19 : 0; }
static int list_width(void) { return COLS < 70 ? COLS : (COLS >= 110 ? 35 : 29); }

static void display_time(const char *stamp, char *out, size_t size, int seconds)
{
    int hour = atoi(stamp + 11);
    const char *period = hour < 12 ? "AM" : "PM";
    int clock_hour = hour % 12 ? hour % 12 : 12;
    if (seconds)
        snprintf(out, size, "%d:%.2s:%.2s %s", clock_hour, stamp + 14, stamp + 17, period);
    else
        snprintf(out, size, "%d:%.2s %s", clock_hour, stamp + 14, period);
}

static NoteGeometry reader_geometry(void)
{
    int left = COLS < 70 ? 0 : sidebar_width() + list_width();
    int right_width = COLS - left;
    int width = right_width > 84 ? 80 : (right_width > 4 ? right_width - 4 : 1);
    return (NoteGeometry){left + (right_width - width) / 2,
        LINES > 7 ? 3 : 1, width,
        LINES > 7 ? LINES - 6 : (LINES > 3 ? LINES - 3 : 1), left};
}

static void browser_focus(NoteApp *a, int target)
{
    int first = sidebar_width() ? 0 : 1;
    if (target < first) target = first;
    if (target > 2) target = 2;
    if (target == a->focus) return;
    if (target == 0) {
        a->sidebar_row = 0;
        for (size_t i = 0; i < a->year_count; i++)
            if (a->years[i] == a->year_filter) a->sidebar_row = (int)i + 1;
    } else if (a->focus == 0 && target == 1) {
        if ((size_t)a->sidebar_row > a->year_count) a->sidebar_row = 0;
        int year = a->sidebar_row ? a->years[a->sidebar_row - 1] : 0;
        if (year != a->year_filter) {
            a->year_filter = year; a->selected = 0; a->list_top = 0;
            rebuild_list(a, NULL);
        }
    }
    a->focus = target; a->mouse.dragging = 0; a->mouse.note_id[0] = 0;
    a->full_redraw = 1;
}

static void scroll_reader(NoteApp *a, int delta)
{
    int maximum = 0;
    if (a->visible_count) {
        NoteGeometry g = reader_geometry();
        maximum = ssr_visual_rows(a->visible[a->selected]->text, g.width) - g.height;
        if (maximum < 0) maximum = 0;
    }
    long next = (long)a->read_scroll + delta;
    a->read_scroll = next < 0 ? 0 : (next > maximum ? maximum : (int)next);
}

static void sync_mouse(NoteApp *a)
{
    int enable = a->view == BROWSE;
    if (enable == a->mouse.enabled) return;
    if (enable) {
        mmask_t events = BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON3_PRESSED |
                        BUTTON4_PRESSED | REPORT_MOUSE_POSITION;
#ifdef BUTTON5_PRESSED
        events |= BUTTON5_PRESSED;
#endif
        mouseinterval(0);
        if (!(mousemask(events, NULL) & BUTTON1_PRESSED)) return;
        /* Report drags rather than every pointer movement. */
        fputs("\033[?1003l\033[?1002h", stdout);
    } else {
        fputs("\033[?1002l", stdout); mousemask(0, NULL);
        a->mouse.dragging = 0;
    }
    fflush(stdout); a->mouse.enabled = enable;
}

static int clipboard_write(const char *text, size_t length, int primary)
{
    char *argv[6] = {0}, *read_argv[7] = {0};
    const char *wayland = getenv("WAYLAND_DISPLAY"), *x11 = getenv("DISPLAY");
    if (wayland && *wayland && ssp_command_available("wl-copy") && ssp_command_available("wl-paste")) {
        argv[0] = "wl-copy"; argv[1] = "--type"; argv[2] = "text/plain";
        if (primary) argv[3] = "--primary";
        read_argv[0] = "wl-paste"; read_argv[1] = "--no-newline";
        read_argv[2] = "--type"; read_argv[3] = "text/plain";
        if (primary) read_argv[4] = "--primary";
    } else if (x11 && *x11 && ssp_command_available("xclip")) {
        argv[0] = "xclip"; argv[1] = "-selection"; argv[2] = primary ? "primary" : "clipboard";
        read_argv[0] = "xclip"; read_argv[1] = "-selection";
        read_argv[2] = argv[2]; read_argv[3] = "-o";
    } else if (x11 && *x11 && ssp_command_available("xsel")) {
        argv[0] = "xsel"; argv[1] = primary ? "--primary" : "--clipboard"; argv[2] = "--input";
        read_argv[0] = "xsel"; read_argv[1] = argv[1]; read_argv[2] = "--output";
#ifdef __APPLE__
    } else if (!primary && ssp_command_available("/usr/bin/pbcopy") && ssp_command_available("/usr/bin/pbpaste")) {
        argv[0] = "/usr/bin/pbcopy";
        read_argv[0] = "/usr/bin/pbpaste";
#endif
    } else return 0;
    char path[] = "/tmp/simplenote-clip-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(path); return -1; }
    int ok = fwrite(text, 1, length, file) == length;
    if (fclose(file)) ok = 0;
    if (ok) ok = ssp_detach_argv_with_input_file(path, argv);
    unlink(path);
    if (!ok) return -1;
    /* A detached helper can fail after its launcher reports success. Confirm
     * ownership by reading back the exact bytes, allowing a bounded startup. */
    int64_t deadline = sui_monotonic_ms() + 1000;
    while (!note_stop) {
        int64_t remaining = deadline - sui_monotonic_ms();
        if (remaining <= 0) break;
        char *copied = NULL;
        int got = ssp_capture_argv(read_argv, &copied, SN_TEXT_LIMIT + 1,
                                   remaining > 150 ? 150 : (int)remaining);
        int matches = got && strlen(copied) == length && !memcmp(copied, text, length);
        free(copied);
        if (matches) return 1;
        (void)poll(NULL, 0, 20);
    }
    return -1;
}

static void copy_note_text(NoteApp *a, int whole)
{
    if (!a->visible_count) return;
    SnNote *note = a->visible[a->selected];
    size_t start = 0, end = note->len;
    if (!whole && !strcmp(a->mouse.note_id, note->id) && a->mouse.start < a->mouse.end) {
        start = a->mouse.start; end = a->mouse.end;
    }
    if (start >= end || end > note->len) return;
    char *copy = strndup(note->text + start, end - start);
    if (!copy) { message(a, "Out of memory"); return; }
    free(a->kill); a->kill = copy;
    a->mouse.start = start; a->mouse.end = end;
    strcpy(a->mouse.note_id, note->id);
    int copied = clipboard_write(copy, end - start, 0);
    if (copied > 0) {
        int primary = clipboard_write(copy, end - start, 1);
        message(a, primary < 0 ? "Text copied to clipboard; primary selection could not be confirmed" :
                   (whole ? "Note text copied" : "Selected note text copied"));
    } else message(a, copied < 0 ? "Clipboard copy could not be confirmed; text retained internally" :
                    "Copied internally; system copy needs wl-copy/wl-paste, xclip, or xsel");
}

/* Map screen cells back into the original bytes, never into rendered padding. */
static size_t reader_position(NoteApp *a, SnNote *note, NoteGeometry g, int y, int x,
                              int *on_glyph)
{
    *on_glyph = 0;
    if (!text_rows(a, note->text, note->len, g.width)) return 0;
    int row = y - g.top;
    if (row < 0) row = 0;
    if (row >= g.height) row = g.height - 1;
    row += a->read_scroll;
    if ((size_t)row >= a->row_count) return note->len;
    NoteRow r = a->rows[row];
    int target = x - g.left;
    if (target < 0) return r.start;
    for (size_t pos = r.start; pos < r.end; ) {
        size_t next = char_next(note->text, pos);
        int width = ssr_visual_col_range(note->text + r.start, (int)(r.end - r.start),
                                         0, (int)(next - r.start));
        if (target < width) { *on_glyph = 1; return pos; }
        pos = next;
    }
    return r.end;
}

static int word_class(const char *text, size_t pos)
{
    wchar_t wc; int used;
    ssr_utf8_decode_n(text + pos, (int)strnlen(text + pos, MB_LEN_MAX), &wc, &used);
    return iswspace(wc) ? 0 : (iswalnum(wc) || wc == '_' ? 1 : 2);
}

static void reader_span(NoteApp *a, SnNote *note, size_t pos, int on_glyph,
                         size_t *start, size_t *end)
{
    *start = *end = pos;
    if (a->mouse.clicks == 3) {
        while (*start && note->text[*start - 1] != '\n') (*start)--;
        while (note->text[*end] && note->text[*end] != '\n') (*end)++;
        if (note->text[*end]) (*end)++;
    } else if (on_glyph) {
        *end = char_next(note->text, pos);
        if (a->mouse.clicks == 2) {
            int kind = word_class(note->text, pos);
            while (*start && note->text[*start - 1] != '\n') {
                size_t prev = char_prev(note->text, *start);
                if (word_class(note->text, prev) != kind) break;
                *start = prev;
            }
            while (note->text[*end] && note->text[*end] != '\n' &&
                   word_class(note->text, *end) == kind) *end = char_next(note->text, *end);
        }
    }
}

static void browser_mouse(NoteApp *a, const MEVENT *event)
{
    NoteGeometry g = reader_geometry();
    mmask_t state = event->bstate;
    int reader = (COLS >= 70 || a->focus == 2) && event->x >= g.pane_left;
    int wheel = state & BUTTON4_PRESSED ? -3 : 0;
#ifdef BUTTON5_PRESSED
    if (state & BUTTON5_PRESSED) wheel = 3;
#endif
    if (wheel) {
        a->mouse.dragging = 0;
        if (reader) a->read_scroll += wheel;
        else if (a->visible_count) {
            long index = (long)a->selected + wheel;
            if (index < 0) index = 0;
            if ((size_t)index >= a->visible_count) index = (long)a->visible_count - 1;
            a->selected = (size_t)index; a->read_scroll = 0;
        }
        return;
    }
    if (a->mouse.dragging && a->visible_count) {
        SnNote *note = a->visible[a->selected];
        if (strcmp(a->mouse.note_id, note->id)) { a->mouse.dragging = 0; return; }
        if (!(state & (BUTTON1_PRESSED | BUTTON1_RELEASED | REPORT_MOUSE_POSITION))) return;
        if (event->x != a->mouse.press_x || event->y != a->mouse.press_y) a->mouse.moved = 1;
        if (a->mouse.moved) {
            if (!(state & BUTTON1_RELEASED)) {
                if (event->y < g.top) a->read_scroll--;
                else if (event->y >= g.top + g.height) a->read_scroll++;
                int maximum = ssr_visual_rows(note->text, g.width) - g.height;
                if (a->read_scroll > maximum) a->read_scroll = maximum;
                if (a->read_scroll < 0) a->read_scroll = 0;
            }
            int on_glyph; size_t start, end;
            size_t pos = reader_position(a, note, g, event->y, event->x, &on_glyph);
            reader_span(a, note, pos, on_glyph, &start, &end);
            a->mouse.start = start < a->mouse.anchor_start ? start : a->mouse.anchor_start;
            a->mouse.end = end > a->mouse.anchor_end ? end : a->mouse.anchor_end;
        }
        if (state & BUTTON1_RELEASED) {
            a->mouse.dragging = 0;
            if (a->mouse.moved || a->mouse.clicks > 1) copy_note_text(a, 0);
        }
        return;
    }
    if (reader && a->visible_count && event->y >= g.top && event->y < g.top + g.height) {
        if (state & BUTTON3_PRESSED) { copy_note_text(a, 0); return; }
        if (state & BUTTON1_PRESSED) {
            SnNote *note = a->visible[a->selected];
            int64_t now = sui_monotonic_ms();
            if (a->mouse.clicked_at && now - a->mouse.clicked_at <= 400 &&
                !strcmp(a->mouse.note_id, note->id) && a->mouse.press_x == event->x &&
                a->mouse.press_y == event->y) a->mouse.clicks = a->mouse.clicks % 3 + 1;
            else a->mouse.clicks = 1;
            a->mouse.clicked_at = now; a->mouse.press_x = event->x; a->mouse.press_y = event->y;
            a->mouse.dragging = 1; a->mouse.moved = 0; strcpy(a->mouse.note_id, note->id);
            int on_glyph;
            size_t pos = reader_position(a, note, g, event->y, event->x, &on_glyph);
            reader_span(a, note, pos, on_glyph, &a->mouse.anchor_start, &a->mouse.anchor_end);
            a->mouse.start = a->mouse.anchor_start;
            a->mouse.end = a->mouse.clicks > 1 ? a->mouse.anchor_end : a->mouse.start;
            a->focus = 2;
        }
    } else if (state & BUTTON1_PRESSED) {
        a->mouse.note_id[0] = 0; a->mouse.clicked_at = 0;
        int side = sidebar_width();
        if (event->x < side && event->y >= 3 && event->y < LINES - 3) {
            int row = a->sidebar_top + event->y - 3;
            if ((size_t)row <= a->year_count) {
                a->sidebar_row = row; a->year_filter = row ? a->years[row - 1] : 0;
                a->selected = 0; rebuild_list(a, NULL); a->focus = 0;
            }
        } else if (event->x >= side && event->x < side + list_width() && event->y >= 2 && event->y < LINES - 2) {
            size_t index = (size_t)(a->list_top + (event->y - 2) / 4);
            if (index < a->visible_count) { a->selected = index; a->read_scroll = 0; a->focus = 1; }
        }
    }
}

static void draw_browser_list(NoteApp *a, int side, int list)
{
    int available = LINES - 4, slots = available / 4;
    if (slots < 1) slots = 1;
    if ((int)a->selected < a->list_top) a->list_top = (int)a->selected;
    if ((int)a->selected >= a->list_top + slots) a->list_top = (int)a->selected - slots + 1;
    if (a->list_top < 0) a->list_top = 0;
    for (int y = 0; y < LINES; y++) clear_line(y, 0, side + list, A_NORMAL);
    if (side) {
        clipped(0, 2, "simplenote", side - 3, A_BOLD);
        int year_slots = LINES > 7 ? LINES - 6 : 1;
        if (a->sidebar_row < a->sidebar_top) a->sidebar_top = a->sidebar_row;
        if (a->sidebar_row >= a->sidebar_top + year_slots) a->sidebar_top = a->sidebar_row - year_slots + 1;
        for (size_t i = (size_t)a->sidebar_top; i <= a->year_count && (int)i - a->sidebar_top < year_slots; i++) {
            int y = (int)i - a->sidebar_top + 3, yr = i ? a->years[i - 1] : 0;
            int active = a->focus == 0 ? a->sidebar_row == (int)i : a->year_filter == yr;
            attr_t attr = active ? highlight(a) : A_NORMAL;
            char label[64];
            if (yr) snprintf(label, sizeof label, "%d  %zu", yr, year_count(a, yr));
            else snprintf(label, sizeof label, "All notes  %zu", year_count(a, 0));
            clear_line(y, 1, side - 2, attr); clipped(y, 2, label, side - 4, attr);
        }
        clipped(LINES - 3, 2, a->trash ? "Trash (open)" : "Trash", side - 3, a->trash ? A_BOLD : A_DIM);
        for (int y = 0; y < LINES - 1; y++) { attrset(A_DIM); mvaddch(y, side - 1, ACS_VLINE); }
    }
    char heading[128];
    if (a->search[0]) snprintf(heading, sizeof heading, "Search: %.100s", a->search);
    else if (a->trash) snprintf(heading, sizeof heading, "Trash - %zu", a->visible_count);
    else if (a->year_filter) snprintf(heading, sizeof heading, "%d - %zu notes", a->year_filter, a->visible_count);
    else snprintf(heading, sizeof heading, "Notes - %zu", a->visible_count);
    clipped(0, side + 1, heading, list - 2, A_BOLD);
    if (!a->visible_count) clipped(3, side + 1, "No notes. Press n to write.", list - 2, A_DIM);
    for (int i = 0; i < slots && (size_t)(a->list_top + i) < a->visible_count; i++) {
        size_t index = (size_t)(a->list_top + i);
        SnNote *note = a->visible[index];
        int y = 2 + i * 4;
        attr_t attr = index == a->selected && a->focus != 0 ? highlight(a) : A_NORMAL;
        char date[80], clock[24], summary[512];
        display_time(note->created, clock, sizeof clock, 0);
        snprintf(date, sizeof date, "%.10s  %s", note->created, clock);
        preview(note->text, summary, sizeof summary);
        for (int row = y; row < y + 3; row++) clear_line(row, side, list - 1, attr);
        clipped(y, side + 1, date, list - 3, attr);
        int end, next;
        ssr_wrap_segment(summary, (int)strlen(summary), 0, list - 3, &end, &next);
        char saved = summary[end]; summary[end] = 0;
        clipped(y + 1, side + 1, summary, list - 3, attr);
        summary[end] = saved;
        if (summary[next]) clipped(y + 2, side + 1, summary + next, list - 3, attr);
    }
}

static void draw_browser(NoteApp *a)
{
    curs_set(0);
    int side = sidebar_width(), list = list_width(), left = side + list;
    if (!side && a->focus == 0) a->focus = 1;
    int show_reader = COLS >= 70 || a->focus == 2;
    NoteGeometry g = reader_geometry();
    if (COLS < 70 && show_reader) {
        left = 0;
        if (!ssr_geometry_matches(&a->renderer, g.top, g.left, g.height, g.width)) {
            erase(); ssr_invalidate(&a->renderer);
        }
    } else draw_browser_list(a, side, list);

    /* The body renderer presents the screen. Finish the footer first so no
     * intermediate frame exposes the blanks left by rebuilding the list. */
    clear_line(LINES - 1, 0, COLS, A_REVERSE);
    const char *help = a->focus == 2 ?
        "Left/Right Panes   Up/Down Scroll   PgUp/PgDn Page   c Copy   e Edit   ? Help   q Quit" :
        (a->trash ? "Left/Right Panes   n New   r Restore   c Copy   / Search   t Notes   ? Help   q Quit" :
        "Left/Right Panes   n New   e Edit   c Copy   d Trash   / Search   ? Help   q Quit");
    clipped(LINES - 1, 1, a->status[0] ? a->status : help, COLS - 2, A_REVERSE);

    int presented = 0;
    if (show_reader) {
        if (COLS >= 70) for (int y = 0; y < LINES - 1; y++) { attrset(A_DIM); mvaddch(y, left - 1, ACS_VLINE); }
        int right_width = COLS - left;
        clear_line(0, left, right_width, A_NORMAL);
        clear_line(1, left, right_width, A_NORMAL);
        clear_line(LINES - 2, left, right_width, A_NORMAL);
        const char *text = "";
        if (a->visible_count) {
            SnNote *note = a->visible[a->selected];
            char date[128], clock[24];
            display_time(note->created, clock, sizeof clock, 1);
            snprintf(date, sizeof date, "%.10s  %s  %s", note->created,
                     clock, a->trash ? "Trash" : "");
            attr_t date_attr = A_BOLD | (a->focus == 2
                ? (a->colors ? COLOR_PAIR(2) : A_UNDERLINE) : A_NORMAL);
            clipped(0, left + 2, date, right_width - 4, date_attr);
            text = note->text;
        }
        int max_scroll = ssr_visual_rows(text, g.width) - g.height;
        if (a->read_scroll > max_scroll) a->read_scroll = max_scroll;
        if (a->read_scroll < 0) a->read_scroll = 0;
        if (a->visible_count && !strcmp(a->mouse.note_id, a->visible[a->selected]->id) &&
            a->mouse.start < a->mouse.end) {
            SsrSpan span = {a->mouse.start, a->mouse.end, A_REVERSE};
            presented = ssr_render_text_spans(&a->renderer, text, a->read_scroll, g.top, g.left,
                                             g.height, g.width, A_NORMAL, &span, 1);
        } else presented = ssr_render_text(&a->renderer, text, a->read_scroll, g.top, g.left, g.height, g.width, A_NORMAL);
    } else ssr_deactivate(&a->renderer);
    attrset(A_NORMAL);
    if (!presented) refresh();
}

static void draw_help(NoteApp *a)
{
    static const char *help =
        "simplenote\n\n"
        "Start with a blank page. Notes autosave while you write.\n"
        "Ctrl-X Ctrl-S saves and opens the journal browser.\n\n"
        "Writing\n"
        "  Arrows / Home / End       Move; Ctrl-A / Ctrl-E line start / end\n"
        "  Ctrl-B / F / P / N        Left / right / up / down\n"
        "  Shift-arrows, Ctrl-Space  Select text\n"
        "  Alt-B / Alt-F             Previous / next word\n"
        "  Ctrl-W / Alt-W / Ctrl-Y   Cut / copy / paste selection\n"
        "  Ctrl-K                    Cut rest of line\n"
        "  Ctrl-_ or Ctrl-X u        Undo\n"
        "  F2 / Ctrl-S               Save and open browser\n"
        "  Esc / Ctrl-X Ctrl-C       Discard writing and open browser\n"
        "                            Existing notes keep their saved version\n\n"
        "Browser\n"
        "  Left / Right, h / l      Move between panes\n"
        "  Up / Down, j / k         Select a year or note; scroll in the reader\n"
        "  Enter / e                Edit selected note\n"
        "  n                         New blank note\n"
        "  /                         Search text; empty search clears filter\n"
        "  a                         All years\n"
        "  y                         Choose a year (also on narrow terminals)\n"
        "  d                         Move note to trash (confirm with y)\n"
        "  t / r                     Open trash / restore selected note\n"
        "  Right from a year        Open that year's notes\n"
        "  Page Up / Page Down      Jump one page in the current pane\n"
        "  Mouse drag                Select and copy only note text\n"
        "  Double / triple click     Copy a word / logical line\n"
        "  c                         Copy the whole note without its date\n"
        "  q / Ctrl-X Ctrl-C        Quit\n\n"
        "Storage\n"
        "  ~/writing/notes/YYYY-MM-DD-NNN.txt holds up to 100 notes.\n"
        "  Edits keep a note in its original file. Trash is recoverable.\n"
        "  On the first run of a new year, older files move into YYYY/.\n"
        "  .draft recovers interrupted writing. Only one session writes at a time.\n\n"
        "Press any key to return.";
    erase(); ssr_invalidate(&a->renderer);
    ssr_render_text(&a->renderer, help, a->read_scroll, 1, 2,
                     LINES > 2 ? LINES - 2 : 1, COLS > 4 ? COLS - 4 : 1, A_NORMAL);
}

static void draw(NoteApp *a)
{
    if (a->full_redraw) { erase(); ssr_invalidate(&a->renderer); a->full_redraw = 0; }
    if (a->view == COMPOSE) draw_editor(a);
    else if (a->view == BROWSE) draw_browser(a);
    else draw_help(a);
}

static char *read_terminal_paste(NoteApp *a, size_t limit, size_t *length);
static void paste_terminal(NoteApp *a);

static int prompt(NoteApp *a, const char *label, char *out, size_t size)
{
    size_t len = strlen(out);
    wtimeout(stdscr, -1);
    for (;;) {
        if (note_stop) break;
        clear_line(LINES - 1, 0, COLS, A_REVERSE);
        clipped(LINES - 1, 1, label, COLS - 2, A_REVERSE);
        int offset = (int)strlen(label) + 1;
        clipped(LINES - 1, offset, out, COLS - offset - 1, A_REVERSE);
        int col = offset + ssr_visual_col_range(out, (int)len, 0, (int)len);
        if (col >= COLS) col = COLS - 1;
        move(LINES - 1, col); refresh(); curs_set(1);
        wint_t ch;
        int rc = get_wch(&ch);
        if (rc == ERR) { if (note_stop) break; continue; }
        if (rc == KEY_CODE_YES && ch == SN_PASTE) {
            size_t length;
            char *text = read_terminal_paste(a, size - len - 1, &length);
            if (text) {
                for (size_t i = 0; i < length; i++) {
                    unsigned char c = (unsigned char)text[i];
                    if (c == '\r' || c == '\n' || c == '\t') {
                        out[len++] = ' ';
                        if (c == '\r' && i + 1 < length && text[i + 1] == '\n') i++;
                    } else if (c >= 32 && c != 127) out[len++] = (char)c;
                }
                out[len] = 0; free(text);
            }
            wtimeout(stdscr, -1);
            continue;
        }
        if (ch == '\n' || ch == '\r') { wtimeout(stdscr, 100); return 1; }
        if (ch == 27) break;
        if (rc == KEY_CODE_YES && ch == KEY_RESIZE) { a->full_redraw = 1; draw(a); continue; }
        if ((rc == KEY_CODE_YES && ch == KEY_BACKSPACE) || ch == 127 || ch == CTRL('H')) {
            len = char_prev(out, len); out[len] = 0;
        } else if (ch == CTRL('U')) { len = 0; out[0] = 0; }
        else if (rc == OK && ch >= 32 && ch != 127) {
            char bytes[MB_LEN_MAX]; mbstate_t state = {0};
            size_t used = wcrtomb(bytes, (wchar_t)ch, &state);
            if (used != (size_t)-1 && len + used < size) {
                memcpy(out + len, bytes, used); len += used; out[len] = 0;
            }
        }
    }
    wtimeout(stdscr, 100); return 0;
}

static void trash_note(NoteApp *a, int restore)
{
    if (!a->visible_count) return;
    SnNote *note = a->visible[a->selected];
    if (!restore) {
        char answer[8] = "";
        if (!prompt(a, "Move note to trash? y / n: ", answer, sizeof answer) ||
            (strcmp(answer, "y") && strcmp(answer, "Y"))) return;
    }
    char updated[25]; sn_timestamp(updated);
    if (!sn_store_put(&a->store, note->id, note->created, updated, note->text, !restore, NULL))
        message(a, a->store.error);
    else { rebuild_list(a, NULL); message(a, restore ? "Note restored" : "Moved to trash - t opens trash"); }
    a->full_redraw = 1;
}

static void word_move(NoteApp *a, int direction)
{
    size_t pos = a->cursor;
    if (direction < 0) {
        while (pos && isspace((unsigned char)a->edit.text[pos - 1])) pos = char_prev(a->edit.text, pos);
        while (pos && !isspace((unsigned char)a->edit.text[pos - 1])) pos = char_prev(a->edit.text, pos);
    } else {
        while (pos < a->edit.len && !isspace((unsigned char)a->edit.text[pos])) pos = char_next(a->edit.text, pos);
        while (pos < a->edit.len && isspace((unsigned char)a->edit.text[pos])) pos = char_next(a->edit.text, pos);
    }
    a->cursor = pos; a->preferred_col = -1;
}

static void copy_range(NoteApp *a, size_t start, size_t end)
{
    char *text = malloc(end - start + 1);
    if (!text) { message(a, "Out of memory"); return; }
    memcpy(text, a->edit.text + start, end - start); text[end - start] = 0;
    free(a->kill); a->kill = text;
}

static void paste_byte(char **text, size_t *len, size_t *capacity,
                       size_t limit, int *error, int ch)
{
    if (!limit || *error || !ch) return;
    if (*len == limit) { *error = 1; return; }
    if (*len + 1 >= *capacity) {
        size_t next = *capacity ? *capacity * 2 : 1024;
        if (next > limit + 1) next = limit + 1;
        char *p = realloc(*text, next);
        if (!p) { *error = 2; return; }
        *text = p; *capacity = next;
    }
    (*text)[(*len)++] = (char)ch;
}

/* Consume the whole bracketed paste in every view, even after overflow or
 * allocation failure. Payload bytes must never escape into key bindings. */
static char *read_terminal_paste(NoteApp *a, size_t limit, size_t *length)
{
    static const char delimiter[] = "\033[201~";
    char *text = NULL;
    size_t len = 0, capacity = 0, matched = 0;
    int error = 0;
    *length = 0;
    keypad(stdscr, FALSE); nonl(); wtimeout(stdscr, 100);
    while (!note_stop) {
        int ch = getch();
        if (ch == ERR) {
            struct pollfd terminal = {STDIN_FILENO, POLLIN, 0};
            if (poll(&terminal, 1, 0) > 0 && (terminal.revents & (POLLHUP | POLLERR | POLLNVAL)))
                note_stop = SIGHUP;
            autosave(a, sui_monotonic_ms());
            continue;
        }
        if (ch == KEY_RESIZE) { a->full_redraw = 1; continue; }
        if (ch < 0 || ch > UCHAR_MAX) continue;
        if (ch == delimiter[matched]) {
            if (++matched == sizeof delimiter - 1) { matched = 0; break; }
        } else {
            for (size_t i = 0; i < matched; i++)
                paste_byte(&text, &len, &capacity, limit, &error, delimiter[i]);
            matched = 0;
            if (ch == delimiter[0]) matched = 1;
            else paste_byte(&text, &len, &capacity, limit, &error, ch);
        }
    }
    keypad(stdscr, TRUE); nl();
    for (size_t i = 0; i < matched; i++)
        paste_byte(&text, &len, &capacity, limit, &error, delimiter[i]);
    if (error) {
        free(text);
        message(a, error == 1 ? "Paste is too long; text unchanged" : "Out of memory; paste ignored");
        return NULL;
    }
    if (text) text[len] = 0;
    *length = len;
    return text;
}

static void paste_terminal(NoteApp *a)
{
    size_t len;
    char *text = read_terminal_paste(a, SN_TEXT_LIMIT, &len);
    if (text) {
        size_t clean = 0;
        for (size_t i = 0; i < len; i++) {
            if (text[i] == '\r') { text[clean++] = '\n'; if (i + 1 < len && text[i + 1] == '\n') i++; }
            else text[clean++] = text[i];
        }
        text[clean] = 0;
        insert_text(a, text, clean);
    }
    free(text);
}

static void compose_key(NoteApp *a, wint_t ch, int keycode)
{
    /* Wide characters can have the same numeric value as ncurses key codes. */
    if (!keycode && ch >= 128) {
        char bytes[MB_LEN_MAX]; mbstate_t state = {0};
        size_t len = wcrtomb(bytes, (wchar_t)ch, &state);
        if (len != (size_t)-1) insert_text(a, bytes, len);
        return;
    }
    int extend = ch == KEY_SLEFT || ch == KEY_SRIGHT || ch == KEY_SR || ch == KEY_SF;
    if (extend && !a->selection) { a->anchor = a->cursor; a->selection = 1; }
    if (ch == 27) { discard_edit(a); return; }
    if (ch == CTRL('S') || ch == KEY_F(2)) { show_browser(a); return; }
    if (ch == 0) {
        a->selection = !a->selection; a->anchor = a->cursor; return;
    }
    if (ch == CTRL('_')) { undo_edit(a); return; }
    if (ch == KEY_UP || ch == CTRL('P') || ch == KEY_SR) { cursor_vertical(a, -1); return; }
    if (ch == KEY_DOWN || ch == CTRL('N') || ch == KEY_SF) { cursor_vertical(a, 1); return; }
    if (ch == KEY_PPAGE || ch == KEY_NPAGE) {
        cursor_vertical(a, ch == KEY_PPAGE ? -editor_height() : editor_height()); return;
    }
    a->preferred_col = -1;
    if (ch == KEY_LEFT || ch == CTRL('B') || ch == KEY_SLEFT) a->cursor = char_prev(a->edit.text, a->cursor);
    else if (ch == KEY_RIGHT || ch == CTRL('F') || ch == KEY_SRIGHT) a->cursor = char_next(a->edit.text, a->cursor);
    else if (ch == KEY_HOME || ch == CTRL('A')) { while (a->cursor && a->edit.text[a->cursor - 1] != '\n') a->cursor--; }
    else if (ch == KEY_END || ch == CTRL('E')) { while (a->edit.text[a->cursor] && a->edit.text[a->cursor] != '\n') a->cursor++; }
    else if (ch == KEY_BACKSPACE || ch == 127 || ch == CTRL('H')) {
        if (a->selection && a->cursor != a->anchor) insert_text(a, "", 0);
        else if (a->cursor) replace_range(a, char_prev(a->edit.text, a->cursor), a->cursor, "", 0);
    } else if (ch == KEY_DC || ch == CTRL('D')) {
        if (a->selection && a->cursor != a->anchor) insert_text(a, "", 0);
        else if (a->cursor < a->edit.len) replace_range(a, a->cursor, char_next(a->edit.text, a->cursor), "", 0);
    } else if (ch == CTRL('K')) {
        size_t end = a->cursor;
        while (a->edit.text[end] && a->edit.text[end] != '\n') end++;
        if (end == a->cursor && end < a->edit.len) end++;
        copy_range(a, a->cursor, end); replace_range(a, a->cursor, end, "", 0);
    } else if (ch == CTRL('W')) {
        size_t end = a->cursor, start = a->anchor;
        if (!a->selection) { word_move(a, -1); start = a->cursor; a->cursor = end; }
        if (start > end) { size_t swap = start; start = end; end = swap; }
        copy_range(a, start, end); replace_range(a, start, end, "", 0);
    } else if (ch == CTRL('Y')) { if (a->kill) insert_text(a, a->kill, strlen(a->kill)); }
    else if (ch == '\n' || ch == '\r') insert_text(a, "\n", 1);
    else if (ch == '\t') insert_text(a, "\t", 1);
    else if (!keycode && ch >= 32 && ch != 127) {
        char bytes[MB_LEN_MAX]; mbstate_t state = {0};
        size_t len = wcrtomb(bytes, (wchar_t)ch, &state);
        if (len != (size_t)-1) insert_text(a, bytes, len);
    }
}

static void browser_key(NoteApp *a, wint_t ch, int keycode)
{
    if (!keycode && ch >= 128) return;
    a->mouse.dragging = 0;
    if (ch == 'c') { copy_note_text(a, 1); return; }
    if (ch == 'n' || ch == CTRL('N')) { a->trash = 0; open_editor(a, NULL); return; }
    if (ch == KEY_LEFT || ch == 'h') { browser_focus(a, a->focus - 1); return; }
    if (ch == KEY_RIGHT || ch == 'l') { browser_focus(a, a->focus + 1); return; }
    if (ch == '/' || ch == CTRL('S')) {
        char query[sizeof a->search]; strcpy(query, a->search);
        if (prompt(a, "Search: ", query, sizeof query)) {
            strcpy(a->search, query); a->selected = 0; a->list_top = 0; rebuild_list(a, NULL);
        }
        a->full_redraw = 1; return;
    }
    if (ch == 'a') { a->year_filter = 0; a->search[0] = 0; a->selected = 0; rebuild_list(a, NULL); return; }
    if (ch == 'y') {
        char year[16] = "";
        if (prompt(a, "Year (blank = all): ", year, sizeof year)) {
            if (year[0] && (strlen(year) != 4 || strspn(year, "0123456789") != 4 || atoi(year) < 1900))
                message(a, "Enter a four-digit year or leave it blank");
            else { a->year_filter = atoi(year); a->selected = 0; rebuild_list(a, NULL); }
        }
        a->full_redraw = 1; return;
    }
    if (ch == 't') { a->trash = !a->trash; a->selected = 0; rebuild_list(a, NULL); return; }
    if (ch == 'd' && !a->trash) { trash_note(a, 0); return; }
    if (ch == 'r' && a->trash) { trash_note(a, 1); return; }
    if ((ch == '\n' || ch == '\r') && a->focus == 0) {
        a->year_filter = a->sidebar_row ? a->years[a->sidebar_row - 1] : 0;
        a->selected = 0; rebuild_list(a, NULL); a->focus = 1; return;
    }
    if ((ch == '\n' || ch == '\r' || ch == 'e') && a->visible_count) {
        if (a->trash) message(a, "Restore this note with r before editing");
        else open_editor(a, a->visible[a->selected]);
        return;
    }
    int delta = ch == KEY_UP || ch == 'k' ? -1 : (ch == KEY_DOWN || ch == 'j' ? 1 : 0);
    if (ch == KEY_PPAGE || ch == KEY_NPAGE) {
        int page = a->focus == 2 ? reader_geometry().height :
            (a->focus == 0 ? LINES - 6 : (LINES - 4) / 4);
        if (page < 1) page = 1;
        delta = ch == KEY_PPAGE ? -page : page;
    }
    if (a->focus == 0 && delta) {
        a->sidebar_row += delta;
        if (a->sidebar_row < 0) a->sidebar_row = 0;
        if ((size_t)a->sidebar_row > a->year_count) a->sidebar_row = (int)a->year_count;
    } else if (a->focus == 2 && delta) scroll_reader(a, delta);
    else if (delta && a->visible_count) {
        long next = (long)a->selected + delta;
        if (next < 0) next = 0;
        if ((size_t)next >= a->visible_count) next = (long)a->visible_count - 1;
        a->selected = (size_t)next;
        a->read_scroll = 0;
    }
}

static void usage(void)
{
    puts("simplenote - a blank page and a journal browser\n"
         "  simplenote                 write a new note\n"
         "  simplenote --browse        browse saved notes\n"
         "  simplenote --list          print note IDs, dates, and previews\n"
         "  simplenote --data-dir DIR  use a different notes directory\n"
         "  simplenote --version\n\n"
         "Ctrl-X Ctrl-S saves and opens the browser. Esc or Ctrl-X Ctrl-C discards\n"
         "writing and opens the browser; when editing an old note, its saved version returns.\n"
         "In the browser, q or Ctrl-X Ctrl-C quits.\n"
         "Left/Right moves between panes; Up/Down scrolls in the reading pane,\n"
         "and Page Up/Page Down jumps by the visible page height.\n"
         "Drag in the reading pane to copy only note text; c copies the whole note.\n"
         "Notes autosave to ~/writing/notes; F1 shows all keys.\n"
         "Each dated UTF-8 text file holds up to 100 notes. Older years are archived\n"
         "into YYYY/ on the first run in a new year. Editing keeps the original file.");
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    NoteApp a = {0}; a.store.lock_fd = -1; a.preferred_col = -1;
    const char *dir = NULL;
    char default_dir[PATH_MAX];
    int browse = 0, list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        if (!strcmp(argv[i], "--version")) { puts("simplenote " SN_VERSION); return 0; }
        if (!strcmp(argv[i], "--browse")) browse = 1;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) dir = argv[++i];
        else { fprintf(stderr, "simplenote: unknown or incomplete option: %s\n", argv[i]); return 2; }
    }
    if (!dir) {
        const char *home = getenv("HOME");
        if (!home || !*home || snprintf(default_dir, sizeof default_dir, "%s/writing/notes", home) >= (int)sizeof default_dir) {
            fputs("simplenote: HOME is missing or too long\n", stderr); return 1;
        }
        dir = default_dir;
    }
    if (!sn_store_open(&a.store, dir, current_year())) {
        fprintf(stderr, "simplenote: %s\n", a.store.error); sn_store_close(&a.store); return 1;
    }
    if (!rebuild_list(&a, NULL)) { sn_store_close(&a.store); return 1; }
    if (list) {
        for (size_t i = 0; i < a.visible_count; i++) {
            char summary[256]; preview(a.visible[i]->text, summary, sizeof summary);
            printf("%s\t%s\t%s\n", a.visible[i]->id, a.visible[i]->created, summary);
        }
        free(a.visible); free(a.years); sn_store_close(&a.store); return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("simplenote: an interactive terminal is required (or use --list)\n", stderr);
        free(a.visible); free(a.years); sn_store_close(&a.store); return 1;
    }
    if (!open_editor(&a, NULL)) { sn_store_close(&a.store); return 1; }
    SnNote *draft = NULL, *original = NULL;
    int baseline_known;
    if (!sn_load_edit_draft(&a.store, &draft, &original, &baseline_known)) {
        fprintf(stderr, "simplenote: %s\n", a.store.error); free(a.edit.text);
        free(a.visible); free(a.years); sn_store_close(&a.store); return 1;
    }
    if (draft) {
        SnNote *saved = sn_find(&a.store, draft->id);
        if (!baseline_known && saved) {
            original = copy_note(saved);
            if (!original) {
                fputs("simplenote: out of memory\n", stderr); sn_free_note(draft);
                free(a.edit.text); free(a.visible); free(a.years); sn_store_close(&a.store); return 1;
            }
        }
        a.original = original;
        free(a.edit.text); a.edit = *draft; free(draft);
        a.cursor = a.edit.len; a.dirty = a.draft_dirty = 1;
        a.changed_at = a.dirty_since = a.draft_since = sui_monotonic_ms();
        message(&a, "Recovered interrupted note");
    } else if (browse) { a.view = BROWSE; a.focus = 1; }
    struct sigaction sa = {0}; sa.sa_handler = note_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    initscr(); raw(); noecho(); keypad(stdscr, TRUE); wtimeout(stdscr, 100);
    set_escdelay(SUI_ESCAPE_DELAY_MS);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(1, COLOR_BLACK, COLOR_CYAN);
        init_pair(2, COLOR_CYAN, -1);
        a.colors = 1;
    }
    define_key("\033[200~", SN_PASTE);
    define_key("\033[1;2A", KEY_SR); define_key("\033[1;2B", KEY_SF);
    define_key("\033[1;2D", KEY_SLEFT); define_key("\033[1;2C", KEY_SRIGHT);
    fputs("\033[?2004h", stdout); fflush(stdout);
    ssr_init(&a.renderer); a.full_redraw = 1;
    int running = 1, redraw = 1, result = 0, recovery_saved = 0;
    while (running) {
        int64_t now = sui_monotonic_ms();
        if (note_stop) {
            if (!save_edit(&a) || !sn_clear_draft(&a.store)) {
                recovery_saved = sn_save_edit_draft(&a.store, &a.edit, a.original); result = 1;
            }
            break;
        }
        if (a.status[0] && now >= a.status_until) { a.status[0] = 0; redraw = 1; }
        if (now - a.last_rollover >= 60000) {
            if (current_year() != a.store.year && !sn_rollover(&a.store, current_year())) message(&a, a.store.error);
            a.last_rollover = now;
        }
        if (autosave(&a, now)) redraw = 1;
        sync_mouse(&a);
        if (redraw) { draw(&a); redraw = 0; }
        wint_t ch;
        int rc = get_wch(&ch);
        if (rc == ERR) {
            struct pollfd terminal = {STDIN_FILENO, POLLIN, 0};
            if (poll(&terminal, 1, 0) > 0 && (terminal.revents & (POLLHUP | POLLERR | POLLNVAL)))
                note_stop = SIGHUP;
            continue;
        }
        redraw = 1;
        if (ch == KEY_RESIZE && rc == KEY_CODE_YES) { a.full_redraw = 1; a.mouse.dragging = 0; continue; }
        if (ch == KEY_MOUSE && rc == KEY_CODE_YES) {
            MEVENT event;
            if (getmouse(&event) == OK && a.view == BROWSE) browser_mouse(&a, &event);
            continue;
        }
        if (ch == SN_PASTE && rc == KEY_CODE_YES) {
            a.control_x = 0;
            if (a.view == COMPOSE) paste_terminal(&a);
            else {
                size_t ignored;
                free(read_terminal_paste(&a, 0, &ignored));
                message(&a, "Paste ignored here; open a note or Search to paste text");
            }
            continue;
        }
        if (a.view == HELP) {
            if (ch == KEY_NPAGE || ch == KEY_PPAGE) {
                a.read_scroll += ch == KEY_NPAGE ? LINES - 3 : -(LINES - 3);
                if (a.read_scroll < 0) a.read_scroll = 0;
            } else { a.view = a.before_help; a.full_redraw = 1; a.read_scroll = 0; }
            continue;
        }
        if (a.control_x) {
            a.control_x = 0;
            if (ch == CTRL('S') || ch == 's') { if (a.view == COMPOSE) show_browser(&a); }
            else if (ch == CTRL('C') || ch == 'c') {
                if (a.view == COMPOSE) discard_edit(&a);
                else running = 0;
            }
            else if (ch == 'u' && a.view == COMPOSE) undo_edit(&a);
            else if (ch == 'b') show_browser(&a);
            continue;
        }
        if (ch == CTRL('X')) { a.control_x = 1; continue; }
        if ((rc == KEY_CODE_YES && ch == KEY_F(1)) || (a.view == BROWSE && ch == '?')) {
            a.before_help = a.view; a.view = HELP; a.full_redraw = 1; a.read_scroll = 0; continue;
        }
        if (a.view == BROWSE && (ch == 'q' || ch == CTRL('C'))) { running = 0; continue; }
        if (a.view == COMPOSE && ch == 27) {
            wint_t following;
            wtimeout(stdscr, SUI_ESCAPE_DELAY_MS);
            int next_rc = get_wch(&following);
            wtimeout(stdscr, 100);
            if (next_rc != ERR) {
                if (following == 'b' || following == 'f') word_move(&a, following == 'b' ? -1 : 1);
                else if (following == 'w' && a.selection) {
                    size_t start = a.cursor < a.anchor ? a.cursor : a.anchor;
                    size_t end = a.cursor > a.anchor ? a.cursor : a.anchor;
                    copy_range(&a, start, end); message(&a, "Selection copied");
                } else unget_wch(following);
                continue;
            }
        }
        if (a.view == COMPOSE) compose_key(&a, ch, rc == KEY_CODE_YES);
        else browser_key(&a, ch, rc == KEY_CODE_YES);
    }
    a.view = COMPOSE; sync_mouse(&a);
    ssr_destroy(&a.renderer); endwin();
    fputs("\033[?2004l", stdout); fflush(stdout);
    if (result) fprintf(stderr, "simplenote: %s%s\n", a.store.error, recovery_saved ? " (recovery file retained)" : "");
    undo_clear(&a); free(a.edit.text); sn_free_note(a.original);
    free(a.kill); free(a.rows); free(a.visible); free(a.years);
    sn_store_close(&a.store);
    return result;
}
