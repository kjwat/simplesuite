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
    clear_cursor_affinity();
}

static void reset_two_line_buffer(const char *first, const char *second)
{
    reset_test_buffer(first);
    line_count = 2;
    lines[1] = new_line(second);
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

static void expect_roundtrip_visual_positions(const char *label)
{
    int rows = document_visual_rows();
    int body_width = body_geometry().body_width;

    for (int row = 0; row < rows; row++) {
        WrapRow wrap;

        if (!layout_row_for_doc_row(row, &wrap)) {
            fail_case(label, "missing visual row %d", row);
            continue;
        }
        if (wrap.render_end != wrap.next_start)
            fail_case(label, "row=%d hides document span %d..%d",
                      row, wrap.render_end, wrap.next_start);
        if (wrap.cursor_start != wrap.render_start ||
            wrap.cursor_end != wrap.render_end)
            fail_case(label,
                      "row=%d cursor span %d..%d differs from rendered span %d..%d",
                      row, wrap.cursor_start, wrap.cursor_end,
                      wrap.render_start, wrap.render_end);
        if (body_width > 1 && wrap.visual_width >= body_width)
            fail_case(label,
                      "row=%d visual_width=%d does not reserve cursor cell in body_width=%d",
                      row, wrap.visual_width, body_width);

        for (int col = 0; col <= wrap.visual_width; col++) {
            int out_line = -1;
            int out_x = -1;
            int affinity_col = -1;
            int mapped_row = -1;
            int mapped_col = -1;

            if (!visual_to_pos_with_affinity(row, col, &out_line, &out_x,
                                             &affinity_col)) {
                fail_case(label, "visual_to_pos failed row=%d col=%d",
                          row, col);
                continue;
            }

            pos_to_visual_with_affinity(out_line, out_x, row, affinity_col,
                                        &mapped_row, &mapped_col);
            if (mapped_row != row || mapped_col != affinity_col)
                fail_case(label,
                          "row=%d col=%d -> line=%d x=%d aff_col=%d -> row=%d col=%d",
                          row, col, out_line, out_x, affinity_col,
                          mapped_row, mapped_col);
        }
    }
}

static void expect_cursor(const char *label, int expected_x,
                          int expected_doc_row, int expected_col)
{
    int doc_row;
    int col;

    if (cx != expected_x)
        fail_case(label, "cx=%d expected=%d", cx, expected_x);
    cursor_visual_pos(&doc_row, &col);
    if (doc_row != expected_doc_row || col != expected_col)
        fail_case(label, "cursor row=%d col=%d expected row=%d col=%d",
                  doc_row, col, expected_doc_row, expected_col);
}

static void expect_cursor_screen(const char *label, int expected_row,
                                 int expected_col)
{
    int row;
    int col;

    cursor_screen_pos(&row, &col);
    if (row != expected_row || col != expected_col)
        fail_case(label, "screen cursor row=%d col=%d expected row=%d col=%d",
                  row, col, expected_row, expected_col);
}

static void expect_line(const char *label, const char *expected)
{
    if (strcmp(lines[0], expected) != 0)
        fail_case(label, "line='%s' expected='%s'", lines[0], expected);
}

static void type_text(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        insert_char(*p);
}

static void check_typing_cursor_positions(void)
{
    test_screen(4, 20);
    reset_test_buffer("");
    type_text("abc");
    expect_cursor("typing to visual margin", 3, 0, 3);
    expect_cursor_screen("screen cursor at reserved margin", 0, 3);
    insert_char('d');
    expect_cursor("insert at visual margin", 4, 1, 1);
    expect_cursor_screen("inserted margin char keeps cursor after it", 1, 1);
    expect_line("insert at visual margin text", "abcd");
    backspace();
    expect_cursor("backspace from visual margin insert", 3, 0, 3);
    expect_line("backspace from visual margin text", "abc");
    insert_char(' ');
    expect_cursor("space at visual margin", 4, 1, 1);
    expect_line("space at visual margin text", "abc ");
    backspace();
    expect_cursor("backspace margin space", 3, 0, 3);
    expect_line("backspace margin space text", "abc");
    insert_char('d');
    insert_char('e');
    expect_cursor("typing one character past wrapped row", 5, 1, 2);

    test_screen(4, 20);
    reset_test_buffer("");
    type_text("abc");
    expect_cursor("typing exact capacity before break space", 3, 0, 3);
    insert_char(' ');
    expect_cursor("typing one break space at wrap boundary", 4, 1, 1);
    insert_char(' ');
    expect_cursor("typing two break spaces at wrap boundary", 5, 1, 2);
    insert_char(' ');
    expect_cursor("typing three break spaces at wrap boundary", 6, 1, 3);
    insert_char('e');
    expect_cursor("typing first char after break spaces", 7, 2, 1);
    expect_doc_row("visible break space run row 0", 0, 0, 0, 3, 3, 0, 3, 3);
    expect_doc_row("visible break space run row 1", 1, 0, 3, 6, 6, 3, 6, 3);
    expect_doc_row("visible break space run row 2", 2, 0, 6, 7, 7, 6, 7, 1);
    expect_roundtrip_positions("visible break space run");
    expect_roundtrip_visual_positions("visible break space run visual");

    test_screen(4, 20);
    reset_test_buffer("abc def");
    cx = 1;
    move_visual_end(0);
    expect_cursor("soft wrap insertion starts at visible row end", 3, 0, 3);
    insert_char(' ');
    expect_cursor("one inserted soft-wrap space", 4, 1, 1);
    insert_char(' ');
    expect_cursor("two inserted soft-wrap spaces", 5, 1, 2);
    insert_char(' ');
    expect_cursor("three inserted soft-wrap spaces", 6, 1, 3);
    insert_char('x');
    expect_cursor("printable after inserted soft-wrap spaces", 7, 2, 1);
    expect_doc_row("soft-wrap inserted spaces row 0", 0, 0, 0, 3, 3, 0, 3, 3);
    expect_doc_row("soft-wrap inserted spaces row 1", 1, 0, 3, 6, 6, 3, 6, 3);
    expect_doc_row("soft-wrap inserted spaces row 2", 2, 0, 6, 8, 8, 6, 8, 2);
    expect_doc_row("soft-wrap inserted spaces row 3", 3, 0, 8, 11, 11, 8, 11, 3);
    expect_roundtrip_positions("soft-wrap inserted spaces");
    expect_roundtrip_visual_positions("soft-wrap inserted spaces visual");

    test_screen(4, 20);
    reset_test_buffer("");
    type_text("abc ");
    expect_cursor("typing soft break pending space", 4, 1, 1);
    insert_char('d');
    expect_cursor("typing first char after soft break", 5, 1, 2);
    type_text("ef ghi");
    expect_cursor("typing prose with spaces", 11, 4, 3);

    test_screen(4, 20);
    reset_test_buffer("");
    type_text("abcdefghi");
    expect_cursor("typing long unbreakable word", 9, 2, 3);
}

static void check_exact_width_word(void)
{
    expect_rows("exact width word", "abcd", 4, 2);
    expect_doc_row("exact width word row 0", 0, 0, 0, 3, 3, 0, 3, 3);
    expect_doc_row("exact width word row 1", 1, 0, 3, 4, 4, 3, 4, 1);
    expect_pos("exact width word first row end", 3, 0, 3);
    expect_pos("exact width word end", 4, 1, 1);
    expect_roundtrip_positions("exact width word");
    expect_roundtrip_visual_positions("exact width word visual");
}

static void check_one_character_overflow(void)
{
    expect_rows("one character overflow", "abcde", 4, 2);
    expect_doc_row("one character overflow row 0", 0, 0, 0, 3, 3, 0, 3, 3);
    expect_doc_row("one character overflow row 1", 1, 0, 3, 5, 5, 3, 5, 2);
    expect_pos("one character overflow boundary", 3, 0, 3);
    expect_pos("one character overflow second char", 4, 1, 1);
    expect_roundtrip_positions("one character overflow");
    expect_roundtrip_visual_positions("one character overflow visual");
}

static void check_long_unbreakable_word(void)
{
    expect_rows("long unbreakable", "abcdefghi", 4, 3);
    expect_doc_row("long unbreakable row 0", 0, 0, 0, 3, 3, 0, 3, 3);
    expect_doc_row("long unbreakable row 1", 1, 0, 3, 6, 6, 3, 6, 3);
    expect_doc_row("long unbreakable row 2", 2, 0, 6, 9, 9, 6, 9, 3);
    expect_roundtrip_positions("long unbreakable");
    expect_roundtrip_visual_positions("long unbreakable visual");
}

static void check_leading_spaces(void)
{
    expect_rows("leading spaces", "    word", 4, 4);
    expect_doc_row("leading spaces row 0", 0, 0, 0, 3, 3, 0, 3, 3);
    expect_doc_row("leading spaces row 1", 1, 0, 3, 4, 4, 3, 4, 1);
    expect_doc_row("leading spaces row 2", 2, 0, 4, 7, 7, 4, 7, 3);
    expect_doc_row("leading spaces row 3", 3, 0, 7, 8, 8, 7, 8, 1);
    expect_roundtrip_positions("leading spaces");
    expect_roundtrip_visual_positions("leading spaces visual");
}

static void check_trailing_spaces(void)
{
    expect_rows("trailing spaces", "trail     ", 5, 3);
    expect_doc_row("trailing spaces row 0", 0, 0, 0, 4, 4, 0, 4, 4);
    expect_doc_row("trailing spaces row 1", 1, 0, 4, 8, 8, 4, 8, 4);
    expect_doc_row("trailing spaces row 2", 2, 0, 8, 10, 10, 8, 10, 2);
    expect_pos("trailing spaces run start", 5, 1, 1);
    expect_pos("trailing spaces run middle", 7, 1, 3);
    expect_pos("trailing spaces run end", 10, 2, 2);
    expect_roundtrip_positions("trailing spaces");
    expect_roundtrip_visual_positions("trailing spaces visual");
}

static void check_multiple_spaces(void)
{
    expect_rows("multiple spaces", "word     next", 5, 4);
    expect_doc_row("multiple spaces row 0", 0, 0, 0, 4, 4, 0, 4, 4);
    expect_doc_row("multiple spaces row 1", 1, 0, 4, 8, 8, 4, 8, 4);
    expect_doc_row("multiple spaces row 2", 2, 0, 8, 9, 9, 8, 9, 1);
    expect_doc_row("multiple spaces row 3", 3, 0, 9, 13, 13, 9, 13, 4);
    expect_pos("multiple spaces before break", 4, 0, 4);
    expect_pos("multiple spaces run start", 5, 1, 1);
    expect_pos("multiple spaces run middle", 7, 1, 3);
    expect_pos("multiple spaces run end", 9, 2, 1);
    expect_pos("multiple spaces next word first char", 10, 3, 1);
    expect_roundtrip_positions("multiple spaces");
    expect_roundtrip_visual_positions("multiple spaces visual");
}

static void check_tabs(void)
{
    expect_rows("tabs", "a\tb\tc", 4, 5);
    expect_doc_row("tabs row 0", 0, 0, 0, 1, 1, 0, 1, 1);
    expect_doc_row("tabs row 1", 1, 0, 1, 2, 2, 1, 2, 3);
    expect_doc_row("tabs row 2", 2, 0, 2, 3, 3, 2, 3, 1);
    expect_doc_row("tabs row 3", 3, 0, 3, 4, 4, 3, 4, 3);
    expect_doc_row("tabs row 4", 4, 0, 4, 5, 5, 4, 5, 1);
    expect_roundtrip_positions("tabs");
    expect_roundtrip_visual_positions("tabs visual");
}

static void expect_visual_maps_to(const char *label, int doc_row, int col,
                                  int expected_x)
{
    int out_line = -1;
    int out_x = -1;
    int affinity_col = -1;
    int mapped_row = -1;
    int mapped_col = -1;

    if (!visual_to_pos_with_affinity(doc_row, col, &out_line, &out_x,
                                     &affinity_col)) {
        fail_case(label, "visual_to_pos failed row=%d col=%d", doc_row, col);
        return;
    }
    if (out_line != 0 || out_x != expected_x || affinity_col != col)
        fail_case(label,
                  "row=%d col=%d -> line=%d x=%d aff_col=%d expected x=%d",
                  doc_row, col, out_line, out_x, affinity_col, expected_x);

    pos_to_visual_with_affinity(out_line, out_x, doc_row, affinity_col,
                                &mapped_row, &mapped_col);
    if (mapped_row != doc_row || mapped_col != col)
        fail_case(label,
                  "row=%d col=%d -> line=%d x=%d -> row=%d col=%d",
                  doc_row, col, out_line, out_x, mapped_row, mapped_col);
}

static void check_visible_tab_columns(void)
{
    test_screen(4, 20);
    reset_test_buffer("\tb");

    expect_doc_row("visible tab row 0", 0, 0, 0, 1, 1, 0, 1, 3);
    expect_doc_row("visible tab row 1", 1, 0, 1, 2, 2, 1, 2, 1);
    expect_visual_maps_to("visible tab col 0", 0, 0, 0);
    expect_visual_maps_to("visible tab col 1", 0, 1, 0);
    expect_visual_maps_to("visible tab col 2", 0, 2, 0);
    expect_visual_maps_to("visible tab end", 0, 3, 1);
    expect_roundtrip_visual_positions("visible tab visual");
}

static void check_empty_lines(void)
{
    expect_rows("empty line", "", 4, 1);
    expect_doc_row("empty line", 0, 0, 0, 0, 0, 0, 0, 0);
    expect_pos("empty line cursor", 0, 0, 0);
    expect_roundtrip_positions("empty line");
    expect_roundtrip_visual_positions("empty line visual");
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
        fail_case("selection across wraps", "wrap break space is not selected");
    if (!expect_doc_row("selection soft row", 0, 0, 0, 3, 3, 0, 3, 3))
        return;
    if (!expect_doc_row("selection wrap space row", 1, 0, 3, 4, 4, 3, 4, 1))
        return;
    if (!expect_doc_row("selection continuation row", 2, 0, 4, 7, 7, 4, 7, 3))
        return;
}

static void check_up_down_at_wrap_boundaries(void)
{
    test_screen(4, 20);
    reset_test_buffer("abc def");

    cx = 3;
    move_visual_line(1, 0);
    expect_cursor("down from soft wrap boundary", 4, 1, 1);
    move_visual_line(-1, 0);
    expect_cursor("up to soft wrap boundary", 3, 0, 3);

    reset_test_buffer("abcdefghi");
    cx = 0;
    move_visual_line(1, 0);
    expect_cursor("down through hard wrap", 3, 1, 0);
    move_visual_line(1, 0);
    expect_cursor("down through hard wrap second", 6, 2, 0);
    move_visual_line(-1, 0);
    expect_cursor("up through hard wrap", 3, 1, 0);
}

static void check_page_movement(void)
{
    test_screen(4, 6);
    reset_test_buffer("abcdefghijklmnop");
    move_page(1, 0);
    expect_cursor("page down", 15, 5, 0);
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
    expect_cursor("home of continuation row", 3, 1, 0);
    move_visual_end(0);
    expect_cursor("end of continuation row", 4, 1, 1);

    reset_test_buffer("abcde");
    cx = 1;
    move_visual_end(0);
    expect_cursor("end of hard wrapped first row", 3, 0, 3);
    move_visual_end(0);
    expect_cursor("repeated end stays on hard wrapped first row", 3, 0, 3);
    move_visual_home(0);
    expect_cursor("home from hard wrapped first row end", 0, 0, 0);
    move_visual_end(0);
    move_right(0);
    expect_cursor("right to hard wrapped continuation start", 3, 1, 0);
    move_visual_home(0);
    expect_cursor("home of hard wrapped continuation", 3, 1, 0);
    move_visual_end(0);
    expect_cursor("end of hard wrapped continuation", 5, 1, 2);
    move_visual_home(0);
    move_visual_line(-1, 0);
    expect_cursor("up from hard continuation start", 0, 0, 0);

    reset_test_buffer("abcde");
    cx = 1;
    move_visual_end(0);
    move_visual_line(1, 0);
    expect_cursor("down from hard wrapped row end", 5, 1, 2);

    reset_test_buffer("abcde");
    cx = 4;
    expect_cursor("default hard continuation interior", 4, 1, 1);
    move_left(0);
    expect_cursor("left from default hard boundary reaches prior row end",
                  3, 0, 3);
}

static void check_resize_narrow_terminal(void)
{
    expect_body_width("narrow terminal width", 6, 6);
    expect_rows("narrow terminal rewrap", "abcdefghijkl", 6, 3);
    expect_doc_row("narrow terminal row 0", 0, 0, 0, 5, 5, 0, 5, 5);
    expect_doc_row("narrow terminal row 1", 1, 0, 5, 10, 10, 5, 10, 5);
    expect_doc_row("narrow terminal row 2", 2, 0, 10, 12, 12, 10, 12, 2);

    test_screen(4, 6);
    reset_test_buffer("abcdefghijklmnop");
    top = 99;
    clamp_top();
    if (top != 0)
        fail_case("resize top clamp", "top=%d expected=0", top);
}

static void check_narrow_tabs(void)
{
    test_screen(2, 20);
    reset_test_buffer("a\tb");
    for (int row = 0; row < document_visual_rows(); row++) {
        WrapRow wrap;

        if (!layout_row_for_doc_row(row, &wrap)) {
            fail_case("narrow tabs", "missing row %d", row);
            continue;
        }
        if (wrap.visual_width > body_geometry().body_width)
            fail_case("narrow tabs", "row=%d visual_width=%d body_width=%d",
                      row, wrap.visual_width, body_geometry().body_width);
    }
    expect_roundtrip_visual_positions("narrow tabs visual");
}

static void check_vertical_tabs(void)
{
    test_screen(8, 20);
    reset_two_line_buffer("xx", "a\tb");

    cx = 2;
    move_visual_line(1, 0);
    expect_cursor("down into tab interior", 1, 1, 2);
    if (goal_col != 2)
        fail_case("down into tab interior", "goal_col=%d expected=2",
                  goal_col);
    move_visual_line(-1, 0);
    expect_cursor("up from tab interior", 2, 0, 2);
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
    check_visible_tab_columns();
    check_empty_lines();
    check_typing_cursor_positions();
    check_selection_across_wraps();
    check_up_down_at_wrap_boundaries();
    check_page_movement();
    check_home_end();
    check_resize_narrow_terminal();
    check_narrow_tabs();
    check_vertical_tabs();

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
