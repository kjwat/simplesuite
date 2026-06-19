
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define REPO "."
#define LOG  "/tmp/simplever.log"

static char status[512] = "Ready.";
static char output[8192] = "Welcome to simplever.\n";

static void load_output(void) {
    FILE *f = fopen(LOG, "r");
    if (!f) {
        snprintf(output, sizeof(output), "No output yet.\n");
        return;
    }

    size_t n = fread(output, 1, sizeof(output) - 1, f);
    output[n] = '\0';
    fclose(f);

    if (n == 0) {
        snprintf(output, sizeof(output), "(No output.)\n");
    }
}

static int run_cmd(const char *cmd) {
    char full[4096];
    snprintf(full, sizeof(full),
        "cd '%s' && { %s; } > '%s' 2>&1",
        REPO, cmd, LOG);

    int r = system(full);
    load_output();

    if (r == -1) {
        snprintf(status, sizeof(status), "Could not run command.");
        return 1;
    }

    if (WIFEXITED(r) && WEXITSTATUS(r) == 0) {
        snprintf(status, sizeof(status), "Success.");
        return 0;
    } else {
        snprintf(status, sizeof(status), "Git complained. See output pane.");
        return 1;
    }
}

static void clean_empty_output(const char *clean_msg) {
    if (output[0] == '\0' || strcmp(output, "(No output.)\n") == 0) {
        snprintf(output, sizeof(output), "%s\n", clean_msg);
    }
}

static void draw_wrapped_text(int y, int x, int maxh, int maxw, const char *text) {
    int row = 0;
    int col = 0;

    for (const char *s = text; *s && row < maxh; s++) {
        if (*s == '\n') {
            row++;
            col = 0;
            continue;
        }

        if (col >= maxw) {
            row++;
            col = 0;
            if (row >= maxh) break;
        }

        mvaddch(y + row, x + col, *s);
        col++;
    }
}

static void draw(void) {
    erase();

    int h, w;
    getmaxyx(stdscr, h, w);

    mvprintw(1, (w - 9) / 2, "simplever");

    int menu_x = (w - 34) / 2;
    if (menu_x < 2) menu_x = 2;

    mvprintw(4, menu_x, "[ p ]  pull repo");
    mvprintw(5, menu_x, "[ t ]  status");
    mvprintw(6, menu_x, "[ d ]  diff / changed files");
    mvprintw(7, menu_x, "[ u ]  upload only");
    mvprintw(8, menu_x, "[ s ]  save / push");
    mvprintw(9, menu_x, "[ l ]  latest commits");
    mvprintw(10, menu_x, "[ q ]  quit");

    int out_y = 11;
    int out_h = h - 16;
    if (out_h < 4) out_h = 4;

    mvhline(out_y - 1, 0, ACS_HLINE, w);
    mvprintw(out_y, 2, "Output:");
    draw_wrapped_text(out_y + 2, 2, out_h, w - 4, output);

    mvhline(h - 3, 0, ACS_HLINE, w);
    mvprintw(h - 2, 2, "Status: %.*s", w - 12, status);

    refresh();
}

static void prompt_commit(char *buf, size_t n) {
    int h, w;
    getmaxyx(stdscr, h, w);

    echo();
    curs_set(1);

    mvhline(h - 5, 0, ACS_HLINE, w);
    mvprintw(h - 4, 2, "Commit message: ");
    clrtoeol();
    refresh();

    getnstr(buf, n - 1);

    noecho();
    curs_set(0);
}

int main(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    run_cmd("git status -sb");
    clean_empty_output("Working tree clean.");

    while (1) {
        draw();
        int ch = getch();

        if (ch == 'q') break;

        if (ch == 'p') {
            snprintf(status, sizeof(status), "Pulling...");
            snprintf(output, sizeof(output), "Running git pull...\n");
            draw();
            run_cmd("git pull");
        }

        if (ch == 't') {
            snprintf(status, sizeof(status), "Checking status...");
            snprintf(output, sizeof(output), "Running git status --short...\n");
            draw();
            run_cmd("git fetch --quiet && git status -sb");
            clean_empty_output("Working tree clean.");
        }

        if (ch == 'd') {
            snprintf(status, sizeof(status), "Checking diff...");
            snprintf(output, sizeof(output), "Running git diff --stat...\n");
            draw();
            run_cmd("git diff --stat");
            clean_empty_output("No unstaged diff.");
        }

        if (ch == 'u') {
            snprintf(status, sizeof(status), "Uploading...");
            snprintf(output, sizeof(output), "Running git push...\n");
            draw();
            run_cmd("git push");
        }

        if (ch == 'l') {
            snprintf(status, sizeof(status), "Showing latest commits...");
            snprintf(output, sizeof(output), "Loading latest commits...\n");
            draw();
            run_cmd("git log --oneline --decorate -n 20");
        }

        if (ch == 's') {
            char msg[256] = {0};
            prompt_commit(msg, sizeof(msg));

            if (strlen(msg) == 0) {
                snprintf(status, sizeof(status), "Cancelled: empty commit message.");
                snprintf(output, sizeof(output), "Save cancelled.\n");
                continue;
            }

            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "git add -A && "
                "(git diff --cached --quiet || git commit -m \"%s\") && "
                "git push",
                msg);

            snprintf(status, sizeof(status), "Saving and pushing...");
            snprintf(output, sizeof(output), "Saving and pushing...\n");
            draw();
            run_cmd(cmd);
        }
    }

    endwin();
    return 0;
}
