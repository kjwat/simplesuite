#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main

#include <assert.h>

static void expect_command(const char *text, int cursor_pos)
{
    assert(strcmp(command, text) == 0);
    assert(command_len == (int)strlen(text));
    assert(command_cursor == cursor_pos);
}

int main(void)
{
    start_command("rename sample.txt");
    expect_command("rename sample.txt", 17);

    handle_command_input(KEY_LEFT);
    handle_command_input(KEY_LEFT);
    handle_command_input(KEY_LEFT);
    handle_command_input('X');
    expect_command("rename sample.Xtxt", 15);

    handle_command_input(KEY_BACKSPACE);
    expect_command("rename sample.txt", 14);

    handle_command_input(KEY_HOME);
    handle_command_input('>');
    expect_command(">rename sample.txt", 1);

    handle_command_input(KEY_END);
    handle_command_input('<');
    expect_command(">rename sample.txt<", 19);

    handle_command_input(KEY_LEFT);
    handle_command_input(KEY_DC);
    expect_command(">rename sample.txt", 18);

    return 0;
}
