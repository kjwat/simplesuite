static void hold_completed_read(void);
#define SIMPLEFILES_NETWORK_TEST_AFTER_READ() hold_completed_read()
#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main
#include <assert.h>

static int completion_gate = -1;
static int completion_notice = -1;

static void hold_completed_read(void) {
    char byte = 'r';
    if (completion_gate >= 0) {
        assert(write(completion_notice, &byte, 1) == 1);
        assert(read(completion_gate, &byte, 1) == 1);
    }
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static const NetworkReadResult *wait_for_read(const char *path) {
    for (int i = 0; i < 300; i++) {
        (void)network_poll();
        const NetworkReadResult *result = network_read(path);
        if (result)
            return result;
        usleep(10000);
    }
    assert(!"network reader did not finish");
    return NULL;
}

#ifdef __linux__
/* Feed the real network worker from local fixtures, then publish its result
 * under a share path. No server or mount is needed for these UI checks. */
static void start_backed_network_read(const char *backing_path, const char *path) {
    for (int i = 0; i < NETWORK_READ_SLOTS; i++) {
        NetworkRead *slot = &network_reads[i];
        if (strcmp(slot->path, path) == 0) {
            assert(slot->pid == 0);
            safe_copy(slot->path, sizeof(slot->path), backing_path);
            slot->started = network_now_ms() - 2000;
            break;
        }
    }
    (void)network_read(backing_path);
    for (int i = 0; i < NETWORK_READ_SLOTS; i++) {
        NetworkRead *slot = &network_reads[i];
        if (strcmp(slot->path, backing_path) == 0) {
            assert(slot->pid > 0);
            safe_copy(slot->path, sizeof(slot->path), path);
            return;
        }
    }
    assert(!"network read did not start");
}

static int wait_for_network_updates(void) {
    int changed = 0;
    for (int i = 0; i < 300 && network_pending(); i++) {
        changed |= check_network_updates();
        if (network_pending())
            usleep(10000);
    }
    assert(network_pending() == 0);
    return changed;
}

static void test_network_paint_scope(void) {
    FILE *input = tmpfile();
    FILE *output = tmpfile();
    SCREEN *screen;

    assert(input && output);
    screen = newterm("xterm-256color", output, input);
    assert(screen);
    set_term(screen);
    assert(resizeterm(24, 120) != ERR);
    noecho();
    curs_set(0);

    /* Use the already loaded network capacity; this test needs no mount. */
    assert(browsing_statvfs(cwd_path, &capacity_ready_vfs) == 0);
    safe_copy(capacity_ready_path, sizeof(capacity_ready_path), cwd_path);
    assert(clock_gettime(CLOCK_MONOTONIC, &capacity_ready_time) == 0);
    capacity_ready_valid = 1;
    draw_ui();
    assert(parent_win && current_win && preview_win);

    mvwaddch(parent_win, 0, 0, 'L');
    mvwaddch(current_win, 0, 0, 'C');
    mvwaddch(preview_win, 0, 0, 'P');
    wnoutrefresh(parent_win);
    wnoutrefresh(current_win);
    wnoutrefresh(preview_win);
    present_screen();
    assert(fflush(output) == 0);
    long before = ftell(output);
    assert(before >= 0);

    /* A visible read finishing during navigation must not paint or consume
     * the pending preview. Once idle, only its preview/status may change. */
    draw_network_updates(NETWORK_DETAILS_CHANGED, 1);
    assert(fflush(output) == 0 && ftell(output) == before);
    assert((mvwinch(preview_win, 0, 0) & A_CHARTEXT) == 'P');
    draw_network_updates(NETWORK_DETAILS_CHANGED, 0);
    assert((mvwinch(parent_win, 0, 0) & A_CHARTEXT) == 'L');
    assert((mvwinch(current_win, 0, 0) & A_CHARTEXT) == 'C');
    assert((mvwinch(preview_win, 0, 0) & A_CHARTEXT) != 'P');

    /* Directory notifications still repaint the corresponding list. */
    draw_network_updates(NETWORK_PARENT_CHANGED, 1);
    assert((mvwinch(parent_win, 0, 0) & A_CHARTEXT) != 'L');
    assert((mvwinch(current_win, 0, 0) & A_CHARTEXT) == 'C');
    draw_network_updates(NETWORK_DIRECTORY_CHANGED, 1);
    assert((mvwinch(current_win, 0, 0) & A_CHARTEXT) != 'C');

    destroy_windows();
    endwin();
    delscreen(screen);
    fclose(output);
    fclose(input);
    capacity_ready_valid = 0;
    last_lines = last_cols = 0;
}
#endif

static void test_scroll_preservation(const char *root) {
    char listing[PATH_MAX], file[PATH_MAX];
    const struct {
        int cursor, top, key, expected_cursor, expected_top;
    } moves[] = {
        {30, 20, 'j', 31, 20},
        {30, 20, 'k', 29, 20},
        {39, 20, 'j', 40, 21},
        {20, 20, 'k', 19, 19}
    };

    join_path(listing, root, "scroll-list");
    assert(mkdir(listing, 0700) == 0);
    for (int i = 0; i < 64; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file-%03d.txt", i);
        join_path(file, listing, name);
        write_text(file, "preview\n");
    }
    safe_copy(cwd_path, sizeof(cwd_path), listing);
    load_dir(cwd_path);
    assert(entry_count == 64);

    /* A normal folder refresh must retain both in-pane and edge scrolling. */
    for (size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        cursor = moves[i].cursor;
        top = moves[i].top;
        handle_normal_input(moves[i].key);
        adjust_current_view(20);
        assert(cursor == moves[i].expected_cursor && top == moves[i].expected_top);
        refresh_loaded_directory();
        adjust_current_view(20);
        assert(cursor == moves[i].expected_cursor && top == moves[i].expected_top);
    }

#ifdef __linux__
    char remote[PATH_MAX], preview[PATH_MAX], renamed[PATH_MAX];
    const char *user_home = getenv("HOME");
    assert(user_home && *user_home);
    assert(safe_join3(remote, sizeof(remote), user_home, "/SimpleServe/",
                      "test-peer/scroll-list"));
    network_forget_reads();
    safe_copy(cwd_path, sizeof(cwd_path), remote);
    start_backed_network_read(listing, remote);
    assert(wait_for_network_updates());
    assert(entry_count == 64 && loaded_dir_stat_valid);

    for (size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        cursor = moves[i].cursor;
        top = moves[i].top;
        handle_normal_input(moves[i].key);
        adjust_current_view(20);
        assert(cursor == moves[i].expected_cursor && top == moves[i].expected_top);
        unsigned long generation = directory_listing_generation;
        join_path(file, listing, entries[cursor].name);
        join_path(preview, remote, entries[cursor].name);
        start_backed_network_read(file, preview);
        assert(wait_for_network_updates());
        adjust_current_view(20);
        assert(cursor == moves[i].expected_cursor && top == moves[i].expected_top);
        assert(directory_listing_generation == generation);
    }

    /* Capacity-only changes to the directory result must also leave its
     * listing alone. Force a metadata difference independent of the disk. */
    for (int i = 0; i < NETWORK_READ_SLOTS; i++) {
        if (strcmp(network_reads[i].path, remote) == 0) {
            assert(network_reads[i].ready);
            assert(network_reads[i].ready->vfs.f_bsize > 0);
            network_reads[i].ready->vfs.f_bsize = 0;
        }
    }
    unsigned long generation = directory_listing_generation;
    start_backed_network_read(listing, remote);
    assert(wait_for_network_updates());
    assert(directory_listing_generation == generation);

    /* Additions, renames and removals still refresh promptly, keeping the
     * focused filename on its original row when its sorted index changes. */
    cursor = 30;
    top = 20;
    join_path(file, listing, "aaa-first.txt");
    write_text(file, "new file\n");
    start_backed_network_read(listing, remote);
    assert(wait_for_network_updates());
    assert(directory_listing_generation == generation + 1 && entry_count == 65);
    assert(strcmp(entries[cursor].name, "file-030.txt") == 0);
    assert(cursor == 31 && top == 21);
    join_path(renamed, listing, "zzz-last.txt");
    assert(rename(file, renamed) == 0);
    start_backed_network_read(listing, remote);
    assert(wait_for_network_updates());
    assert(directory_listing_generation == generation + 2 && entry_count == 65);
    assert(strcmp(entries[cursor].name, "file-030.txt") == 0);
    assert(cursor == 30 && top == 20);
    assert(unlink(renamed) == 0);
    start_backed_network_read(listing, remote);
    assert(wait_for_network_updates());
    assert(directory_listing_generation == generation + 3 && entry_count == 64);
    assert(strcmp(entries[cursor].name, "file-030.txt") == 0);
    assert(cursor == 30 && top == 20);

    /* Losing and regaining the directory must still update availability. */
    join_path(file, root, "missing-scroll-list");
    start_backed_network_read(file, remote);
    assert(wait_for_network_updates());
    assert(entry_count == 0 && !loaded_dir_stat_valid);
    start_backed_network_read(listing, remote);
    assert(wait_for_network_updates());
    assert(entry_count == 64 && loaded_dir_stat_valid);

    /* A preview that finishes after focus moved away must stay invisible.
     * Otherwise each old read interrupts key repeat with a full UI redraw. */
    cursor = 30;
    top = 20;
    join_path(file, listing, "file-000.txt");
    join_path(preview, remote, "file-000.txt");
    start_backed_network_read(file, preview);
    assert(!wait_for_network_updates());

    /* Browsing more previews than the cache can hold must retain the two
     * displayed directory listings, so a later repaint cannot blank them. */
    char parent[PATH_MAX];
    safe_copy(parent, sizeof(parent), remote);
    *strrchr(parent, '/') = '\0';
    start_backed_network_read(root, parent);
    assert(wait_for_network_updates());
    generation = directory_listing_generation;
    for (int i = 1; i <= NETWORK_READ_SLOTS + 4; i++) {
        cursor = i;
        join_path(file, listing, entries[cursor].name);
        join_path(preview, remote, entries[cursor].name);
        start_backed_network_read(file, preview);
        assert(wait_for_network_updates() == NETWORK_DETAILS_CHANGED);
        int retained = 0;
        for (int j = 0; j < NETWORK_READ_SLOTS; j++) {
            NetworkRead *slot = &network_reads[j];
            if (strcmp(slot->path, remote) == 0 ||
                strcmp(slot->path, parent) == 0) {
                assert(slot->ready && !slot->ready->error);
                retained++;
            }
        }
        assert(retained == 2);
        assert(directory_listing_generation == generation);
    }
    test_network_paint_scope();

    /* The SimpleServe root itself is local, but its highlighted peer still
     * uses a network preview and must display a completed result. */
    network_forget_reads();
    assert(safe_join3(cwd_path, sizeof(cwd_path), user_home, "/", "SimpleServe"));
    assert(!network_ui_path(cwd_path));
    entry_count = 1;
    cursor = 0;
    safe_copy(entries[0].name, sizeof(entries[0].name), "test-peer");
    join_path(preview, cwd_path, entries[0].name);
    start_backed_network_read(root, preview);
    assert(wait_for_network_updates() == NETWORK_DETAILS_CHANGED);
#endif

    safe_copy(cwd_path, sizeof(cwd_path), listing);
    for (int i = 0; i < 64; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file-%03d.txt", i);
        join_path(file, listing, name);
        assert(unlink(file) == 0);
    }
    refresh_loaded_directory();
    assert(entry_count == 0 && cursor == 0 && top == 0);
    assert(rmdir(listing) == 0);
    safe_copy(cwd_path, sizeof(cwd_path), root);
    puts("SimpleFiles navigation: stable previews and refreshes, edge scrolling preserved");
}

int main(void) {
    char root[] = "/tmp/simplefiles-network.XXXXXX";
    char file[PATH_MAX], directory[PATH_MAX], actual[PATH_MAX];
    int gate[2], notice[2];
    char byte;
    assert(mkdtemp(root));
    join_path(file, root, "probe.txt");
    join_path(directory, root, "folder");
    assert(mkdir(directory, 0700) == 0);
    write_text(file, "old mount\n");
    network_ui_pid = getpid();
    (void)network_poll();

    /* A stalled read cannot block the UI or keep it on an old NFS cwd. */
    assert(pipe(gate) == 0 && pipe(notice) == 0);
    completion_gate = gate[0];
    completion_notice = notice[1];
    long long started = network_now_ms();
    assert(network_read(file) == NULL);
    assert(network_now_ms() - started < 250);
    struct pollfd notification = {notice[0], POLLIN, 0};
    assert(poll(&notification, 1, 3000) == 1);
    assert(read(notice[0], &byte, 1) == 1);
    assert(network_pending() == 1);
    pid_t old_reader = 0;
    for (int i = 0; i < NETWORK_READ_SLOTS; i++)
        if (network_reads[i].pid > 0)
            old_reader = network_reads[i].pid;
    assert(old_reader > 0);
    started = network_now_ms();
    assert(network_read(file) == NULL);
    (void)network_poll();
    assert(network_now_ms() - started < 250);

    /* A replacement mount gets a new reader while the old read stays alive.
     * Its eventual completion must not overwrite the new mount's results. */
    completion_gate = completion_notice = -1;
    write_text(file, "new mount\n");
    network_forget_reads();
    assert(kill(old_reader, 0) == 0);
    const NetworkReadResult *result = wait_for_read(file);
    assert(result->error == 0 && result->text_size == strlen("new mount\n"));
    assert(memcmp(result->text, "new mount\n", result->text_size) == 0);
    assert(write(gate[1], "x", 1) == 1);
    for (int i = 0; i < 300 && network_pending(); i++) {
        (void)network_poll();
        usleep(10000);
    }
    assert(network_pending() == 0);
    result = network_read(file);
    assert(result && memcmp(result->text, "new mount\n", result->text_size) == 0);
    close(gate[0]); close(gate[1]); close(notice[0]); close(notice[1]);

    result = wait_for_read(root);
    assert(!result->error && S_ISDIR(result->file_stat.st_mode));
    assert(result->count == 2);
    assert(strcmp(result->entries[0].name, "folder") == 0);
    assert(result->entries[0].is_dir == 1);

#ifdef __linux__
    const char *user_home = getenv("HOME");
    assert(user_home && *user_home);
    char remote[PATH_MAX];
    assert(safe_join3(remote, sizeof(remote), user_home, "/SimpleServe/", "test-peer/test-share"));
    assert(startup_chdir_resolved(remote));
    assert(strcmp(cwd_path, remote) == 0);
    assert(getcwd(actual, sizeof(actual)) && strcmp(actual, "/") == 0);
    assert(startup_chdir_resolved("child"));
    assert(strstr(cwd_path, "/test-share/child") != NULL);
    assert(getcwd(actual, sizeof(actual)) && strcmp(actual, "/") == 0);
    /* The brief unmounted interval must preserve the requested directory,
     * so the new route can reopen it instead of moving to a parent. */
    safe_copy(remote, sizeof(remote), cwd_path);
    result = wait_for_read(remote);
    assert(result->error == ENOENT);
    load_dir(cwd_path);
    assert(strcmp(cwd_path, remote) == 0 && entry_count == 0);
    assert(startup_chdir_resolved(root));
    assert(getcwd(actual, sizeof(actual)) && strcmp(actual, root) == 0);
#else
    (void)actual;
#endif

    test_scroll_preservation(root);
    network_forget_reads();
    assert(chdir("/") == 0);
    assert(unlink(file) == 0 && rmdir(directory) == 0 && rmdir(root) == 0);
    puts("SimpleFiles network reads: responsive, replacement retried, stale result ignored");
    return 0;
}
