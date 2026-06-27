// simplepdf.c
// Build: cc simplepdf.c -Wall -Wextra -O2 -lncursesw -o simplepdf
// Needs: pdftotext from poppler-utils

#define _XOPEN_SOURCE 700
#include <curses.h>
#include <wchar.h>
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

/* SimpleWords/SimpleMail rendering stack:
   - prose lives in its own body window
   - scrolling repaints only the body pane
   - header/footer are redrawn only when their text or size changes
   - no terminal scroll tricks, no touchwin() shoving the body around */
static WINDOW *body_win = NULL;
static int cached_term_h = -1;
static int cached_term_w = -1;
static int body_win_h = 0;
static int body_win_w = 0;
static int chrome_dirty = 1;
static char last_header[1024] = "";
static char last_footer[256] = "";

static int body_win_left = 0;

typedef struct {
    int valid;
    int line_idx;
    int source_col;
    int body_w;
    int last_match;
    char search_term[256];
} BodyRowCache;

static BodyRowCache *body_row_cache = NULL;
static int body_row_cache_h = 0;

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

static attr_t body_attr(void)
{
    return A_NORMAL;
}

static attr_t search_attr(void)
{
    return A_REVERSE;
}

static void invalidate_body_cache(void)
{
    if (!body_row_cache)
        return;

    for (int i = 0; i < body_row_cache_h; i++)
        body_row_cache[i].valid = 0;
}

static void ensure_body_cache(int h)
{
    if (h < 1)
        h = 1;

    if (body_row_cache_h == h && body_row_cache)
        return;

    free(body_row_cache);
    body_row_cache = calloc((size_t)h, sizeof(*body_row_cache));
    if (!body_row_cache) {
        endwin();
        perror("calloc");
        exit(1);
    }
    body_row_cache_h = h;
}

static void destroy_body_window(void)
{
    if (body_win) {
        delwin(body_win);
        body_win = NULL;
    }
    body_win_h = 0;
    body_win_w = 0;
    body_win_left = 0;
    invalidate_body_cache();
}

static int utf8_decode_pdf(const char *s, wchar_t *wc, int *bytes_used)
{
    mbstate_t st;
    size_t n;
    int w;

    memset(&st, 0, sizeof(st));
    n = mbrtowc(wc, s, MB_CUR_MAX, &st);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *wc = L'\xfffd';
        *bytes_used = 1;
        return 1;
    }

    *bytes_used = (int)n;
    w = wcwidth(*wc);
    return w < 1 ? 1 : w;
}

static const char *utf8_ptr_at_col(const char *s, int target_col)
{
    int col = 0;

    if (target_col <= 0)
        return s;

    while (*s && col < target_col) {
        int bytes = 1;
        int w;

        if (*s == '\t') {
            w = 4 - (col % 4);
            bytes = 1;
        } else {
            wchar_t wc;
            w = utf8_decode_pdf(s, &wc, &bytes);
        }

        if (col + w > target_col)
            break;
        col += w;
        s += bytes;
    }

    return s;
}

static int utf8_width_between(const char *s, const char *end)
{
    int col = 0;

    while (*s && (!end || s < end)) {
        int bytes = 1;
        int w;

        if (*s == '\t') {
            w = 4 - (col % 4);
            bytes = 1;
        } else {
            wchar_t wc;
            w = utf8_decode_pdf(s, &wc, &bytes);
        }

        if (end && s + bytes > end)
            break;
        col += w;
        s += bytes;
    }

    return col;
}

static int draw_utf8_range_clipped(WINDOW *win, int row, int col,
                                   const char *s, const char *end,
                                   attr_t attr, int maxw)
{
    int used_width = 0;

    if (!win || row < 0 || col < 0 || maxw <= 0)
        return 0;

    while (*s && (!end || s < end) && used_width < maxw) {
        int bytes = 1;
        int width;

        if (*s == '\t') {
            int spaces = 4 - ((col + used_width) % 4);
            if (spaces > maxw - used_width)
                spaces = maxw - used_width;
            wattrset(win, attr);
            mvwhline(win, row, col + used_width, ' ', spaces);
            used_width += spaces;
            s++;
            continue;
        }

        wchar_t wc;
        wchar_t text[2];
        cchar_t cell;

        width = utf8_decode_pdf(s, &wc, &bytes);
        if (end && s + bytes > end)
            break;
        if (used_width + width > maxw)
            break;

        text[0] = wc;
        text[1] = L'\0';
        setcchar(&cell, text, attr, 0, NULL);
        mvwadd_wch(win, row, col + used_width, &cell);
        used_width += width;
        s += bytes;
    }

    return used_width;
}

static void draw_utf8_clipped(WINDOW *win, int row, int col,
                              const char *s, attr_t attr, int maxw)
{
    draw_utf8_range_clipped(win, row, col, s, NULL, attr, maxw);
}

static void ensure_body_window(int desired_left, int desired_w, int desired_h)
{
    int h, w;
    getmaxyx(stdscr, h, w);

    if (desired_h < 1)
        desired_h = 1;
    if (desired_w < 1)
        desired_w = 1;
    if (desired_left < 0)
        desired_left = 0;
    if (desired_left + desired_w > w)
        desired_w = w - desired_left;
    if (desired_w < 1)
        desired_w = 1;

    if (h != cached_term_h || w != cached_term_w) {
        cached_term_h = h;
        cached_term_w = w;
        chrome_dirty = 1;
        destroy_body_window();
        werase(stdscr);
    }

    ensure_body_cache(desired_h);

    if (!body_win || body_win_h != desired_h ||
        body_win_w != desired_w || body_win_left != desired_left)
    {
        destroy_body_window();
        body_win_h = desired_h;
        body_win_w = desired_w;
        body_win_left = desired_left;
        body_win = newwin(body_win_h, body_win_w, 1, body_win_left);
        if (!body_win) {
            endwin();
            fprintf(stderr, "simplepdf: could not create body window\n");
            exit(1);
        }
        keypad(body_win, TRUE);
        wbkgdset(body_win, (chtype)' ' | body_attr());
        scrollok(body_win, FALSE);
        idlok(body_win, FALSE);
        leaveok(body_win, TRUE);
        chrome_dirty = 1;
        invalidate_body_cache();
    }
}

static void compose_header(char *out, size_t outsz, int w)
{
    const char *help = "q quit  f find  n/N next/prev  arrows scroll/pan  c center";
    int help_len = (int)strlen(help);
    int help_col = w - help_len - 2;

    if (help_col > 2) {
        char heading[640];
        snprintf(heading, sizeof heading, "simplepdf: %s", title);
        snprintf(out, outsz, "%-*.*s%s", help_col - 2, help_col - 3,
                 heading, help);
    } else {
        char heading[640];
        snprintf(heading, sizeof heading, "simplepdf: %s", title);
        snprintf(out, outsz, "%.*s", w > 4 ? w - 4 : 0, heading);
    }
}

static void compose_footer(char *out, size_t outsz, int body_h, int page_w)
{
    if (epub_mode) {
        int screen_lines = body_h > 0 ? body_h : 1;
        int screen = top / screen_lines + 1;
        int screens = (line_count + screen_lines - 1) / screen_lines;
        if (screens < 1)
            screens = 1;

        snprintf(out, outsz, "screen %d/%d  line %d/%d  width:%d  x:%d",
                 screen, screens, top + 1, line_count, page_w, hscroll);
    } else {
        int page = page_for_line(top);
        snprintf(out, outsz, "page %d/%d  line %d/%d  width:%d  x:%d",
                 page + 1, page_count > 0 ? page_count : 1,
                 top + 1, line_count, page_w, hscroll);
    }
}

static void draw_chrome_if_needed(int h, int w, int body_h, int page_w)
{
    char header[sizeof last_header];
    char footer[sizeof last_footer];

    compose_header(header, sizeof header, w);
    compose_footer(footer, sizeof footer, body_h, page_w);

    int header_changed = chrome_dirty || strcmp(header, last_header) != 0;
    int footer_changed = chrome_dirty || strcmp(footer, last_footer) != 0;

    if (!header_changed && !footer_changed)
        return;

    if (header_changed) {
        snprintf(last_header, sizeof last_header, "%s", header);
        attrset(body_attr());
        move(0, 0);
        clrtoeol();
        if (w > 4)
            draw_utf8_clipped(stdscr, 0, 2, header, body_attr(), w - 4);
    }

    if (footer_changed) {
        snprintf(last_footer, sizeof last_footer, "%s", footer);
        attrset(body_attr());
        move(h - 1, 0);
        clrtoeol();
        if (w > 4)
            draw_utf8_clipped(stdscr, h - 1, 2, footer, body_attr(), w - 4);
    }

    wnoutrefresh(stdscr);
    chrome_dirty = 0;
}

static int row_cache_matches(int y, int idx, int source_col, int body_w)
{
    BodyRowCache *c;

    if (!body_row_cache || y < 0 || y >= body_row_cache_h)
        return 0;

    c = &body_row_cache[y];
    return c->valid &&
           c->line_idx == idx &&
           c->source_col == source_col &&
           c->body_w == body_w &&
           c->last_match == last_match &&
           strcmp(c->search_term, search_term) == 0;
}

static void remember_row_cache(int y, int idx, int source_col, int body_w)
{
    BodyRowCache *c;

    if (!body_row_cache || y < 0 || y >= body_row_cache_h)
        return;

    c = &body_row_cache[y];
    c->valid = 1;
    c->line_idx = idx;
    c->source_col = source_col;
    c->body_w = body_w;
    c->last_match = last_match;
    snprintf(c->search_term, sizeof c->search_term, "%s", search_term);
}

static void clear_body_row(int y, int body_w)
{
    if (y < 0 || y >= body_win_h)
        return;
    wattrset(body_win, body_attr());
    mvwhline(body_win, y, 0, ' ', body_w);
}

static void draw_body_row(int y, int idx, int source_col, int body_w)
{
    const char *s;
    const char *visible;
    const char *visible_end;

    clear_body_row(y, body_w);

    if (idx < 0 || idx >= line_count)
        return;

    s = lines[idx];

    if (strcmp(s, PAGE_SEPARATOR) == 0) {
        if (!epub_mode) {
            wattrset(body_win, body_attr());
            for (int x = 0; x < body_w; x += 2)
                mvwaddch(body_win, y, x, '.');
        }
        return;
    }

    if (source_col < 0)
        source_col = 0;

    visible = utf8_ptr_at_col(s, source_col);
    visible_end = utf8_ptr_at_col(s, source_col + body_w);

    draw_utf8_clipped(body_win, y, 0, visible, body_attr(), body_w);

    if (idx == last_match && search_term[0]) {
        char *hit = case_find(s, search_term);
        if (hit) {
            const char *hit_start = hit;
            const char *hit_end = hit + strlen(search_term);

            if (hit_end > visible && hit_start < visible_end) {
                const char *draw_start = hit_start < visible ? visible : hit_start;
                const char *draw_end = hit_end > visible_end ? visible_end : hit_end;
                int screen_x = utf8_width_between(visible, draw_start);

                if (screen_x < body_w)
                    draw_utf8_range_clipped(body_win, y, screen_x,
                                            draw_start, draw_end,
                                            search_attr(), body_w - screen_x);
            }
        }
    }
}

static void draw_body(int body_h, int body_w)
{
    int source_col = layout_left < MAX_LINE ? layout_left + hscroll : hscroll;

    if (source_col < 0)
        source_col = 0;

    for (int y = 0; y < body_h; y++) {
        int idx = top + y;

        if (idx >= line_count) {
            if (body_row_cache && y < body_row_cache_h && body_row_cache[y].valid) {
                clear_body_row(y, body_w);
                body_row_cache[y].valid = 0;
            }
            continue;
        }

        if (row_cache_matches(y, idx, source_col, body_w))
            continue;

        draw_body_row(y, idx, source_col, body_w);
        remember_row_cache(y, idx, source_col, body_w);
    }

    wnoutrefresh(body_win);
}

static void draw(void)
{
    int h, w;
    int body_h;
    int page_w;
    int left, body_w, max_hscroll;

    getmaxyx(stdscr, h, w);

    body_h = h - 2;
    if (body_h < 1)
        body_h = 1;

    page_w = page_width();
    page_viewport(w, &left, &body_w, &max_hscroll);
    (void)max_hscroll;

    ensure_body_window(left, body_w, body_h);

    draw_body(body_h, body_w);
    draw_chrome_if_needed(h, w, body_h, page_w);
    doupdate();
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
        else if (ch == KEY_RESIZE) {
            chrome_dirty = 1;
            invalidate_body_cache();
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
            chrome_dirty = 1;
            invalidate_body_cache();

            if (rc != ERR && buf[0]) {
                snprintf(search_term, sizeof search_term, "%s", buf);
                int hit = find_next_match(search_term, top + 1);
                if (hit >= 0) {
                    top = hit;
                    last_match = hit;
                    search_direction = 1;
                    invalidate_body_cache();
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
                    invalidate_body_cache();
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
                    invalidate_body_cache();
                }
            }
        }
        else if (ch == KEY_NPAGE) top += LINES - 3;
        else if (ch == KEY_PPAGE) top -= LINES - 3;
        else if (ch == KEY_HOME || ch == 'g') top = 0;
        else if (ch == KEY_END || ch == 'G') top = line_count;
    }

    destroy_body_window();
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
    snprintf(txtpath, sizeof txtpath, "/tmp/simplepdf-XXXXXX");
    int tmpfd = mkstemp(txtpath);
    if (tmpfd < 0) {
        fprintf(stderr, "simplepdf: cannot create temporary file: %s\n", strerror(errno));
        free(lines);
        return 1;
    }
    close(tmpfd);

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
