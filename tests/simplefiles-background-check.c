#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main

#include <assert.h>

static void write_file(const char *path, const char *contents, mode_t mode)
{
    FILE *file = fopen(path, "w");

    assert(file);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
    assert(chmod(path, mode) == 0);
}

static void write_pattern_file(const char *path, size_t size)
{
    unsigned char pattern[8192];
    size_t remaining = size;
    int fd;

    for (size_t i = 0; i < sizeof(pattern); i++)
        pattern[i] = (unsigned char)((i * 37U + 11U) & 0xffU);

    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    while (remaining > 0) {
        size_t amount = remaining < sizeof(pattern) ?
                            remaining : sizeof(pattern);
        ssize_t written = write(fd, pattern, amount);

        assert(written > 0);
        remaining -= (size_t)written;
    }
    assert(close(fd) == 0);
}

static void assert_files_equal(const char *left, const char *right)
{
    unsigned char left_buffer[16384];
    unsigned char right_buffer[16384];
    int left_fd = open(left, O_RDONLY);
    int right_fd = open(right, O_RDONLY);

    assert(left_fd >= 0);
    assert(right_fd >= 0);
    for (;;) {
        ssize_t left_count = read(left_fd, left_buffer, sizeof(left_buffer));
        ssize_t right_count = read(right_fd, right_buffer,
                                   sizeof(right_buffer));

        assert(left_count >= 0);
        assert(right_count >= 0);
        assert(left_count == right_count);
        if (left_count == 0)
            break;
        assert(memcmp(left_buffer, right_buffer, (size_t)left_count) == 0);
    }
    assert(close(left_fd) == 0);
    assert(close(right_fd) == 0);
}

static void test_copy_engines(const char *root)
{
    const size_t file_size = 3U * 1024U * 1024U + 137U;
    char source[PATH_MAX];
    char accelerated[PATH_MAX];
    char buffered[PATH_MAX];
    int in;
    int out;

    join_path(source, root, "copy-source.bin");
    join_path(accelerated, root, "copy-accelerated.bin");
    join_path(buffered, root, "copy-buffered.bin");
    write_pattern_file(source, file_size);

    assert(ensure_paste_progress());
    memset((void *)paste_progress, 0, sizeof(*paste_progress));
    paste_progress->active = 1;
    assert(copy_file(source, accelerated, 0600) == 0);
    paste_progress->active = 0;
    assert(paste_progress->done_bytes == file_size);
    assert_files_equal(source, accelerated);

    in = open(source, O_RDONLY);
    out = open(buffered, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(in >= 0);
    assert(out >= 0);
    assert(copy_file_buffered(in, out) == 0);
    assert(close(in) == 0);
    assert(close(out) == 0);
    assert_files_equal(source, buffered);
}

static void test_paste_error_detail(const char *root)
{
    char missing[PATH_MAX];
    PasteResult result;

    join_path(missing, root, "no-such-parent/missing-source");
    clipboard_paths = calloc(1, sizeof(*clipboard_paths));
    assert(clipboard_paths);
    clipboard_count = 1;
    clipboard_capacity = 1;
    clipboard_mode = 'y';
    safe_copy(clipboard_paths[0], PATH_MAX, missing);

    perform_paste(root, &result);
    assert(result.ok == 0);
    assert(result.fail == 1);
    assert(result.first_error == ENOENT);

    free(clipboard_paths);
    clipboard_paths = NULL;
    clipboard_count = 0;
    clipboard_capacity = 0;
    clipboard_mode = 0;
}

static void wait_for_file_operation(void)
{
    for (int i = 0; i < 300 && file_operation_pid > 0; i++) {
        (void)check_background_file_operation();
        if (file_operation_pid > 0) usleep(10000);
    }
    assert(file_operation_pid < 0);
}

int main(void)
{
    char root[] = "/tmp/simplefiles-background-test.XXXXXX";
    char bin[PATH_MAX];
    char zip_tool[PATH_MAX];
    char unzip_tool[PATH_MAX];
    char input[PATH_MAX];
    char archive[PATH_MAX];
    char extracted[PATH_MAX];
    char path_env[PATH_MAX * 2];
    const char *old_path = getenv("PATH");

    assert(mkdtemp(root));
    join_path(bin, root, "bin");
    join_path(zip_tool, bin, "zip");
    join_path(unzip_tool, bin, "unzip");
    join_path(input, root, "input.txt");
    join_path(archive, root, "bundle.zip");
    join_path(extracted, root, "bundle/extracted.txt");
    assert(mkdir(bin, 0700) == 0);
    write_file(input, "input\n", 0600);
    test_copy_engines(root);
    test_paste_error_detail(root);
    write_file(zip_tool,
               "#!/bin/sh\nsleep 0.2\n: > \"$2\"\n",
               0700);
    write_file(unzip_tool,
               "#!/bin/sh\nsleep 0.2\n: > \"$4/extracted.txt\"\n",
               0700);
    snprintf(path_env, sizeof path_env, "%s:%s", bin,
             old_path && *old_path ? old_path : "/usr/bin:/bin");
    assert(setenv("PATH", path_env, 1) == 0);

    safe_copy(cwd_path, sizeof cwd_path, root);
    load_dir(cwd_path);
    set_cursor_to_name("input.txt");
    assert(strcmp(entries[cursor].name, "input.txt") == 0);
    command_compress("bundle");
    assert(file_operation_pid > 0);
    assert(file_operation_kind == FILE_OPERATION_COMPRESS);
    assert(strcmp(message, "compressing in background") == 0);
    wait_for_file_operation();
    assert(path_exists(archive));
    assert(strcmp(message, "compressed") == 0);

    set_cursor_to_name("bundle.zip");
    assert(strcmp(entries[cursor].name, "bundle.zip") == 0);
    command_extract();
    assert(file_operation_pid > 0);
    assert(file_operation_kind == FILE_OPERATION_EXTRACT);
    assert(strcmp(message, "extracting in background") == 0);
    wait_for_file_operation();
    assert(path_exists(extracted));
    assert(strcmp(message, "extracted") == 0);

    assert(remove_recursive(root) == 0);
    return 0;
}
