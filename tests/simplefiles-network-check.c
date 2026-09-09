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

    network_forget_reads();
    assert(chdir("/") == 0);
    assert(unlink(file) == 0 && rmdir(directory) == 0 && rmdir(root) == 0);
    puts("SimpleFiles network reads: responsive, replacement retried, stale result ignored");
    return 0;
}
