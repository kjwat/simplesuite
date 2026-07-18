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
