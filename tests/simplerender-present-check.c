#define _XOPEN_SOURCE 700
#define _XOPEN_SOURCE_EXTENDED 1

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include "../simplerender.h"

static char presented_char_at(int row, int col)
{
    chtype cell = mvwinch(curscr, row, col);

    assert(cell != (chtype)ERR);
    return (char)(cell & A_CHARTEXT);
}

int main(void)
{
    FILE *input;
    FILE *output;
    SCREEN *screen;
    SsrRenderer renderer;

    setlocale(LC_ALL, "");
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
    curs_set(0);

    ssr_init(&renderer);
    renderer.windowed_redraw_enabled = 1;

    erase();
    assert(ssr_render_text(&renderer, "article body", 0,
                           2, 4, 3, 12, A_NORMAL));
    assert(presented_char_at(2, 4) == 'a');

    /*
     * Reproduce SimpleNews' refresh path: stdscr is rebuilt, while the
     * overlapping article window and renderer cache remain unchanged.
     */
    erase();
    mvaddstr(0, 0, "TechCrunch");
    assert(ssr_render_text(&renderer, "article body", 0,
                           2, 4, 3, 12, A_NORMAL));
    assert(presented_char_at(2, 4) == 'a');

    ssr_destroy(&renderer);
    endwin();
    delscreen(screen);
    fclose(output);
    fclose(input);
    return 0;
}
