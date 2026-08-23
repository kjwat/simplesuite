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
    int conflict_fd;
    FILE *fp;

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
    save_session();

    assert(workspace_session_path(session_file, sizeof(session_file)));
    fp = fopen(session_file, "r");
    assert(fp);
    assert(fgets(header, sizeof(header), fp));
    assert(strncmp(header, SESSION_V2_MAGIC, strlen(SESSION_V2_MAGIC)) == 0);
    assert(fclose(fp) == 0);

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

    split_editor_window(LAYOUT_ABOVE_BELOW);
    assert(window_count_for_test() == 3);
    delete_editor_window();
    assert(window_count_for_test() == 2);
    delete_other_editor_windows();
    assert(window_count_for_test() == 1);
    assert(buffer_count() == 3);

    first = buffer_index_for_path(first_path);
    kill_buffer_index(first);
    assert(buffer_count() == 2);
    assert(buffer_index_for_path(first_path) < 0);
    for (int i = 0; i < MAX_EDITOR_WINDOWS; i++)
        assert(!editor_windows[i].used ||
               editor_windows[i].buffer_index != first);

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
