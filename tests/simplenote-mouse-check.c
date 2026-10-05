#define main simplenote_program_main
#include "../simplenote.c"
#undef main
#include <assert.h>

static void mouse(NoteApp *a, mmask_t state, int y, int x)
{
    MEVENT event = {.y = y, .x = x, .bstate = state};
    browser_mouse(a, &event);
}

static void check_highlight(NoteApp *a, const char *text, int columns)
{
    assert(resizeterm(32, columns) == OK);
    erase(); ssr_invalidate(&a->renderer);
    a->view = BROWSE; a->focus = 2; a->read_scroll = 0;
    a->mouse.clicked_at = 0; a->mouse.note_id[0] = 0;
    draw_browser(a);
    NoteGeometry g = reader_geometry();
    chtype header_attr = mvinch(0, g.pane_left + 2) & A_ATTRIBUTES;
    mouse(a, BUTTON1_PRESSED, g.top, g.pane_left);
    mouse(a, REPORT_MOUSE_POSITION, g.top + g.height - 1, COLS - 1);
    mouse(a, BUTTON1_RELEASED, g.top + g.height - 1, COLS - 1);
    assert(a->kill && !strcmp(a->kill, text));
    draw_browser(a);
    /* The pane heading keeps its focus style; selection stays inside the body. */
    assert((mvinch(0, g.pane_left + 2) & A_ATTRIBUTES) == header_attr);
    for (int y = 1; y < LINES - 1; y++) for (int x = g.pane_left; x < COLS; x++) {
        if (y < g.top || y >= g.top + g.height || x < g.left || x >= g.left + g.width)
            assert(!(mvinch(y, x) & A_REVERSE));
    }
    assert(mvinch(g.top, g.left + 4) & A_REVERSE);
    int last = g.top + ssr_visual_rows(text, g.width);
    for (int y = last; y < g.top + g.height; y++)
        for (int x = g.left; x < g.left + g.width; x++) assert(!(mvinch(y, x) & A_REVERSE));
    int line_end = ssr_visual_col_range(text, (int)strcspn(text, "\n"), 0, (int)strcspn(text, "\n"));
    if (line_end < g.width)
            for (int x = g.left + line_end; x < g.left + g.width; x++) assert(!(mvinch(g.top, x) & A_REVERSE));
}

static void check_selection_and_scroll(NoteApp *a)
{
    const char *id = "00000000000000000000000000000002";
    const char *stamp = "2026-10-05T12:00:00-0400";
    char text[2048]; size_t length = 0;
    for (int i = 0; i < 100; i++)
        length += (size_t)snprintf(text + length, sizeof text - length, "Scroll row %03d%s", i, i == 99 ? "" : "\n");
    assert(sn_store_put(&a->store, id, stamp, stamp, text, 0, NULL));
    const char *old_id = "00000000000000000000000000000003";
    const char *old_stamp = "2025-10-05T12:00:00-0400";
    assert(sn_store_put(&a->store, old_id, old_stamp, old_stamp, "An archived note", 0, NULL));
    assert(rebuild_list(a, id));
    assert(resizeterm(32, 140) == OK); a->focus = 1; a->read_scroll = 0;
    draw_browser(a);
    assert(curs_set(0) == 0);
    assert(mvinch(2, sidebar_width() + 1) & A_REVERSE);
    assert(mvinch(3, 2) & A_REVERSE);

    browser_key(a, KEY_LEFT, 1); draw_browser(a);
    assert(curs_set(0) == 0);
    assert(mvinch(3, 2) & A_REVERSE);
    assert(!(mvinch(2, sidebar_width() + 1) & A_REVERSE));
    browser_key(a, KEY_DOWN, 1); browser_key(a, KEY_DOWN, 1);
    browser_key(a, KEY_RIGHT, 1);
    assert(a->year_filter == 2025 && a->visible_count == 1 && !strcmp(a->visible[0]->id, old_id));
    draw_browser(a);
    assert(curs_set(0) == 0);
    assert(mvinch(5, 2) & A_REVERSE);
    assert(mvinch(2, sidebar_width() + 1) & A_REVERSE);
    browser_key(a, KEY_LEFT, 1); browser_key(a, KEY_UP, 1); browser_key(a, KEY_RIGHT, 1);
    assert(a->year_filter == 2026 && a->visible_count == 2 && !strcmp(a->visible[0]->id, id));
    browser_key(a, KEY_RIGHT, 1); draw_browser(a);
    NoteGeometry g = reader_geometry();
    assert(curs_set(0) == 0);
    assert(mvinch(4, 2) & A_REVERSE);
    assert(mvinch(2, sidebar_width() + 1) & A_REVERSE);
    assert(!(mvinch(5, 2) & A_REVERSE));
    assert(!(mvinch(6, sidebar_width() + 1) & A_REVERSE));
    size_t selected = a->selected;
    browser_key(a, KEY_DOWN, 1); draw_browser(a);
    char row[32]; assert(mvwinnstr(curscr, g.top, g.left, row, 14) != ERR);
    assert(!strcmp(row, "Scroll row 001") && a->selected == selected);
    browser_key(a, KEY_NPAGE, 1); draw_browser(a);
    assert(mvwinnstr(curscr, g.top, g.left, row, 14) != ERR);
    assert(!strcmp(row, "Scroll row 027"));
    browser_key(a, KEY_LEFT, 1); browser_key(a, KEY_RIGHT, 1);
    assert(a->read_scroll == 27 && a->selected == selected);
    browser_key(a, KEY_PPAGE, 1); browser_key(a, KEY_UP, 1);
    assert(a->read_scroll == 0);
    browser_key(a, KEY_PPAGE, 1); assert(a->read_scroll == 0);
    for (int i = 0; i < 10; i++) browser_key(a, KEY_NPAGE, 1);
    assert(a->read_scroll == 74);
    browser_key(a, KEY_RIGHT, 1); assert(a->focus == 2);
    assert(resizeterm(12, 40) == OK); draw_browser(a);
    browser_key(a, KEY_PPAGE, 1); assert(a->read_scroll == 68);
    browser_key(a, KEY_LEFT, 1); browser_key(a, KEY_LEFT, 1); assert(a->focus == 1);
    browser_key(a, KEY_RIGHT, 1); assert(a->read_scroll == 68);
    assert(a->selected == selected && !strcmp(a->visible[selected]->text, text));
}

int main(void)
{
    setlocale(LC_ALL, "C.UTF-8");
    /* Rendering tests never write to the user's desktop clipboard. */
    unsetenv("DISPLAY"); unsetenv("WAYLAND_DISPLAY");
    FILE *out = tmpfile(), *in = tmpfile(); assert(out && in);
    SCREEN *screen = newterm("xterm-256color", out, in); assert(screen);
    set_term(screen);
    char tmp[] = "/tmp/simplenote-mouse.XXXXXX"; assert(mkdtemp(tmp));
    NoteApp a = {0}; a.store.lock_fd = -1;
    assert(sn_store_open(&a.store, tmp, 2026));
    const char *id = "00000000000000000000000000000001";
    const char *stamp = "2026-10-05T12:00:00-0400";
    const char *text = "    abc caf\xc3\xa9 \xe4\xb8\x96\xe7\x95\x8c e\xcc\x81 \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb end\n\n\tKeep indentation and trailing space. \n"
        "This paragraph is long enough to wrap on narrow screens without inserting extra copied line breaks.";
    assert(sn_store_put(&a.store, id, stamp, stamp, text, 0, NULL));
    assert(rebuild_list(&a, id)); ssr_init(&a.renderer);
    const int columns[] = {140, 95, 40};
    for (size_t i = 0; i < sizeof columns / sizeof *columns; i++) check_highlight(&a, text, columns[i]);

    assert(resizeterm(32, 140) == OK); a.mouse.note_id[0] = 0; a.mouse.clicked_at = 0;
    a.read_scroll = 0; draw_browser(&a);
    NoteGeometry g = reader_geometry();
    /* Starting in either half of a wide glyph keeps the complete UTF-8 glyph. */
    mouse(&a, BUTTON1_PRESSED, g.top, g.left + 14);
    mouse(&a, REPORT_MOUSE_POSITION, g.top, g.left + 16);
    mouse(&a, BUTTON1_RELEASED, g.top, g.left + 16);
    assert(!strcmp(a.kill, "\xe4\xb8\x96\xe7\x95\x8c"));
    draw_browser(&a);
    assert(mvinch(g.top, g.left + 14) & A_REVERSE);
    assert(!(mvinch(g.top, g.left + 10) & A_REVERSE));
    a.mouse.clicked_at = 0;
    mouse(&a, BUTTON1_PRESSED, g.top, g.left + 18);
    mouse(&a, REPORT_MOUSE_POSITION, g.top, g.left + 19);
    mouse(&a, BUTTON1_RELEASED, g.top, g.left + 19);
    assert(!strcmp(a.kill, "e\xcc\x81 "));
    draw_browser(&a);
    assert(mvinch(g.top, g.left + 18) & A_REVERSE);
    assert(!(mvinch(g.top, g.left + 17) & A_REVERSE));
    a.mouse.clicked_at = 0;
    mouse(&a, BUTTON1_PRESSED, g.top, g.left + 20);
    mouse(&a, REPORT_MOUSE_POSITION, g.top, g.left + 21);
    mouse(&a, BUTTON1_RELEASED, g.top, g.left + 21);
    assert(!strcmp(a.kill, "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb"));
    draw_browser(&a);
    assert(mvinch(g.top, g.left + 20) & A_REVERSE);
    assert(!(mvinch(g.top, g.left + 23) & A_REVERSE));
    a.mouse.clicked_at = 0;
    mouse(&a, BUTTON1_PRESSED, g.top, g.left + 6);
    mouse(&a, BUTTON1_RELEASED, g.top, g.left + 6);
    mouse(&a, BUTTON1_PRESSED, g.top, g.left + 6);
    mouse(&a, BUTTON1_RELEASED, g.top, g.left + 6);
    assert(!strcmp(a.kill, "abc"));
    mouse(&a, BUTTON1_PRESSED, g.top, g.left + 6);
    mouse(&a, BUTTON1_RELEASED, g.top, g.left + 6);
    assert(!strcmp(a.kill, "    abc caf\xc3\xa9 \xe4\xb8\x96\xe7\x95\x8c e\xcc\x81 \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb end\n"));

    check_selection_and_scroll(&a);
    for (size_t i = 0; i < a.store.count; i++) assert(!unlink(a.store.batches[i]->path));
    sn_store_close(&a.store);
    char archive[PATH_MAX]; snprintf(archive, sizeof archive, "%s/2025", tmp);
    assert(!rmdir(archive));
    char lock[PATH_MAX]; snprintf(lock, sizeof lock, "%s/.lock", tmp);
    assert(!unlink(lock) && !rmdir(tmp));
    free(a.kill); free(a.rows); free(a.visible); free(a.years);
    ssr_destroy(&a.renderer); endwin(); delscreen(screen); fclose(in); fclose(out);
    puts("OK simplenote browser: persistent year/note highlights, hidden browser caret, Left/Right navigation, rendered line/page scroll, narrow panes, note-only highlight, exact Unicode, and word/line clicks");
    return 0;
}
