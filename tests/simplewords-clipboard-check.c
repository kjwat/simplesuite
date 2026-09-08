#define SIMPLEWORDS_TYPEWRITER_TEST
#define main simplewords_program_main
#include "../simplewords.c"
#undef main

static void document(const char *first, const char *second)
{
    for (int i = 0; i < MAX_BUFFERS; i++)
        free_buffer_storage(i);
    memset(editor_windows, 0, sizeof(editor_windows));
    memset(layout_nodes, 0, sizeof(layout_nodes));
    memset(&mouse_selection, 0, sizeof(mouse_selection));
    active_buffer_index = active_window_index = layout_root = 0;
    buffer_system_ready = pane_rendering = distraction_free = 0;
    center_lock_enabled = 0;
    layout_nodes[0] = (LayoutNode){
        .used = 1, .kind = LAYOUT_LEAF, .parent = -1, .window_index = 0
    };
    initialize_restored_buffer_slot(0);
    free(lines[0]);
    line_count = second ? 2 : 1;
    lines[0] = new_line(first);
    if (second)
        lines[1] = new_line(second);
    initialize_buffer_system();
    clear_cursor_affinity();
    reset_wrap_cache();
}

static void mouse(mmask_t state, int y, int x)
{
    MEVENT event = {.y = y, .x = x, .bstate = state};

    handle_editor_mouse(&event);
}

static void drag(int from_y, int from_x, int to_y, int to_x)
{
    mouse_selection.click_time = 0;
    mouse(BUTTON1_PRESSED, from_y, from_x);
    mouse(REPORT_MOUSE_POSITION, to_y, to_x);
    mouse(BUTTON1_RELEASED, to_y, to_x);
}

static void check_margins_and_rendering(void)
{
    const char *first =
        "    Keep the actual indentation,  two spaces, and a long paragraph "
        "that wraps across several screen rows without acquiring newlines.";
    const char *second = "\tKeep this tab, café, 世界, and the final space. ";
    char expected[1024];
    const int widths[] = {12, 80};
    const int columns[] = {20, 160, 240};

    snprintf(expected, sizeof(expected), "%s\n%s", first, second);
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (size_t c = 0; c < sizeof(columns) / sizeof(columns[0]); c++) {
            BodyGeometry geo;

            document(first, second);
            config.text_width = widths[w];
            assert(resizeterm(80, columns[c]) == OK);
            geo = body_geometry();
            drag(0, 0, 70, COLS - 1);
            assert(clip && strcmp(clip, expected) == 0);
            assert(!dirty && !autosave_dirty && undo_count == 0);
            assert(strcmp(lines[0], first) == 0);
            assert(strcmp(lines[1], second) == 0);

            draw_screen_impl(0);
            for (int y = geo.top_pad; y < geo.bottom; y++) {
                for (int x = 0; x < COLS; x++) {
                    if (x < geo.left || x >= geo.left + geo.body_width)
                        assert(!(mvinch(y, x) & A_REVERSE));
                }
            }
            assert(mvinch(geo.top_pad, geo.left) & A_REVERSE);
            drag(70, COLS - 1, 0, 0);
            assert(strcmp(clip, expected) == 0);
        }
    }
}

static void check_partial_ranges_and_clicks(void)
{
    BodyGeometry geo;

    assert(resizeterm(30, 160) == OK);
    config.text_width = 5;
    document("abcdefghijklmno", NULL);
    geo = body_geometry();
    drag(geo.top_pad, geo.left + 3, geo.top_pad + 2, geo.left + 1);
    assert(strcmp(clip, "defghijkl") == 0);
    drag(geo.top_pad + 2, geo.left + 1, geo.top_pad, geo.left + 3);
    assert(strcmp(clip, "defghijkl") == 0);

    config.text_width = 80;
    document("abc café 世界 xyz", "next paragraph");
    geo = body_geometry();
    /* Endpoints in either half of a wide glyph copy complete UTF-8. */
    drag(geo.top_pad, geo.left + 10, geo.top_pad, geo.left + 12);
    assert(strcmp(clip, "世界") == 0);
    drag(geo.top_pad, geo.left + 12, geo.top_pad, geo.left + 10);
    assert(strcmp(clip, "世界") == 0);

    mouse_selection.click_time = 0;
    mouse(BUTTON1_PRESSED, geo.top_pad, geo.left + 5);
    mouse(BUTTON1_RELEASED, geo.top_pad, geo.left + 5);
    assert(!selection_nonempty());
    assert(strcmp(clip, "世界") == 0); /* A click must not erase the clipboard. */
    mouse(BUTTON1_PRESSED, geo.top_pad, geo.left + 5);
    mouse(BUTTON1_RELEASED, geo.top_pad, geo.left + 5);
    assert(strcmp(clip, "café") == 0);
    mouse(BUTTON1_PRESSED, geo.top_pad, geo.left + 5);
    mouse(BUTTON1_RELEASED, geo.top_pad, geo.left + 5);
    assert(strcmp(clip, "abc café 世界 xyz\n") == 0);

    mouse_selection.click_time = 0;
    mouse(BUTTON1_PRESSED, geo.top_pad, geo.left);
    mouse(REPORT_MOUSE_POSITION, geo.top_pad, geo.left + 2);
    mouse(REPORT_MOUSE_POSITION, geo.top_pad, geo.left);
    mouse(BUTTON1_RELEASED, geo.top_pad, geo.left);
    assert(strcmp(clip, "a") == 0);
}

static void check_panes(void)
{
    const int layouts[] = {LAYOUT_SIDE_BY_SIDE, LAYOUT_ABOVE_BELOW};

    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        EditorRect rect;

        document("Left buffer must stay out of the clipboard", NULL);
        initialize_restored_buffer_slot(1);
        strcpy(editor_buffers[1].text_lines[0], "    Right café 世界");
        editor_windows[1] = (EditorWindow){
            .used = 1, .kind = EDITOR_WINDOW_DOCUMENT, .buffer_index = 1
        };
        layout_nodes[0] = (LayoutNode){
            .used = 1, .kind = layouts[i], .parent = -1,
            .first = 1, .second = 2, .ratio = 50
        };
        layout_nodes[1] = (LayoutNode){
            .used = 1, .kind = LAYOUT_LEAF, .parent = 0, .window_index = 0
        };
        layout_nodes[2] = (LayoutNode){
            .used = 1, .kind = LAYOUT_LEAF, .parent = 0, .window_index = 1
        };
        recompute_layout_rectangles();
        rect = editor_window_rects[1];
        drag(rect.y + 1, rect.x, rect.y + rect.height - 2,
             rect.x + rect.width - 1);
        assert(active_window_index == 1 && active_buffer_index == 1);
        assert(strcmp(clip, "    Right café 世界") == 0);
        assert(strcmp(editor_buffers[0].text_lines[0],
                      "Left buffer must stay out of the clipboard") == 0);

        /* Dragging across the divider cannot change the source document. */
        drag(rect.y + rect.height - 2, rect.x + rect.width - 1,
             rect.y + 1, 0);
        assert(active_buffer_index == 1);
        assert(strcmp(clip, "    Right café 世界") == 0);

        distraction_free = 1;
        drag(1, 0, LINES - 2, COLS - 1);
        assert(active_window_index == 1);
        assert(strcmp(clip, "    Right café 世界") == 0);
    }
}

static void check_scrolled_selection(void)
{
    BodyGeometry geo;

    document("line 00", NULL);
    assert(resizeterm(20, 160) == OK);
    for (int i = 1; i < 80; i++) {
        lines[i] = new_line("");
        snprintf(lines[i], MAX_LINE, "line %02d", i);
    }
    line_count = 80;
    reset_wrap_cache();
    cy = top = 30;
    center_lock_enabled = 1;
    geo = body_geometry();
    drag(geo.top_pad + 2, 0, geo.bottom, COLS - 1);
    assert(strncmp(clip, "line 32\nline 33\n", 16) == 0);
    assert(strstr(clip, "line 46") != NULL);
    assert(top == 31); /* No extra scrolling on release or recentering. */
    keep_cursor_visible();
    assert(top == 31);
}

int main(void)
{
    FILE *input = tmpfile();
    FILE *output = tmpfile();
    SCREEN *screen;

    assert(setlocale(LC_ALL, "C.UTF-8") || setlocale(LC_ALL, ""));
    assert(input && output);
    screen = newterm("xterm-256color", output, input);
    assert(screen);
    clip_backend = CLIP_BACKEND_NONE;
    clip_warned = 1;
    check_margins_and_rendering();
    check_partial_ranges_and_clicks();
    check_panes();
    check_scrolled_selection();
    for (int i = 0; i < MAX_BUFFERS; i++)
        free_buffer_storage(i);
    reset_wrap_cache();
    free(wrap_cache);
    free(clip);
    endwin();
    delscreen(screen);
    fclose(input);
    fclose(output);
    puts("simplewords mouse selection and clipboard checks passed");
    return 0;
}
