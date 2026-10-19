#define SIMPLEWORDS_TYPEWRITER_TEST
#define main simplewords_program_main
#include "../simplewords.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[PATH_MAX]; snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
            remove_tree(child);
        }
        closedir(dir); assert(rmdir(path) == 0);
    } else assert(unlink(path) == 0);
}

int main(void)
{
    setlocale(LC_ALL, "C.UTF-8");
    signal(SIGPIPE, SIG_IGN);
    OpenLocation at;
    assert(parse_open_location("12", &at) && at.line == 12 && at.column == 1 && at.length == 0);
    assert(parse_open_location("12:4:5", &at) && at.line == 12 && at.column == 4 && at.length == 5);
    const char *bad[] = {"", "0", "1:0", "-1", "1:-1", "1:2:", "1:2:3:4", "1:2x", "4294967296", "1: 2"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) assert(!parse_open_location(bad[i], &at));
    char home[] = "/tmp/simplewords-location-XXXXXX"; assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    unsetenv("XDG_STATE_HOME"); unsetenv("XDG_CONFIG_HOME");
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/a document.txt", home);
    FILE *file = fopen(path, "w"); assert(file);
    fputs("first line\nUn café, puis Café encore.\nthird line\n", file); fclose(file);
    LINES = 30; COLS = 100;
    make_untitled_name(); lines[0] = new_line(""); initialize_buffer_system(); acquire_workspace_lock();
    char *paths[] = {path};
    open_startup_files_additively(1, paths);
    at = (OpenLocation){2, 16, 5};
    assert(apply_open_location(path, &at));
    assert(cy == 1 && cx == 15);
    assert(!strcmp(last_find, "Café"));
    assert(find_active && find_match_y == 1 && find_match_x == 15 && find_match_len == 5);
    assert(char_find_highlight(cy, cx));
    assert(!char_find_highlight(cy, cx - 1));
    assert(!dirty);
    /* Clamp outdated positions and UTF-8 continuation bytes safely. */
    at = (OpenLocation){2, 8, 1};
    assert(apply_open_location(path, &at));
    assert(cx == 6); /* é starts at byte six; column eight points inside it. */
    assert(!find_active);
    at = (OpenLocation){UINT32_MAX, UINT32_MAX, UINT32_MAX};
    assert(apply_open_location(path, &at));
    assert(cy == line_count - 1 && cx == (int)strlen(lines[cy]));
    int old_y = cy, old_x = cx;
    assert(!apply_open_location("/nonexistent/different.txt", &at));
    assert(cy == old_y && cx == old_x);
    /* Location requests work on existing buffers and preserve unsaved text. */
    cy = 0; cx = 0;
    insert_char('~');
    assert(dirty && lines[0][0] == '~');
    memset(lines[2], 'k', 300); lines[2][300] = 0;
    at = (OpenLocation){3, 1, 300};
    assert(apply_open_location(path, &at));
    assert(strlen(last_find) == 300 && char_find_highlight(2, 299));
    assert(!char_find_highlight(2, 300));
    assert(start_workspace_server());
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        OpenLocation request = {2, 16, 5};
        _exit(forward_files_to_workspace_at(1, paths, &request) ? 0 : 1);
    }
    int received = 0, child_status = 0;
    for (int i = 0; i < 200; i++) {
        received += poll_workspace_requests();
        if (waitpid(child, &child_status, WNOHANG) == child) break;
        struct timespec pause = {0, 10000000}; nanosleep(&pause, NULL);
        assert(i != 199);
    }
    assert(received == 1);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    assert(buffer_count() == 1 && cy == 1 && cx == 15);
    assert(dirty && lines[0][0] == '~');
    assert(find_active && char_find_highlight(1, 15));
    stop_workspace_server(); release_workspace_lock();
    for (int i = 0; i < MAX_BUFFERS; i++) if (editor_buffers[i].used) free_buffer_storage(i);
    reset_wrap_cache(); free(wrap_cache); wrap_cache = NULL;
    remove_tree(home);
    puts("simplewords: location parsing, exact cursor/highlight, UTF-8 bounds, workspace forwarding, and unsaved-buffer preservation passed");
    return 0;
}
