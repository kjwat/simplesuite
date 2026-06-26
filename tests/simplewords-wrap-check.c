#define main simplewords_editor_main
#include "../simplewords.c"
#undef main

#include <stdarg.h>

typedef struct {
    int start;
    int end;
    int width;
} WrapTestSegment;

static int failures = 0;

static void fail_case(int width, const char *line, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "wrap check failed width=%d line=\"%s\": ", width, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    failures++;
}

static int build_test_segments(const char *line, int width,
                               WrapTestSegment *segments, int max_segments)
{
    int len = (int)strlen(line);
    int start = 0;
    int count = 0;

    config.text_width = width;
    if (!len)
        return 0;

    while (start < len) {
        int end;
        int next_start;
        int screen_width;

        if (count >= max_segments) {
            fail_case(width, line, "too many wrap segments");
            return count;
        }

        wrap_segment(line, start, &end, &next_start);
        screen_width = visual_col_range(line, start, end);

        if (end <= start)
            fail_case(width, line, "zero-length segment at %d..%d", start, end);
        if (next_start <= start)
            fail_case(width, line, "non-progressing next_start=%d at start=%d",
                      next_start, start);
        if (end > len || next_start > len)
            fail_case(width, line, "segment exceeds length: end=%d next=%d len=%d",
                      end, next_start, len);
        if (end != next_start)
            fail_case(width, line, "segment gap or overlap: end=%d next=%d",
                      end, next_start);
        if (screen_width <= 0)
            fail_case(width, line, "non-empty segment has screen_width=%d",
                      screen_width);

        segments[count++] = (WrapTestSegment){
            .start = start,
            .end = end,
            .width = screen_width
        };
        start = next_start;
    }

    return count;
}

static void check_cursor_mapping(const char *line, int width)
{
    int len = (int)strlen(line);
    WrapTestSegment segments[MAX_LINE];
    int segment_count = build_test_segments(line, width, segments, MAX_LINE);
    int expected_rows = len ? segment_count : 1;
    int rows = visual_rows_for_line(line);

    config.text_width = width;
    if (rows != expected_rows)
        fail_case(width, line, "visual_rows_for_line=%d expected=%d",
                  rows, expected_rows);

    for (int cx = 0; cx <= len; cx++) {
        int row;
        int col;

        wrapped_pos_for_index(line, cx, &row, &col);
        if (row < 0 || row >= expected_rows) {
            fail_case(width, line, "cx=%d maps to row=%d outside 0..%d",
                      cx, row, expected_rows - 1);
            continue;
        }

        if (len == 0) {
            if (col != 0)
                fail_case(width, line, "empty line maps cx=%d to col=%d",
                          cx, col);
            continue;
        }

        if (col < 0 || col > segments[row].width)
            fail_case(width, line,
                      "cx=%d maps to col=%d outside row width=%d",
                      cx, col, segments[row].width);
    }

    for (int row = 0; row < segment_count; row++) {
        int mapped_row;
        int mapped_col;

        wrapped_pos_for_index(line, segments[row].end, &mapped_row, &mapped_col);
        if (mapped_row != row || mapped_col != segments[row].width) {
            fail_case(width, line,
                      "segment end cx=%d maps to row=%d col=%d, expected row=%d col=%d",
                      segments[row].end, mapped_row, mapped_col,
                      row, segments[row].width);
        }
    }
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
    top = 0;
    goal_col = -1;
    selecting = 0;
    dirty = 0;
    autosave_dirty = 0;
}

static void check_backspace_at_wrap_start(const char *line, int width)
{
    char expected[MAX_LINE];
    int len = (int)strlen(line);
    WrapTestSegment segments[MAX_LINE];
    int segment_count;

    if (len <= 0 || len >= MAX_LINE)
        return;

    config.text_width = width;
    segment_count = build_test_segments(line, width, segments, MAX_LINE);
    for (int row = 1; row < segment_count; row++) {
        int start = segments[row].start;

        if (start <= 0 || line[start - 1] != ' ')
            continue;

        snprintf(expected, sizeof(expected), "%.*s%s",
                 start - 1, line, line + start);

        reset_test_buffer(line);
        cx = start;
        backspace();

        if (cx != start - 1)
            fail_case(width, line,
                      "backspace at wrapped row start left cx=%d expected=%d",
                      cx, start - 1);
        if (strcmp(lines[0], expected) != 0)
            fail_case(width, line,
                      "backspace deleted wrong byte: got=\"%s\" expected=\"%s\"",
                      lines[0], expected);
        check_cursor_mapping(lines[0], width);
        return;
    }

    fail_case(width, line, "no wrapped row starting after a real space");
}

static void expect_cursor_at(const char *label, int width,
                             int expected_row, int expected_col)
{
    int row;
    int col;

    config.text_width = width;
    wrapped_pos_for_index(lines[cy], cx, &row, &col);
    if (row != expected_row || col != expected_col)
        fail_case(width, lines[cy],
                  "%s cursor row=%d col=%d expected row=%d col=%d",
                  label, row, col, expected_row, expected_col);
}

static void check_typing_at_wrap_boundary(void)
{
    reset_test_buffer("abc def");
    config.text_width = 4;
    cx = 4;
    expect_cursor_at("before typing at boundary", 4, 0, 4);

    insert_char('X');
    if (strcmp(lines[0], "abc Xdef") != 0 || cx != 5)
        fail_case(4, lines[0],
                  "typing at boundary produced line=\"%s\" cx=%d",
                  lines[0], cx);
    expect_cursor_at("after typing at boundary", 4, 1, 1);
    check_cursor_mapping(lines[0], 4);
}

static void check_backspace_at_wrap_boundary_policy(void)
{
    reset_test_buffer("abc def");
    config.text_width = 4;
    cx = 4;
    expect_cursor_at("before backspace at boundary", 4, 0, 4);

    backspace();
    if (strcmp(lines[0], "abcdef") != 0 || cx != 3)
        fail_case(4, lines[0],
                  "backspace at boundary produced line=\"%s\" cx=%d",
                  lines[0], cx);
    expect_cursor_at("after backspace at boundary", 4, 0, 3);
    check_cursor_mapping(lines[0], 4);
}

static void check_long_word_collapse_delete(void)
{
    reset_test_buffer("abcdefghi");
    config.text_width = 4;
    cx = (int)strlen(lines[0]);

    for (int i = 0; i < 5; i++) {
        backspace();
        check_cursor_mapping(lines[0], 4);
    }

    if (strcmp(lines[0], "abcd") != 0 || cx != 4)
        fail_case(4, lines[0],
                  "long-word collapse delete produced line=\"%s\" cx=%d",
                  lines[0], cx);
    expect_cursor_at("after long-word collapse", 4, 0, 4);
}

static void check_up_down_goal_col_at_boundary(void)
{
    reset_test_buffer("abc def");
    config.text_width = 4;
    cx = 4;
    goal_col = -1;
    expect_cursor_at("before down at boundary", 4, 0, 4);

    move_visual_line(1, 0);
    if (cx != 7 || goal_col != 4)
        fail_case(4, lines[0],
                  "down from boundary left cx=%d goal_col=%d",
                  cx, goal_col);
    expect_cursor_at("after down from boundary", 4, 1, 3);

    move_visual_line(-1, 0);
    if (cx != 4 || goal_col != 4)
        fail_case(4, lines[0],
                  "up after boundary down left cx=%d goal_col=%d",
                  cx, goal_col);
    expect_cursor_at("after up to boundary", 4, 0, 4);
}

static void check_find_at_wrap_boundary(void)
{
    int fy = -1;
    int fx = -1;

    reset_test_buffer("abc def");
    config.text_width = 4;
    if (!find_text_forward("def", 0, 0, &fy, &fx) || fy != 0 || fx != 4)
        fail_case(4, lines[0], "find did not land on boundary match fx=%d", fx);

    cy = fy;
    cx = fx;
    find_active = 1;
    find_match_y = fy;
    find_match_x = fx;
    find_match_len = 3;

    if (!char_find_highlight(0, 4) || !char_find_highlight(0, 6) ||
        char_find_highlight(0, 3))
        fail_case(4, lines[0], "find highlight around boundary is wrong");
    expect_cursor_at("find cursor at boundary", 4, 0, 4);

    find_active = 0;
    find_match_y = -1;
    find_match_x = -1;
    find_match_len = 0;
}

int main(void)
{
    static const char *cases[] = {
        "",
        "a",
        "abcdef",
        "abcdefghijklmnop",
        "     ",
        "                    ",
        "abc ",
        "abc  ",
        "abc def",
        "abc  def",
        "aa   bb",
        "word     next",
        "\t",
        "\t\t\t",
        "a\tb\tc",
        "a \t \t b",
        "trail     ",
        "trail      next"
    };

    setlocale(LC_ALL, "");
    for (int width = 1; width <= 5; width++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            check_cursor_mapping(cases[i], width);
    }

    check_backspace_at_wrap_start("aa   bb", 5);
    check_backspace_at_wrap_start("word     next", 5);
    check_backspace_at_wrap_start("trail      next", 5);
    check_typing_at_wrap_boundary();
    check_backspace_at_wrap_boundary_policy();
    check_long_word_collapse_delete();
    check_up_down_goal_col_at_boundary();
    check_find_at_wrap_boundary();

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
