/* Use the real retention algorithm with a smaller budget for boundary tests. */
#define UNDO_BYTE_LIMIT (1024u * 1024u)
#define SIMPLEWORDS_UNDO_TEST
#define SIMPLEWORDS_TYPEWRITER_TEST
#define main simplewords_program_main
#include "../simplewords.c"
#undef main

#include <assert.h>

static void fixture(const char *text)
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
    assert(replace_range_raw(0, 0, 0, 0, text, NULL));
    cy = cx = top = 0;
    SET_DIRTY(0, "test fixture");
    clear_undo_history();
    initialize_buffer_system();
    reset_wrap_cache();
    undo_test_time_ms = 1000;
}

static void expect_text(const char *expected)
{
    char *actual = range_text(0, 0, line_count - 1,
                              (int)strlen(lines[line_count - 1]));

    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "Expected: %.200s\nActual: %.200s\n", expected, actual);
        abort();
    }
    free(actual);
    assert(cy >= 0 && cy < line_count);
    assert(cx >= 0 && cx <= (int)strlen(lines[cy]));
}

static void type_text(const char *text)
{
    while (*text) {
        assert(insert_char((unsigned char)*text++));
        undo_test_time_ms += 20;
    }
}

static void select_range(int sy, int sx, int ey, int ex, int backwards)
{
    break_undo_burst();
    selecting = 1;
    sel_cy = backwards ? ey : sy;
    sel_cx = backwards ? ex : sx;
    cy = backwards ? sy : ey;
    cx = backwards ? sx : ex;
}

static void select_all(void)
{
    select_range(0, 0, line_count - 1,
                 (int)strlen(lines[line_count - 1]), 0);
}

static void expect_selection(int sy, int sx, int ey, int ex, int backwards)
{
    assert(selecting);
    assert(sel_cy == (backwards ? ey : sy));
    assert(sel_cx == (backwards ? ex : sx));
    assert(cy == (backwards ? sy : ey));
    assert(cx == (backwards ? sx : ex));
}

static void check_typing_runs(void)
{
    char long_word[301];

    fixture("");
    type_text("Hello world");
    assert(undo_count == 1 && pending_undo_group.op_count == 1);
    assert(strcmp(pending_undo_group.ops[0].new_text, "world") == 0);
    do_undo();
    expect_text("Hello ");
    assert(cx == 6);
    do_undo();
    expect_text("");
    assert(!dirty);
    do_redo();
    expect_text("Hello ");
    do_redo();
    expect_text("Hello world");
    assert(cx == 11 && dirty);

    fixture("");
    memset(long_word, 'a', sizeof(long_word) - 1);
    long_word[sizeof(long_word) - 1] = '\0';
    type_text(long_word);
    assert(undo_count == 0 && pending_undo_group.op_count == 1);
    undo_test_time_ms += UNDO_PAUSE_MS;
    type_text("x");
    do_undo();
    expect_text(long_word);
    do_undo();
    expect_text("");

    /* A pause between UTF-8 bytes must never create half a character in undo. */
    fixture("");
    assert(insert_char(0xc3));
    undo_test_time_ms += UNDO_PAUSE_MS * 2;
    assert(insert_char(0xa9));
    expect_text("é");
    do_undo();
    expect_text("");
    do_redo();
    expect_text("é");

    fixture("");
    type_text("abc");
    move_left(0);
    move_right(0);
    type_text("d");
    move_left(0);
    do_undo();
    expect_text("abc");
    assert(cx == 3);
    move_visual_home(0);
    do_redo();
    expect_text("abcd");
    assert(cx == 4); /* Navigation before redo cannot overwrite edit context. */
}

static void check_deletion_runs(void)
{
    fixture("one two");
    cx = 7;
    for (int i = 0; i < 4; i++)
        assert(backspace());
    assert(pending_undo_group.op_count == 1);
    expect_text("one");
    assert(backspace());
    do_undo();
    expect_text("one");
    do_undo();
    expect_text("one two");
    assert(cx == 7);
    do_redo();
    expect_text("one");

    fixture("one two");
    for (int i = 0; i < 4; i++)
        assert(delete_forward());
    expect_text("two");
    assert(delete_forward());
    do_undo();
    expect_text("two");
    do_undo();
    expect_text("one two");
    assert(cx == 0);

    fixture("café 世界🙂");
    cx = (int)strlen(lines[0]);
    assert(backspace());
    assert(backspace());
    assert(backspace());
    expect_text("café ");
    do_undo();
    expect_text("café 世界🙂");
    move_visual_home(0);
    for (int i = 0; i < 4; i++)
        assert(delete_forward());
    expect_text(" 世界🙂");
    do_undo();
    expect_text("café 世界🙂");

    fixture("abc\ndef");
    cy = 1;
    cx = 1;
    assert(backspace());
    assert(backspace());
    assert(backspace());
    expect_text("abef");
    do_undo();
    expect_text("abcef");
    do_undo();
    expect_text("abc\nef");
    do_undo();
    expect_text("abc\ndef");

    fixture("abc");
    cx = 2;
    assert(backspace());
    assert(delete_forward());
    do_undo();
    expect_text("ac");
    do_undo();
    expect_text("abc");
    assert(cx == 2);
    assert(backspace());
    undo_test_time_ms += UNDO_PAUSE_MS;
    assert(backspace());
    do_undo();
    expect_text("ac");
}

static void check_selection_replacement(void)
{
    const char *original = "old words\nnext paragraph";

    for (int backwards = 0; backwards < 2; backwards++) {
        fixture(original);
        select_range(0, 0, 0, 3, backwards);
        type_text("new");
        break_undo_burst();
        assert(undo_count == 1 && undo_stack[0].op_count == 1);
        assert(undo_stack[0].kind == UNDO_REPLACE);
        expect_text("new words\nnext paragraph");
        assert(!selecting);
        do_undo();
        expect_text(original);
        expect_selection(0, 0, 0, 3, backwards);
        do_redo();
        expect_text("new words\nnext paragraph");
        assert(!selecting && cy == 0 && cx == 3);

        for (int action = 0; action < 4; action++) {
            fixture(original);
            select_range(0, 0, 1, 14, backwards);
            if (action == 0)
                assert(keyboard_newline());
            else if (action == 1)
                assert(keyboard_tab());
            else if (action == 2)
                assert(insert_pasted_text("café\r\n\t世界"));
            else
                cut_selection();
            assert(undo_count == 1 && undo_stack[0].op_count == 1);
            assert(!selecting);
            expect_text(action == 0 ? "\n" : action == 1 ? "\t" :
                        action == 2 ? "café\n\t世界" : "");
            do_undo();
            expect_text(original);
            expect_selection(0, 0, 1, 14, backwards);
            assert(!dirty);
            do_redo();
            assert(!selecting);
        }
    }
    fixture("abc");
    select_range(0, 1, 0, 1, 0);
    assert(backspace()); /* An empty selection still allows ordinary deletion. */
    expect_text("bc");
}

static void check_noops_and_failures(void)
{
    char *long_text = xmalloc(MAX_LINE + 6);
    uint64_t revision;

    fixture("same");
    cx = 4;
    type_text("x");
    do_undo();
    revision = edit_revision;
    select_all();
    assert(insert_pasted_text("same"));
    assert(redo_count == 1 && undo_count == 0 && edit_revision == revision);
    assert(!dirty && !selecting);
    do_redo();
    expect_text("samex");

    memcpy(long_text, "left\n", 5);
    memset(long_text + 5, 'x', MAX_LINE - 1);
    long_text[MAX_LINE + 4] = '\0';
    fixture(long_text);
    type_text("?");
    do_undo();
    select_range(0, 4, 1, 0, 0);
    assert(!insert_char('Y'));
    expect_text(long_text);
    expect_selection(0, 4, 1, 0, 0);
    assert(redo_count == 1 && undo_count == 0 && !dirty);
    assert(keyboard_newline() && !dirty); /* Same newline is a harmless no-op. */
    clear_selection();
    cy = 1;
    cx = 0;
    assert(!backspace());
    expect_text(long_text);
    assert(redo_count == 1 && !dirty);
    do_redo();
    assert(lines[0][0] == '?');
    free(long_text);

    /* Reject excessive paragraph counts before deleting the selection. */
    fixture("keep me");
    select_all();
    long_text = xmalloc(MAX_LINES + 1);
    memset(long_text, '\n', MAX_LINES);
    long_text[MAX_LINES] = '\0';
    assert(!insert_pasted_text(long_text));
    expect_text("keep me");
    expect_selection(0, 0, 0, 7, 0);
    assert(!dirty && !undo_count);
    free(long_text);
}

static void check_compound_replay_is_atomic(void)
{
    fixture("ab");
    begin_undo_group();
    assert(replace_range_recorded(0, 0, 0, 1, "X"));
    assert(replace_range_recorded(0, 1, 0, 2, "Y"));
    mark_edit();
    end_undo_group();
    assert(undo_count == 1 && undo_stack[0].op_count == 2);
    select_all();
    undo_stack[0].ops[0].new_text[0] = '!';
    do_undo(); /* First inverse succeeds; the second deliberately mismatches. */
    expect_text("XY");
    expect_selection(0, 0, 0, 2, 0);
    assert(undo_count == 1 && redo_count == 0 && dirty);
    undo_stack[0].ops[0].new_text[0] = 'X';
    do_undo();
    expect_text("ab");
    assert(!dirty && !selecting);
    redo_stack[0].ops[1].old_text[0] = '!';
    do_redo();
    expect_text("ab");
    assert(!dirty && undo_count == 0 && redo_count == 1);
    redo_stack[0].ops[1].old_text[0] = 'b';
    do_redo();
    expect_text("XY");
}

static void check_saved_state(const char *test_dir)
{
    char path[PATH_MAX];
    char failed_path[PATH_MAX];
    uint64_t savepoint;

    assert(format_string(path, sizeof(path), "%s/document.txt", test_dir));
    assert(format_string(failed_path, sizeof(failed_path),
                         "%s/missing/document.txt", test_dir));
    fixture("");
    type_text("first");
    assert(save_document_to_path(path));
    assert(!dirty && undo_count == 1);
    savepoint = saved_revision;
    type_text("X");
    assert(autosave_file_now());
    do_undo();
    expect_text("first");
    assert(!dirty && autosave_dirty && saved_revision == savepoint);
    assert(autosave_file_now());
    do_undo();
    expect_text("");
    assert(dirty);
    do_redo();
    expect_text("first");
    assert(!dirty);
    do_redo();
    expect_text("firstX");
    assert(dirty);
    do_undo();
    do_undo();
    type_text("other");
    assert(dirty && redo_count == 0 && edit_revision != savepoint);
    assert(!save_document_to_path(failed_path));
    assert(dirty && saved_revision == savepoint);
    assert(save_document_to_path(path));
    assert(!dirty);
    do_undo();
    assert(dirty);
    do_redo();
    assert(!dirty);

    /* An external disk edit invalidates the old savepoint, not our history. */
    type_text("x");
    FILE *external = fopen(path, "w");
    assert(external);
    assert(fputs("external change", external) >= 0);
    assert(fclose(external) == 0);
    do_undo();
    expect_text("other");
    assert(dirty);

    fixture("recovered draft");
    SET_DIRTY(1, "recovery test");
    type_text("x");
    do_undo();
    expect_text("recovered draft");
    assert(dirty); /* A recovered draft is not its saved disk baseline. */
}

static void check_buffer_and_window_boundaries(void)
{
    int second;

    fixture("");
    type_text("A");
    create_blank_buffer();
    second = active_buffer_index;
    type_text("B");
    select_buffer_in_active_window(0);
    type_text("C");
    do_undo();
    expect_text("A");
    assert(strcmp(editor_buffers[second].text_lines[0], "B") == 0);
    do_undo();
    expect_text("");
    select_buffer_in_active_window(second);
    do_undo();
    expect_text("");
    do_redo();
    expect_text("B");

    fixture("");
    type_text("A");
    assert(split_editor_window(LAYOUT_SIDE_BY_SIDE));
    type_text("B");
    select_other_editor_window();
    type_text("C");
    do_undo();
    expect_text("AB");
    do_undo();
    expect_text("A");
    do_undo();
    expect_text("");
}

static char *large_document(size_t length, char letter)
{
    char *text = xmalloc(length + 1);

    for (size_t i = 0; i < length; i++)
        text[i] = i % 1000 == 999 ? '\n' : letter;
    text[length] = '\0';
    return text;
}

static void check_retention_limits(void)
{
    char *first;
    char *second;
    int retained;

    fixture("");
    for (int i = 0; i < UNDO_DEPTH + 30; i++) {
        type_text("x");
        break_undo_burst();
    }
    assert(undo_count == UNDO_DEPTH);
    for (int i = 0; i < UNDO_DEPTH; i++)
        do_undo();
    assert(strlen(lines[0]) == 30 && !undo_count && dirty);
    do_undo();
    assert(strlen(lines[0]) == 30);
    for (int i = 0; i < UNDO_DEPTH; i++)
        do_redo();
    assert(strlen(lines[0]) == UNDO_DEPTH + 30);

    first = large_document(UNDO_BYTE_LIMIT / 10, 'a');
    second = large_document(UNDO_BYTE_LIMIT / 10, 'b');
    fixture(first);
    for (int i = 0; i < 12; i++) {
        select_all();
        assert(insert_pasted_text(i % 2 ? first : second));
    }
    assert(undo_count > 0 && undo_count < 12);
    assert(undo_stack_retained_bytes(undo_stack, undo_count) <= UNDO_BYTE_LIMIT);
    retained = undo_count;
    for (int i = 0; i < retained; i++) {
        do_undo();
        expect_text(i % 2 ? first : second);
    }
    assert(!undo_count);
    free(first);
    free(second);

    first = large_document(UNDO_BYTE_LIMIT * 3 / 5, 'a');
    second = large_document(UNDO_BYTE_LIMIT * 3 / 5, 'b');
    fixture(first);
    type_text("x");
    do_undo();
    select_all();
    assert(!insert_pasted_text(second));
    expect_text(first);
    assert(selecting && !dirty && redo_count == 1);
    do_redo();
    assert(lines[0][0] == 'x');
    free(first);
    free(second);
}

static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (!directory) {
        assert(unlink(path) == 0);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        assert(format_string(child, sizeof(child), "%s/%s", path, entry->d_name));
        remove_tree(child);
    }
    assert(closedir(directory) == 0);
    assert(rmdir(path) == 0);
}

int main(void)
{
    char test_dir[] = "/tmp/simplewords-undo-test.XXXXXX";

    assert(mkdtemp(test_dir));
    assert(setenv("HOME", test_dir, 1) == 0);
    assert(setlocale(LC_ALL, "C.UTF-8"));
    clip_backend = CLIP_BACKEND_NONE;
    LINES = 30;
    COLS = 120;
    check_typing_runs();
    check_deletion_runs();
    check_selection_replacement();
    check_noops_and_failures();
    check_compound_replay_is_atomic();
    check_saved_state(test_dir);
    check_buffer_and_window_boundaries();
    check_retention_limits();
    for (int i = 0; i < MAX_BUFFERS; i++)
        free_buffer_storage(i);
    reset_wrap_cache();
    free(wrap_cache);
    free(clip);
    remove_tree(test_dir);
    puts("SimpleWords undo grouping, selection, savepoint, failure and retention checks passed");
    return 0;
}
