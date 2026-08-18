#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main

#include <assert.h>

static void use_utf8_locale(void)
{
    static const char *const candidates[] = {
        "C.UTF-8", "C.utf8", "en_US.UTF-8", "UTF-8", ""
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (setlocale(LC_ALL, candidates[i]) && MB_CUR_MAX > 1)
            return;
    }
    assert(!"a UTF-8 locale is required for this test");
}

static void expect_command(const char *text, int cursor_pos)
{
    assert(strcmp(command, text) == 0);
    assert(command_len == (int)strlen(text));
    assert(command_cursor == cursor_pos);
    assert(text_boundary_at_or_before(command, command_len,
                                      command_cursor) == command_cursor);
}

static void press_command_key(int key)
{
    handle_command_input((wint_t)key, 1);
}

static void type_command_character(wint_t character)
{
    handle_command_input(character, 0);
}

static void check_rename_target_is_stable(void)
{
    char root[] = "/tmp/simplefiles-command-XXXXXX";
    char original[PATH_MAX];
    char other[PATH_MAX];
    char renamed[PATH_MAX];
    const char *rename_command = "rename renamed.txt";
    int fd;

    assert(mkdtemp(root));
    join_path(original, root, "original.txt");
    join_path(other, root, "other.txt");
    join_path(renamed, root, "renamed.txt");

    fd = open(original, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
    fd = open(other, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);

    safe_copy(cwd_path, sizeof(cwd_path), root);
    memset(entries, 0, sizeof(entries));
    entry_count = 2;
    cursor = 0;
    safe_copy(entries[0].name, sizeof(entries[0].name), "original.txt");
    safe_copy(entries[1].name, sizeof(entries[1].name), "other.txt");

    start_rename_command();
    assert(strcmp(command_entry_path, original) == 0);

    /* Background paste completion can move the live cursor while the rename
     * editor is open.  Submitting must still rename the captured entry. */
    cursor = 1;
    safe_copy(command, sizeof(command), rename_command);
    command_len = (int)strlen(command);
    command_cursor = command_len;
    handle_command_input(L'\n', 0);

    assert(access(original, F_OK) != 0);
    assert(access(renamed, F_OK) == 0);
    assert(access(other, F_OK) == 0);
    assert(command_entry_path[0] == '\0');

    assert(unlink(renamed) == 0);
    assert(unlink(other) == 0);
    assert(rmdir(root) == 0);
}

static void check_command_render(void)
{
    FILE *input;
    FILE *output;
    SCREEN *screen;
    WINDOW *footer;
    int cursor_y;
    int cursor_x;
    int expected_x;

    assert(setenv("TERM", "xterm-256color", 1) == 0);
    input = tmpfile();
    output = tmpfile();
    assert(input);
    assert(output);

    screen = newterm(NULL, output, input);
    assert(screen);
    set_term(screen);
    noecho();
    cbreak();

    footer = newwin(1, 40, 0, 0);
    assert(footer);
    wbkgdset(footer, (chtype)' ' | A_REVERSE);
    assert(mvwhline(footer, 0, 0, ']', 40) != ERR);

    start_command("rename \xE7\x95\x8C");
    draw_command_status(footer, 40);
    expected_x = 1 + text_columns_between(
        command, command_len, 0, command_cursor);
    getyx(footer, cursor_y, cursor_x);
    assert(cursor_y == 0);
    assert(cursor_x == expected_x);

    /* The old closing bracket is gone from every cell after the new text. */
    for (int x = expected_x; x < 40; x++) {
        chtype cell = mvwinch(footer, 0, x);
        assert(cell != (chtype)ERR);
        assert((cell & A_CHARTEXT) == ' ');
    }

    delwin(footer);
    endwin();
    delscreen(screen);
    fclose(output);
    fclose(input);
}

int main(void)
{
    use_utf8_locale();

    start_command("rename sample.txt");
    expect_command("rename sample.txt", 17);

    press_command_key(KEY_LEFT);
    press_command_key(KEY_LEFT);
    press_command_key(KEY_LEFT);
    type_command_character(L'X');
    expect_command("rename sample.Xtxt", 15);

    press_command_key(KEY_BACKSPACE);
    expect_command("rename sample.txt", 14);

    press_command_key(KEY_HOME);
    type_command_character(L'>');
    expect_command(">rename sample.txt", 1);

    press_command_key(KEY_END);
    type_command_character(L'<');
    expect_command(">rename sample.txt<", 19);

    press_command_key(KEY_LEFT);
    press_command_key(KEY_DC);
    expect_command(">rename sample.txt", 18);

    /* The rename cursor starts before the extension.  The first Backspace
     * therefore removes the closing video-ID bracket instead of leaving it
     * stranded to the right of the cursor. */
    memset(entries, 0, sizeof(entries));
    entry_count = 1;
    cursor = 0;
    safe_copy(entries[0].name, sizeof(entries[0].name),
              "Don Carlo Gesualdo (1566-1613) : Tenebrae Responsoria "
              "(excerpts) [gMQPTINinvs].mp3");
    start_rename_command();
    assert(command[command_cursor] == '.');
    assert(command[command_cursor - 1] == ']');
    press_command_key(KEY_BACKSPACE);
    assert(strstr(command, "[gMQPTINinvs].mp3") == NULL);
    assert(strstr(command, "[gMQPTINinvs.mp3") != NULL);
    assert(command[command_cursor] == '.');

    /* A multibyte character is one editing unit: Left, Backspace and Delete
     * can never stop in the middle of its UTF-8 bytes. */
    safe_copy(entries[0].name, sizeof(entries[0].name),
              "caf\xC3\xA9 \xF0\x9F\x8E\xB5.mp3");
    start_rename_command();
    assert(command[command_cursor] == '.');
    press_command_key(KEY_BACKSPACE);
    expect_command("rename caf\xC3\xA9 .mp3",
                   (int)strlen("rename caf\xC3\xA9 "));
    press_command_key(KEY_BACKSPACE);
    press_command_key(KEY_BACKSPACE);
    expect_command("rename caf.mp3", (int)strlen("rename caf"));

    start_command("rename .profile");
    entries[0].is_dir = 0;
    safe_copy(entries[0].name, sizeof(entries[0].name), ".profile");
    start_rename_command();
    expect_command("rename .profile", (int)strlen("rename .profile"));

    entries[0].is_dir = 1;
    safe_copy(entries[0].name, sizeof(entries[0].name), "folder.with.dot");
    start_rename_command();
    expect_command("rename folder.with.dot",
                   (int)strlen("rename folder.with.dot"));

    /* Wide-character input is encoded once and cannot collide with a KEY_*
     * value that happens to have the same integer code point. */
    start_command("rename ");
    type_command_character(L'\x03BB');
    expect_command("rename \xCE\xBB", (int)strlen("rename \xCE\xBB"));

    start_command("");
    type_command_character((wint_t)KEY_DOWN); /* U+0102, not Down. */
    assert(command_len > 0);
    assert(command_cursor == command_len);

    /* Horizontal scrolling uses terminal cells, not UTF-8 byte counts. */
    start_command("rename \xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C");
    adjust_command_view(5);
    assert(command_view_start > 0);
    assert(text_boundary_at_or_before(command, command_len,
                                      command_view_start) ==
           command_view_start);
    assert(text_columns_between(command, command_len, command_view_start,
                                command_cursor) <= 4);

    start_search();
    handle_search_input(L'\x03BB', 0);
    assert(strcmp(search_query, "\xCE\xBB") == 0);
    handle_search_input(KEY_BACKSPACE, 1);
    assert(search_query[0] == '\0');

    check_command_render();
    check_rename_target_is_stable();

    return 0;
}
