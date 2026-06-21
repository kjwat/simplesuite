// simplepdf.c
// Build: cc simplepdf.c -Wall -Wextra -O2 -lncurses -o simplepdf
// Needs: pdftotext from poppler-utils

#define _XOPEN_SOURCE 700
#include <ncurses.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINES 200000
#define MAX_LINE  4096
#define MAX_PAGES 20000
#define PAGE_LEFT_NUDGE 4
#define PAGE_SEPARATOR "\001simplepdf-page-break"

static char **lines = NULL;
static int line_count = 0;
static int top = 0;
static int hscroll = 0;
static int hscroll_user_set = 0;
static int layout_width = 0;
static int layout_left = MAX_LINE;
static int layout_right = 0;
static int wrap_width = 80;
static char title[512];
static int epub_mode = 0;
static char search_term[256] = "";
static int last_match = -1;
static int search_direction = 1;

typedef struct {
    int start;
    int end;
    int width;
} Page;

static Page pages[MAX_PAGES];
static int page_count = 0;

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p) {
        endwin();
        perror("strdup");
        exit(1);
    }
    return p;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;

    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = 0;

    return s;
}

static int is_only_page_number(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;

    if (!isdigit((unsigned char)*s)) return 0;

    int digits = 0;
    while (isdigit((unsigned char)*s)) {
        digits++;
        s++;
    }

    while (*s && isspace((unsigned char)*s)) s++;

    return *s == 0 && digits <= 4;
}



static int is_page_marker(const char *s)
{
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", s);
    char *t = trim(buf);

    size_t n = strlen(t);

    if (n >= 3 && t[0] == '-' && t[n - 1] == '-') {
        int ok = 1;

        for (size_t i = 1; i < n - 1; i++) {
            char c = t[i];

            if (!isdigit((unsigned char)c) &&
                strchr("IVXLCDMivxlcdm", c) == NULL)
            {
                ok = 0;
                break;
            }
        }

        if (ok) return 1;
    }

    return 0;
}


static int looks_like_page_marker(const char *s)
{
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", s);
    char *t = trim(buf);

    if (is_only_page_number(t)) return 1;
    if (is_page_marker(t)) return 1;

    int a, b;

    if (sscanf(t, "Page %d", &a) == 1) return 1;
    if (sscanf(t, "page %d", &a) == 1) return 1;
    if (sscanf(t, "%d / %d", &a, &b) == 2) return 1;
    if (sscanf(t, "%d/%d", &a, &b) == 2) return 1;
    if (sscanf(t, "%d of %d", &a, &b) == 2) return 1;

    return 0;
}

static int junk_line(const char *s)
{
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", s);
    char *t = trim(buf);

    if (*t == 0) return 0;
    if (looks_like_page_marker(t)) return 1;

    if (strcmp(t, "\f") == 0) return 1;

    return 0;
}

static void add_line(const char *s)
{
    if (line_count >= MAX_LINES) return;
    lines[line_count++] = xstrdup(s);
}

__attribute__((unused)) static int ends_sentence(const char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n <= 0) return 0;

    char c = s[n - 1];
    return c == '.' || c == '?' || c == '!' || c == ':' || c == ';' || c == '"' || c == '\'';
}

static void add_wrapped_paragraph(char *para)
{
    char *p = trim(para);
    if (!*p) return;

    char out[MAX_LINE];
    int len = 0;

    char *word = strtok(p, " \t\r\n");
    while (word) {
        int w = (int)strlen(word);

        if (len == 0) {
            snprintf(out, sizeof out, "%s", word);
            len = w;
        } else if (len + 1 + w > wrap_width) {
            add_line(out);
            snprintf(out, sizeof out, "%s", word);
            len = w;
        } else {
            strncat(out, " ", sizeof(out) - strlen(out) - 1);
            strncat(out, word, sizeof(out) - strlen(out) - 1);
            len += 1 + w;
        }

        word = strtok(NULL, " \t\r\n");
    }

    if (len > 0) add_line(out);
    add_line("");
}

static void free_lines(void)
{
    for (int i = 0; i < line_count; i++) free(lines[i]);
    line_count = 0;
    page_count = 0;
    layout_width = 0;
    layout_left = MAX_LINE;
    layout_right = 0;
    top = 0;
    hscroll = 0;
    hscroll_user_set = 0;
}

static int visual_len(const char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        n--;
    return n;
}

static int first_nonblank_col(const char *s)
{
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    return s[i] ? i : -1;
}

static int graphic_artifact_line(const char *s)
{
    int letters = 0;
    int digits = 0;
    int punctuation = 0;
    int marks = 0;
    int nonspace = 0;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isspace(*p))
            continue;

        nonspace++;

        if (isalpha(*p))
            letters++;
        else if (isdigit(*p))
            digits++;
        else if (ispunct(*p)) {
            punctuation++;
            if (strchr("_-=+*#.:;,'`~^|\\/<>[]{}()", *p))
                marks++;
        }
    }

    if (nonspace < 8)
        return 0;

    if (letters == 0 && digits <= 2 && punctuation * 100 / nonspace >= 70)
        return 1;

    if (marks >= 12 && letters * 100 / nonspace < 25)
        return 1;

    return 0;
}

static void begin_page(void)
{
    if (page_count >= MAX_PAGES)
        return;

    pages[page_count].start = line_count;
    pages[page_count].end = line_count;
    pages[page_count].width = 0;
    page_count++;
}

static void finish_page(void)
{
    if (page_count <= 0)
        return;

    pages[page_count - 1].end = line_count;
    if (pages[page_count - 1].width > layout_width)
        layout_width = pages[page_count - 1].width;
}

static void add_layout_line(const char *s)
{
    add_line(s);

    if (page_count <= 0)
        begin_page();

    int w = visual_len(s);
    if (w > pages[page_count - 1].width)
        pages[page_count - 1].width = w;

    int first = first_nonblank_col(s);
    if (first >= 0) {
        if (first < layout_left)
            layout_left = first;
        if (w > layout_right)
            layout_right = w;
    }
}

static void add_page_gap(void)
{
    add_line(PAGE_SEPARATOR);
}

static void add_wrapped_epub_paragraph(char *para)
{
    char *p = trim(para);
    if (!*p) return;

    char out[MAX_LINE];
    int len = 0;

    char *word = strtok(p, " \t\r\n");
    while (word) {
        int w = (int)strlen(word);

        if (len == 0) {
            snprintf(out, sizeof out, "%s", word);
            len = w;
        } else if (len + 1 + w > wrap_width) {
            add_layout_line(out);
            snprintf(out, sizeof out, "%s", word);
            len = w;
        } else {
            strncat(out, " ", sizeof(out) - strlen(out) - 1);
            strncat(out, word, sizeof(out) - strlen(out) - 1);
            len += 1 + w;
        }

        word = strtok(NULL, " \t\r\n");
    }

    if (len > 0)
        add_layout_line(out);

    /* EPUBs do not have PDF-style physical page breaks. Keep paragraph
       spacing readable, but do not invent dotted separator lines. */
    add_layout_line("");
}

static void load_layout_text(const char *txtpath)
{
    FILE *f = fopen(txtpath, "r");
    if (!f) {
        endwin();
        perror(txtpath);
        exit(1);
    }

    free_lines();
    begin_page();

    char raw[MAX_LINE];
    char segment[MAX_LINE];
    int seg_len = 0;

    while (fgets(raw, sizeof raw, f)) {
        for (char *p = raw; ; p++) {
            unsigned char c = (unsigned char)*p;

            if (c == '\f' || c == '\n' || c == '\0') {
                segment[seg_len] = '\0';

                if (seg_len > 0 || c == '\n' || c == '\f') {
                    if (graphic_artifact_line(segment))
                        add_layout_line("");
                    else
                        add_layout_line(segment);
                }

                seg_len = 0;

                if (c == '\f') {
                    finish_page();
                    add_page_gap();
                    begin_page();
                }

                if (c == '\0' || c == '\n')
                    break;
            } else if (c != '\r') {
                if (seg_len < MAX_LINE - 1)
                    segment[seg_len++] = (char)c;
            }
        }
    }

    fclose(f);

    finish_page();

    while (page_count > 1 &&
           pages[page_count - 1].start == pages[page_count - 1].end)
        page_count--;

    if (layout_left == MAX_LINE) {
        layout_left = 0;
        layout_right = layout_width;
    }

    if (line_count == 0) {
        begin_page();
        add_layout_line("[no readable text found]");
        finish_page();
    }
}

__attribute__((unused)) static void load_clean_text(const char *txtpath)
{
    FILE *f = fopen(txtpath, "r");
    if (!f) {
        endwin();
        perror(txtpath);
        exit(1);
    }

    free_lines();

    char raw[MAX_LINE];
    char para[MAX_LINE * 4];
    para[0] = 0;


    while (fgets(raw, sizeof raw, f)) {
        raw[strcspn(raw, "\n")] = 0;

        for (char *c = raw; *c; c++) {
            if (*c == '\f') *c = ' ';
        }

        char *t = trim(raw);

        if (junk_line(t)) continue;

        if (*t == 0) {
            if (para[0]) {
                add_wrapped_paragraph(para);
                para[0] = 0;
            }
            continue;
        }

        size_t plen = strlen(para);
        size_t tlen = strlen(t);

        if (plen + tlen + 4 >= sizeof para) {
            add_wrapped_paragraph(para);
            para[0] = 0;
            plen = 0;
        }

        if (plen == 0) {
            strncat(para, t, sizeof(para) - strlen(para) - 1);
        } else {
            int hyphen = plen > 0 && para[plen - 1] == '-';

            if (hyphen) {
                para[plen - 1] = 0;
                strncat(para, t, sizeof(para) - strlen(para) - 1);
            } else {
                strncat(para, " ", sizeof(para) - strlen(para) - 1);
                strncat(para, t, sizeof(para) - strlen(para) - 1);
            }
        }
    }

    if (para[0]) add_wrapped_paragraph(para);

    fclose(f);

    if (line_count == 0)
        add_line("[no readable text found]");
}


static void load_epub_text(const char *txtpath, int term_w, int term_h)
{
    FILE *f = fopen(txtpath, "r");
    if (!f) {
        endwin();
        perror(txtpath);
        exit(1);
    }

    free_lines();
    (void)term_h;

    int margin = term_w >= 100 ? 24 : 10;
    wrap_width = term_w - margin;
    if (wrap_width > 96)
        wrap_width = 96;
    if (wrap_width < 45)
        wrap_width = term_w > 8 ? term_w - 4 : term_w;
    if (wrap_width < 20)
        wrap_width = 20;

    begin_page();

    char raw[MAX_LINE];
    char para[MAX_LINE * 8];
    para[0] = 0;
    int blank_streak = 0;

    while (fgets(raw, sizeof raw, f)) {
        raw[strcspn(raw, "\n")] = 0;

        for (char *c = raw; *c; c++) {
            if (*c == '\f')
                *c = ' ';
        }

        char *t = trim(raw);

        if (junk_line(t))
            continue;

        /* EPUB/Pandoc sometimes emits ornamental divider lines:
           . . . . . . . . . . .
           These are not real page breaks, and they make EPUB reading ugly. */
        if (graphic_artifact_line(t))
            continue;

        if (*t == 0) {
            blank_streak++;
            if (para[0]) {
                add_wrapped_epub_paragraph(para);
                para[0] = 0;
            }
            continue;
        }

        /* Pandoc's plain writer may leave headings as short standalone lines.
           Preserve those instead of gluing them to the following prose. */
        if (blank_streak >= 2 && strlen(t) <= 72 && para[0] == 0) {
            char nextbuf[MAX_LINE];
            long pos = ftell(f);
            int followed_by_blank = 0;
            if (fgets(nextbuf, sizeof nextbuf, f)) {
                nextbuf[strcspn(nextbuf, "\n")] = 0;
                char *nt = trim(nextbuf);
                followed_by_blank = (*nt == 0);
                fseek(f, pos, SEEK_SET);
            }
            if (followed_by_blank) {
                add_wrapped_epub_paragraph(t);
                continue;
            }
        }


        size_t plen = strlen(para);
        size_t tlen = strlen(t);

        if (plen + tlen + 4 >= sizeof para) {
            add_wrapped_epub_paragraph(para);
            para[0] = 0;
            plen = 0;
        }

        if (plen == 0) {
            strncat(para, t, sizeof(para) - strlen(para) - 1);
        } else {
            int hyphen = plen > 0 && para[plen - 1] == '-';

            if (hyphen) {
                para[plen - 1] = 0;
                strncat(para, t, sizeof(para) - strlen(para) - 1);
            } else {
                strncat(para, " ", sizeof(para) - strlen(para) - 1);
                strncat(para, t, sizeof(para) - strlen(para) - 1);
            }
        }
    }

    if (para[0])
        add_wrapped_epub_paragraph(para);

    fclose(f);
    finish_page();

    while (page_count > 1 && pages[page_count - 1].start == pages[page_count - 1].end)
        page_count--;

    layout_left = 0;
    layout_right = layout_width;

    if (line_count == 0) {
        begin_page();
        add_layout_line("[no readable text found]");
        finish_page();
    }
}


static int has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ext) == 0;
}

static int run_extract_text(const char *infile, const char *txt)
{
    pid_t pid = fork();

    if (pid < 0) return -1;

    if (pid == 0) {
        if (has_ext(infile, ".epub")) {
            execlp("pandoc", "pandoc", infile, "-t", "plain", "--wrap=none", "-o", txt, NULL);
            _exit(127);
        } else {
            execlp("pdftotext", "pdftotext", "-layout", "-enc", "UTF-8", infile, txt, NULL);
            _exit(127);
        }
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;

    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int page_for_line(int line)
{
    if (page_count <= 0)
        return 0;

    for (int i = 0; i < page_count; i++) {
        if (line >= pages[i].start && line < pages[i].end)
            return i;
        if (i + 1 < page_count && line < pages[i + 1].start)
            return i;
    }

    return page_count - 1;
}

static int page_width(void)
{
    if (layout_right > layout_left)
        return layout_right - layout_left;
    return layout_width > 0 ? layout_width : 80;
}

static void page_viewport(int term_w, int *left, int *view_w, int *max_hscroll)
{
    int pw = page_width();
    int margin;

    if (epub_mode)
        margin = term_w >= 100 ? 24 : 10;
    else
        margin = term_w >= 90 ? 8 : 4;

    int avail = term_w - margin;

    if (avail < 20)
        avail = term_w;
    if (avail < 1)
        avail = 1;

    *view_w = pw < avail ? pw : avail;
    if (*view_w < 1)
        *view_w = 1;

    *left = (term_w - *view_w) / 2;
    if (*left < 0)
        *left = 0;

    if (!epub_mode && *left + PAGE_LEFT_NUDGE + *view_w <= term_w)
        *left += PAGE_LEFT_NUDGE;

    *max_hscroll = pw - *view_w;
    if (*max_hscroll < 0)
        *max_hscroll = 0;
}

static char *case_find(const char *haystack, const char *needle);

static void draw(void)
{
    erase();

    int h, w;
    getmaxyx(stdscr, h, w);

    const char *help = "q quit  f find  n/N next/prev  arrows scroll/pan  c center";
    int help_len = (int)strlen(help);
    int help_col = w - help_len - 2;

    if (help_col > 2) {
        char heading[640];
        snprintf(heading, sizeof heading, "simplepdf: %s", title);
        mvaddnstr(0, 2, heading, help_col - 3);
        mvaddnstr(0, help_col, help, help_len);
    } else {
        char heading[640];
        snprintf(heading, sizeof heading, "simplepdf: %s", title);
        mvaddnstr(0, 2, heading, w > 4 ? w - 4 : 0);
    }

    int body_h = h - 2;
    int page_w = page_width();
    int left, body_w, max_hscroll;
    page_viewport(w, &left, &body_w, &max_hscroll);
    (void)max_hscroll;

    for (int y = 0; y < body_h; y++) {
        int idx = top + y;
        if (idx >= line_count) break;

        const char *s = lines[idx];
        int len = (int)strlen(s);

        if (strcmp(s, PAGE_SEPARATOR) == 0) {
            if (!epub_mode) {
                for (int x = 0; x < body_w; x += 2)
                    mvaddch(y + 1, left + x, '.');
            }
            continue;
        }

        int source_col = layout_left < MAX_LINE ? layout_left + hscroll : hscroll;
        if (source_col < 0)
            source_col = 0;

        if (source_col < len)
            mvaddnstr(y + 1, left, s + source_col, body_w);

        if (idx == last_match && search_term[0]) {
            char *hit = case_find(s, search_term);
            if (hit) {
                int hit_col = (int)(hit - s);
                int term_len = (int)strlen(search_term);

                if (hit_col + term_len > source_col &&
                    hit_col < source_col + body_w)
                {
                    int screen_x = left + hit_col - source_col;
                    int draw_len = term_len;

                    if (screen_x < left) {
                        draw_len -= left - screen_x;
                        hit += left - screen_x;
                        screen_x = left;
                    }

                    if (screen_x + draw_len > left + body_w)
                        draw_len = left + body_w - screen_x;

                    if (draw_len > 0) {
                        attron(A_REVERSE);
                        mvaddnstr(y + 1, screen_x, hit, draw_len);
                        attroff(A_REVERSE);
                    }
                }
            }
        }
    }

    if (epub_mode) {
        int screen_lines = body_h > 0 ? body_h : 1;
        int screen = top / screen_lines + 1;
        int screens = (line_count + screen_lines - 1) / screen_lines;
        if (screens < 1)
            screens = 1;

        mvprintw(h - 1, 2, "screen %d/%d  line %d/%d  width:%d  x:%d",
                 screen, screens, top + 1, line_count, page_w, hscroll);
    } else {
        int page = page_for_line(top);
        mvprintw(h - 1, 2, "page %d/%d  line %d/%d  width:%d  x:%d",
                 page + 1, page_count > 0 ? page_count : 1,
                 top + 1, line_count, page_w, hscroll);
    }

    refresh();
}


static char *case_find(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return NULL;

    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return (char *)p;
    }

    return NULL;
}


static int find_prev_match(const char *term, int start)
{
    if (!term || !*term || line_count <= 0)
        return -1;

    if (start < 0)
        start = line_count - 1;

    if (start >= line_count)
        start = line_count - 1;

    for (int i = start; i >= 0; i--)
        if (case_find(lines[i], term))
            return i;

    for (int i = line_count - 1; i > start; i--)
        if (case_find(lines[i], term))
            return i;

    return -1;
}

static int find_next_match(const char *term, int start)
{
    if (!term || !*term || line_count <= 0)
        return -1;

    if (start < 0) start = 0;
    if (start >= line_count) start = 0;

    for (int i = start; i < line_count; i++)
        if (case_find(lines[i], term))
            return i;

    for (int i = 0; i < start; i++)
        if (case_find(lines[i], term))
            return i;

    return -1;
}

static void clamp_top(void)
{
    int h, w;
    getmaxyx(stdscr, h, w);

    int max_top = line_count - (h - 2);
    if (max_top < 0) max_top = 0;

    if (top < 0) top = 0;
    if (top > max_top) top = max_top;

    int left, body_w, max_hscroll;
    page_viewport(w, &left, &body_w, &max_hscroll);
    (void)left;
    (void)body_w;

    if (!hscroll_user_set)
        hscroll = max_hscroll / 2;
    if (hscroll < 0) hscroll = 0;
    if (hscroll > max_hscroll) hscroll = max_hscroll;
}

static void viewer_loop(const char *txtpath)
{
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int h, w;
    getmaxyx(stdscr, h, w);

    if (epub_mode)
        load_epub_text(txtpath, w, h);
    else
        load_layout_text(txtpath);

    int ch;
    while (1) {
        clamp_top();
        draw();

        ch = getch();

        if (ch == 'q' || ch == 27) break;
        else if (ch == KEY_DOWN || ch == 'j') top++;
        else if (ch == KEY_UP || ch == 'k') top--;
        else if (ch == KEY_RIGHT || ch == 'l') {
            hscroll += 4;
            hscroll_user_set = 1;
        }
        else if (ch == KEY_LEFT || ch == 'h') {
            hscroll -= 4;
            hscroll_user_set = 1;
        }
        else if (ch == 'c') {
            hscroll_user_set = 0;
        }
        else if (ch == 'f') {
            echo();
            curs_set(1);

            move(LINES - 1, 2);
            clrtoeol();
            mvprintw(LINES - 1, 2, "Find: ");

            char buf[sizeof search_term];
            snprintf(buf, sizeof buf, "%s", search_term);

            int rc = getnstr(buf, sizeof(buf) - 1);

            noecho();
            curs_set(0);

            if (rc != ERR && buf[0]) {
                snprintf(search_term, sizeof search_term, "%s", buf);
                int hit = find_next_match(search_term, top + 1);
                if (hit >= 0) {
                    top = hit;
                    last_match = hit;
                    search_direction = 1;
                }
            }
        }
        else if (ch == 'n') {
            if (search_term[0]) {
                int hit = find_next_match(search_term, last_match + 1);
                if (hit >= 0) {
                    top = hit;
                    last_match = hit;
                    search_direction = 1;
                }
            }
        }
        else if (ch == 'N') {
            if (search_term[0]) {
                int hit = find_prev_match(search_term, last_match - 1);
                if (hit >= 0) {
                    top = hit;
                    last_match = hit;
                    search_direction = -1;
                }
            }
        }
        else if (ch == KEY_NPAGE) top += LINES - 3;
        else if (ch == KEY_PPAGE) top -= LINES - 3;
        else if (ch == KEY_HOME || ch == 'g') top = 0;
        else if (ch == KEY_END || ch == 'G') top = line_count;
    }

    endwin();
}

static const char *basename_simple(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}



static int choose_file_tui(char *out, size_t outsz)
{
    FILE *fp = popen("find \"$HOME\" -type f \\( -iname '*.pdf' -o -iname '*.epub' \\) 2>/dev/null | fzf --height=90% --border --prompt='simplepdf> '", "r");
    if (!fp) return -1;

    if (!fgets(out, outsz, fp)) {
        pclose(fp);
        return -1;
    }

    out[strcspn(out, "\n")] = 0;

    int rc = pclose(fp);
    if (rc != 0 || out[0] == 0) return -1;

    return 0;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    char chosen[PATH_MAX];

    if (argc < 2) {
        if (choose_file_tui(chosen, sizeof chosen) != 0)
            return 1;
        argv[1] = chosen;
    }

    snprintf(title, sizeof title, "%s", basename_simple(argv[1]));
    epub_mode = has_ext(argv[1], ".epub");

    lines = calloc(MAX_LINES, sizeof(char *));
    if (!lines) {
        perror("calloc");
        return 1;
    }

    char txtpath[PATH_MAX];
    snprintf(txtpath, sizeof txtpath, "/tmp/simplepdf-%ld.txt", (long)getpid());

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    erase();
    mvprintw(0, 2, "simplepdf: %s", title);
    mvprintw(2, 2, "extracting text...");
    mvprintw(4, 2, "If fans go nuts here, pdftotext is wrestling the PDF.");
    refresh();

    int rc = run_extract_text(argv[1], txtpath);
    if (rc != 0) {
        endwin();
        fprintf(stderr, "simplepdf: text extraction failed. Need poppler-utils for PDF or pandoc for EPUB.\n");
        unlink(txtpath);
        free(lines);
        return 1;
    }

    endwin();
    viewer_loop(txtpath);

    unlink(txtpath);
    free_lines();
    free(lines);

    return 0;
}
