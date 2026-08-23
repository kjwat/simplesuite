#define SIMPLEWORDS_TYPEWRITER_TEST
#define main simplewords_program_main
#include "../simplewords.c"
#undef main

#include <assert.h>

static void write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    assert(fp);
    assert(fputs(text, fp) >= 0);
    assert(fclose(fp) == 0);
}

static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);

    if (directory) {
        struct dirent *entry;

        while ((entry = readdir(directory)) != NULL) {
            char child[PATH_MAX];

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            assert(snprintf(child, sizeof(child), "%s/%s", path,
                            entry->d_name) > 0);
            remove_tree(child);
        }
        closedir(directory);
        assert(rmdir(path) == 0);
    } else {
        assert(unlink(path) == 0);
    }
}

static void strip_workspace_histories(const char *path)
{
    char temporary[PATH_MAX];
    char line[4096];
    int found = 0;
    int fd;
    FILE *input = fopen(path, "r");
    FILE *output;

    assert(input);
    assert(snprintf(temporary, sizeof(temporary), "%s.old.XXXXXX", path) > 0);
    fd = mkstemp(temporary);
    assert(fd >= 0);
    output = fdopen(fd, "w");
    assert(output);
    while (fgets(line, sizeof(line), input)) {
        if (strncmp(line, "HISTORY_COUNT ", 14) == 0) {
            assert(fputs("END\n", output) >= 0);
            found = 1;
            break;
        }
        assert(fputs(line, output) >= 0);
    }
    assert(found);
    assert(fclose(input) == 0);
    assert(fclose(output) == 0);
    assert(rename(temporary, path) == 0);
}

static int window_count_for_test(void)
{
    int count = 0;

    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++)
        if (editor_windows[i].used)
            count++;
    return count;
}

static int find_untitled_with_text(const char *text)
{
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (editor_buffers[i].used && !editor_buffers[i].path[0] &&
            editor_buffers[i].text_line_count > 0 &&
            strcmp(editor_buffers[i].text_lines[0], text) == 0)
            return i;
    }
    return -1;
}

static int buffer_shelf_window_for_test(void)
{
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++)
        if (editor_windows[i].used &&
            editor_windows[i].kind == EDITOR_WINDOW_BUFFER_SHELF)
            return i;
    return -1;
}

static void check_terminal_disconnect_detection(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    int slave;
    char *slave_name;

    assert(master >= 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    slave_name = ptsname(master);
    assert(slave_name);
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    assert(slave >= 0);
    assert(!terminal_input_disconnected(slave));
    close(master);
    assert(terminal_input_disconnected(slave));
    close(slave);
}

int main(void)
{
    char home[] = "/tmp/simplewords-buffers-test.XXXXXX";
    char first_path[PATH_MAX];
    char second_path[PATH_MAX];
    char command_path[PATH_MAX];
    char session_file[PATH_MAX];
    char lock_directory[PATH_MAX];
    char lock_path[PATH_MAX];
    char header[128];
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    int first;
    int second;
    int draft;
    int document_window;
    int shelf_window;
    int history_windows[MAX_EDITOR_WINDOWS];
    int history_window_count;
    int conflict_fd;
    FILE *fp;

    check_terminal_disconnect_detection();
    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(snprintf(first_path, sizeof(first_path), "%s/canon.txt", home) > 0);
    assert(snprintf(second_path, sizeof(second_path), "%s/notes.txt", home) > 0);
    assert(snprintf(command_path, sizeof(command_path), "%s/fuckyou.txt", home) > 0);
    write_text(first_path, "the canon\n");
    write_text(second_path, "the notes\n");
    write_text(command_path, "from the command line\n");

    LINES = 30;
    COLS = 120;
    make_untitled_name();
    lines[0] = new_line("");
    initialize_buffer_system();
    acquire_workspace_lock();
    assert(workspace_session_owner);

    assert(load_file_at_position(first_path, 1, 0, 0, 0, 0) ==
           LOAD_RESULT_DISK);
    mark_active_buffer_used();
    save_active_window_view();
    visit_file_in_buffer(second_path);
    assert(buffer_count() == 2);
    assert(strcmp(filename, second_path) == 0);
    first = buffer_index_for_path(first_path);
    second = buffer_index_for_path(second_path);
    assert(first >= 0 && second >= 0 && first != second);

    create_blank_buffer();
    assert(buffer_count() == 3);
    draft = active_buffer_index;
    assert(!filename[0]);
    strcpy(lines[0], "moleskin thought");
    mark_edit();
    autosave_file_now();
    assert(!autosave_dirty);

    split_editor_window(LAYOUT_SIDE_BY_SIDE);
    assert(layout_nodes[layout_root].kind == LAYOUT_SIDE_BY_SIDE);
    assert(window_count_for_test() == 2);
    select_other_editor_window();
    select_buffer_in_active_window(first);
    document_window = active_window_index;
    show_buffer_shelf_window();
    assert(active_window_index == document_window);
    assert(window_count_for_test() == 3);
    shelf_window = buffer_shelf_window_for_test();
    assert(shelf_window >= 0);
    recompute_layout_rectangles();
    assert(editor_window_rects[shelf_window].x >
           editor_window_rects[document_window].x);
    close_buffer_shelf_window(shelf_window);
    assert(active_window_index == document_window);
    assert(buffer_shelf_window_for_test() < 0);
    show_buffer_shelf_window();
    assert(active_window_index == document_window);
    shelf_window = buffer_shelf_window_for_test();
    assert(shelf_window >= 0);
    assert(editor_windows[shelf_window].previous_buffer_count == 0);
    assert(editor_windows[shelf_window].next_buffer_count == 0);
    load_editor_window(shelf_window);
    visit_file_in_buffer(command_path);
    assert(active_window_index == shelf_window);
    assert(!active_window_is_buffer_shelf());
    assert(editor_windows[shelf_window].previous_buffer_count == 0);
    assert(editor_windows[shelf_window].next_buffer_count == 0);
    assert(buffer_index_for_path(command_path) == active_buffer_index);
    kill_buffer_index(active_buffer_index);
    assert(buffer_index_for_path(command_path) < 0);
    assert(window_count_for_test() == 2);
    assert(!editor_windows[shelf_window].used);
    assert(active_window_index == document_window);

    show_buffer_shelf_window();
    assert(active_window_index == document_window);
    shelf_window = buffer_shelf_window_for_test();
    assert(shelf_window >= 0);
    save_session();

    assert(workspace_session_path(session_file, sizeof(session_file)));
    fp = fopen(session_file, "r");
    assert(fp);
    assert(fgets(header, sizeof(header), fp));
    assert(strncmp(header, SESSION_V2_MAGIC, strlen(SESSION_V2_MAGIC)) == 0);
    assert(fclose(fp) == 0);
    strip_workspace_histories(session_file);

    workspace_session_owner = 0;
    assert(load_session());
    workspace_session_owner = 1;
    assert(buffer_count() == 3);
    assert(window_count_for_test() == 3);
    assert(layout_nodes[layout_root].kind == LAYOUT_SIDE_BY_SIDE);
    assert(buffer_index_for_path(first_path) >= 0);
    assert(buffer_index_for_path(second_path) >= 0);
    assert(find_untitled_with_text("moleskin thought") >= 0);

    shelf_window = buffer_shelf_window_for_test();
    assert(shelf_window >= 0);
    for (int attempts = 0;
         active_window_index != shelf_window && attempts < MAX_EDITOR_WINDOWS;
         attempts++)
        select_other_editor_window();
    assert(active_window_index == shelf_window);
    assert(active_window_is_buffer_shelf());
    handle_buffer_shelf_key(27);
    assert(buffer_shelf_window_for_test() < 0);
    assert(!active_window_is_buffer_shelf());
    assert(window_count_for_test() == 2);

    first = buffer_index_for_path(first_path);
    second = buffer_index_for_path(second_path);
    draft = find_untitled_with_text("moleskin thought");
    assert(first >= 0 && second >= 0 && draft >= 0);
    history_window_count = 0;
    collect_layout_windows(layout_root, history_windows,
                           &history_window_count);
    assert(history_window_count == 2);

    load_editor_window(history_windows[0]);
    select_buffer_in_active_window(draft);
    editor_windows[active_window_index].previous_buffer_count = 0;
    editor_windows[active_window_index].next_buffer_count = 0;
    cy = 0;
    cx = 3;
    top = 0;
    save_active_window_view();
    select_buffer_in_active_window(first);

    load_editor_window(history_windows[1]);
    select_buffer_in_active_window(second);
    editor_windows[active_window_index].previous_buffer_count = 0;
    editor_windows[active_window_index].next_buffer_count = 0;
    cy = 0;
    cx = 7;
    top = 0;
    save_active_window_view();
    select_buffer_in_active_window(first);
    save_session();

    workspace_session_owner = 0;
    assert(load_session());
    workspace_session_owner = 1;
    first = buffer_index_for_path(first_path);
    second = buffer_index_for_path(second_path);
    draft = find_untitled_with_text("moleskin thought");
    assert(first >= 0 && second >= 0 && draft >= 0);
    history_window_count = 0;
    collect_layout_windows(layout_root, history_windows,
                           &history_window_count);
    assert(history_window_count == 2);
    assert(editor_windows[history_windows[0]].buffer_index == first);
    assert(editor_windows[history_windows[1]].buffer_index == first);
    assert(editor_windows[history_windows[0]].previous_buffer_count == 1);
    assert(editor_windows[history_windows[0]].previous_buffers[0].buffer_index ==
           draft);
    assert(editor_windows[history_windows[1]].previous_buffer_count == 1);
    assert(editor_windows[history_windows[1]].previous_buffers[0].buffer_index ==
           second);

    kill_buffer_index(first);
    assert(buffer_count() == 2);
    assert(buffer_index_for_path(first_path) < 0);
    assert(editor_windows[history_windows[0]].buffer_index == draft);
    assert(editor_windows[history_windows[0]].cursor_x == 3);
    assert(editor_windows[history_windows[1]].buffer_index == second);
    assert(editor_windows[history_windows[1]].cursor_x == 7);

    load_editor_window(history_windows[1]);
    select_buffer_in_active_window(draft);
    cx = 5;
    save_active_window_view();
    cycle_editor_buffer(-1);
    assert(active_buffer_index == second);
    assert(cx == 7);
    cycle_editor_buffer(1);
    assert(active_buffer_index == draft);
    assert(cx == 5);

    split_editor_window(LAYOUT_ABOVE_BELOW);
    assert(window_count_for_test() == 3);
    delete_editor_window();
    assert(window_count_for_test() == 2);
    delete_other_editor_windows();
    assert(window_count_for_test() == 1);
    assert(buffer_count() == 2);

    second = buffer_index_for_path(second_path);
    select_buffer_in_active_window(second);
    assert(simplewords_lock_dir(lock_directory, sizeof(lock_directory)));
    assert(format_string(lock_path, sizeof(lock_path), "%s/%016llx.lock",
                         lock_directory, path_hash(filename)));
    conflict_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    assert(conflict_fd >= 0);
    assert(flock(conflict_fd, LOCK_EX | LOCK_NB) == 0);
    assert(!ensure_document_edit_lock());
    assert(editor_buffers[second].lock_blocked);
    assert(flock(conflict_fd, LOCK_UN) == 0);
    close(conflict_fd);
    assert(ensure_document_edit_lock());
    release_document_edit_lock(&editor_buffers[second]);

    write_text(second_path, "changed by somebody else and longer\n");
    strcpy(lines[0], "our stale edit");
    mark_edit();
    assert(!save_document_to_path(second_path));
    assert(errno == ESTALE);
    assert(dirty);
    allow_stale_document_write = 1;
    assert(save_document_to_path(second_path));
    allow_stale_document_write = 0;
    assert(!dirty);

    assert(start_workspace_server());
    {
        char *request[] = {command_path};

        assert(forward_files_to_workspace(1, request));
        assert(poll_workspace_requests() == 1);
    }
    assert(buffer_index_for_path(command_path) >= 0);
    stop_workspace_server();

    release_workspace_lock();
    assert(!workspace_session_owner);
    assert(claim_workspace_if_available());
    assert(workspace_session_owner);
    assert(workspace_server_fd >= 0);
    stop_workspace_server();
    release_workspace_lock();
    for (int i = 0; i < MAX_BUFFERS; i++)
        if (editor_buffers[i].used)
            free_buffer_storage(i);
    reset_wrap_cache();
    free(wrap_cache);
    wrap_cache = NULL;
    remove_tree(home);

    if (saved_home) {
        assert(setenv("HOME", saved_home, 1) == 0);
        free(saved_home);
    } else {
        assert(unsetenv("HOME") == 0);
    }
    (void)draft;
    return 0;
}
