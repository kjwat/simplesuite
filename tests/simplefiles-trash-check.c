#define SIMPLEFILES_TRASH_TEST 1
#define SIMPLEFILES_TRASH_IDLE_MS 150
#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main

#include <assert.h>

static void make_directory(const char *path)
{
    assert(mkdir(path, 0700) == 0);
}

static void make_file(const char *path)
{
    static const char contents[] = "trash test\n";
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);

    assert(fd >= 0);
    assert(write(fd, contents, sizeof(contents) - 1) ==
           (ssize_t)(sizeof(contents) - 1));
    assert(close(fd) == 0);
}

static int directory_is_empty(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;

    assert(dir);
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return 1;
}


#ifdef __linux__
static const char *blocked_volume;
static void block_location(const char *path)
{
    if (blocked_volume && strcmp(path, blocked_volume) == 0)
        for (;;) pause();
}

static void make_trash(const char *root)
{
    char path[PATH_MAX];
    make_directory(root);
    join_path(path, root, "files");
    make_directory(path);
    join_path(path, root, "info");
    make_directory(path);
    join_path(path, root, "files/deleted");
    make_directory(path);
    join_path(path, root, "files/deleted/nested");
    make_file(path);
    join_path(path, root, "info/deleted.trashinfo");
    make_file(path);
}

static void assert_trash(const char *root, int present)
{
    char path[PATH_MAX];
    join_path(path, root, "files");
    assert(directory_is_empty(path) == !present);
    join_path(path, root, "info/deleted.trashinfo");
    assert(path_exists(path) == present);
}

static void test_offline_trash(const char *root)
{
    char data[PATH_MAX], local[PATH_MAX], online[PATH_MAX], offline[PATH_MAX];
    char online_trash[PATH_MAX], offline_trash[PATH_MAX], table[PATH_MAX];
    join_path(data, root, "data");
    make_directory(data);
    join_path(local, data, "Trash");
    make_trash(local);
    assert(setenv("XDG_DATA_HOME", data, 1) == 0);
    join_path(online, root, "online");
    join_path(offline, root, "offline");
    make_directory(online);
    make_directory(offline);
    assert(snprintf(online_trash, sizeof(online_trash), "%s/.Trash-%lu",
                    online, (unsigned long)getuid()) < (int)sizeof(online_trash));
    assert(snprintf(offline_trash, sizeof(offline_trash), "%s/.Trash-%lu",
                    offline, (unsigned long)getuid()) < (int)sizeof(offline_trash));
    make_trash(online_trash);
    make_trash(offline_trash);
    join_path(table, root, "mounts");
    FILE *mounts = fopen(table, "w");
    assert(mounts);
    fprintf(mounts, "server:/online %s nfs rw 0 0\n", online);
    fprintf(mounts, "server:/offline %s nfs rw 0 0\n", offline);
    assert(fclose(mounts) == 0);
    trash_mount_table_test_path = table;
    trash_location_test_hook = block_location;
    blocked_volume = offline;
    config_trash_dir[0] = '\0';

    empty_trash_now();
    for (int i = 0; i < 200 && file_operation_pid > 0; i++) {
        (void)check_background_file_operation();
        if (file_operation_pid > 0) usleep(10000);
    }
    assert(file_operation_pid < 0);
    assert(strcmp(message,
                  "reachable trash emptied; unavailable trash deferred") == 0);
    assert_trash(local, 0);
    assert_trash(online_trash, 0);
    assert_trash(offline_trash, 1);

    /* The same normal operation picks up the deferred files on recovery. */
    blocked_volume = NULL;
    assert(empty_default_trash() == 0);
    assert_trash(offline_trash, 0);

    /* Refuse a symlinked payload directory, preserving unrelated data. */
    char payload[PATH_MAX], victim[PATH_MAX];
    join_path(payload, local, "files");
    assert(rmdir(payload) == 0);
    join_path(victim, root, "victim");
    make_directory(victim);
    assert(symlink(victim, payload) == 0);
    join_path(payload, victim, "keep");
    make_file(payload);
    assert(empty_trash_bounded(local, 0) == 1);
    assert(path_exists(payload));

    /* Custom trash on an unavailable filesystem also returns promptly. */
    blocked_volume = offline;
    safe_copy(config_trash_dir, sizeof(config_trash_dir), offline);
    assert(empty_trash_bounded(offline, 2) == TRASH_DEFERRED);
    assert(path_exists(offline_trash));
    trash_location_test_hook = NULL;
    trash_mount_table_test_path = NULL;
}
#endif

int main(void)
{
    char root[] = "/tmp/simplefiles-trash-test.XXXXXX";
    char default_trash[PATH_MAX];
    char default_file[PATH_MAX];
    char default_dir[PATH_MAX];
    char custom_trash[PATH_MAX];
    char custom_file[PATH_MAX];
    char custom_dir[PATH_MAX];
    char nested_file[PATH_MAX];
    char decoy_file[PATH_MAX];


#ifndef __linux__
    assert(strcmp(select_delete_authorizer(1, 1, 1), "sudo") == 0);
    assert(strcmp(select_delete_authorizer(1, 0, 1), "pkexec") == 0);
    assert(strcmp(select_delete_authorizer(0, 1, 1), "pkexec") == 0);
    assert(strcmp(select_delete_authorizer(0, 1, 0), "sudo") == 0);
    assert(select_delete_authorizer(1, 0, 0) == NULL);
#endif

    assert(mkdtemp(root));
    join_path(default_trash, root, "default");
    join_path(default_file, default_trash, "file");
    join_path(default_dir, default_trash, "empty-dir");
    join_path(custom_trash, root, "custom");
    join_path(custom_file, custom_trash, "file");
    join_path(custom_dir, custom_trash, "nested");
    join_path(nested_file, custom_dir, "file");
    join_path(decoy_file, default_trash, "keep");

    make_directory(default_trash);
    make_file(default_file);
    make_directory(default_dir);
    make_directory(custom_trash);

    safe_copy(cwd_path, sizeof(cwd_path), root);
    config_trash_dir[0] = '\0';
    int ok = 0;
    int fail = 0;

    make_file(decoy_file);
    make_file(custom_file);
    make_directory(custom_dir);
    make_file(nested_file);
    safe_copy(config_trash_dir, sizeof(config_trash_dir), custom_trash);

    ok = 0;
    fail = 0;
    assert(empty_configured_trash(&ok, &fail) == 0);
    assert(ok == 2 && fail == 0);
    assert(directory_is_empty(custom_trash));
    assert(path_exists(decoy_file));

    ok = 0;
    fail = 0;
    assert(empty_configured_trash(&ok, &fail) == 0);
    assert(ok == 0 && fail == 0);

    /* The interactive path must hand a potentially large recursive removal
     * to a worker instead of holding the curses event loop. */
    make_file(custom_file);
    empty_trash_now();
    assert(file_operation_pid > 0);
    assert(file_operation_kind == FILE_OPERATION_EMPTY_TRASH);
    for (int i = 0; i < 200 && file_operation_pid > 0; i++) {
        (void)check_background_file_operation();
        if (file_operation_pid > 0) usleep(10000);
    }
    assert(file_operation_pid < 0);
    assert(directory_is_empty(custom_trash));
    assert(strcmp(message, "trash emptied") == 0);

#ifdef __linux__
    test_offline_trash(root);
#endif
    assert(remove_recursive(root) == 0);
    return 0;
}
