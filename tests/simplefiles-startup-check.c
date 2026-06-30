#define main simplefiles_main
#include "../simplefiles.c"
#undef main

#include <stdarg.h>

static int failures = 0;

static void fail_case(const char *label, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "simplefiles startup check failed: %s: ", label);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    failures++;
}

static void expect_cwd_path(const char *label, const char *expected)
{
    if (strcmp(cwd_path, expected) != 0)
        fail_case(label, "cwd_path='%s' expected='%s'", cwd_path, expected);
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        perror(path);
        exit(1);
    }
}

static void join_test_path(char *out, const char *dir, const char *name)
{
    if (!safe_join3(out, PATH_MAX, dir, "/", name)) {
        fprintf(stderr, "test path too long\n");
        exit(1);
    }
}

int main(void)
{
    char template[] = "/tmp/simplefiles-startup-XXXXXX";
    char *root = mkdtemp(template);
    char writing[PATH_MAX];
    char downloads[PATH_MAX];
    char target[PATH_MAX];
    char home[PATH_MAX];
    char orphan[PATH_MAX];
    char rel_target[] = "../target";
    int saved_cwd;

    if (!root) {
        perror("mkdtemp");
        return 1;
    }

    saved_cwd = open(".", O_RDONLY);
    if (saved_cwd < 0) {
        perror("open cwd");
        return 1;
    }

    join_test_path(writing, root, "writing");
    join_test_path(downloads, root, "Downloads");
    join_test_path(target, root, "target");
    join_test_path(home, root, "home");
    join_test_path(orphan, root, "orphan");
    make_dir(writing);
    make_dir(downloads);
    make_dir(target);
    make_dir(home);
    make_dir(orphan);
    setenv("HOME", home, 1);

    if (chdir(writing) != 0) {
        perror("chdir writing");
        return 1;
    }
    if (!startup_set_directory(NULL))
        fail_case("cwd startup writing", "startup_set_directory failed");
    expect_cwd_path("cwd startup writing", writing);

    if (chdir(downloads) != 0) {
        perror("chdir downloads");
        return 1;
    }
    if (!startup_set_directory(NULL))
        fail_case("cwd startup downloads", "startup_set_directory failed");
    expect_cwd_path("cwd startup downloads", downloads);

    if (chdir(writing) != 0) {
        perror("chdir writing for argv");
        return 1;
    }
    if (!startup_set_directory(rel_target))
        fail_case("argv path wins", "startup_set_directory failed");
    expect_cwd_path("argv path wins", target);

    if (chdir(orphan) != 0) {
        perror("chdir orphan");
        return 1;
    }
    if (rmdir(orphan) != 0) {
        perror("rmdir orphan");
        return 1;
    }
    startup_use_cwd_or_home();
    expect_cwd_path("getcwd fallback home", home);

    if (fchdir(saved_cwd) != 0) {
        perror("restore cwd");
        return 1;
    }
    close(saved_cwd);
    remove_recursive(root);

    if (failures) {
        fprintf(stderr, "%d simplefiles startup checks failed\n", failures);
        return 1;
    }

    puts("simplefiles startup checks passed");
    return 0;
}
