#define main simplewords_editor_main
#include "../simplewords.c"
#undef main

#include <stdarg.h>

static int failures = 0;
static char test_root[PATH_MAX];
static int home_counter = 0;

static void fail_case(const char *label, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "persistence check failed: %s: ", label);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    failures++;
}

static void join_test_path(char *out, const char *dir, const char *name)
{
    if (!snprintf_ok(snprintf(out, PATH_MAX, "%s/%s", dir, name), PATH_MAX)) {
        fprintf(stderr, "test path too long\n");
        exit(2);
    }
}

static void make_dir_or_die(const char *path)
{
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        perror(path);
        exit(2);
    }
}

static void fresh_home(char *out)
{
    char name[64];

    snprintf(name, sizeof(name), "home-%d", ++home_counter);
    join_test_path(out, test_root, name);
    make_dir_or_die(out);
}

static void write_text_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (!fp) {
        perror(path);
        exit(2);
    }
    fputs(text, fp);
    if (fclose(fp) != 0) {
        perror(path);
        exit(2);
    }
}

static char *read_text_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long len;
    char *buf;

    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = calloc((size_t)len + 1, 1);
    if (!buf) {
        fclose(fp);
        exit(2);
    }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

static void set_file_time(const char *path, time_t when)
{
    struct utimbuf times;

    times.actime = when;
    times.modtime = when;
    if (utime(path, &times) != 0) {
        perror(path);
        exit(2);
    }
}

static void reset_editor_state(const char *home)
{
    setenv("HOME", home, 1);
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);

    for (int i = 0; i < line_count; i++)
        free(lines[i]);

    line_count = 1;
    lines[0] = new_line("");
    cy = 0;
    cx = 0;
    top = 0;
    goal_col = -1;
    filename[0] = '\0';
    last_open_file[0] = '\0';
    last_open_directory[0] = '\0';
    last_save_directory[0] = '\0';
    snprintf(untitled_name, sizeof(untitled_name), "untitled-test");
    dirty = 0;
    autosave_dirty = 0;
    last_edit_time = 0;
    status_msg[0] = '\0';
    status_time = 0;
    selecting = 0;
    sel_cy = 0;
    sel_cx = 0;
    suppress_undo = 0;
    last_type_time = 0;
    burst_chars = 0;
    terminate_requested = 0;
    screen_cache_valid = 0;
    config.autosave_interval = 60;
    clear_cursor_affinity();
}

static void set_single_line(const char *text)
{
    for (int i = 0; i < line_count; i++)
        free(lines[i]);
    line_count = 1;
    lines[0] = new_line(text);
    cy = 0;
    cx = (int)strlen(text);
    top = 0;
}

static void expect_buffer(const char *label, const char *expected)
{
    if (line_count != 1 || strcmp(lines[0], expected) != 0)
        fail_case(label, "buffer='%s' line_count=%d expected='%s'",
                  line_count > 0 && lines[0] ? lines[0] : "(null)",
                  line_count, expected);
}

static void expect_file_text(const char *label, const char *path, const char *expected)
{
    char *actual = read_text_file(path);

    if (!actual) {
        fail_case(label, "missing file %s", path);
        return;
    }
    if (strcmp(actual, expected) != 0)
        fail_case(label, "file='%s' expected='%s'", actual, expected);
    free(actual);
}

static void check_named_nonexistent_load_recovers_autosave(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    char autosave[PATH_MAX];
    LoadResult result;

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "missing.txt");
    autosave_path_for(doc, autosave, sizeof(autosave));
    write_text_file(autosave, "draft text");
    set_file_time(autosave, 1700000100);

    reset_editor_state(home);
    result = load_file_at_position(doc, 1, 0, 0, 0, 0);
    if (result != LOAD_RESULT_AUTOSAVE)
        fail_case("named nonexistent autosave", "result=%d", result);
    if (strcmp(filename, doc) != 0)
        fail_case("named nonexistent autosave", "filename='%s'", filename);
    if (!dirty || autosave_dirty)
        fail_case("named nonexistent autosave", "dirty=%d autosave_dirty=%d", dirty, autosave_dirty);
    expect_buffer("named nonexistent autosave", "draft text");
    if (strcmp(status_msg, "Recovered autosave") != 0)
        fail_case("named nonexistent autosave", "status='%s'", status_msg);
}

static void check_real_newer_wins_over_autosave(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    char autosave[PATH_MAX];
    LoadResult result;

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "saved.txt");
    write_text_file(doc, "saved");
    autosave_path_for(doc, autosave, sizeof(autosave));
    write_text_file(autosave, "stale draft");
    set_file_time(autosave, 1700000000);
    set_file_time(doc, 1700000100);

    reset_editor_state(home);
    result = load_file_at_position(doc, 1, 0, 0, 0, 0);
    if (result != LOAD_RESULT_DISK)
        fail_case("real newer wins", "result=%d", result);
    if (dirty || autosave_dirty)
        fail_case("real newer wins", "dirty=%d autosave_dirty=%d", dirty, autosave_dirty);
    expect_buffer("real newer wins", "saved");
}

static void check_autosave_newer_wins_over_real(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    char autosave[PATH_MAX];
    LoadResult result;

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "saved.txt");
    write_text_file(doc, "saved");
    autosave_path_for(doc, autosave, sizeof(autosave));
    write_text_file(autosave, "new draft");
    set_file_time(doc, 1700000000);
    set_file_time(autosave, 1700000100);

    reset_editor_state(home);
    result = load_file_at_position(doc, 1, 0, 0, 0, 0);
    if (result != LOAD_RESULT_AUTOSAVE)
        fail_case("autosave newer wins", "result=%d", result);
    if (!dirty || autosave_dirty)
        fail_case("autosave newer wins", "dirty=%d autosave_dirty=%d", dirty, autosave_dirty);
    expect_buffer("autosave newer wins", "new draft");
}

static void check_untitled_session_recovers_autosave(void)
{
    char home[PATH_MAX];
    int restored;

    fresh_home(home);
    reset_editor_state(home);
    snprintf(untitled_name, sizeof(untitled_name), "untitled-recover");
    set_single_line("untitled draft");
    mark_edit();
    flush_recovery_state();

    reset_editor_state(home);
    restored = load_session();
    if (!restored)
        fail_case("untitled session", "load_session returned 0");
    if (filename[0])
        fail_case("untitled session", "filename='%s'", filename);
    if (strcmp(untitled_name, "untitled-recover") != 0)
        fail_case("untitled session", "untitled='%s'", untitled_name);
    if (!dirty || autosave_dirty)
        fail_case("untitled session", "dirty=%d autosave_dirty=%d", dirty, autosave_dirty);
    expect_buffer("untitled session", "untitled draft");
    if (strcmp(status_msg, "Recovered untitled autosave") != 0)
        fail_case("untitled session", "status='%s'", status_msg);
}

static void check_missing_blank_session_does_not_claim_restore(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    int restored;

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "not-created.txt");
    snprintf(filename, sizeof(filename), "%s", doc);
    save_session();

    reset_editor_state(home);
    restored = load_session();
    if (restored)
        fail_case("missing blank session", "load_session returned 1");
    if (strcmp(status_msg, "Session restored") == 0)
        fail_case("missing blank session", "misleading status");
    expect_buffer("missing blank session", "");
}

static void check_named_session_recovers_missing_autosave(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    char autosave[PATH_MAX];
    int restored;

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "new-name.txt");
    snprintf(filename, sizeof(filename), "%s", doc);
    save_session();
    autosave_path_for(doc, autosave, sizeof(autosave));
    write_text_file(autosave, "session draft");
    set_file_time(autosave, 1700000100);

    reset_editor_state(home);
    restored = load_session();
    if (!restored)
        fail_case("named session missing autosave", "load_session returned 0");
    expect_buffer("named session missing autosave", "session draft");
    if (strcmp(status_msg, "Recovered autosave") != 0)
        fail_case("named session missing autosave", "status='%s'", status_msg);
}

static void check_flush_forces_autosave_before_interval(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    char autosave[PATH_MAX];

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "quick.txt");
    snprintf(filename, sizeof(filename), "%s", doc);
    set_single_line("quick draft");
    mark_edit();
    config.autosave_interval = 3600;
    autosave_path_for(doc, autosave, sizeof(autosave));

    flush_recovery_state();
    expect_file_text("forced flush", autosave, "quick draft");
}

static void check_save_removes_named_autosave(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    char autosave[PATH_MAX];

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "save.txt");
    snprintf(filename, sizeof(filename), "%s", doc);
    set_single_line("saved draft");
    mark_edit();
    autosave_path_for(doc, autosave, sizeof(autosave));
    autosave_file_now();
    if (!regular_file(autosave))
        fail_case("save removes autosave", "autosave was not created");

    save_file(0);
    if (regular_file(autosave))
        fail_case("save removes autosave", "autosave still exists");
    if (dirty || autosave_dirty)
        fail_case("save removes autosave", "dirty=%d autosave_dirty=%d", dirty, autosave_dirty);
    expect_file_text("save removes autosave", doc, "saved draft");
}

static void check_blank_autosave_is_written_not_unlinked(void)
{
    char home[PATH_MAX];
    char doc[PATH_MAX];
    char autosave[PATH_MAX];

    fresh_home(home);
    reset_editor_state(home);
    join_test_path(doc, home, "blank.txt");
    snprintf(filename, sizeof(filename), "%s", doc);
    autosave_path_for(doc, autosave, sizeof(autosave));

    set_single_line("old draft");
    mark_edit();
    autosave_file_now();
    if (!regular_file(autosave))
        fail_case("blank autosave", "initial autosave missing");

    set_single_line("");
    mark_edit();
    autosave_file_now();
    if (!regular_file(autosave))
        fail_case("blank autosave", "blank autosave was unlinked");
    expect_file_text("blank autosave", autosave, "");
}

int main(void)
{
    char templ[] = "/tmp/simplewords-persistence-XXXXXX";
    char *root = mkdtemp(templ);

    if (!root) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(test_root, sizeof(test_root), "%s", root);

    check_named_nonexistent_load_recovers_autosave();
    check_real_newer_wins_over_autosave();
    check_autosave_newer_wins_over_real();
    check_untitled_session_recovers_autosave();
    check_missing_blank_session_does_not_claim_restore();
    check_named_session_recovers_missing_autosave();
    check_flush_forces_autosave_before_interval();
    check_save_removes_named_autosave();
    check_blank_autosave_is_written_not_unlinked();

    reset_editor_state(test_root);
    free(lines[0]);
    lines[0] = NULL;

    if (failures) {
        fprintf(stderr, "%d persistence checks failed\n", failures);
        return 1;
    }

    puts("simplewords persistence checks passed");
    return 0;
}
