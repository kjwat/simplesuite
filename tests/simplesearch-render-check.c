#define main simplesearch_program_main
#include "../simplesearch.c"
#undef main
#include <assert.h>

static wchar_t cell_at(int row, int col, attr_t *attributes)
{
    cchar_t cell; wchar_t text[CCHARW_MAX]; short pair;
    assert(mvwin_wch(stdscr, row, col, &cell) != ERR);
    assert(getcchar(&cell, text, attributes, &pair, NULL) != ERR);
    return text[0];
}

static void check_highlights(int row, const wchar_t *expected)
{
    wchar_t text[160]; attr_t attrs[160];
    for (int x = 0; x < COLS; x++) text[x] = cell_at(row, x, &attrs[x]);
    text[COLS] = 0;
    wchar_t *found = wcsstr(text, expected);
    assert(found);
    for (size_t i = 0; i < wcslen(expected); i++) {
        attr_t attr = attrs[found - text + i];
        assert(attr & A_BOLD); assert(attr & A_UNDERLINE);
    }
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "C.UTF-8");
    FILE *output = tmpfile(), *input = tmpfile();
    assert(output && input);
    SCREEN *screen = newterm("xterm-256color", output, input); assert(screen);
    resizeterm(26, 100);
    start_color(); use_default_colors(); init_pair(1, COLOR_CYAN, -1); init_pair(2, COLOR_YELLOW, -1);
    accent = A_BOLD | COLOR_PAIR(1); muted = A_DIM; highlight = A_BOLD | A_UNDERLINE | COLOR_PAIR(2);
    Results *r = calloc(1, sizeof *r); assert(r);
    r->pattern = make_pattern("winter light");
    const char *lines[] = {
        "The winter light comes early. WINTER LIGHT stays on the kitchen wall. The whole paragraph continues beyond the first screen line, with more context to read. A third line carries the rest of the thought, and there is room for a fourth line when needed.",
        "I keep returning to the winter light across the orchard.",
        "From the far side of the room, after a very long paragraph of ordinary context that must be cropped on a narrow terminal, the winter light enters again."
    };
    const char *paths[] = {"/writing/journal/2026/10-19.txt", "/writing/essays/the orchard.md", "/writing/drafts/notes.txt"};
    for (int i = 0; i < 3; i++) {
        size_t length; const char *hit = find_phrase(lines[i], &r->pattern, &length); assert(hit);
        assert(add_match(r, paths[i], lines[i], (unsigned long)(19 + i * 30), hit, length));
    }
    View v = {.results = r}; wcscpy(v.query, L"winter light"); v.length = v.cursor = wcslen(v.query);
    wcstombs(v.bytes, v.query, sizeof v.bytes);
    draw_view(&v, "/writing");
    check_highlights(RESULT_ROW + 1, L"winter light");
    check_highlights(RESULT_ROW + 1, L"WINTER LIGHT");
    int continuation = 0;
    for (int y = RESULT_ROW + 2; y <= RESULT_ROW + EXCERPT_ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            attr_t attr;
            if (cell_at(y, x, &attr) != L' ') continuation++;
        }
    }
    assert(continuation > 100); /* More than a single line of paragraph context. */
    check_highlights(RESULT_ROW + 2 * RESULT_HEIGHT + 1, L"winter light");
    for (int y = 0; y < LINES; y++) for (int x = 0; x < COLS; x++) {
        attr_t attr; cell_at(y, x, &attr); assert(!(attr & A_ALTCHARSET));
    }
    /* Save the actual curses cells for visual inspection, without a desktop. */
    if (argc > 1) {
        FILE *snapshot = fopen(argv[1], "w"); assert(snapshot);
        fprintf(snapshot, "%d %d\n", LINES, COLS);
        for (int y = 0; y < LINES; y++) for (int x = 0; x < COLS; x++) {
            attr_t attr; wchar_t wc = cell_at(y, x, &attr);
            fprintf(snapshot, "%d %d %lx %u %d %d\n", y, x, (unsigned long)wc,
                    (unsigned)PAIR_NUMBER(attr), !!(attr & A_BOLD), !!(attr & A_DIM));
        }
        fclose(snapshot);
    }
    r->pattern = make_pattern("CAFÉ");
    free_results(r); r = calloc(1, sizeof *r); assert(r); r->pattern = make_pattern("CAFÉ");
    const char *unicode = "Une lumière douce au café, puis CAFÉ encore.";
    size_t length; const char *hit = find_phrase(unicode, &r->pattern, &length); assert(hit);
    assert(length == strlen("café")); assert(add_match(r, paths[0], unicode, 2, hit, length));
    v.results = r; v.selected = v.top = 0;
    draw_view(&v, "/writing"); check_highlights(RESULT_ROW + 1, L"café"); check_highlights(RESULT_ROW + 1, L"CAFÉ");
    resizeterm(18, 42); draw_view(&v, "/writing"); check_highlights(RESULT_ROW + 1, L"café");
    resizeterm(5, 12); draw_view(&v, "/writing");
    free_results(r); endwin(); delscreen(screen); fclose(output); fclose(input);
    puts("simplesearch: quiet layout, repeated/Unicode highlights, wrapped paragraphs, cropping, and resize passed");
    return 0;
}
