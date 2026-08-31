#define SIMPLEWORDS_TYPEWRITER_TEST
#define SIMPLEWORDS_PERSISTENCE_TEST
#define main simplewords_program_main
#include "../simplewords.c"
#undef main

#include <assert.h>

static void write_bytes(const char *path, const void *data, size_t length)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);

    assert(fd >= 0);
    assert(write_all_fd(fd, data, length));
    assert(close(fd) == 0);
}

static void write_text(const char *path, const char *text)
{
    write_bytes(path, text, strlen(text));
}

static size_t read_bytes(const char *path, unsigned char *out, size_t outsz)
{
    int fd = open(path, O_RDONLY);
    ssize_t count;

    assert(fd >= 0);
    count = read(fd, out, outsz);
    assert(count >= 0);
    assert(close(fd) == 0);
    return (size_t)count;
}

static void assert_file_text(const char *path, const char *expected)
{
    unsigned char contents[8192];
    size_t length = read_bytes(path, contents, sizeof(contents));

    assert(length == strlen(expected));
    assert(memcmp(contents, expected, length) == 0);
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
            assert(format_string(child, sizeof(child), "%s/%s", path,
                                 entry->d_name));
            remove_tree(child);
        }
        assert(closedir(directory) == 0);
        assert(rmdir(path) == 0);
    } else {
        assert(unlink(path) == 0);
    }
}

static void reset_document(void)
{
    release_document_edit_lock(&editor_buffers[active_buffer_index]);
    for (int i = 0; i < line_count; i++)
        free(lines[i]);
    clear_undo_history();
    line_count = 1;
    lines[0] = new_line("");
    cy = cx = top = 0;
    filename[0] = '\0';
    make_untitled_name();
    clear_pending_recovery();
    clear_opened_recovery();
    dirty = 0;
    autosave_dirty = 0;
    last_edit_time = 0;
    document_newline_crlf = 0;
    document_final_newline = 0;
    editor_buffers[active_buffer_index].disk_revision_known = 0;
    reset_edit_state_after_load();
}

static void set_mtime(const char *path, time_t seconds, long nanoseconds)
{
    struct timespec times[2];

    times[0].tv_sec = seconds;
    times[0].tv_nsec = nanoseconds;
    times[1] = times[0];
    assert(utimensat(AT_FDCWD, path, times, 0) == 0);
}

static int count_prefixed_files(const char *directory, const char *prefix)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    int count = 0;

    assert(dir);
    while ((entry = readdir(dir)) != NULL)
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0)
            count++;
    assert(closedir(dir) == 0);
    return count;
}

static void check_utf8_editing(void)
{
    char prompt[32] = "A\xc3\xa9" "B";
    int prompt_length = 4;
    int prompt_cursor = 3;

    reset_document();
    strcpy(lines[0], "A\xc3\xa9" "B");
    cx = 3;
    assert(backspace());
    assert(strcmp(lines[0], "AB") == 0);
    assert(cx == 1);

    reset_document();
    strcpy(lines[0], "A\xc3\xa9" "B");
    cx = 1;
    assert(delete_forward());
    assert(strcmp(lines[0], "AB") == 0);
    assert(cx == 1);

    reset_document();
    strcpy(lines[0], "A\xc3\xa9" "B");
    cx = 3;
    move_left(0);
    assert(cx == 1);
    move_right(0);
    assert(cx == 3);

    assert(prompt_backspace(prompt, &prompt_length, &prompt_cursor));
    assert(strcmp(prompt, "AB") == 0);
    assert(prompt_cursor == 1 && prompt_length == 2);
}

static void check_long_path_save(const char *directory)
{
    char component_a[201];
    char component_b[201];
    char long_name[121];
    char dir_a[PATH_MAX];
    char dir_b[PATH_MAX];
    char long_path[PATH_MAX];
    char canonical[PATH_MAX];
    char truncated[512];

    memset(component_a, 'a', sizeof(component_a) - 1);
    component_a[sizeof(component_a) - 1] = '\0';
    memset(component_b, 'b', sizeof(component_b) - 1);
    component_b[sizeof(component_b) - 1] = '\0';
    memset(long_name, 'c', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    assert(format_string(dir_a, sizeof(dir_a), "%s/%s", directory,
                         component_a));
    assert(mkdir(dir_a, 0700) == 0);
    assert(format_string(dir_b, sizeof(dir_b), "%s/%s", dir_a,
                         component_b));
    assert(mkdir(dir_b, 0700) == 0);
    assert(format_string(long_path, sizeof(long_path), "%s/%s", dir_b,
                         long_name));
    assert(strlen(long_path) > 511);
    memcpy(truncated, long_path, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
    write_text(long_path, "long-path original");
    write_text(truncated, "unrelated prefix");

    reset_document();
    assert(canonical_visit_path(long_path, canonical, sizeof(canonical)));
    assert(strcmp(canonical, long_path) == 0);
    assert(load_file_at_position(canonical, 0, 0, 0, 0, 0) ==
           LOAD_RESULT_DISK);
    assert(strcmp(filename, long_path) == 0);
    cx = (int)strlen(lines[0]);
    assert(insert_char('!'));
    assert(save_document_to_path(filename));
    assert_file_text(long_path, "long-path original!");
    assert_file_text(truncated, "unrelated prefix");
}

static void check_path_completion_overflow(const char *directory)
{
    static const char entry_name[] =
        "completion-entry-that-cannot-fit-in-the-absolute-path.txt";
    char deep[PATH_MAX];
    char next[PATH_MAX];
    char component[201];
    char input[PATH_MAX];
    const size_t target_length = sizeof(deep) - 16;
    PathCompletion *items;
    int directory_fd;
    int entry_fd;
    int count = -1;
    int base_length = -1;
    int completion_error = 0;

    assert(copy_string(deep, sizeof(deep), directory));
    while (strlen(deep) < target_length) {
        size_t remaining = target_length - strlen(deep);
        size_t component_length;

        if (remaining <= 1)
            break;
        component_length = remaining > sizeof(component) ?
                           sizeof(component) - 1 : remaining - 1;
        memset(component, 'd', component_length);
        component[component_length] = '\0';
        assert(format_string(next, sizeof(next), "%s/%s", deep, component));
        assert(mkdir(next, 0700) == 0);
        assert(copy_string(deep, sizeof(deep), next));
    }

    directory_fd = open(deep, O_RDONLY);
    assert(directory_fd >= 0);
    entry_fd = openat(directory_fd, entry_name,
                      O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(entry_fd >= 0);
    assert(close(entry_fd) == 0);
    assert(format_string(input, sizeof(input), "%s/", deep));

    items = path_completions(input, &count, &base_length,
                             &completion_error);
    assert(!items);
    assert(completion_error == ENAMETOOLONG);

    assert(unlinkat(directory_fd, entry_name, 0) == 0);
    assert(close(directory_fd) == 0);
}

static void check_nanosecond_revisions(const char *directory)
{
    char path[PATH_MAX];
    char autosave[PATH_MAX];
    struct stat before;
    struct timespec changed[2];
    int fd;

    assert(format_string(path, sizeof(path), "%s/revision.txt", directory));
    write_text(path, "aaaa");
    reset_document();
    assert(load_file_at_position(path, 0, 0, 0, 0, 0) == LOAD_RESULT_DISK);
    assert(stat(path, &before) == 0);
    fd = open(path, O_WRONLY);
    assert(fd >= 0);
    assert(pwrite(fd, "bbbb", 4, 0) == 4);
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);
    changed[0] = before.st_atim;
    changed[1] = stat_mtime_value(&before);
    changed[1].tv_nsec = changed[1].tv_nsec == 999999999L ?
                         999999998L : changed[1].tv_nsec + 1;
    assert(utimensat(AT_FDCWD, path, changed, 0) == 0);
    assert(disk_revision_changed(&editor_buffers[active_buffer_index], path));

    assert(format_string(path, sizeof(path), "%s/recovery.txt", directory));
    write_text(path, "disk version");
    autosave_path_for(path, autosave, sizeof(autosave));
    assert(autosave[0]);
    write_text(autosave, "newer recovered version");
    set_mtime(path, 1800000000, 100);
    set_mtime(autosave, 1800000000, 200);
    reset_document();
    assert(load_file_at_position(path, 1, 0, 0, 0, 0) == LOAD_RESULT_DISK);
    assert(strcmp(lines[0], "disk version") == 0);
    assert(pending_recovery_for(path));
}

static void check_unique_draft_recovery(void)
{
    char first_name[sizeof(untitled_name)];
    char first_path[PATH_MAX];

    reset_document();
    strcpy(lines[0], "first draft");
    mark_edit();
    assert(autosave_file_common(1));
    copy_string(first_name, sizeof(first_name), untitled_name);
    untitled_autosave_path_for(first_name, first_path, sizeof(first_path));
    assert(regular_file(first_path));

    make_buffer_draft_name(active_buffer_index);
    assert(strcmp(first_name, untitled_name) != 0);
    strcpy(lines[0], "second draft");
    mark_edit();
    assert(autosave_file_common(1));
    assert_file_text(first_path, "first draft");
}

static void check_loader_boundaries(const char *directory)
{
    static const unsigned char embedded_nul[] = {
        'a', 'b', 'c', 0, 'd', 'e', 'f', '\n'
    };
    char path[PATH_MAX];
    char max_line[MAX_LINE];

    assert(format_string(path, sizeof(path), "%s/embedded-nul.txt",
                         directory));
    write_bytes(path, embedded_nul, sizeof(embedded_nul));
    reset_document();
    strcpy(lines[0], "keep me");
    errno = 0;
    assert(!read_document_into_buffer(path));
    assert(errno == EILSEQ);
    assert(strcmp(lines[0], "keep me") == 0);

    memset(max_line, 'x', sizeof(max_line) - 1);
    max_line[sizeof(max_line) - 1] = '\0';
    assert(format_string(path, sizeof(path), "%s/max-line.txt", directory));
    write_text(path, max_line);
    reset_document();
    assert(load_file_at_position(path, 0, 0, 0, 0, 0) == LOAD_RESULT_DISK);
    assert(strlen(lines[0]) == MAX_LINE - 1);

    assert(format_string(path, sizeof(path), "%s/oversized-line.txt",
                         directory));
    write_bytes(path, max_line, sizeof(max_line) - 1);
    {
        int fd = open(path, O_WRONLY | O_APPEND);
        assert(fd >= 0);
        assert(write_all_fd(fd, "x", 1));
        assert(close(fd) == 0);
    }
    strcpy(lines[0], "still loaded");
    errno = 0;
    assert(!read_document_into_buffer(path));
    assert(errno == EOVERFLOW);
    assert(strcmp(lines[0], "still loaded") == 0);
}

static void check_newline_preservation(const char *directory)
{
    char path[PATH_MAX];

    assert(format_string(path, sizeof(path), "%s/crlf.txt", directory));
    write_text(path, "alpha\r\nbeta\r\n");
    reset_document();
    assert(load_file_at_position(path, 0, 0, 0, 0, 0) == LOAD_RESULT_DISK);
    assert(document_newline_crlf);
    assert(document_final_newline);
    strcpy(lines[0], "ALPHA");
    mark_edit();
    assert(save_document_to_path(path));
    assert_file_text(path, "ALPHA\r\nbeta\r\n");

    assert(format_string(path, sizeof(path), "%s/final-lf.txt", directory));
    write_text(path, "last line\n");
    reset_document();
    assert(load_file_at_position(path, 0, 0, 0, 0, 0) == LOAD_RESULT_DISK);
    assert(!document_newline_crlf);
    assert(document_final_newline);
    assert(save_document_to_path(path));
    assert_file_text(path, "last line\n");
}

static void check_paste_limit_preflight(void)
{
    size_t length = SYSTEM_CLIPBOARD_LIMIT;
    char *paste = malloc(length + 1);

    assert(paste);
    memset(paste, '\n', length);
    paste[length] = '\0';
    reset_document();
    assert(!insert_pasted_text(paste));
    assert(line_count == 1 && lines[0][0] == '\0');
    free(paste);
}

static void check_autosave_rate(void)
{
    char autosave[PATH_MAX];
    struct stat first;
    struct stat second;

    reset_document();
    strcpy(lines[0], "one edit then idle");
    mark_edit();
    last_edit_time = time(NULL) - 10;
    untitled_autosave_path_for(untitled_name, autosave, sizeof(autosave));
    assert(autosave_file_common(0));
    assert(!autosave_dirty);
    assert(stat(autosave, &first) == 0);
    assert(autosave_file_common(0));
    assert(stat(autosave, &second) == 0);
    assert(first.st_ino == second.st_ino);
    assert(timespec_compare(stat_mtime_value(&first),
                            stat_mtime_value(&second)) == 0);
}

static void check_atomic_faults(const char *directory)
{
    static const int faults[] = {
        PERSISTENCE_FAULT_WRITE,
        PERSISTENCE_FAULT_RENAME,
        PERSISTENCE_FAULT_FSYNC,
        PERSISTENCE_FAULT_INTERRUPTED_WRITE
    };
    char path[PATH_MAX];

    assert(format_string(path, sizeof(path), "%s/atomic.txt", directory));
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        write_text(path, "original");
        reset_document();
        strcpy(lines[0], "replacement text");
        persistence_test_fault = faults[i];
        persistence_test_errno = faults[i] == PERSISTENCE_FAULT_WRITE ?
                                 ENOSPC : EIO;
        persistence_test_bytes_before_failure = 3;
        assert(!write_document(path));
        persistence_test_reset_fault();
        assert_file_text(path, "original");
    }
}

static void check_link_safety(const char *directory)
{
    char target[PATH_MAX];
    char symlink_path[PATH_MAX];
    char hardlink_path[PATH_MAX];
    char directory_path[PATH_MAX];
    struct stat first;
    struct stat second;

    assert(format_string(target, sizeof(target), "%s/link-target.txt",
                         directory));
    assert(format_string(symlink_path, sizeof(symlink_path), "%s/link.txt",
                         directory));
    write_text(target, "old target");
    assert(symlink(target, symlink_path) == 0);
    reset_document();
    strcpy(lines[0], "new target");
    mark_edit();
    assert(save_document_to_path(symlink_path));
    assert_file_text(target, "new target");
    assert(lstat(symlink_path, &first) == 0 && S_ISLNK(first.st_mode));

    assert(format_string(hardlink_path, sizeof(hardlink_path), "%s/hard.txt",
                         directory));
    assert(link(target, hardlink_path) == 0);
    assert(stat(target, &first) == 0);
    reset_document();
    strcpy(lines[0], "must not split links");
    mark_edit();
    errno = 0;
    assert(!save_document_to_path(target));
    assert(errno == EMLINK);
    assert(stat(hardlink_path, &second) == 0);
    assert(first.st_ino == second.st_ino);
    assert_file_text(target, "new target");

    /* The low-level writer must independently reject targets that cannot be
     * replaced atomically.  save_document_to_path() normally catches the
     * hard-link case first, so exercise both defenses directly. */
    errno = 0;
    assert(!write_document(target));
    assert(errno == EMLINK);
    assert_file_text(target, "new target");

    assert(format_string(directory_path, sizeof(directory_path), "%s/folder",
                         directory));
    assert(mkdir(directory_path, 0700) == 0);
    errno = 0;
    assert(!write_document(directory_path));
    assert(errno == EINVAL);
}

static void check_metadata_preservation(const char *directory)
{
    char path[PATH_MAX];
    struct stat st;

    assert(format_string(path, sizeof(path), "%s/metadata.txt", directory));
    write_text(path, "old metadata document");
    assert(chmod(path, 0640) == 0);
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    {
        static const char attribute[] = "kept attribute";
        static const char attribute_name[] =
#ifdef __APPLE__
            "com.simplewords.persistence-test";
#elif defined(__FreeBSD__)
            "simplewords.persistence-test";
#else
            "user.simplewords-persistence-test";
#endif
        char value[sizeof(attribute)] = {0};
        int xattrs_supported =
#ifdef __APPLE__
            setxattr(path, attribute_name, attribute, sizeof(attribute),
                     0, 0) == 0;
#elif defined(__FreeBSD__)
            extattr_set_file(path, EXTATTR_NAMESPACE_USER, attribute_name,
                             attribute, sizeof(attribute)) ==
                (ssize_t)sizeof(attribute);
#else
            setxattr(path, attribute_name, attribute, sizeof(attribute),
                     0) == 0;
#endif

        if (!xattrs_supported)
            assert(errno == ENOTSUP || errno == EOPNOTSUPP);

        reset_document();
        assert(load_file_at_position(path, 0, 0, 0, 0, 0) ==
               LOAD_RESULT_DISK);
        strcpy(lines[0], "new metadata document");
        mark_edit();
        assert(save_document_to_path(path));
        assert(stat(path, &st) == 0);
        assert((st.st_mode & 07777) == 0640);
        if (xattrs_supported) {
            ssize_t length =
#ifdef __APPLE__
                getxattr(path, attribute_name, value, sizeof(value), 0, 0);
#elif defined(__FreeBSD__)
                extattr_get_file(path, EXTATTR_NAMESPACE_USER, attribute_name,
                                 value, sizeof(value));
#else
                getxattr(path, attribute_name, value, sizeof(value));
#endif

            assert(length == (ssize_t)sizeof(attribute));
            assert(memcmp(value, attribute, sizeof(attribute)) == 0);
        }
    }
#else
    reset_document();
    assert(load_file_at_position(path, 0, 0, 0, 0, 0) == LOAD_RESULT_DISK);
    strcpy(lines[0], "new metadata document");
    mark_edit();
    assert(save_document_to_path(path));
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 07777) == 0640);
#endif
}

static void check_recovery_failures(const char *valid_home)
{
    const char *invalid_home = "/proc/simplewords-unwritable-test-home";
    int index;
    int count_before;

    reset_document();
    strcpy(lines[0], "must survive failed recovery");
    mark_edit();
    assert(setenv("HOME", invalid_home, 1) == 0);
    assert(!autosave_file_common(1));
    assert(dirty && autosave_dirty);
    assert(strcmp(lines[0], "must survive failed recovery") == 0);

    index = active_buffer_index;
    count_before = buffer_count();
    persistence_test_auto_confirm_kill = 1;
    kill_buffer_index(index);
    persistence_test_auto_confirm_kill = 0;
    assert(editor_buffers[index].used);
    assert(buffer_count() == count_before);
    assert(strcmp(lines[0], "must survive failed recovery") == 0);
    assert(!flush_recovery_state());
    assert(setenv("HOME", valid_home, 1) == 0);
}

static void check_backup_retention(const char *directory)
{
    char path[PATH_MAX];
    char backup_dir[PATH_MAX];
    char prefix[32];

    assert(format_string(path, sizeof(path), "%s/retained.txt", directory));
    write_text(path, "version 0");
    reset_document();
    assert(load_file_at_position(path, 0, 0, 0, 0, 0) == LOAD_RESULT_DISK);
    for (int version = 1; version <= BACKUP_RETENTION + 7; version++) {
        snprintf(lines[0], MAX_LINE, "version %d", version);
        mark_edit();
        assert(save_document_to_path(path));
    }
    assert(simplewords_backup_dir(backup_dir, sizeof(backup_dir)));
    snprintf(prefix, sizeof(prefix), "%016llx-", path_hash(path));
    assert(count_prefixed_files(backup_dir, prefix) == BACKUP_RETENTION);
}

int main(void)
{
    char home[] = "/tmp/simplewords-persistence-test.XXXXXX";
    char documents[PATH_MAX];
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;

    assert(mkdtemp(home));
    assert(setenv("HOME", home, 1) == 0);
    assert(format_string(documents, sizeof(documents), "%s/documents", home));
    assert(mkdir(documents, 0700) == 0);
    assert(setlocale(LC_ALL, "C.UTF-8"));
    LINES = 30;
    COLS = 120;
    make_untitled_name();
    lines[0] = new_line("");
    initialize_buffer_system();
    acquire_workspace_lock();
    assert(workspace_session_owner);

    check_utf8_editing();
    check_long_path_save(documents);
    check_path_completion_overflow(documents);
    check_nanosecond_revisions(documents);
    check_unique_draft_recovery();
    check_loader_boundaries(documents);
    check_newline_preservation(documents);
    check_paste_limit_preflight();
    check_autosave_rate();
    check_atomic_faults(documents);
    check_link_safety(documents);
    check_metadata_preservation(documents);
    check_recovery_failures(home);
    check_backup_retention(documents);

    persistence_test_reset_fault();
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
    return 0;
}
