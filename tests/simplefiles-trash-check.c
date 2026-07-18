#define SIMPLEFILES_TRASH_TEST 1
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
    char *default_uri;

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

    default_uri = g_filename_to_uri(default_trash, NULL, NULL);
    assert(default_uri);
    safe_copy(cwd_path, sizeof(cwd_path), root);
    config_trash_dir[0] = '\0';

    int ok = 0;
    int fail = 0;
    assert(empty_freedesktop_trash(default_uri, &ok, &fail) == 0);
    assert(ok == 2 && fail == 0);
    assert(directory_is_empty(default_trash));

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

    g_free(default_uri);
    assert(remove_recursive(root) == 0);
    return 0;
}
