#define main simplewords_editor_main
#include "../simplewords.c"
#undef main

#include <stdarg.h>

static int failures = 0;

static void fail_case(const char *label, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "wrap check failed: %s: ", label);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    failures++;
}

static void test_screen(int width, int height)
{
    COLS = width;
    LINES = height;
    config.text_width = 80;
    config.top_pad = 0;
    distraction_free = 1;
    center_lock_enabled = 0;
    goal_col = -1;
    top = 0;
}

static void reset_test_buffer(const char *line)
{
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);

    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    line_count = 1;
    lines[0] = new_line(line);
    cy = 0;
    cx = 0;
    sel_cy = 0;
    sel_cx = 0;
    selecting = 0;
    top = 0;
    goal_col = -1;
    dirty = 0;
    autosave_dirty = 0;
    screen_cache_valid = 0;
}

static void expect_body_width(const char *label, int cols, int expected)
{
    BodyGeometry geo;

    test_screen(cols, 20);
    geo = body_geometry();
    if (geo.body_width != expected)
        fail_case(label, "body_width=%d expected=%d",
                  geo.body_width, expected);
}

static int expect_doc_row(const char *label, int doc_row,
                          int line, int render_start, int render_end,
                          int next_start, int cursor_start, int cursor_end,
                          int visual_width)
{
    WrapRow row;

    if (!layout_row_for_doc_row(doc_row, &row)) {
        fail_case(label, "missing doc row %d", doc_row);
        return 0;
    }

    if (row.line != line ||
        row.render_start != render_start ||
        row.render_end != render_end ||
        row.next_start != next_start ||
        row.cursor_start != cursor_start ||
        row.cursor_end != cursor_end ||
        row.visual_width != visual_width ||
        row.doc_row != doc_row) {
        fail_case(label,
                  "row=%d line=%d render=%d..%d next=%d cursor=%d..%d width=%d",
                  row.doc_row, row.line, row.render_start, row.render_end,
                  row.next_start, row.cursor_start, row.cursor_end,
                  row.visual_width);
        return 0;
    }

    return 1;
}

static void expect_rows(const char *label, const char *line, int width,
                        int expected_rows)
{
    int rows;

    test_screen(width, 20);
    reset_test_buffer(line);
    rows = visual_rows_for_line(lines[0]);
    if (rows != expected_rows)
        fail_case(label, "rows=%d expected=%d", rows, expected_rows);
}

static void expect_pos(const char *label, int x,
                       int expected_doc_row, int expected_col)
{
    int doc_row;
    int col;

    pos_to_visual(0, x, &doc_row, &col);
    if (doc_row != expected_doc_row || col != expected_col)
        fail_case(label, "x=%d -> row=%d col=%d expected row=%d col=%d",
                  x, doc_row, col, expected_doc_row, expected_col);
}

static void expect_roundtrip_positions(const char *label)
{
    int len = (int)strlen(lines[0]);

    for (int x = 0; x <= len; x++) {
        int doc_row;
        int col;
        int out_line = -1;
        int out_x = -1;

        pos_to_visual(0, x, &doc_row, &col);
        if (!visual_to_pos(doc_row, col, &out_line, &out_x)) {
            fail_case(label, "visual_to_pos failed for x=%d row=%d col=%d",
                      x, doc_row, col);
            continue;
        }
        if (out_line != 0 || out_x != x)
            fail_case(label,
                      "x=%d -> row=%d col=%d -> line=%d x=%d",
                      x, doc_row, col, out_line, out_x);
    }
}

static void expect_cursor(const char *label, int expected_x,
                          int expected_doc_row, int expected_col)
{
    int doc_row;
    int col;

    if (cx != expected_x)
        fail_case(label, "cx=%d expected=%d", cx, expected_x);
    pos_to_visual(cy, cx, &doc_row, &col);
    if (doc_row != expected_doc_row || col != expected_col)
        fail_case(label, "cursor row=%d col=%d expected row=%d col=%d",
                  doc_row, col, expected_doc_row, expected_col);
}

static void check_exact_width_word(void)
{
    expect_rows("exact width word", "abcd", 4, 1);
    expect_doc_row("exact width word", 0, 0, 0, 4, 4, 0, 4, 4);
    expect_pos("exact width word end", 4, 0, 4);
    expect_roundtrip_positions("exact width word");
}

static void check_one_character_overflow(void)
{
    expect_rows("one character overflow", "abcde", 4, 2);
    expect_doc_row("one character overflow row 0", 0, 0, 0, 4, 4, 0, 4, 4);
    expect_doc_row("one character overflow row 1", 1, 0, 4, 5, 5, 4, 5, 1);
    expect_pos("one character overflow boundary", 4, 1, 0);
    expect_roundtrip_positions("one character overflow");
}

static void check_long_unbreakable_word(void)
{
    expect_rows("long unbreakable", "abcdefghi", 4, 3);
    expect_doc_row("long unbreakable row 0", 0, 0, 0, 4, 4, 0, 4, 4);
    expect_doc_row("long unbreakable row 1", 1, 0, 4, 8, 8, 4, 8, 4);
    expect_doc_row("long unbreakable row 2", 2, 0, 8, 9, 9, 8, 9, 1);
    expect_roundtrip_positions("long unbreakable");
}

static void check_leading_spaces(void)
{
    expect_rows("leading spaces", "    word", 4, 2);
    expect_doc_row("leading spaces row 0", 0, 0, 0, 4, 4, 0, 4, 4);
    expect_doc_row("leading spaces row 1", 1, 0, 4, 8, 8, 4, 8, 4);
    expect_roundtrip_positions("leading spaces");
}

static void check_trailing_spaces(void)
{
    expect_rows("trailing spaces", "trail     ", 5, 2);
    expect_doc_row("trailing spaces row 0", 0, 0, 0, 5, 5, 0, 5, 5);
    expect_doc_row("trailing spaces row 1", 1, 0, 5, 10, 10, 5, 10, 5);
    expect_roundtrip_positions("trailing spaces");
}

static void check_multiple_spaces(void)
{
    expect_rows("multiple spaces", "word     next", 5, 3);
    expect_doc_row("multiple spaces row 0", 0, 0, 0, 4, 5, 0, 4, 4);
    expect_doc_row("multiple spaces row 1", 1, 0, 5, 10, 10, 5, 10, 5);
    expect_doc_row("multiple spaces row 2", 2, 0, 10, 13, 13, 10, 13, 3);
    expect_pos("multiple spaces before hidden break", 4, 0, 4);
    expect_pos("multiple spaces after hidden break", 5, 1, 0);
    expect_roundtrip_positions("multiple spaces");
}

static void check_tabs(void)
{
    expect_rows("tabs", "a\tb\tc", 4, 3);
    expect_doc_row("tabs row 0", 0, 0, 0, 1, 2, 0, 1, 1);
    expect_doc_row("tabs row 1", 1, 0, 2, 3, 4, 2, 3, 1);
    expect_doc_row("tabs row 2", 2, 0, 4, 5, 5, 4, 5, 1);
    expect_roundtrip_positions("tabs");
}

static void check_empty_lines(void)
{
    expect_rows("empty line", "", 4, 1);
    expect_doc_row("empty line", 0, 0, 0, 0, 0, 0, 0, 0);
    expect_pos("empty line cursor", 0, 0, 0);
    expect_roundtrip_positions("empty line");
}

static void check_selection_across_wraps(void)
{
    test_screen(4, 20);
    reset_test_buffer("abc def");
    selecting = 1;
    sel_cy = 0;
    sel_cx = 0;
    cy = 0;
    cx = (int)strlen(lines[0]);

    if (!char_selected(0, 3))
        fail_case("selection across wraps", "hidden break space is not selected");
    if (!expect_doc_row("selection soft row", 0, 0, 0, 3, 4, 0, 3, 3))
        return;
    if (!expect_doc_row("selection continuation row", 1, 0, 4, 7, 7, 4, 7, 3))
        return;
}

static void check_up_down_at_wrap_boundaries(void)
{
    test_screen(4, 20);
    reset_test_buffer("abc def");

    cx = 3;
    move_visual_line(1, 0);
    expect_cursor("down from soft wrap boundary", 7, 1, 3);
    move_visual_line(-1, 0);
    expect_cursor("up to soft wrap boundary", 3, 0, 3);

    reset_test_buffer("abcdefghi");
    cx = 0;
    move_visual_line(1, 0);
    expect_cursor("down through hard wrap", 4, 1, 0);
    move_visual_line(1, 0);
    expect_cursor("down through hard wrap second", 8, 2, 0);
    move_visual_line(-1, 0);
    expect_cursor("up through hard wrap", 4, 1, 0);
}

static void check_page_movement(void)
{
    test_screen(4, 6);
    reset_test_buffer("abcdefghijklmnop");
    move_page(1, 0);
    expect_cursor("page down", 12, 3, 0);
    move_page(-1, 0);
    expect_cursor("page up", 0, 0, 0);
}

static void check_home_end(void)
{
    test_screen(4, 20);
    reset_test_buffer("abc def");

    cx = 1;
    move_visual_end(0);
    expect_cursor("end of soft wrapped first row", 3, 0, 3);
    move_visual_line(1, 0);
    move_visual_home(0);
    expect_cursor("home of continuation row", 4, 1, 0);
    move_visual_end(0);
    expect_cursor("end of continuation row", 7, 1, 3);
}

static void check_resize_narrow_terminal(void)
{
    expect_body_width("narrow terminal width", 6, 6);
    expect_rows("narrow terminal rewrap", "abcdefghijkl", 6, 2);
    expect_doc_row("narrow terminal row 0", 0, 0, 0, 6, 6, 0, 6, 6);
    expect_doc_row("narrow terminal row 1", 1, 0, 6, 12, 12, 6, 12, 6);

    test_screen(4, 6);
    reset_test_buffer("abcdefghijklmnop");
    top = 99;
    clamp_top();
    if (top != 0)
        fail_case("resize top clamp", "top=%d expected=0", top);
}

int main(void)
{
    setlocale(LC_ALL, "");

    check_exact_width_word();
    check_one_character_overflow();
    check_long_unbreakable_word();
    check_leading_spaces();
    check_trailing_spaces();
    check_multiple_spaces();
    check_tabs();
    check_empty_lines();
    check_selection_across_wraps();
    check_up_down_at_wrap_boundaries();
    check_page_movement();
    check_home_end();
    check_resize_narrow_terminal();

    reset_test_buffer("");
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
    free(lines[0]);
    lines[0] = NULL;

    if (failures) {
        fprintf(stderr, "%d wrap checks failed\n", failures);
        return 1;
    }

    puts("simplewords wrap checks passed");
    return 0;
}
