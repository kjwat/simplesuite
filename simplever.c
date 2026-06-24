
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define LOG  "/tmp/simplever.log"

static char repo_root[4096] = {0};

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

static void shell_quote(const char *src, char *dst, size_t n) {
    size_t j = 0;
    if (n == 0) return;
    dst[j++] = '\'';
    for (size_t i = 0; src[i] && j + 5 < n; i++) {
        if (src[i] == '\'') {
            memcpy(dst + j, "'\\''", 4);
            j += 4;
        } else {
            dst[j++] = src[i];
        }
    }
    if (j + 1 < n) dst[j++] = '\'';
    dst[j] = '\0';
}

static int find_repo_root(void) {
    FILE *fp = popen("git rev-parse --show-toplevel 2>/dev/null", "r");
    if (!fp) return 0;

    if (!fgets(repo_root, sizeof(repo_root), fp)) {
        pclose(fp);
        repo_root[0] = '\0';
        return 0;
    }

    pclose(fp);
    repo_root[strcspn(repo_root, "\n")] = '\0';
    return repo_root[0] != '\0';
}

static int run_cmd(const char *cmd) {
    char full[8192];
    char quoted_repo[8192];

    if (repo_root[0] == '\0' && !find_repo_root()) {
        snprintf(output, sizeof(output),
            "Not inside a git repository.\n\n"
            "cd into a repo first, then run simplever.\n");
        snprintf(status, sizeof(status), "Not inside a git repository.");
        return 1;
    }

    shell_quote(repo_root, quoted_repo, sizeof(quoted_repo));

    snprintf(full, sizeof(full),
        "cd %s && { %s; } > '%s' 2>&1",
        quoted_repo, cmd, LOG);

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

static void draw_commit_input(const char *buf, int cursor) {
    int h, w;
    getmaxyx(stdscr, h, w);

    const char *label_text = "Commit message: ";
    int label_len = (int)strlen(label_text);
    int single_y = h - 3;
    int footer_line = h - 2;
    int footer_y = h - 1;

    /* Clear entire elastic prompt zone before redrawing, so shrinking
       after backspace doesn't leave ghost labels behind. */
    for (int r = h - 8; r < h; r++) {
        if (r >= 0) {
            move(r, 0);
            clrtoeol();
        }
    }

    int single_cap = w - 4 - label_len;
    if (single_cap < 8) single_cap = 8;

    int len = (int)strlen(buf);

    /*
     * Start skinny: one input line.
     * Only grow upward after the commit message consumes that line.
     */
    if (len <= single_cap) {
        mvhline(h - 4, 0, ACS_HLINE, w);

        move(single_y, 0);
        clrtoeol();
        mvprintw(single_y, 2, "%s", label_text);
        addnstr(buf, w - 4 - label_len);

        mvhline(footer_line, 0, ACS_HLINE, w);
        move(footer_y, 0);
        clrtoeol();
        mvprintw(footer_y, 2, "Enter commits, Esc cancels.");

        move(single_y, 2 + label_len + cursor);
        refresh();
        return;
    }

    int maxw = w - 6;
    if (maxw < 10) maxw = 10;

    int rows = (len + maxw - 1) / maxw;
    if (rows < 2) rows = 2;

    int max_rows = h - 8;
    if (max_rows < 2) max_rows = 2;
    if (rows > max_rows) rows = max_rows;

    int last = h - 3;
    int first = last - rows + 1;
    int label = first - 1;
    int top = label - 1;

    mvhline(top, 0, ACS_HLINE, w);

    move(label, 0);
    clrtoeol();
    mvprintw(label, 2, "Commit message:");

    for (int r = first; r <= last; r++) {
        move(r, 0);
        clrtoeol();
    }

    int start_i = 0;
    int visible_chars = rows * maxw;
    if (len > visible_chars)
        start_i = len - visible_chars;

    int row = 0;
    int col = 0;
    for (int i = start_i; buf[i] && row < rows; i++) {
        if (col >= maxw) {
            row++;
            col = 0;
            if (row >= rows) break;
        }
        mvaddch(first + row, 4 + col, buf[i]);
        col++;
    }

    int visible_cursor = cursor - start_i;
    if (visible_cursor < 0) visible_cursor = 0;

    int crow = visible_cursor / maxw;
    int ccol = visible_cursor % maxw;
    if (crow >= rows) {
        crow = rows - 1;
        ccol = maxw - 1;
    }

    mvhline(footer_line, 0, ACS_HLINE, w);
    move(footer_y, 0);
    clrtoeol();
    mvprintw(footer_y, 2, "Enter commits, Esc cancels.");

    move(first + crow, 4 + ccol);
    refresh();
}


static void prompt_commit(char *buf, size_t n) {
    int len = 0;
    int ch;

    if (n == 0) return;
    buf[0] = '\0';

    noecho();
    curs_set(1);
    keypad(stdscr, TRUE);

    draw_commit_input(buf, len);

    while (1) {
        ch = getch();

        if (ch == 27) {
            buf[0] = '\0';
            break;
        }

        if (ch == '\n' || ch == '\r')
            break;

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                buf[--len] = '\0';
                draw_commit_input(buf, len);
            }
            continue;
        }

        if (ch >= 32 && ch < 127 && len + 1 < (int)n) {
            buf[len++] = (char)ch;
            buf[len] = '\0';
            draw_commit_input(buf, len);
        }
    }

    curs_set(0);
    noecho();
}


int main(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (find_repo_root()) {
        char msg[4608];
        snprintf(msg, sizeof(msg), "Repo: %s\n\n", repo_root);
        run_cmd("git status -sb");
        if (strlen(output) + strlen(msg) < sizeof(output)) {
            memmove(output + strlen(msg), output, strlen(output) + 1);
            memcpy(output, msg, strlen(msg));
        }
        clean_empty_output("Working tree clean.");
    } else {
        snprintf(output, sizeof(output),
            "Not inside a git repository.\n\n"
            "cd into a repo first, then run simplever.\n");
        snprintf(status, sizeof(status), "Not inside a git repository.");
    }

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
