
#include <ncurses.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <locale.h>
#include <sys/wait.h>

static char repo_root[4096] = {0};
static char log_path[4096] = "simplever.log";

static char status[512] = "Ready.";
static char output[262144] = "Welcome to simplever.\n";
static int output_scroll = 0;

static int path_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

static int ensure_dir(const char *path) {
    struct stat st;

    if (!path || !*path) return 0;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    if (mkdir(path, 0700) == 0) return 1;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdirs(const char *path) {
    char tmp[4096];
    size_t len;

    if (!path || !*path) return 0;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len >= sizeof(tmp)) return 0;
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!ensure_dir(tmp)) return 0;
            *p = '/';
        }
    }

    return ensure_dir(tmp);
}

static int home_path(char *out, size_t size, const char *suffix) {
    const char *home = getenv("HOME");
    int written;

    if (!home || !*home || !suffix || !*suffix) return 0;
    written = snprintf(out, size, "%s/%s", home, suffix);
    return written > 0 && (size_t)written < size;
}

static int join_path(char *out, size_t size, const char *base, const char *leaf) {
    size_t base_len;
    size_t leaf_len;
    int needs_slash;

    if (!out || size == 0 || !base || !*base || !leaf || !*leaf) return 0;
    base_len = strlen(base);
    leaf_len = strlen(leaf);
    needs_slash = base[base_len - 1] != '/';
    if (base_len >= size || leaf_len >= size - base_len ||
        (size_t)needs_slash >= size - base_len - leaf_len)
        return 0;

    memcpy(out, base, base_len);
    if (needs_slash) out[base_len++] = '/';
    memcpy(out + base_len, leaf, leaf_len + 1);
    return 1;
}

static int copy_file_path(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[8192];
    size_t n;
    int ok = 1;

    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) ok = 0;
    if (fclose(in) != 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    if (!ok) unlink(dst);
    return ok;
}

static void migrate_log_file(const char *old_path, const char *new_path) {
    if (!old_path || !new_path || !*old_path || !*new_path) return;
    if (!path_exists(old_path) || path_exists(new_path)) return;
    if (rename(old_path, new_path) == 0) return;
    if (copy_file_path(old_path, new_path)) unlink(old_path);
}

static void load_output(void) {
    FILE *f = fopen(log_path, "r");
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

static void init_log_path(void) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CACHE_HOME");
    char state_dir[4096];
    char old_log[4096];

    if (!home || !*home ||
        !home_path(state_dir, sizeof(state_dir), ".local/state/simplever") ||
        !mkdirs(state_dir) ||
        !home_path(log_path, sizeof(log_path), ".local/state/simplever/simplever.log")) {
        log_path[0] = '\0';
        return;
    }

    if (xdg && *xdg &&
        join_path(old_log, sizeof(old_log), xdg, "simplever.log")) {
        migrate_log_file(old_log, log_path);
    }
    if (home_path(old_log, sizeof(old_log), ".cache/simplever.log")) {
        migrate_log_file(old_log, log_path);
    }
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
    char quoted_repo[8192];
    char quoted_log[8192];
    char *full;
    size_t full_size;

    if (!log_path[0]) {
        snprintf(output, sizeof(output),
            "No state directory available.\n\n"
            "Set HOME so simplever can write ~/.local/state/simplever/simplever.log.\n");
        snprintf(status, sizeof(status), "No state directory.");
        return 1;
    }

    if (repo_root[0] == '\0' && !find_repo_root()) {
        snprintf(output, sizeof(output),
            "Not inside a git repository.\n\n"
            "cd into a repo first, then run simplever.\n");
        snprintf(status, sizeof(status), "Not inside a git repository.");
        return 1;
    }

    shell_quote(repo_root, quoted_repo, sizeof(quoted_repo));
    shell_quote(log_path, quoted_log, sizeof(quoted_log));

    full_size = strlen(quoted_repo) + strlen(cmd) + strlen(quoted_log) + 32;
    full = malloc(full_size);
    if (!full) {
        snprintf(status, sizeof(status), "Out of memory.");
        return 1;
    }
    snprintf(full, full_size,
        "cd %s && { %s; } > %s 2>&1",
        quoted_repo, cmd, quoted_log);

    int r = system(full);
    free(full);
    load_output();
    output_scroll = 0;

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

static int wrapped_visual_rows(const char *text, int maxw) {
    int row = 0;
    int col = 0;

    if (maxw < 1) maxw = 1;
    if (!text || !*text) return 1;

    for (const char *s = text; *s; s++) {
        if (*s == '\n') {
            row++;
            col = 0;
            continue;
        }

        if (col >= maxw) {
            row++;
            col = 0;
        }

        col++;
    }

    return row + 1;
}

static int current_output_height(void) {
    int h, w;
    (void)w;
    getmaxyx(stdscr, h, w);

    int out_h = h - 16;
    if (out_h < 4) out_h = 4;
    return out_h;
}

static int current_output_width(void) {
    int h, w;
    (void)h;
    getmaxyx(stdscr, h, w);

    int out_w = w - 4;
    if (out_w < 1) out_w = 1;
    return out_w;
}

static int output_max_scroll(void) {
    int total = wrapped_visual_rows(output, current_output_width());
    int visible = current_output_height();
    int max = total - visible;
    return max > 0 ? max : 0;
}

static void clamp_output_scroll(void) {
    int max = output_max_scroll();
    if (output_scroll < 0) output_scroll = 0;
    if (output_scroll > max) output_scroll = max;
}

static void draw_wrapped_text(int y, int x, int maxh, int maxw, const char *text, int scroll) {
    int row = 0;
    int col = 0;

    if (maxw < 1) maxw = 1;
    if (scroll < 0) scroll = 0;

    for (const char *s = text; *s; s++) {
        if (*s == '\n') {
            row++;
            col = 0;
            if (row >= scroll + maxh) break;
            continue;
        }

        if (col >= maxw) {
            row++;
            col = 0;
            if (row >= scroll + maxh) break;
        }

        if (row >= scroll && row < scroll + maxh) {
            mvaddch(y + row - scroll, x + col, *s);
        }

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
    clamp_output_scroll();
    mvprintw(out_y, 2, "Output:");
    draw_wrapped_text(out_y + 2, 2, out_h, w - 4, output, output_scroll);

    int max_scroll = output_max_scroll();
    if (max_scroll > 0) {
        mvprintw(out_y, w - 32, "scroll %d/%d", output_scroll, max_scroll);
    }

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
    int cursor = 0;
    int ch;

    if (n == 0) return;
    buf[0] = '\0';

    noecho();
    curs_set(1);
    keypad(stdscr, TRUE);

    draw_commit_input(buf, cursor);

    while (1) {
        ch = getch();

        if (ch == 27) {
            buf[0] = '\0';
            break;
        }

        if (ch == '\n' || ch == '\r')
            break;

        if (ch == KEY_LEFT) {
            if (cursor > 0) cursor--;
            draw_commit_input(buf, cursor);
            continue;
        }

        if (ch == KEY_RIGHT) {
            if (cursor < len) cursor++;
            draw_commit_input(buf, cursor);
            continue;
        }

        if (ch == KEY_HOME || ch == 1) {
            cursor = 0;
            draw_commit_input(buf, cursor);
            continue;
        }

        if (ch == KEY_END || ch == 5) {
            cursor = len;
            draw_commit_input(buf, cursor);
            continue;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor, (size_t)(len - cursor + 1));
                cursor--;
                len--;
                draw_commit_input(buf, cursor);
            }
            continue;
        }

        if (ch == KEY_DC) {
            if (cursor < len) {
                memmove(buf + cursor, buf + cursor + 1, (size_t)(len - cursor));
                len--;
                draw_commit_input(buf, cursor);
            }
            continue;
        }

        if (ch >= 32 && ch < 127 && len + 1 < (int)n) {
            memmove(buf + cursor + 1, buf + cursor, (size_t)(len - cursor + 1));
            buf[cursor++] = (char)ch;
            len++;
            draw_commit_input(buf, cursor);
        }
    }

    curs_set(0);
    noecho();
}


int main(void) {
    setlocale(LC_ALL, "");
    init_log_path();

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

        if (ch == KEY_UP || ch == 'k') {
            output_scroll--;
            clamp_output_scroll();
            continue;
        }

        if (ch == KEY_DOWN || ch == 'j') {
            output_scroll++;
            clamp_output_scroll();
            continue;
        }

        if (ch == KEY_PPAGE) {
            output_scroll -= current_output_height();
            clamp_output_scroll();
            continue;
        }

        if (ch == KEY_NPAGE) {
            output_scroll += current_output_height();
            clamp_output_scroll();
            continue;
        }

        if (ch == KEY_HOME) {
            output_scroll = 0;
            continue;
        }

        if (ch == KEY_END) {
            output_scroll = output_max_scroll();
            continue;
        }

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
            def_prog_mode();
            endwin();

            fprintf(stderr, "[2J[H");
            fflush(stderr);

            int r = system("(git diff --color=always; printf '\n\n'; git ls-files --others --exclude-standard | while IFS= read -r f; do printf '\n\033[1;33mUNTRACKED FILE: %s\033[0m\n\n' \"$f\"; sed -n '1,200p' \"$f\"; printf '\n'; done) | ${PAGER:-less} -R");

            reset_prog_mode();
            refresh();

            if (r == 0) {
                snprintf(output, sizeof(output), "Returned from git diff.\n");
                snprintf(status, sizeof(status), "Ready.");
            } else {
                snprintf(output, sizeof(output), "git diff exited.\n");
                snprintf(status, sizeof(status), "Ready.");
            }
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
            char quoted_msg[1200];
            prompt_commit(msg, sizeof(msg));

            if (strlen(msg) == 0) {
                snprintf(status, sizeof(status), "Cancelled: empty commit message.");
                snprintf(output, sizeof(output), "Save cancelled.\n");
                continue;
            }

            shell_quote(msg, quoted_msg, sizeof(quoted_msg));

            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                "git add -A && "
                "(git diff --cached --quiet || git commit -m %s) && "
                "git push",
                quoted_msg);

            snprintf(status, sizeof(status), "Saving and pushing...");
            snprintf(output, sizeof(output), "Saving and pushing...\n");
            draw();
            run_cmd(cmd);
        }
    }

    endwin();
    return 0;
}
