/* Compile once for each application and compare each visible editing step. */
#ifdef SURFACE_NOTE
#define main simplenote_program_main
#include "../simplenote.c"
#undef main
static NoteApp surface_note;
#else
#define SIMPLEWORDS_TYPEWRITER_TEST
#define main simplewords_program_main
#ifdef SURFACE_REFERENCE
#include "../build/text-surface-reference.c"
#else
#include "../simplewords.c"
#endif
#undef main
#endif
#include <assert.h>

static void print_hex(const char *text)
{
    for (; text && *text; text++) printf("%02x", (unsigned char)*text);
    putchar('|');
}

static char *read_hex(const char *text)
{
    size_t length = strcspn(text, "\r\n");
    assert(!(length % 2));
    char *out = calloc(length / 2 + 1, 1); assert(out);
    for (size_t i = 0; i < length; i += 2) {
        unsigned int byte; assert(sscanf(text + i, "%2x", &byte) == 1);
        out[i / 2] = (char)byte;
    }
    return out;
}

static void fixture(const char *text, int rows, int cols)
{
    assert(resizeterm(rows, cols) == OK);
#ifdef SURFACE_NOTE
    NoteApp *a = &surface_note;
    assert(open_editor(a, NULL));
    free(a->edit.text); a->edit.text = strdup(text); assert(a->edit.text);
    a->edit.len = strlen(text); a->cursor = 0;
    free(a->kill); a->kill = NULL;
    a->terminal_focused = 1; a->refocus_until = a->refocus_release = 0;
#else
    for (int i = 0; i < MAX_BUFFERS; i++) free_buffer_storage(i);
    memset(editor_windows, 0, sizeof editor_windows);
    memset(layout_nodes, 0, sizeof layout_nodes);
    memset(&mouse_selection, 0, sizeof mouse_selection);
    active_buffer_index = active_window_index = layout_root = 0;
    buffer_system_ready = pane_rendering = distraction_free = center_lock_enabled = 0;
    layout_nodes[0] = (LayoutNode){.used = 1, .kind = LAYOUT_LEAF, .parent = -1, .window_index = 0};
    initialize_restored_buffer_slot(0);
    assert(replace_range_raw(0, 0, 0, 0, text, NULL));
    cy = cx = top = 0; goal_col = -1; clear_cursor_affinity(); SET_DIRTY(0, "surface fixture");
    clear_undo_history(); initialize_buffer_system(); reset_wrap_cache();
    free(clip); clip = NULL;
    terminal_focused = 1; refocus_click_until = refocus_mouse_release = 0;
#endif
}

static void surface_move(int action, int extend)
{
#ifdef SURFACE_NOTE
    const int normal[] = {0, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PPAGE, KEY_NPAGE};
    const int shifted[] = {0, KEY_SLEFT, KEY_SRIGHT, KEY_SR, KEY_SF, KEY_HOME, KEY_END, SN_PAGE_UP, SN_PAGE_DOWN};
    compose_key(&surface_note, (wint_t)(extend ? shifted[action] : normal[action]), 1);
#else
    mouse_selection.dragging = mouse_selection.hold_view = 0;
    switch (action) {
    case 1: move_left(extend); break;
    case 2: move_right(extend); break;
    case 3: move_visual_line(-1, extend); break;
    case 4: move_visual_line(1, extend); break;
    case 5: move_visual_home(extend); break;
    case 6: move_visual_end(extend); break;
    case 7: move_page(-1, extend); break;
    case 8: move_page(1, extend); break;
    }
#endif
}

static void edit_key(char key)
{
#ifdef SURFACE_NOTE
    NoteApp *a = &surface_note;
    a->mouse.dragging = a->mouse.hold_view = 0; a->mouse.clicked_at = 0;
    switch (key) {
    case 'B': compose_key(a, KEY_BACKSPACE, 1); break;
    case 'D': compose_key(a, KEY_DC, 1); break;
    case 'U': compose_key(a, CTRL('Z'), 0); break;
    case 'R': compose_key(a, CTRL('R'), 0); break;
    case 'X': compose_key(a, CTRL('W'), 0); break;
    case 'Y': compose_key(a, CTRL('Y'), 0); break;
    case 'C': a->run_at = 0; copy_editor_selection(a, 0); break;
    }
#else
    mouse_selection.dragging = mouse_selection.hold_view = 0;
    mouse_selection.click_time = 0;
    switch (key) {
    case 'B': keyboard_backspace(); break;
    case 'D': keyboard_delete_forward(); break;
    case 'U': do_undo(); break;
    case 'R': do_redo(); break;
    case 'X': cut_selection(); break;
    case 'Y': paste_clipboard(); break;
    case 'C': break_undo_burst(); copy_selection(); break;
    }
#endif
}

static void type_text(char *text, int pasted)
{
#ifdef SURFACE_NOTE
    surface_note.mouse.dragging = surface_note.mouse.hold_view = 0;
    surface_note.mouse.clicked_at = 0;
    if (pasted) paste_text(&surface_note, text);
    else for (size_t i = 0, length = strlen(text); i < length; ) {
        wchar_t wc; int used;
        sst_decode(text + i, length - i, &wc, &used);
        compose_key(&surface_note, (wint_t)wc, 0); i += (size_t)used;
    }
#else
    mouse_selection.dragging = mouse_selection.hold_view = 0;
    mouse_selection.click_time = 0;
    if (pasted) insert_pasted_text(text);
    else for (; *text; text++) {
        if (*text == '\n') keyboard_newline();
        else if (*text == '\t') keyboard_tab();
        else insert_char((unsigned char)*text);
    }
#endif
}

static void mouse_input(char action, int y, int x)
{
    mmask_t state = action == 'p' ? BUTTON1_PRESSED :
        action == 'r' ? BUTTON1_RELEASED : action == 'd' ? REPORT_MOUSE_POSITION :
        action == 'm' ? BUTTON2_PRESSED : action == 'u' ? BUTTON4_PRESSED : BUTTON5_PRESSED;
    MEVENT event = {.bstate = state, .x = x, .y = y};
#ifdef SURFACE_NOTE
    if (!ignore_refocus_mouse(&surface_note, &event)) editor_mouse(&surface_note, &event);
#else
    if (!ignore_refocus_mouse(&event)) handle_editor_mouse(&event);
#endif
}

static unsigned long long hash_cell(unsigned long long hash, wchar_t wc, int kind, attr_t attr)
{
    hash = (hash ^ (unsigned int)wc) * 1099511628211ULL;
    hash = (hash ^ (unsigned int)kind) * 1099511628211ULL;
    return (hash ^ !!(attr & A_REVERSE)) * 1099511628211ULL;
}

#ifndef SURFACE_NOTE
static size_t offset(int y, int x)
{
    size_t result = (size_t)x;
    for (int i = 0; i < y; i++) result += strlen(lines[i]) + 1;
    return result;
}
#endif

static void snapshot(void)
{
    unsigned long long hash = 1469598103934665603ULL;
#ifdef SURFACE_NOTE
    NoteApp *a = &surface_note;
    assert(editor_rows(a, editor_width()));
    int col, row = (int)cursor_row(a, &col);
    keep_editor_cursor_visible(a, row, col);
    int width = editor_width(), height = editor_height();
    draw_editor(a);
    for (int i = 0; i < width * height; i++) {
        SsrCell *cell = &a->renderer.desired_cells[i];
        hash = hash_cell(hash, cell->text[0], cell->kind, cell->attr);
    }
    printf("%zu,%zu,%d,%d,%d,%d,%d,%d,%llu|", a->cursor, a->selection ? a->anchor : 0,
           a->selection, row, col, a->preferred_col, a->scroll,
           !a->selection || a->cursor == a->anchor, hash);
    print_hex(a->edit.text); print_hex(a->kill);
#else
    keep_cursor_visible();
    int row, col; cursor_visual_pos(&row, &col);
    BodyGeometry geo = body_geometry(); ensure_screen_storage(geo.body_width);
    build_desired_body_cells(geo.body_width);
    for (int y = geo.top_pad; y < geo.bottom; y++) for (int x = 0; x < geo.body_width; x++) {
        ScreenCell *cell = &desired_cells[y * geo.body_width + x];
        hash = hash_cell(hash, cell->wc, cell->kind, cell->attr);
    }
    printf("%zu,%zu,%d,%d,%d,%d,%d,%d,%llu|", offset(cy, cx), selecting ? offset(sel_cy, sel_cx) : 0,
           selecting, row, col, goal_col, top, editor_cursor_visibility(), hash);
    char *text = range_text(0, 0, line_count - 1, (int)strlen(lines[line_count - 1]));
    print_hex(text); print_hex(clip); free(text);
#endif
    putchar('\n');
}

int main(void)
{
    assert(setlocale(LC_ALL, "C.UTF-8"));
    unsetenv("DISPLAY"); unsetenv("WAYLAND_DISPLAY");
    FILE *in = tmpfile(), *out = tmpfile(); assert(in && out);
    SCREEN *screen = newterm("xterm-256color", out, in); assert(screen); set_term(screen);
#ifdef SURFACE_NOTE
    ssr_init(&surface_note.renderer);
#endif
    char *command = NULL; size_t capacity = 0;
    while (getline(&command, &capacity, stdin) > 0) {
        int first, second, consumed;
        if (command[0] == 'F') {
            assert(sscanf(command + 1, "%d %d %n", &first, &second, &consumed) == 2);
            char *text = read_hex(command + 1 + consumed); fixture(text, first, second); free(text);
        } else if (command[0] == 'K') {
            assert(sscanf(command + 1, "%d %d", &first, &second) == 2); surface_move(first, second);
        } else if (command[0] == 'T' || command[0] == 'P') {
            char *text = read_hex(command + 2); type_text(text, command[0] == 'P'); free(text);
        } else if (command[0] == 'M') {
            char action; assert(sscanf(command + 1, " %c %d %d", &action, &first, &second) == 3);
            mouse_input(action, first, second);
        } else if (command[0] == 'H') {
            assert(sscanf(command + 1, "%d", &first) == 1);
#ifdef SURFACE_NOTE
            terminal_focus(&surface_note, first);
#else
            update_terminal_focus(first);
#endif
        } else if (command[0] == 'Z') {
            assert(sscanf(command + 1, "%d %d", &first, &second) == 2);
            assert(resizeterm(first, second) == OK);
        } else edit_key(command[0]);
        snapshot();
    }
    free(command);
#ifdef SURFACE_NOTE
    undo_clear(&surface_note); free(surface_note.edit.text); free(surface_note.rows); free(surface_note.kill);
    ssr_destroy(&surface_note.renderer);
#else
    for (int i = 0; i < MAX_BUFFERS; i++) free_buffer_storage(i);
    reset_wrap_cache(); free(wrap_cache); free(clip);
    free(screen_cells); free(desired_cells); free(desired_rows);
#endif
    endwin(); delscreen(screen); fclose(in); fclose(out);
    return 0;
}
