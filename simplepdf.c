// simplepdf.c
// Build from the SimpleSuite directory with ./build.sh.
// Needs: pdftotext and pdftohtml from poppler-utils; unzip for fast EPUBs

#define _XOPEN_SOURCE 700
#include <curses.h>
#include <wchar.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "simpleepub.h"

#define INITIAL_LINE_CAPACITY 16384
#define MAX_LINE  4096
#define MAX_PAGES 20000
#define MAX_PARAGRAPH (MAX_LINE * 8)
#define READER_MAX_WIDTH 88
#define READER_MIN_WIDTH 36
#define PDF_CACHE_VERSION "2"
#define EPUB_CACHE_VERSION "4"
#define MAX_NAV_HISTORY 64
#define PDF_MAX_EXTRACT_JOBS 8
#define PDF_MIN_PAGES_PER_JOB 256
#define PDF_EXTRACT_RETRY_SINGLE (-2)
#define EPUB_TOC_SOURCE_LIMIT 6000
#define PDF_KEY_LINK_PREV (KEY_MAX + 101)
#define PDF_KEY_LINK_NEXT (KEY_MAX + 102)

static char **lines = NULL;
static unsigned char *line_kinds = NULL;
static int *line_pages = NULL;
static int line_count = 0;
static size_t line_capacity = 0;
static int top = 0;
static int hscroll = 0;
static int hscroll_user_set = 0;
static int layout_width = 0;
static int layout_left = MAX_LINE;
static int layout_right = 0;
static int wrap_width = 80;
static char title[512];
static int epub_mode = 0;
static int reading_mode = 1;
static char search_term[256] = "";
static int last_match = -1;
static int show_ui = 1;
static char notice[256] = "";
static char document_path[PATH_MAX] = "";
static int pdf_page_total = 0;

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
static int body_win_top = 0;

typedef struct {
    int valid;
    int line_idx;
    int source_col;
    int body_w;
    int last_match;
    int selected_link;
    unsigned int link_revision;
    char search_term[256];
} BodyRowCache;

static BodyRowCache *body_row_cache = NULL;
static int body_row_cache_h = 0;

typedef struct {
    int start;
    int end;
    int width;
    int left;
    int right;
} Page;

static Page pages[MAX_PAGES];
static int page_count = 0;

typedef struct {
    char *label;
    char *source_text;
    int source_offset;
    int source_page;
    int target_page;
    int target_line;
    int repair_target;
    int synthetic;
    int exact_source;
    int line_idx;
    int start_byte;
    int end_byte;
} DocumentLink;

typedef struct {
    char *label;
    int line_idx;
    int page;
} Chapter;

typedef struct {
    char *key;
    int line_idx;
} EpubTarget;

typedef struct {
    char *target_key;
    char *label;
    int source_hint;
} EpubPendingLink;

typedef struct {
    char *target_key;
    char *label;
    size_t label_length;
    size_t label_capacity;
    int source_hint;
} EpubMarkerState;

typedef struct {
    int top;
    int hscroll;
    int hscroll_user_set;
    int page;
} NavHistory;

static DocumentLink *document_links = NULL;
static int document_link_count = 0;
static int document_link_capacity = 0;
static unsigned char link_page_state[MAX_PAGES];
static int selected_link = -1;
static unsigned int link_revision = 1;

static Chapter *chapters = NULL;
static int chapter_count = 0;
static int chapter_capacity = 0;
static int *epub_heading_lines = NULL;
static int epub_heading_count = 0;
static EpubTarget *epub_targets = NULL;
static int epub_target_count = 0;
static int epub_target_capacity = 0;
static EpubPendingLink *epub_pending_links = NULL;
static int epub_pending_link_count = 0;
static int epub_pending_link_capacity = 0;
static int epub_exact_link_count = 0;

static NavHistory nav_history[MAX_NAV_HISTORY];
static int nav_history_count = 0;

typedef struct {
    int code;
    int action;
} PdfKeyMapping;

static PdfKeyMapping runtime_keys[8];
static int runtime_key_count = 0;

enum {
    LINE_BODY = 0,
    LINE_BLANK,
    LINE_HEADING,
    LINE_TOC,
    LINE_PAGE_BREAK
};

static void rebuild_chapter_index(void);
static void remap_document_links(void);
static void merge_chapters_from_links(int page);
static int ensure_pdf_links_for_page(int page);
static int ensure_pdf_links_for_range(int first_page, int last_page);
static void clear_link_selection(void);
static int follow_selected_link(void);
static int select_link(int direction);
static int show_chapter_overlay(void);
static int link_at_body_position(int body_y, int body_x);
static int page_start_line(int page);
static void go_to_page(int page);
static void set_notice(const char *fmt, ...);
static void free_document_links(void);

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

static void add_pdf_terminfo_key(const char *capability, int action)
{
    const char *sequence = tigetstr((char *)capability);
    int code;

    if (!sequence || sequence == (char *)-1 || !*sequence)
        return;
    code = key_defined(sequence);
    if (code <= 0 || runtime_key_count >=
                         (int)(sizeof runtime_keys / sizeof runtime_keys[0]))
        return;
    runtime_keys[runtime_key_count].code = code;
    runtime_keys[runtime_key_count].action = action;
    runtime_key_count++;
}

static void discover_pdf_keys(void)
{
    runtime_key_count = 0;
    add_pdf_terminfo_key("kUP", PDF_KEY_LINK_PREV);
    add_pdf_terminfo_key("kri", PDF_KEY_LINK_PREV);
    add_pdf_terminfo_key("kDN", PDF_KEY_LINK_NEXT);
    add_pdf_terminfo_key("kind", PDF_KEY_LINK_NEXT);
}

static int normalize_pdf_key(int ch)
{
    for (int i = 0; i < runtime_key_count; i++)
        if (runtime_keys[i].code == ch)
            return runtime_keys[i].action;
#ifdef KEY_SR
    if (ch == KEY_SR)
        return PDF_KEY_LINK_PREV;
#endif
#ifdef KEY_SF
    if (ch == KEY_SF)
        return PDF_KEY_LINK_NEXT;
#endif
    return ch;
}

static int parse_pdf_csi(const char *sequence)
{
    int first;
    int modifier;
    char final;

    if (sscanf(sequence, "[1;%d%c", &modifier, &final) == 2 ||
        sscanf(sequence, "O1;%d%c", &modifier, &final) == 2 ||
        sscanf(sequence, "[%d;%d%c", &first, &modifier, &final) == 3) {
        if (modifier > 0 && ((modifier - 1) & 1) != 0) {
            if (final == 'A')
                return PDF_KEY_LINK_PREV;
            if (final == 'B')
                return PDF_KEY_LINK_NEXT;
        }
    }
    return 0;
}

static int read_pdf_key(void)
{
    char sequence[32];
    int length = 0;
    int ch = getch();

    if (ch != 27)
        return normalize_pdf_key(ch);

    timeout(25);
    ch = getch();
    if (ch != '[' && ch != 'O') {
        if (ch != ERR)
            ungetch(ch);
        timeout(-1);
        return 27;
    }

    sequence[length++] = (char)ch;
    while (length < (int)sizeof sequence - 1) {
        ch = getch();
        if (ch == ERR || ch < 0 || ch > UCHAR_MAX)
            break;
        sequence[length++] = (char)ch;
        if ((ch >= '@' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') || ch == '~')
            break;
    }
    sequence[length] = 0;
    timeout(-1);

    ch = parse_pdf_csi(sequence);
    return ch ? ch : ERR;
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

    int a, b, used = 0;

    if (sscanf(t, "Page %d%n", &a, &used) == 1 && t[used] == 0) return 1;
    used = 0;
    if (sscanf(t, "page %d%n", &a, &used) == 1 && t[used] == 0) return 1;
    used = 0;
    if (sscanf(t, "%d / %d%n", &a, &b, &used) == 2 && t[used] == 0) return 1;
    used = 0;
    if (sscanf(t, "%d/%d%n", &a, &b, &used) == 2 && t[used] == 0) return 1;
    used = 0;
    if (sscanf(t, "%d of %d%n", &a, &b, &used) == 2 && t[used] == 0) return 1;

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

static void ensure_line_capacity(size_t needed)
{
    if (needed <= line_capacity)
        return;

    size_t capacity = line_capacity ? line_capacity : INITIAL_LINE_CAPACITY;
    while (capacity < needed) {
        if (capacity > (size_t)INT_MAX / 2) {
            endwin();
            fprintf(stderr, "simplepdf: document has too many text rows\n");
            exit(1);
        }
        capacity *= 2;
    }

    char **grown_lines = calloc(capacity, sizeof(*grown_lines));
    unsigned char *grown_kinds = calloc(capacity, sizeof(*grown_kinds));
    int *grown_pages = calloc(capacity, sizeof(*grown_pages));
    if (!grown_lines || !grown_kinds || !grown_pages) {
        free(grown_lines);
        free(grown_kinds);
        free(grown_pages);
        endwin();
        perror("calloc");
        exit(1);
    }

    if (line_count > 0) {
        memcpy(grown_lines, lines, (size_t)line_count * sizeof(*grown_lines));
        memcpy(grown_kinds, line_kinds,
               (size_t)line_count * sizeof(*grown_kinds));
        memcpy(grown_pages, line_pages,
               (size_t)line_count * sizeof(*grown_pages));
    }
    free(lines);
    free(line_kinds);
    free(line_pages);
    lines = grown_lines;
    line_kinds = grown_kinds;
    line_pages = grown_pages;
    line_capacity = capacity;
}

static void add_line_kind(const char *s, int kind, int page)
{
    ensure_line_capacity((size_t)line_count + 1);
    lines[line_count] = xstrdup(s);
    line_kinds[line_count] = (unsigned char)kind;
    line_pages[line_count] = page >= 0 ? page : 0;
    line_count++;
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
    int col = 0;
    const char *p = s;
    const char *end = s + n;
    mbstate_t state;

    memset(&state, 0, sizeof state);
    while (p < end) {
        if (*p == '\t') {
            col += 4 - (col % 4);
            p++;
            memset(&state, 0, sizeof state);
            continue;
        }

        wchar_t wc;
        size_t used = mbrtowc(&wc, p, (size_t)(end - p), &state);
        if (used == (size_t)-1 || used == (size_t)-2 || used == 0) {
            col++;
            p++;
            memset(&state, 0, sizeof state);
            continue;
        }

        int width = wcwidth(wc);
        col += width > 0 ? width : 1;
        p += used;
    }

    return col;
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
    pages[page_count].left = MAX_LINE;
    pages[page_count].right = 0;
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

static void add_layout_line_kind(const char *s, int kind)
{
    if (page_count <= 0)
        begin_page();

    add_line_kind(s, kind, page_count - 1);

    int w = visual_len(s);
    if (w > pages[page_count - 1].width)
        pages[page_count - 1].width = w;

    int first = first_nonblank_col(s);
    if (first >= 0) {
        if (first < pages[page_count - 1].left)
            pages[page_count - 1].left = first;
        if (w > pages[page_count - 1].right)
            pages[page_count - 1].right = w;
        if (first < layout_left)
            layout_left = first;
        if (w > layout_right)
            layout_right = w;
    }
}

static void add_layout_line(const char *s)
{
    add_layout_line_kind(s, s[0] ? LINE_BODY : LINE_BLANK);
}

static void add_blank_line(void)
{
    if (line_count == 0 || line_kinds[line_count - 1] == LINE_BLANK ||
        line_kinds[line_count - 1] == LINE_PAGE_BREAK)
        return;

    add_layout_line_kind("", LINE_BLANK);
}

static void trim_trailing_blank_lines(void)
{
    while (line_count > 0 && line_kinds[line_count - 1] == LINE_BLANK) {
        free(lines[line_count - 1]);
        lines[line_count - 1] = NULL;
        line_count--;
    }
}

static void add_page_gap(void)
{
    int previous_page = page_count - 1;
    int next_page = page_count;

    /* Give the rule one quiet row on either side. The rule and lower spacer
       belong to the page that follows, so the status remains honest while
       crossing the boundary. */
    if (previous_page >= 0 && pages[previous_page].end > pages[previous_page].start)
        add_line_kind("", LINE_BLANK, previous_page);

    add_line_kind("", LINE_PAGE_BREAK, next_page);
    begin_page();
    add_layout_line_kind("", LINE_BLANK);
}

static void add_wrapped_reading_text(const char *text, int kind);

static void add_wrapped_epub_paragraph(char *para)
{
    char *p = trim(para);
    if (!*p)
        return;

    add_wrapped_reading_text(p, LINE_BODY);
    /* EPUBs do not have PDF-style physical page breaks. */
    add_blank_line();
}

static int reader_width_for_term(int term_w)
{
    int margins;

    if (term_w >= 110)
        margins = 24;
    else if (term_w >= 72)
        margins = 10;
    else
        margins = 4;

    int width = term_w - margins;
    if (width > READER_MAX_WIDTH)
        width = READER_MAX_WIDTH;
    if (width < READER_MIN_WIDTH)
        width = term_w > 4 ? term_w - 4 : term_w;
    if (width < 1)
        width = 1;
    return width;
}

static int looks_like_reading_heading(const char *s, int source_lines,
                                      int source_indent)
{
    int letters = 0;
    int lower = 0;
    int width = visual_len(s);

    if (source_lines != 1 || width < 1 || width > 72)
        return 0;

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalpha(*p)) {
            letters++;
            if (islower(*p))
                lower++;
        }
    }

    if (letters >= 2 && lower == 0)
        return 1;

    if (!strncasecmp(s, "chapter ", 8) || !strncasecmp(s, "book ", 5) ||
        !strncasecmp(s, "part ", 5) || !strcasecmp(s, "prologue") ||
        !strcasecmp(s, "epilogue") || !strcasecmp(s, "contents"))
        return 1;

    /* A strongly indented short line is usually a centered title in
       pdftotext -layout output, rather than a prose paragraph. */
    return source_indent >= 16 && width <= 56;
}

static void add_wrapped_reading_text(const char *text, int kind)
{
    char copy[MAX_PARAGRAPH];
    char out[MAX_LINE];
    char *save = NULL;
    char *word;
    int out_width = 0;

    snprintf(copy, sizeof copy, "%s", text);
    out[0] = 0;

    for (word = strtok_r(copy, " \t\r\n", &save); word;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        int word_width = visual_len(word);
        size_t out_len = strlen(out);
        size_t word_len = strlen(word);

        if (out[0] && (out_width + 1 + word_width > wrap_width ||
                       out_len + 1 + word_len >= sizeof out)) {
            add_layout_line_kind(out, kind);
            out[0] = 0;
            out_width = 0;
            out_len = 0;
        }

        if (out[0]) {
            strncat(out, " ", sizeof(out) - strlen(out) - 1);
            out_width++;
        }
        strncat(out, word, sizeof(out) - strlen(out) - 1);
        out_width += word_width;
    }

    if (out[0])
        add_layout_line_kind(out, kind);
}

static void append_reading_segment(char *paragraph, size_t paragraph_size,
                                   const char *segment)
{
    size_t have = strlen(paragraph);
    size_t need = strlen(segment);

    if (have && paragraph[have - 1] == '-' &&
        isalpha((unsigned char)segment[0])) {
        paragraph[--have] = 0;
    } else if (have && have + 1 < paragraph_size) {
        paragraph[have++] = ' ';
        paragraph[have] = 0;
    }

    if (need >= paragraph_size - have)
        need = paragraph_size - have - 1;
    memcpy(paragraph + have, segment, need);
    paragraph[have + need] = 0;
}

static void flush_reading_block(char *paragraph, int *source_lines,
                                int *source_indent)
{
    char *text = trim(paragraph);

    if (*text) {
        int kind = looks_like_reading_heading(text, *source_lines,
                                              *source_indent)
                       ? LINE_HEADING : LINE_BODY;
        add_wrapped_reading_text(text, kind);
        add_blank_line();
    }

    paragraph[0] = 0;
    *source_lines = 0;
    *source_indent = 0;
}

static int contents_end_marker(const char *text)
{
    return !strncasecmp(text, "Version Info", 12) ||
           !strncasecmp(text, "Version ", 8) ||
           !strncasecmp(text, "Copyright", 9) ||
           !strncmp(text, "©", strlen("©"));
}

static void drop_trailing_empty_pages(void)
{
    int minimum_pages = pdf_page_total > 0 ? pdf_page_total : 1;

    while (page_count > minimum_pages &&
           pages[page_count - 1].start == pages[page_count - 1].end) {
        page_count--;
        if (line_count > 0 && line_kinds[line_count - 1] == LINE_PAGE_BREAK) {
            free(lines[line_count - 1]);
            lines[line_count - 1] = NULL;
            line_count--;
        }
        if (line_count > 0 && line_kinds[line_count - 1] == LINE_BLANK) {
            free(lines[line_count - 1]);
            lines[line_count - 1] = NULL;
            line_count--;
        }
    }
}

static void load_reading_text(const char *txtpath, int term_w)
{
    FILE *f = fopen(txtpath, "r");
    if (!f) {
        endwin();
        perror(txtpath);
        exit(1);
    }

    free_lines();
    wrap_width = reader_width_for_term(term_w);
    begin_page();

    char raw[MAX_LINE];
    char segment[MAX_LINE];
    char paragraph[MAX_PARAGRAPH];
    int seg_len = 0;
    int source_lines = 0;
    int source_indent = 0;
    int contents_mode = 0;
    paragraph[0] = 0;

    while (fgets(raw, sizeof raw, f)) {
        for (char *p = raw; ; p++) {
            unsigned char c = (unsigned char)*p;

            if (c == '\f' || c == '\n' || c == '\0') {
                segment[seg_len] = 0;
                int indent = first_nonblank_col(segment);
                char *text = trim(segment);

                if (*text == 0) {
                    flush_reading_block(paragraph, &source_lines, &source_indent);
                    if (contents_mode)
                        add_blank_line();
                } else if (junk_line(text) || graphic_artifact_line(text)) {
                    flush_reading_block(paragraph, &source_lines, &source_indent);
                } else if (!strcasecmp(text, "Contents")) {
                    flush_reading_block(paragraph, &source_lines, &source_indent);
                    add_wrapped_reading_text(text, LINE_HEADING);
                    add_blank_line();
                    contents_mode = 1;
                } else if (contents_mode) {
                    /* A contents page is a list, not a prose paragraph. Keep
                       every extracted entry on its own readable row so its
                       PDF link can be mapped and underlined independently. */
                    flush_reading_block(paragraph, &source_lines, &source_indent);
                    add_wrapped_reading_text(text, LINE_TOC);
                    if (contents_end_marker(text))
                        contents_mode = 0;
                } else {
                    if (source_lines == 0)
                        source_indent = indent > 0 ? indent : 0;

                    if (strlen(paragraph) + strlen(text) + 3 >= sizeof paragraph)
                        flush_reading_block(paragraph, &source_lines,
                                            &source_indent);

                    append_reading_segment(paragraph, sizeof paragraph, text);
                    source_lines++;
                }

                seg_len = 0;

                if (c == '\f') {
                    flush_reading_block(paragraph, &source_lines, &source_indent);
                    trim_trailing_blank_lines();
                    finish_page();
                    add_page_gap();
                }

                if (c == '\0' || c == '\n')
                    break;
            } else if (c != '\r' && seg_len < MAX_LINE - 1) {
                segment[seg_len++] = (char)c;
            }
        }
    }

    flush_reading_block(paragraph, &source_lines, &source_indent);
    trim_trailing_blank_lines();
    finish_page();
    fclose(f);

    drop_trailing_empty_pages();

    layout_left = 0;
    layout_right = layout_width;

    if (line_count == 0) {
        if (page_count == 0)
            begin_page();
        add_layout_line("[No readable text found]");
        finish_page();
    }
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
    int blank_streak = 0;

    while (fgets(raw, sizeof raw, f)) {
        for (char *p = raw; ; p++) {
            unsigned char c = (unsigned char)*p;

            if (c == '\f' || c == '\n' || c == '\0') {
                segment[seg_len] = '\0';

                if (seg_len > 0) {
                    if (graphic_artifact_line(segment)) {
                        if (blank_streak < 1)
                            add_layout_line("");
                        blank_streak++;
                    } else {
                        add_layout_line(segment);
                        blank_streak = 0;
                    }
                } else if (c == '\n' && blank_streak < 2) {
                    add_layout_line("");
                    blank_streak++;
                }

                seg_len = 0;

                if (c == '\f') {
                    trim_trailing_blank_lines();
                    finish_page();
                    add_page_gap();
                    blank_streak = 0;
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

    trim_trailing_blank_lines();
    finish_page();
    drop_trailing_empty_pages();

    if (layout_left == MAX_LINE) {
        layout_left = 0;
        layout_right = layout_width;
    }

    if (line_count == 0) {
        if (page_count == 0)
            begin_page();
        add_layout_line("[No readable text found]");
        finish_page();
    }
}

static void free_epub_metadata(void)
{
    for (int i = 0; i < epub_target_count; i++)
        free(epub_targets[i].key);
    free(epub_targets);
    epub_targets = NULL;
    epub_target_count = 0;
    epub_target_capacity = 0;

    for (int i = 0; i < epub_pending_link_count; i++) {
        free(epub_pending_links[i].target_key);
        free(epub_pending_links[i].label);
    }
    free(epub_pending_links);
    epub_pending_links = NULL;
    epub_pending_link_count = 0;
    epub_pending_link_capacity = 0;
    epub_exact_link_count = 0;
}

static void add_epub_target(const char *key, int line_idx)
{
    if (!key || !*key)
        return;
    for (int i = 0; i < epub_target_count; i++)
        if (!strcmp(epub_targets[i].key, key))
            return;

    if (epub_target_count == epub_target_capacity) {
        int capacity = epub_target_capacity ? epub_target_capacity * 2 : 128;
        EpubTarget *grown = realloc(epub_targets,
                                    (size_t)capacity * sizeof(*grown));
        if (!grown) {
            endwin();
            perror("realloc");
            exit(1);
        }
        epub_targets = grown;
        epub_target_capacity = capacity;
    }
    epub_targets[epub_target_count].key = xstrdup(key);
    epub_targets[epub_target_count].line_idx = line_idx;
    epub_target_count++;
}

static int epub_target_line(const char *key)
{
    if (!key || !*key)
        return -1;
    for (int i = 0; i < epub_target_count; i++)
        if (!strcmp(epub_targets[i].key, key))
            return epub_targets[i].line_idx;

    const char *fragment = strchr(key, '#');
    if (fragment) {
        size_t path_length = (size_t)(fragment - key);
        for (int i = 0; i < epub_target_count; i++)
            if (strlen(epub_targets[i].key) == path_length &&
                !strncmp(epub_targets[i].key, key, path_length))
                return epub_targets[i].line_idx;
    }
    return -1;
}

static int epub_marker_label_append(EpubMarkerState *state,
                                    const char *data, size_t length)
{
    if (!state->target_key || length == 0)
        return 0;
    if (state->label_length + length + 1 > state->label_capacity) {
        size_t capacity = state->label_capacity ? state->label_capacity * 2
                                                : 128;
        while (capacity < state->label_length + length + 1)
            capacity *= 2;
        char *grown = realloc(state->label, capacity);
        if (!grown)
            return -1;
        state->label = grown;
        state->label_capacity = capacity;
    }
    memcpy(state->label + state->label_length, data, length);
    state->label_length += length;
    state->label[state->label_length] = 0;
    return 0;
}

static void finish_epub_pending_link(EpubMarkerState *state)
{
    if (!state->target_key)
        return;

    if (state->label) {
        char *source = state->label;
        char *dest = state->label;
        int pending_space = 0;

        while (*source && isspace((unsigned char)*source))
            source++;
        while (*source) {
            if (isspace((unsigned char)*source)) {
                pending_space = dest > state->label;
            } else {
                if (pending_space)
                    *dest++ = ' ';
                *dest++ = *source;
                pending_space = 0;
            }
            source++;
        }
        *dest = 0;
    }

    if (state->label && *state->label) {
        if (epub_pending_link_count == epub_pending_link_capacity) {
            int capacity = epub_pending_link_capacity
                               ? epub_pending_link_capacity * 2 : 128;
            EpubPendingLink *grown = realloc(
                epub_pending_links, (size_t)capacity * sizeof(*grown));
            if (!grown) {
                endwin();
                perror("realloc");
                exit(1);
            }
            epub_pending_links = grown;
            epub_pending_link_capacity = capacity;
        }
        EpubPendingLink *link =
            &epub_pending_links[epub_pending_link_count++];
        link->target_key = state->target_key;
        link->label = state->label;
        link->source_hint = state->source_hint;
    } else {
        free(state->target_key);
        free(state->label);
    }

    state->target_key = NULL;
    state->label = NULL;
    state->label_length = 0;
    state->label_capacity = 0;
    state->source_hint = 0;
}

static char *decode_epub_marker(const char *data, size_t length)
{
    if (length % 2 != 0)
        return NULL;
    char *decoded = malloc(length / 2 + 1);
    if (!decoded)
        return NULL;
    for (size_t i = 0; i < length; i += 2) {
        int high = simpleepub_hex_value(data[i]);
        int low = simpleepub_hex_value(data[i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) {
            free(decoded);
            return NULL;
        }
        decoded[i / 2] = (char)(high * 16 + low);
    }
    decoded[length / 2] = 0;
    return decoded;
}

static int strip_epub_markers(char *raw, EpubMarkerState *state)
{
    char *source = raw;
    char *dest = raw;
    int contains_link = state->target_key != NULL;

    while (*source) {
        if (*source == SIMPLEEPUB_MARKER_START && source[1]) {
            char *end = strchr(source + 2, SIMPLEEPUB_MARKER_END);
            if (end) {
                char kind = source[1];
                char *value = decode_epub_marker(
                    source + 2, (size_t)(end - source - 2));
                if (kind == 'T' && value) {
                    add_epub_target(value, line_count);
                    free(value);
                } else if (kind == 'L' && value) {
                    finish_epub_pending_link(state);
                    state->target_key = value;
                    state->source_hint = line_count;
                    contains_link = 1;
                } else if (kind == 'E') {
                    free(value);
                    finish_epub_pending_link(state);
                } else {
                    free(value);
                }
                source = end + 1;
                continue;
            }
        }

        if (state->target_key &&
            epub_marker_label_append(state, source, 1) != 0) {
            endwin();
            perror("realloc");
            exit(1);
        }
        *dest++ = *source++;
    }
    *dest = 0;
    if (state->target_key)
        epub_marker_label_append(state, " ", 1);
    return contains_link;
}

static void load_epub_text(const char *txtpath, int term_w, int term_h)
{
    FILE *f = fopen(txtpath, "r");
    if (!f) {
        endwin();
        perror(txtpath);
        exit(1);
    }

    free_document_links();
    free_epub_metadata();
    free_lines();
    (void)term_h;

    wrap_width = reader_width_for_term(term_w);

    begin_page();

    char raw[MAX_LINE];
    char para[MAX_PARAGRAPH];
    EpubMarkerState marker_state = {0};
    para[0] = 0;
    int blank_streak = 0;

    while (fgets(raw, sizeof raw, f)) {
        raw[strcspn(raw, "\n")] = 0;

        int contains_link = strip_epub_markers(raw, &marker_state);

        for (char *c = raw; *c; c++) {
            if (*c == '\f')
                *c = ' ';
        }

        char *t = trim(raw);

        if (!contains_link && junk_line(t))
            continue;

        /* EPUB extraction sometimes emits ornamental divider lines:
           . . . . . . . . . . .
           These are not real page breaks, and they make EPUB reading ugly. */
        if (!contains_link && graphic_artifact_line(t))
            continue;

        if (*t == 0) {
            blank_streak++;
            if (para[0]) {
                add_wrapped_epub_paragraph(para);
                para[0] = 0;
            }
            continue;
        }

        int preceding_blanks = blank_streak;
        blank_streak = 0;

        /* Pandoc's plain writer may leave headings as short standalone lines.
           Preserve those instead of gluing them to the following prose. */
        if (preceding_blanks >= 2 && strlen(t) <= 72 && para[0] == 0) {
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
                add_wrapped_reading_text(t, LINE_HEADING);
                add_blank_line();
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
    finish_epub_pending_link(&marker_state);

    trim_trailing_blank_lines();
    fclose(f);
    finish_page();

    layout_left = 0;
    layout_right = layout_width;

    if (line_count == 0) {
        if (page_count == 0)
            begin_page();
        add_layout_line("[No readable text found]");
        finish_page();
    }
}


static int has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ext) == 0;
}

static int wait_for_process(pid_t pid)
{
    int status = 0;
    pid_t waited;

    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static pid_t start_pdftotext(const char *infile, const char *txt,
                             int first_page, int last_page)
{
    pid_t pid = fork();

    if (pid != 0)
        return pid;

    if (first_page > 0 && last_page >= first_page) {
        char first[32];
        char last[32];

        snprintf(first, sizeof first, "%d", first_page);
        snprintf(last, sizeof last, "%d", last_page);
        execlp("pdftotext", "pdftotext", "-f", first, "-l", last,
               "-layout", "-enc", "UTF-8", infile, txt, NULL);
    } else {
        execlp("pdftotext", "pdftotext", "-layout", "-enc", "UTF-8",
               infile, txt, NULL);
    }
    _exit(127);
}

static int run_pdftotext_single(const char *infile, const char *txt)
{
    pid_t pid = start_pdftotext(infile, txt, 0, 0);
    return pid < 0 ? -1 : wait_for_process(pid);
}

static int pdf_extract_job_count_for(int pages, long processors)
{
    int jobs;

    if (pages <= 0 || processors <= 1)
        return 1;

    jobs = pages / PDF_MIN_PAGES_PER_JOB;
    if (jobs < 1)
        jobs = 1;
    if (jobs > PDF_MAX_EXTRACT_JOBS)
        jobs = PDF_MAX_EXTRACT_JOBS;
    if (jobs > processors)
        jobs = (int)processors;
    return jobs;
}

static int pdf_extract_job_count(void)
{
    long processors = sysconf(_SC_NPROCESSORS_ONLN);

    if (processors < 1)
        processors = 1;
    return pdf_extract_job_count_for(pdf_page_total, processors);
}

static void remove_pdf_parts(char paths[][PATH_MAX], int count)
{
    for (int i = 0; i < count; i++)
        if (paths[i][0])
            unlink(paths[i]);
}

static int merge_pdf_parts(const char *txt, char paths[][PATH_MAX], int count)
{
    FILE *output = fopen(txt, "wb");
    char buffer[65536];
    int result = 0;

    if (!output)
        return -1;

    for (int i = 0; i < count && result == 0; i++) {
        FILE *part = fopen(paths[i], "rb");
        if (!part) {
            result = -1;
            break;
        }

        size_t bytes;
        while ((bytes = fread(buffer, 1, sizeof buffer, part)) > 0) {
            if (fwrite(buffer, 1, bytes, output) != bytes) {
                result = -1;
                break;
            }
        }
        if (ferror(part))
            result = -1;
        if (fclose(part) != 0)
            result = -1;
    }

    if (fclose(output) != 0)
        result = -1;
    return result;
}

static int run_pdf_extract_parallel(const char *infile, const char *txt,
                                    int jobs)
{
    char paths[PDF_MAX_EXTRACT_JOBS][PATH_MAX];
    pid_t pids[PDF_MAX_EXTRACT_JOBS];
    int result = 0;

    memset(paths, 0, sizeof paths);
    memset(pids, 0, sizeof pids);

    for (int i = 0; i < jobs; i++) {
        if (snprintf(paths[i], sizeof paths[i], "%s.part%d.XXXXXX",
                     txt, i) >= (int)sizeof paths[i]) {
            remove_pdf_parts(paths, jobs);
            return PDF_EXTRACT_RETRY_SINGLE;
        }
        int fd = mkstemp(paths[i]);
        if (fd < 0) {
            remove_pdf_parts(paths, jobs);
            return PDF_EXTRACT_RETRY_SINGLE;
        }
        close(fd);
    }

    for (int i = 0; i < jobs; i++) {
        int first_page = pdf_page_total * i / jobs + 1;
        int last_page = pdf_page_total * (i + 1) / jobs;

        pids[i] = start_pdftotext(infile, paths[i], first_page, last_page);
        if (pids[i] < 0) {
            for (int running = 0; running < i; running++)
                kill(pids[running], SIGTERM);
            for (int running = 0; running < i; running++)
                wait_for_process(pids[running]);
            remove_pdf_parts(paths, jobs);
            return PDF_EXTRACT_RETRY_SINGLE;
        }
    }

    for (int i = 0; i < jobs; i++) {
        int status = wait_for_process(pids[i]);
        if (status != 0 && result == 0)
            result = status < 0 ? -1 : status;
    }

    if (result == 0)
        result = merge_pdf_parts(txt, paths, jobs);
    remove_pdf_parts(paths, jobs);
    return result;
}

static int run_extract_text(const char *infile, const char *txt)
{
    if (!has_ext(infile, ".epub")) {
        int jobs = pdf_extract_job_count();
        if (jobs > 1) {
            int result = run_pdf_extract_parallel(infile, txt, jobs);
            if (result != PDF_EXTRACT_RETRY_SINGLE)
                return result;
        }

        return run_pdftotext_single(infile, txt);
    }

    if (simpleepub_extract(infile, txt) == 0)
        return 0;

    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        execlp("pandoc", "pandoc", infile, "-t", "plain", "--wrap=none",
               "-o", txt, NULL);
        _exit(127);
    }

    return wait_for_process(pid);
}

static uint64_t hash_update(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;

    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int make_directories(const char *path)
{
    char copy[PATH_MAX];

    if (!path || !*path || strlen(path) >= sizeof copy)
        return -1;
    snprintf(copy, sizeof copy, "%s", path);

    for (char *p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }

    if (mkdir(copy, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int cache_text_path(const char *infile, char *out, size_t outsz,
                           int *cache_hit)
{
    struct stat st;
    char resolved[PATH_MAX];
    char cache_dir[PATH_MAX];
    char metadata[128];
    const char *cache_home = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    const char *identity = infile;
    uint64_t hash = UINT64_C(1469598103934665603);
    long mtime_nsec;

    *cache_hit = 0;
    if (stat(infile, &st) != 0)
        return -1;

    if (realpath(infile, resolved))
        identity = resolved;

    if (!cache_home || !*cache_home) {
        if (!home || !*home)
            return -1;
        if (snprintf(cache_dir, sizeof cache_dir, "%s/.cache/simplepdf", home) >=
            (int)sizeof cache_dir)
            return -1;
    } else if (snprintf(cache_dir, sizeof cache_dir, "%s/simplepdf", cache_home) >=
               (int)sizeof cache_dir) {
        return -1;
    }

    if (make_directories(cache_dir) != 0)
        return -1;

    const char *cache_version = epub_mode ? EPUB_CACHE_VERSION
                                          : PDF_CACHE_VERSION;
    hash = hash_update(hash, cache_version, strlen(cache_version));
    hash = hash_update(hash, identity, strlen(identity));
#if defined(__APPLE__)
    mtime_nsec = st.st_mtimespec.tv_nsec;
#else
    mtime_nsec = st.st_mtim.tv_nsec;
#endif
    snprintf(metadata, sizeof metadata, "|%lld|%lld|%ld|%d",
             (long long)st.st_size, (long long)st.st_mtime,
             mtime_nsec, epub_mode);
    hash = hash_update(hash, metadata, strlen(metadata));

    if (snprintf(out, outsz, "%s/%016llx.txt", cache_dir,
                 (unsigned long long)hash) >= (int)outsz)
        return -1;

    *cache_hit = access(out, R_OK) == 0;
    return 0;
}

static void show_loading_screen(void)
{
    int h, w;

    initscr();
    raw();
    noecho();
    curs_set(0);
    getmaxyx(stdscr, h, w);
    erase();

    if (w > 4) {
        attrset(A_BOLD);
        mvaddnstr(1, 2, title, w - 4);
    }
    if (h > 4 && w > 4) {
        attrset(A_DIM);
        mvaddnstr(3, 2, "Preparing document...", w - 4);
    }
    refresh();
}

static int page_for_line(int line)
{
    if (page_count <= 0)
        return 0;

    if (line < 0)
        line = 0;
    if (line >= line_count)
        return page_count - 1;

    int page = line_pages[line];
    if (page < 0)
        page = 0;
    if (page >= page_count)
        page = page_count - 1;
    return page;
}

static int page_width(void)
{
    if (epub_mode || reading_mode)
        return wrap_width > 0 ? wrap_width : 80;

    if (layout_right > layout_left)
        return layout_right - layout_left;
    return layout_width > 0 ? layout_width : 1;
}

static int page_left_for_line(int line)
{
    if (epub_mode || reading_mode)
        return 0;

    int page = page_for_line(line);

    if (page >= 0 && page < page_count && pages[page].left < MAX_LINE)
        return pages[page].left;

    return layout_left < MAX_LINE ? layout_left : 0;
}

static void page_viewport(int term_w, int *left, int *view_w, int *max_hscroll)
{
    int pw = page_width();
    int margin = (epub_mode || reading_mode) ? 4 : 2;

    int avail = term_w - margin * 2;

    if (avail < 1)
        avail = term_w;
    if (avail < 1)
        avail = 1;

    *view_w = pw < avail ? pw : avail;
    if (*view_w < 1)
        *view_w = 1;

    *left = (term_w - *view_w) / 2;
    if (*left < 0)
        *left = 0;

    *max_hscroll = pw - *view_w;
    if (*max_hscroll < 0)
        *max_hscroll = 0;
}

static char *case_find(const char *haystack, const char *needle);

static attr_t body_attr(void)
{
    return A_NORMAL;
}

static attr_t muted_attr(void)
{
    return A_DIM;
}

static attr_t heading_attr(void)
{
    return A_BOLD;
}

static attr_t search_attr(void)
{
    return A_REVERSE;
}

static attr_t link_attr(int line_idx, int is_selected)
{
    attr_t attr = line_idx >= 0 && line_idx < line_count &&
                          line_kinds[line_idx] == LINE_HEADING
                      ? heading_attr()
                      : body_attr();

    attr |= A_UNDERLINE;
    if (is_selected)
        attr |= A_REVERSE | A_BOLD;
    return attr;
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
    body_win_top = 0;
    invalidate_body_cache();
}

static void force_full_redraw(void)
{
    chrome_dirty = 1;
    last_header[0] = 0;
    last_footer[0] = 0;
    clearok(stdscr, TRUE);
    werase(stdscr);
    wnoutrefresh(stdscr);
    destroy_body_window();
    invalidate_body_cache();
}

static void clear_body_area(int h, int w)
{
    if (w <= 0)
        return;

    attrset(body_attr());
    for (int y = 1; y < h - 1; y++)
        mvhline(y, 0, ' ', w);
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

static int body_height_for_term(int h)
{
    int body_h = show_ui ? h - 2 : h;

    if (body_h < 1)
        body_h = 1;
    return body_h;
}

static int body_top_for_term(int h)
{
    return show_ui && h > 1 ? 1 : 0;
}

static void ensure_body_window(int desired_top, int desired_left,
                               int desired_w, int desired_h)
{
    int h, w;
    getmaxyx(stdscr, h, w);

    if (desired_top < 0)
        desired_top = 0;
    if (desired_top >= h)
        desired_top = h > 0 ? h - 1 : 0;
    if (desired_h < 1)
        desired_h = 1;
    if (desired_top + desired_h > h)
        desired_h = h - desired_top;
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
        body_win_w != desired_w || body_win_left != desired_left ||
        body_win_top != desired_top)
    {
        if (body_win)
            clear_body_area(h, w);
        destroy_body_window();
        body_win_h = desired_h;
        body_win_w = desired_w;
        body_win_left = desired_left;
        body_win_top = desired_top;
        body_win = newwin(body_win_h, body_win_w, body_win_top, body_win_left);
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

static void compose_header_status(char *out, size_t outsz, int w)
{
    int page = page_for_line(top) + 1;
    int total = page_count > 0 ? page_count : 1;
    int progress = line_count > 1 ? top * 100 / (line_count - 1) : 100;

    if (progress < 0)
        progress = 0;
    if (progress > 100)
        progress = 100;

    if (w >= 72) {
        if (epub_mode)
            snprintf(out, outsz, "EPUB  %d%%", progress);
        else
            snprintf(out, outsz, "%s  %d / %d  %d%%",
                     reading_mode ? "Reading" : "Layout",
                     page, total, progress);
    } else if (epub_mode) {
        snprintf(out, outsz, "EPUB");
    } else {
        snprintf(out, outsz, "%d / %d", page, total);
    }
}

static void ellipsize_text(char *out, size_t outsz, const char *text, int maxw)
{
    if (maxw <= 0 || outsz == 0) {
        if (outsz)
            out[0] = 0;
        return;
    }

    if (visual_len(text) <= maxw) {
        snprintf(out, outsz, "%s", text);
        return;
    }

    if (maxw == 1) {
        snprintf(out, outsz, "\u2026");
        return;
    }

    const char *end = utf8_ptr_at_col(text, maxw - 1);
    int bytes = (int)(end - text);
    snprintf(out, outsz, "%.*s\u2026", bytes, text);
}

static void compose_footer(char *out, size_t outsz, int w)
{
    if (notice[0])
        snprintf(out, outsz, "%s", notice);
    else if (selected_link >= 0 && selected_link < document_link_count) {
        if (w >= 90)
            snprintf(out, outsz,
                     "Enter open  Shift-\u2191/\u2193 links  \u2022  %.72s",
                     document_links[selected_link].label);
        else
            snprintf(out, outsz, "Enter open  Shift-\u2191/\u2193  \u2022  %.48s",
                     document_links[selected_link].label);
    } else if (nav_history_count > 0 && w >= 90) {
        if (epub_mode)
            snprintf(out, outsz,
                     "Backspace back  \u2022  o chapters  \u2022  / find  \u2022  q quit");
        else
            snprintf(out, outsz,
                     "Backspace back  \u2022  Shift-\u2191/\u2193 links  \u2022  o chapters  \u2022  / find  \u2022  q quit");
    } else if (nav_history_count > 0) {
        snprintf(out, outsz, "Backspace back  \u2022  o chapters  \u2022  q quit");
    } else if (epub_mode && w >= 90)
        snprintf(out, outsz,
                 "\u2191\u2193 scroll  PgUp/PgDn  o chapters  / find  i focus  ? help  q quit");
    else if (epub_mode)
        snprintf(out, outsz, "\u2191\u2193 scroll  o chapters  / find  q quit");
    else if (w >= 112)
        snprintf(out, outsz,
                 "\u2191\u2193 scroll   PgUp/PgDn screen   Shift-\u2191/\u2193 links   Enter open   o chapters   / find   ? help   q quit");
    else if (w >= 90)
        snprintf(out, outsz,
                 "\u2191\u2193 scroll  Shift-\u2191/\u2193 links  Enter open  o chapters  / find  q quit");
    else if (w >= 68)
        snprintf(out, outsz, "\u2191\u2193 scroll  Shift-\u2191/\u2193 links  o chapters  q quit");
    else
        snprintf(out, outsz, "\u2191\u2193 scroll  Shift-\u2191/\u2193 links  q quit");
}

static void draw_chrome_if_needed(int h, int w)
{
    char status[128];
    char header_key[sizeof last_header];
    char footer[sizeof last_footer];

    compose_header_status(status, sizeof status, w);
    snprintf(header_key, sizeof header_key, "%s\n%s", title, status);
    compose_footer(footer, sizeof footer, w);

    int header_changed = chrome_dirty || strcmp(header_key, last_header) != 0;
    int footer_changed = chrome_dirty || strcmp(footer, last_footer) != 0;

    if (!header_changed && !footer_changed)
        return;

    if (header_changed) {
        snprintf(last_header, sizeof last_header, "%s", header_key);
        attrset(body_attr());
        move(0, 0);
        clrtoeol();

        if (w > 4) {
            int status_w = visual_len(status);
            int status_col = w - status_w - 2;
            int title_w = status_col - 4;
            char display_title[sizeof title];

            if (title_w < 0)
                title_w = 0;
            ellipsize_text(display_title, sizeof display_title, title, title_w);
            if (title_w > 0)
                draw_utf8_clipped(stdscr, 0, 2, display_title,
                                  heading_attr(), title_w);
            if (status_col >= 2)
                draw_utf8_clipped(stdscr, 0, status_col, status,
                                  muted_attr(), w - status_col);
        }
    }

    if (footer_changed) {
        snprintf(last_footer, sizeof last_footer, "%s", footer);
        attrset(body_attr());
        move(h - 1, 0);
        clrtoeol();
        if (w > 4)
            draw_utf8_clipped(stdscr, h - 1, 2, footer,
                              notice[0] ? heading_attr() : muted_attr(), w - 4);
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
           c->selected_link == selected_link &&
           c->link_revision == link_revision &&
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
    c->selected_link = selected_link;
    c->link_revision = link_revision;
    snprintf(c->search_term, sizeof c->search_term, "%s", search_term);
}

static void clear_body_row(int y, int body_w)
{
    if (y < 0 || y >= body_win_h)
        return;
    wattrset(body_win, body_attr());
    mvwhline(body_win, y, 0, ' ', body_w);
}

static void draw_page_separator(int y, int idx, int body_w)
{
    char label[64];
    int page = page_for_line(idx) + 1;
    int total = page_count > 0 ? page_count : 1;
    int label_w;
    int rule_w;
    int whole_w;
    int start;

    snprintf(label, sizeof label, " page %d of %d ", page, total);
    label_w = visual_len(label);

    if (label_w >= body_w) {
        draw_utf8_clipped(body_win, y, 0, label, muted_attr(), body_w);
        return;
    }

    rule_w = (body_w - label_w) / 2;
    if (rule_w > 12)
        rule_w = 12;
    whole_w = label_w + rule_w * 2;
    start = (body_w - whole_w) / 2;

    wattrset(body_win, muted_attr());
    if (rule_w > 0)
        mvwhline(body_win, y, start, ACS_HLINE, rule_w);
    draw_utf8_clipped(body_win, y, start + rule_w, label,
                      muted_attr() | A_BOLD, label_w);
    if (rule_w > 0)
        mvwhline(body_win, y, start + rule_w + label_w, ACS_HLINE, rule_w);
}

static void draw_body_row(int y, int idx, int source_col, int body_w)
{
    const char *s;
    const char *visible;
    const char *visible_end;
    attr_t attr = body_attr();
    int draw_col = 0;

    clear_body_row(y, body_w);

    if (idx < 0 || idx >= line_count)
        return;

    s = lines[idx];

    if (line_kinds[idx] == LINE_PAGE_BREAK) {
        draw_page_separator(y, idx, body_w);
        return;
    }

    if (line_kinds[idx] == LINE_BLANK)
        return;

    if (source_col < 0)
        source_col = 0;

    visible = utf8_ptr_at_col(s, source_col);
    visible_end = utf8_ptr_at_col(s, source_col + body_w);

    if (line_kinds[idx] == LINE_HEADING && source_col == 0) {
        int text_w = visual_len(s);
        if (text_w < body_w)
            draw_col = (body_w - text_w) / 2;
        attr = heading_attr();
    }

    draw_utf8_clipped(body_win, y, draw_col, visible, attr,
                      body_w - draw_col);

    if (idx == last_match && search_term[0]) {
        char *hit = case_find(s, search_term);
        if (hit) {
            const char *hit_start = hit;
            const char *hit_end = hit + strlen(search_term);

            if (hit_end > visible && hit_start < visible_end) {
                const char *draw_start = hit_start < visible ? visible : hit_start;
                const char *draw_end = hit_end > visible_end ? visible_end : hit_end;
                int screen_x = draw_col + utf8_width_between(visible, draw_start);

                if (screen_x < body_w)
                    draw_utf8_range_clipped(body_win, y, screen_x,
                                            draw_start, draw_end,
                                            search_attr(), body_w - screen_x);
            }
        }
    }

    /* PDF links keep the familiar visual language of a document viewer:
       understated underlines, with the keyboard-selected link reversed. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < document_link_count; i++) {
            DocumentLink *link = &document_links[i];
            int is_selected = i == selected_link;
            size_t text_len;
            const char *link_start;
            const char *link_end;
            const char *draw_start;
            const char *draw_end;
            int screen_x;

            if (link->line_idx != idx || is_selected != (pass == 1))
                continue;

            text_len = strlen(s);
            if (link->start_byte < 0 || link->end_byte <= link->start_byte ||
                (size_t)link->end_byte > text_len)
                continue;

            link_start = s + link->start_byte;
            link_end = s + link->end_byte;
            if (link_end <= visible || link_start >= visible_end)
                continue;

            draw_start = link_start < visible ? visible : link_start;
            draw_end = link_end > visible_end ? visible_end : link_end;
            screen_x = draw_col + utf8_width_between(visible, draw_start);
            if (screen_x < body_w)
                draw_utf8_range_clipped(body_win, y, screen_x,
                                        draw_start, draw_end,
                                        link_attr(idx, is_selected),
                                        body_w - screen_x);
        }
    }
}

static void draw_body(int body_h, int body_w)
{
    for (int y = 0; y < body_h; y++) {
        int idx = top + y;
        int source_col;

        if (idx >= line_count) {
            if (body_row_cache && y < body_row_cache_h && body_row_cache[y].valid) {
                clear_body_row(y, body_w);
                body_row_cache[y].valid = 0;
            }
            continue;
        }

        source_col = page_left_for_line(idx) + hscroll;
        if (source_col < 0)
            source_col = 0;

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
    int top_row, left, body_w, max_hscroll;

    getmaxyx(stdscr, h, w);

    body_h = body_height_for_term(h);
    top_row = body_top_for_term(h);

    page_viewport(w, &left, &body_w, &max_hscroll);
    (void)max_hscroll;

    ensure_body_window(top_row, left, body_w, body_h);

    /* Stage stdscr first. It covers the whole terminal, so if it is
       noutrefreshed after body_win it can overwrite the freshly drawn
       body pane with blanks on the initial paint after extraction. */
    if (show_ui)
        draw_chrome_if_needed(h, w);
    else if (chrome_dirty) {
        attrset(body_attr());
        werase(stdscr);
        wnoutrefresh(stdscr);
        chrome_dirty = 0;
    }
    draw_body(body_h, body_w);
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

static void clear_link_selection(void)
{
    if (selected_link < 0)
        return;
    selected_link = -1;
    link_revision++;
    chrome_dirty = 1;
    invalidate_body_cache();
}

static void free_chapter_index(void)
{
    for (int i = 0; i < chapter_count; i++)
        free(chapters[i].label);
    free(chapters);
    chapters = NULL;
    chapter_count = 0;
    chapter_capacity = 0;
}

static void free_document_links(void)
{
    for (int i = 0; i < document_link_count; i++) {
        free(document_links[i].label);
        free(document_links[i].source_text);
    }
    free(document_links);
    document_links = NULL;
    document_link_count = 0;
    document_link_capacity = 0;
    selected_link = -1;
    memset(link_page_state, 0, sizeof link_page_state);
    link_revision++;
}

static void reserve_document_links(int needed)
{
    if (needed <= document_link_capacity)
        return;

    int capacity = document_link_capacity ? document_link_capacity * 2 : 64;
    while (capacity < needed)
        capacity *= 2;

    DocumentLink *grown = realloc(document_links,
                                  (size_t)capacity * sizeof(*grown));
    if (!grown) {
        endwin();
        perror("realloc");
        exit(1);
    }
    document_links = grown;
    document_link_capacity = capacity;
}

static void append_document_link(const char *label, const char *source_text,
                                 int source_offset, int source_page,
                                 int target_page, int target_line,
                                 int synthetic)
{
    if (!label || !*label || source_page < 0 || source_page >= MAX_PAGES)
        return;

    reserve_document_links(document_link_count + 1);
    DocumentLink *link = &document_links[document_link_count++];
    memset(link, 0, sizeof *link);
    link->label = xstrdup(label);
    link->source_text = xstrdup(source_text && *source_text
                                    ? source_text : label);
    link->source_offset = source_offset >= 0 ? source_offset : 0;
    link->source_page = source_page;
    link->target_page = target_page;
    link->target_line = target_line;
    link->repair_target = 0;
    link->synthetic = synthetic;
    link->line_idx = -1;
    link->start_byte = -1;
    link->end_byte = -1;
}

static void append_utf8(char *out, size_t outsz, size_t *used,
                        unsigned long codepoint)
{
    unsigned char bytes[4];
    int count;

    if (codepoint <= 0x7f) {
        bytes[0] = (unsigned char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = 0xc0 | (unsigned char)(codepoint >> 6);
        bytes[1] = 0x80 | (unsigned char)(codepoint & 0x3f);
        count = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = 0xe0 | (unsigned char)(codepoint >> 12);
        bytes[1] = 0x80 | (unsigned char)((codepoint >> 6) & 0x3f);
        bytes[2] = 0x80 | (unsigned char)(codepoint & 0x3f);
        count = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = 0xf0 | (unsigned char)(codepoint >> 18);
        bytes[1] = 0x80 | (unsigned char)((codepoint >> 12) & 0x3f);
        bytes[2] = 0x80 | (unsigned char)((codepoint >> 6) & 0x3f);
        bytes[3] = 0x80 | (unsigned char)(codepoint & 0x3f);
        count = 4;
    } else {
        return;
    }

    if (*used + (size_t)count >= outsz)
        return;
    for (int i = 0; i < count; i++)
        out[(*used)++] = (char)bytes[i];
}

static char *decode_xml_text(const char *start, const char *end)
{
    size_t capacity = (size_t)(end - start) + 1;
    char *out = malloc(capacity);
    size_t used = 0;
    int in_tag = 0;

    if (!out)
        return NULL;

    for (const char *p = start; p < end; p++) {
        if (*p == '<') {
            in_tag = 1;
            continue;
        }
        if (in_tag) {
            if (*p == '>')
                in_tag = 0;
            continue;
        }
        if (*p != '&') {
            if (used + 1 < capacity)
                out[used++] = *p;
            continue;
        }

        const char *semi = memchr(p, ';', (size_t)(end - p));
        if (!semi || semi - p > 16) {
            if (used + 1 < capacity)
                out[used++] = *p;
            continue;
        }

        unsigned long codepoint = 0;
        int recognized = 1;
        if (semi - p == 4 && !memcmp(p, "&amp", 4))
            codepoint = '&';
        else if (semi - p == 3 && !memcmp(p, "&lt", 3))
            codepoint = '<';
        else if (semi - p == 3 && !memcmp(p, "&gt", 3))
            codepoint = '>';
        else if (semi - p == 5 && !memcmp(p, "&quot", 5))
            codepoint = '"';
        else if (semi - p == 5 && !memcmp(p, "&apos", 5))
            codepoint = '\'';
        else if (p + 2 < semi && p[1] == '#') {
            char number[16];
            const char *digits = p + 2;
            int base = 10;
            if (digits < semi && (*digits == 'x' || *digits == 'X')) {
                base = 16;
                digits++;
            }
            size_t count = (size_t)(semi - digits);
            if (count == 0 || count >= sizeof number) {
                recognized = 0;
            } else {
                memcpy(number, digits, count);
                number[count] = 0;
                char *number_end = NULL;
                codepoint = strtoul(number, &number_end, base);
                if (!number_end || *number_end)
                    recognized = 0;
            }
        } else {
            recognized = 0;
        }

        if (recognized) {
            append_utf8(out, capacity, &used, codepoint);
            p = semi;
        } else if (used + 1 < capacity) {
            out[used++] = *p;
        }
    }
    out[used] = 0;

    /* pdftohtml can preserve layout padding inside an anchor. Reflow it to
       the same single-space form used by SimplePDF's reading layout. */
    char *src = out;
    char *dst = out;
    int pending_space = 0;
    while (*src) {
        if (isspace((unsigned char)*src) ||
            ((unsigned char)src[0] == 0xc2 &&
             (unsigned char)src[1] == 0xa0)) {
            pending_space = dst != out;
            src += (unsigned char)src[0] == 0xc2 ? 2 : 1;
            continue;
        }
        if (pending_space)
            *dst++ = ' ';
        pending_space = 0;
        *dst++ = *src++;
    }
    *dst = 0;
    return out;
}

static int numeric_link_list(const char *text)
{
    int count = 0;
    const char *p = text;

    while (*p) {
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (!isdigit((unsigned char)*p))
            return 0;
        while (isdigit((unsigned char)*p))
            p++;
        count++;
        if (*p && !isspace((unsigned char)*p))
            return 0;
    }
    return count >= 2;
}

static void append_extracted_link(const char *label, int source_page,
                                  int target_page)
{
    if (target_page == source_page && numeric_link_list(label)) {
        const char *p = label;
        while (*p) {
            while (isspace((unsigned char)*p))
                p++;
            if (!*p)
                break;
            const char *start = p;
            while (isdigit((unsigned char)*p))
                p++;
            char number[32];
            size_t length = (size_t)(p - start);
            if (length >= sizeof number)
                length = sizeof number - 1;
            memcpy(number, start, length);
            number[length] = 0;
            append_document_link(number, label, (int)(start - label),
                                 source_page, target_page, -1, 0);
        }
    } else {
        append_document_link(label, label, 0, source_page, target_page,
                             -1, 0);
    }
}

static int parse_internal_target(const char *href, size_t href_len)
{
    const char *hash = NULL;
    const char *end = href + href_len;

    for (const char *p = href; p < end; p++)
        if (*p == '#')
            hash = p;
    if (!hash || hash + 1 >= end || !isdigit((unsigned char)hash[1]))
        return -1;

    long page = 0;
    for (const char *p = hash + 1;
         p < end && isdigit((unsigned char)*p); p++) {
        page = page * 10 + (*p - '0');
        if (page > MAX_PAGES)
            return -1;
    }
    return page > 0 ? (int)page - 1 : -1;
}

static void parse_pdftohtml_link_line(const char *line, int source_page)
{
    const char *cursor = line;

    while ((cursor = strstr(cursor, "<a ")) != NULL) {
        const char *href = strstr(cursor, "href=");
        const char *open_end = strchr(cursor, '>');
        if (!href || !open_end || href > open_end) {
            cursor += 3;
            continue;
        }

        href += 5;
        char quote = *href;
        if (quote != '"' && quote != '\'') {
            cursor = open_end + 1;
            continue;
        }
        const char *href_end = strchr(href + 1, quote);
        const char *close = strstr(open_end + 1, "</a>");
        if (!href_end || href_end > open_end || !close) {
            cursor = open_end + 1;
            continue;
        }

        int target = parse_internal_target(href + 1,
                                           (size_t)(href_end - href - 1));
        if (target >= 0) {
            char *label = decode_xml_text(open_end + 1, close);
            if (label && *label)
                append_extracted_link(label, source_page, target);
            free(label);
        }
        cursor = close + 4;
    }
}

static int decimal_text(const char *text, int *value)
{
    const char *p = text;
    long result = 0;

    while (isspace((unsigned char)*p))
        p++;
    if (!isdigit((unsigned char)*p))
        return 0;
    while (isdigit((unsigned char)*p)) {
        result = result * 10 + (*p - '0');
        if (result > INT_MAX)
            return 0;
        p++;
    }
    while (isspace((unsigned char)*p))
        p++;
    if (*p)
        return 0;
    *value = (int)result;
    return 1;
}

static int heading_matches_link(const char *heading, const char *label)
{
    char heading_copy[MAX_LINE];
    char label_copy[MAX_LINE];
    int chapter_number;

    snprintf(heading_copy, sizeof heading_copy, "%s", heading);
    snprintf(label_copy, sizeof label_copy, "%s", label);
    char *h = trim(heading_copy);
    char *l = trim(label_copy);

    if (!strcasecmp(h, l))
        return 1;

    if (decimal_text(l, &chapter_number)) {
        int candidate = -1;
        int used = 0;
        if (sscanf(h, "Chapter %d%n", &candidate, &used) == 1) {
            while (h[used] && isspace((unsigned char)h[used]))
                used++;
            if (!h[used] && candidate == chapter_number)
                return 1;
        }

        char query[64];
        snprintf(query, sizeof query, "Chapter %d", chapter_number);
        char *hit = case_find(h, query);
        if (hit) {
            size_t length = strlen(query);
            int left_ok = hit == h || !isalnum((unsigned char)hit[-1]);
            int right_ok = !isalnum((unsigned char)hit[length]);
            if (left_ok && right_ok)
                return 1;
        }
        return 0;
    }

    /* Some ebook conversions change “Book One” into “Book 1”. The title
       after the dash is still a strong, unambiguous repair key. */
    const char *dash = strrchr(l, '-');
    if (dash) {
        dash++;
        while (*dash && isspace((unsigned char)*dash))
            dash++;
        if (strlen(dash) >= 4 && case_find(h, dash))
            return 1;
    }

    return strlen(l) >= 5 && strlen(h) <= strlen(l) + 20 &&
           case_find(h, l) != NULL;
}

static int find_heading_target_after(const char *label, int source_page,
                                     int source_line)
{
    int start = 0;

    if (source_line >= 0 && source_line < line_count)
        start = source_line + 1;
    else if (source_page >= 0 && source_page < page_count)
        start = pages[source_page].end;

    for (int i = start; i < line_count; i++)
        if (line_kinds[i] == LINE_HEADING &&
            heading_matches_link(lines[i], label))
            return i;

    /* A conservative fallback for documents whose heading typography was
       flattened during extraction. */
    for (int i = start; i < line_count; i++)
        if (line_kinds[i] == LINE_BODY && visual_len(lines[i]) <= 90 &&
            heading_matches_link(lines[i], label))
            return i;

    return -1;
}

static int find_heading_on_page(const char *label, int page)
{
    if (page < 0 || page >= page_count)
        return -1;

    for (int i = pages[page].start; i < pages[page].end; i++)
        if (line_kinds[i] == LINE_HEADING &&
            heading_matches_link(lines[i], label))
            return i;
    for (int i = pages[page].start; i < pages[page].end; i++)
        if (line_kinds[i] == LINE_BODY && visual_len(lines[i]) <= 90 &&
            heading_matches_link(lines[i], label))
            return i;
    return -1;
}

static int link_token_find(const char *line, const char *needle,
                           const char **match)
{
    char *hit = case_find(line, needle);
    size_t length = strlen(needle);

    while (hit) {
        int left_ok = hit == line ||
                      !isalnum((unsigned char)hit[-1]);
        int right_ok = !isalnum((unsigned char)hit[length]);
        if (left_ok && right_ok) {
            *match = hit;
            return 1;
        }
        hit = case_find(hit + 1, needle);
    }
    return 0;
}

static void map_document_link(DocumentLink *link)
{
    link->line_idx = -1;
    link->start_byte = -1;
    link->end_byte = -1;

    if (link->source_page < 0 || link->source_page >= page_count)
        return;

    int start = pages[link->source_page].start;
    int end = pages[link->source_page].end;
    for (int i = start; i < end; i++) {
        char *group = case_find(lines[i], link->source_text);
        if (group) {
            const char *candidate = group + link->source_offset;
            size_t label_len = strlen(link->label);
            if ((size_t)(candidate - lines[i]) + label_len <=
                    strlen(lines[i]) &&
                !strncasecmp(candidate, link->label, label_len)) {
                link->line_idx = i;
                link->start_byte = (int)(candidate - lines[i]);
                link->end_byte = link->start_byte + (int)label_len;
                return;
            }
        }
    }

    for (int i = start; i < end; i++) {
        const char *hit = NULL;
        int numeric = 0;
        if (decimal_text(link->label, &numeric)) {
            if (!link_token_find(lines[i], link->label, &hit))
                continue;
        } else {
            hit = case_find(lines[i], link->label);
            if (!hit)
                continue;
        }
        link->line_idx = i;
        link->start_byte = (int)(hit - lines[i]);
        link->end_byte = link->start_byte + (int)strlen(link->label);
        return;
    }
}

static void remap_document_links(void)
{
    for (int i = 0; i < document_link_count; i++) {
        DocumentLink *link = &document_links[i];
        if (!link->exact_source)
            map_document_link(link);
        if (link->exact_source)
            continue;
        if (link->repair_target)
            link->target_line = find_heading_target_after(
                link->label, link->source_page, link->line_idx);
        else if (!link->synthetic)
            link->target_line = find_heading_on_page(link->label,
                                                    link->target_page);
    }
    if (selected_link >= document_link_count)
        selected_link = -1;
    link_revision++;
    chrome_dirty = 1;
    invalidate_body_cache();
}

static void discard_links_from(int first)
{
    if (first < 0)
        first = 0;
    if (first > document_link_count)
        first = document_link_count;
    for (int i = first; i < document_link_count; i++) {
        free(document_links[i].label);
        free(document_links[i].source_text);
    }
    document_link_count = first;
}

static int pdftohtml_source_page(const char *line)
{
    const char *page = strstr(line, "<page ");
    const char *number;
    char quote;
    char *end = NULL;
    long value;

    if (!page || !(number = strstr(page, "number=")))
        return -1;
    number += 7;
    quote = *number;
    if (quote != '"' && quote != '\'')
        return -1;
    errno = 0;
    value = strtol(number + 1, &end, 10);
    if (errno || end == number + 1 || !end || *end != quote || value < 1 ||
        value > MAX_PAGES)
        return -1;
    return (int)value - 1;
}

static int ensure_pdf_links_for_range(int first_page, int last_page)
{
    int output[2];
    pid_t pid;
    int status = 0;
    int first_link;
    int scan_first = -1;
    int scan_last = -1;
    char first_number[32];
    char last_number[32];

    if (epub_mode || page_count <= 0)
        return 0;
    if (first_page < 0)
        first_page = 0;
    if (last_page >= page_count)
        last_page = page_count - 1;
    if (last_page >= MAX_PAGES)
        last_page = MAX_PAGES - 1;
    if (first_page > last_page)
        return 0;

    for (int page = first_page; page <= last_page; page++) {
        if (link_page_state[page] != 0)
            continue;
        if (scan_first < 0)
            scan_first = page;
        scan_last = page;
        link_page_state[page] = 3;
    }
    if (scan_first < 0)
        return 1;

    if (pipe(output) != 0) {
        for (int page = scan_first; page <= scan_last; page++)
            if (link_page_state[page] == 3)
                link_page_state[page] = 2;
        return 0;
    }

    pid = fork();
    if (pid < 0) {
        close(output[0]);
        close(output[1]);
        for (int page = scan_first; page <= scan_last; page++)
            if (link_page_state[page] == 3)
                link_page_state[page] = 2;
        return 0;
    }

    snprintf(first_number, sizeof first_number, "%d", scan_first + 1);
    snprintf(last_number, sizeof last_number, "%d", scan_last + 1);
    if (pid == 0) {
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(output[1]);
        close(STDERR_FILENO);
        execlp("pdftohtml", "pdftohtml",
               "-f", first_number, "-l", last_number,
               "-q", "-i", "-xml", "-hidden", "-stdout",
               document_path, NULL);
        _exit(127);
    }

    close(output[1]);
    FILE *stream = fdopen(output[0], "r");
    first_link = document_link_count;
    if (stream) {
        char *line = NULL;
        size_t capacity = 0;
        int source_page = scan_first;
        while (getline(&line, &capacity, stream) >= 0) {
            int parsed_page = pdftohtml_source_page(line);
            if (parsed_page >= 0)
                source_page = parsed_page;
            if (source_page >= 0 && source_page < MAX_PAGES &&
                link_page_state[source_page] == 3)
                parse_pdftohtml_link_line(line, source_page);
        }
        free(line);
        fclose(stream);
    } else {
        close(output[0]);
    }

    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        discard_links_from(first_link);
        for (int page = scan_first; page <= scan_last; page++)
            if (link_page_state[page] == 3)
                link_page_state[page] = 2;
        return 0;
    }

    for (int page = scan_first; page <= scan_last; page++) {
        if (link_page_state[page] != 3)
            continue;

        int page_links = 0;
        int self_links = 0;
        for (int i = first_link; i < document_link_count; i++) {
            if (document_links[i].source_page != page)
                continue;
            page_links++;
            if (document_links[i].target_page == page)
                self_links++;
        }

        int broken_self_targets = page_links >= 3 &&
                                  self_links * 4 >= page_links * 3;
        for (int i = first_link; i < document_link_count; i++) {
            DocumentLink *link = &document_links[i];
            if (link->source_page != page)
                continue;
            map_document_link(link);
            if (link->target_page < 0 || link->target_page >= page_count ||
                (broken_self_targets && link->target_page == page)) {
                link->repair_target = 1;
                link->target_line = find_heading_target_after(
                    link->label, page, link->line_idx);
            } else {
                link->target_line = find_heading_on_page(link->label,
                                                        link->target_page);
            }
        }

        link_page_state[page] = 1;
        merge_chapters_from_links(page);
    }

    link_revision++;
    chrome_dirty = 1;
    invalidate_body_cache();
    return 1;
}

static int ensure_pdf_links_for_page(int page)
{
    return ensure_pdf_links_for_range(page, page);
}

static void remove_synthetic_links(void)
{
    int write = 0;

    for (int read = 0; read < document_link_count; read++) {
        if (document_links[read].synthetic) {
            free(document_links[read].label);
            free(document_links[read].source_text);
            continue;
        }
        if (write != read)
            document_links[write] = document_links[read];
        write++;
    }
    document_link_count = write;
    selected_link = -1;
}

static void reserve_chapters(int needed)
{
    if (needed <= chapter_capacity)
        return;

    int capacity = chapter_capacity ? chapter_capacity * 2 : 64;
    while (capacity < needed)
        capacity *= 2;

    Chapter *grown = realloc(chapters, (size_t)capacity * sizeof(*grown));
    if (!grown) {
        endwin();
        perror("realloc");
        exit(1);
    }
    chapters = grown;
    chapter_capacity = capacity;
}

static int compare_chapters(const void *left, const void *right)
{
    const Chapter *a = left;
    const Chapter *b = right;
    return (a->line_idx > b->line_idx) - (a->line_idx < b->line_idx);
}

static int useful_chapter_label(const char *label)
{
    int letters = 0;
    int visible = 0;

    if (!label || !*label || visual_len(label) > 100 ||
        !strcmp(label, "[]") || !strcmp(label, "[ ]") ||
        label[0] == '$' || !strncasecmp(label, "ISBN ", 5) ||
        !strcasecmp(label, "NEW YORK") ||
        !strcasecmp(label, "PHILOMEL BOOKS") ||
        !strcasecmp(label, "UC TXT"))
        return 0;

    for (const unsigned char *p = (const unsigned char *)label; *p; p++) {
        if (isalpha(*p))
            letters++;
        if (!isspace(*p))
            visible++;
    }
    return letters >= 2 && visible >= 2;
}

static char *line_label_hit(const char *line, const char *label)
{
    char *hit = case_find(line, label);
    size_t length = strlen(label);

    while (hit) {
        int left_ok = hit == line ||
                      !isalnum((unsigned char)hit[-1]);
        int right_ok = !isalnum((unsigned char)hit[length]);
        if (left_ok && right_ok)
            return hit;
        hit = case_find(hit + 1, label);
    }
    return NULL;
}

static int find_epub_link_source(const char *label, int hint,
                                 int *start_byte, int *end_byte)
{
    char candidate[MAX_LINE];
    int first_text = -1;
    int limit;

    if (!label || !*label || line_count <= 0)
        return -1;
    if (hint < 0)
        hint = 0;
    if (hint >= line_count)
        hint = line_count - 1;
    limit = hint + 256;
    if (limit > line_count)
        limit = line_count;

    snprintf(candidate, sizeof candidate, "%s", label);
    for (;;) {
        for (int line = hint; line < limit; line++) {
            if (line_kinds[line] == LINE_BLANK) {
                if (first_text >= 0)
                    break;
                continue;
            }
            if (first_text < 0)
                first_text = line;

            const char *hit = NULL;
            if (!link_token_find(lines[line], candidate, &hit))
                continue;
            *start_byte = (int)(hit - lines[line]);
            *end_byte = *start_byte + (int)strlen(candidate);
            return line;
        }

        char *space = strrchr(candidate, ' ');
        if (!space)
            break;
        *space = 0;
        while (space > candidate && space[-1] == ' ')
            *--space = 0;
    }
    return -1;
}

static void materialize_epub_links(void)
{
    epub_exact_link_count = 0;
    for (int i = 0; i < document_link_count; i++)
        if (document_links[i].exact_source)
            epub_exact_link_count++;
    for (int i = 0; i < epub_pending_link_count; i++) {
        EpubPendingLink *pending = &epub_pending_links[i];
        int target_line = epub_target_line(pending->target_key);
        int start_byte = -1;
        int end_byte = -1;
        int source_line = find_epub_link_source(
            pending->label, pending->source_hint, &start_byte, &end_byte);

        if (source_line < 0 || target_line < 0)
            continue;
        append_document_link(pending->label, pending->label, 0,
                             page_for_line(source_line),
                             page_for_line(target_line), target_line, 0);
        DocumentLink *link = &document_links[document_link_count - 1];
        link->exact_source = 1;
        link->line_idx = source_line;
        link->start_byte = start_byte;
        link->end_byte = end_byte;
        epub_exact_link_count++;
    }

    for (int i = 0; i < epub_pending_link_count; i++) {
        free(epub_pending_links[i].target_key);
        free(epub_pending_links[i].label);
    }
    free(epub_pending_links);
    epub_pending_links = NULL;
    epub_pending_link_count = 0;
    epub_pending_link_capacity = 0;
}

static int add_epub_toc_label(const char *label, int exact_target)
{
    int source = -1;
    int target = exact_target;
    int best_score = INT_MAX;

    if (!useful_chapter_label(label))
        return 0;

    if (epub_exact_link_count == 0) {
        int source_limit = line_count < EPUB_TOC_SOURCE_LIMIT
                               ? line_count : EPUB_TOC_SOURCE_LIMIT;
        if (target >= 0 && source_limit > target)
            source_limit = target;
        for (int line = 0; line < source_limit; line++) {
            char *hit = line_label_hit(lines[line], label);
            if (!hit)
                continue;
            source = line;
            break;
        }
    }

    for (int index = 0; target < 0 && index < epub_heading_count; index++) {
        int line = epub_heading_lines[index];
        if (source >= 0 && line <= source)
            continue;
        char *hit = line_label_hit(lines[line], label);
        if (!hit)
            continue;
        int score = visual_len(lines[line]) - visual_len(label);
        if (score < 0)
            score = 0;
        score -= 1000;

        char copy[MAX_LINE];
        snprintf(copy, sizeof copy, "%s", lines[line]);
        if (!strcasecmp(trim(copy), label))
            score -= 2000;
        if (hit == lines[line])
            score -= 100;

        if (score < best_score) {
            best_score = score;
            target = line;
        }
    }

    if (target < 0 && source >= 0 && line_kinds[source] == LINE_HEADING) {
        target = source;
        source = -1;
    }
    if (target < 0)
        return 0;

    for (int i = 0; i < chapter_count; i++)
        if (chapters[i].line_idx == target &&
            !strcasecmp(chapters[i].label, label))
            return 0;

    reserve_chapters(chapter_count + 1);
    chapters[chapter_count].label = xstrdup(label);
    chapters[chapter_count].line_idx = target;
    chapters[chapter_count].page = page_for_line(target);
    chapter_count++;

    if (source >= 0)
        append_document_link(label, label, 0, page_for_line(source),
                             page_for_line(target), target, 1);
    return 1;
}

static int load_epub_toc_index(void)
{
    SimpleEpubList entries = {0};
    const char *ncx_path = NULL;
    char *xml = NULL;
    size_t used = 0;
    int added = 0;

    if (!epub_mode)
        return 0;

    free(epub_heading_lines);
    epub_heading_lines = malloc((size_t)line_count *
                                sizeof(*epub_heading_lines));
    epub_heading_count = 0;
    if (epub_heading_lines)
        for (int line = 0; line < line_count; line++)
            if (line_kinds[line] == LINE_HEADING)
                epub_heading_lines[epub_heading_count++] = line;
    if (simpleepub_archive_entries(document_path, &entries) != 0)
        goto done;
    for (int i = 0; i < entries.count; i++)
        if (simpleepub_has_extension(entries.items[i], ".ncx")) {
            ncx_path = entries.items[i];
            break;
        }
    if (!ncx_path ||
        simpleepub_read_entry(document_path, ncx_path, &xml, &used,
                              (size_t)32 * 1024 * 1024) != 0)
        goto done;

    char *cursor = xml;
    while ((cursor = strstr(cursor, "<navLabel")) != NULL) {
        char *block_end = strstr(cursor, "</navLabel>");
        char *text_tag = strstr(cursor, "<text");
        if (!block_end || !text_tag || text_tag > block_end)
            break;
        char *text_start = strchr(text_tag, '>');
        char *text_end = text_start ? strstr(text_start + 1, "</text>") : NULL;
        if (text_start && text_end && text_end < block_end) {
            char *label = decode_xml_text(text_start + 1, text_end);
            if (label) {
                int target_line = -1;
                char *content = strstr(block_end, "<content");
                char *next_label = strstr(block_end, "<navLabel");
                if (content && (!next_label || content < next_label)) {
                    const char *content_end = simpleepub_tag_end(content);
                    if (content_end) {
                        char *src = simpleepub_xml_attribute(
                            content, (size_t)(content_end - content + 1),
                            "src");
                        char *key = src
                                        ? simpleepub_resolve_href_key(ncx_path,
                                                                      src)
                                        : NULL;
                        if (key)
                            target_line = epub_target_line(key);
                        free(key);
                        free(src);
                    }
                }
                added += add_epub_toc_label(label, target_line);
                free(label);
            }
        }
        cursor = block_end + 11;
    }

done:
    free(xml);
    simpleepub_list_free(&entries);
    free(epub_heading_lines);
    epub_heading_lines = NULL;
    epub_heading_count = 0;
    return added;
}

static void add_epub_contents_links(void)
{
    if (!epub_mode)
        return;

    for (int c = 0; c < chapter_count; c++) {
        Chapter *chapter = &chapters[c];
        int limit = chapter->line_idx;
        if (limit > 3000)
            limit = 3000;

        for (int line = 0; line < limit; line++) {
            char *hit = case_find(lines[line], chapter->label);
            if (!hit)
                continue;

            size_t length = strlen(chapter->label);
            int left_ok = hit == lines[line] ||
                          !isalnum((unsigned char)hit[-1]);
            int right_ok = !isalnum((unsigned char)hit[length]);
            if (!left_ok || !right_ok)
                continue;

            append_document_link(chapter->label, chapter->label, 0,
                                 page_for_line(line), chapter->page,
                                 chapter->line_idx, 1);
            break;
        }
    }
}

static void rebuild_chapter_index(void)
{
    free_chapter_index();
    remove_synthetic_links();
    if (epub_mode)
        materialize_epub_links();
    int have_epub_toc = epub_mode ? load_epub_toc_index() : 0;

    for (int line = 0; !have_epub_toc && line < line_count; line++) {
        if (line_kinds[line] != LINE_HEADING)
            continue;

        char copy[MAX_LINE];
        snprintf(copy, sizeof copy, "%s", lines[line]);
        char *label = trim(copy);
        if (!useful_chapter_label(label))
            continue;

        int duplicate = -1;
        for (int i = 0; i < chapter_count; i++) {
            if (!strcasecmp(chapters[i].label, label)) {
                duplicate = i;
                break;
            }
        }

        if (duplicate >= 0) {
            chapters[duplicate].line_idx = line;
            chapters[duplicate].page = page_for_line(line);
            continue;
        }

        reserve_chapters(chapter_count + 1);
        chapters[chapter_count].label = xstrdup(label);
        chapters[chapter_count].line_idx = line;
        chapters[chapter_count].page = page_for_line(line);
        chapter_count++;
    }

    if (chapter_count > 1)
        qsort(chapters, (size_t)chapter_count, sizeof(*chapters),
              compare_chapters);

    if (!have_epub_toc && epub_exact_link_count == 0)
        add_epub_contents_links();
    remap_document_links();
    if (!epub_mode)
        for (int page = 0; page < page_count; page++)
            if (link_page_state[page] == 1)
                merge_chapters_from_links(page);
}

static void remove_chapter_at(int index)
{
    if (index < 0 || index >= chapter_count)
        return;
    free(chapters[index].label);
    if (index + 1 < chapter_count)
        memmove(&chapters[index], &chapters[index + 1],
                (size_t)(chapter_count - index - 1) * sizeof(*chapters));
    chapter_count--;
}

static void merge_chapters_from_links(int page)
{
    if (epub_mode)
        return;

    /* Once a contents page has been inspected, its linked labels should not
       masquerade as chapter destinations in the outline. */
    for (int c = chapter_count - 1; c >= 0; c--) {
        for (int i = 0; i < document_link_count; i++) {
            DocumentLink *link = &document_links[i];
            if (link->source_page != page ||
                chapters[c].line_idx != link->line_idx)
                continue;
            if (heading_matches_link(chapters[c].label, link->label) ||
                case_find(link->source_text, chapters[c].label)) {
                remove_chapter_at(c);
                break;
            }
        }
    }

    for (int c = chapter_count - 1; c >= 0; c--) {
        int page_has_target = 0;
        int line_is_target = 0;
        for (int i = 0; i < document_link_count; i++) {
            DocumentLink *link = &document_links[i];
            if (link->source_page != page || link->target_line < 0)
                continue;
            if (page_for_line(link->target_line) == chapters[c].page) {
                page_has_target = 1;
                if (link->target_line == chapters[c].line_idx)
                    line_is_target = 1;
            }
        }
        if (page_has_target && !line_is_target)
            remove_chapter_at(c);
    }

    for (int i = 0; i < document_link_count; i++) {
        DocumentLink *link = &document_links[i];
        if (link->source_page != page || link->target_line < 0)
            continue;

        char display[MAX_LINE];
        int number = 0;
        if (decimal_text(link->label, &number))
            snprintf(display, sizeof display, "Chapter %d", number);
        else
            snprintf(display, sizeof display, "%s", link->label);
        if (!useful_chapter_label(display))
            continue;

        int existing = -1;
        for (int c = 0; c < chapter_count; c++) {
            if (chapters[c].line_idx == link->target_line ||
                !strcasecmp(chapters[c].label, display)) {
                existing = c;
                break;
            }
        }

        if (existing >= 0) {
            free(chapters[existing].label);
            chapters[existing].label = xstrdup(display);
            chapters[existing].line_idx = link->target_line;
            chapters[existing].page = page_for_line(link->target_line);
        } else {
            reserve_chapters(chapter_count + 1);
            chapters[chapter_count].label = xstrdup(display);
            chapters[chapter_count].line_idx = link->target_line;
            chapters[chapter_count].page = page_for_line(link->target_line);
            chapter_count++;
        }
    }

    if (chapter_count > 1)
        qsort(chapters, (size_t)chapter_count, sizeof(*chapters),
              compare_chapters);
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

static void set_notice(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(notice, sizeof notice, fmt, args);
    va_end(args);
    chrome_dirty = 1;
}

static int page_start_line(int page)
{
    if (page_count <= 0 || line_count <= 0)
        return 0;
    if (page < 0)
        page = 0;
    if (page >= page_count)
        page = page_count - 1;

    int line = pages[page].start;
    while (line < pages[page].end && line_kinds[line] == LINE_BLANK)
        line++;

    if (line >= line_count)
        line = line_count - 1;
    if (line == pages[page].end && line > 0)
        line--;
    return line;
}

static void go_to_page(int page)
{
    if (page < 0)
        page = 0;
    if (page >= page_count)
        page = page_count - 1;

    top = page_start_line(page);
    hscroll = 0;
    hscroll_user_set = 0;
    invalidate_body_cache();
}

static void push_nav_history(void)
{
    if (nav_history_count == MAX_NAV_HISTORY) {
        memmove(nav_history, nav_history + 1,
                (MAX_NAV_HISTORY - 1) * sizeof(*nav_history));
        nav_history_count--;
    }

    NavHistory *entry = &nav_history[nav_history_count++];
    entry->top = top;
    entry->hscroll = hscroll;
    entry->hscroll_user_set = hscroll_user_set;
    entry->page = page_for_line(top);
    chrome_dirty = 1;
}

static int pop_nav_history(void)
{
    if (nav_history_count <= 0) {
        set_notice("No earlier document jump");
        return 0;
    }

    NavHistory entry = nav_history[--nav_history_count];
    top = entry.top;
    if (top < 0 || top >= line_count)
        top = page_start_line(entry.page);
    hscroll = entry.hscroll;
    hscroll_user_set = entry.hscroll_user_set;
    clear_link_selection();
    if (epub_mode)
        set_notice("Returned to previous reading position");
    else
        set_notice("Returned to page %d", page_for_line(top) + 1);
    invalidate_body_cache();
    return 1;
}

static int link_position_after(const DocumentLink *left,
                               const DocumentLink *right)
{
    if (left->line_idx != right->line_idx)
        return left->line_idx > right->line_idx;
    return left->start_byte > right->start_byte;
}

static int link_from_viewport(int page, int direction, int first_line,
                              int last_line)
{
    int best = -1;
    int best_tier = INT_MAX;

    for (int i = 0; i < document_link_count; i++) {
        DocumentLink *link = &document_links[i];
        int tier;

        if (link->source_page != page || link->line_idx < 0)
            continue;
        if (link->line_idx >= first_line && link->line_idx <= last_line)
            tier = 0;
        else if ((direction > 0 && link->line_idx > last_line) ||
                 (direction < 0 && link->line_idx < first_line))
            tier = 1;
        else
            tier = 2;

        if (tier < best_tier ||
            (tier == best_tier &&
             (best < 0 ||
              (direction > 0 &&
               link_position_after(&document_links[best], link)) ||
              (direction < 0 &&
               link_position_after(link, &document_links[best]))))) {
            best = i;
            best_tier = tier;
        }
    }
    return best;
}

static int select_link(int direction)
{
    int page = page_for_line(top);
    int best = -1;
    int edge = -1;
    int body_h = body_height_for_term(LINES);
    int visible_last = top + (body_h > 0 ? body_h - 1 : 0);
    int selected_on_page = selected_link >= 0 &&
                           selected_link < document_link_count &&
                           document_links[selected_link].source_page == page;

    if (!epub_mode)
        ensure_pdf_links_for_page(page);

    if (!selected_on_page)
        best = link_from_viewport(page, direction, top, visible_last);

    for (int i = 0; i < document_link_count; i++) {
        DocumentLink *link = &document_links[i];
        if (link->source_page != page || link->line_idx < 0)
            continue;

        if (edge < 0 ||
            (direction > 0 &&
             link_position_after(&document_links[edge], link)) ||
            (direction < 0 &&
             link_position_after(link, &document_links[edge])))
            edge = i;

        if (selected_on_page) {
            int after = link_position_after(link,
                                            &document_links[selected_link]);
            int before = link_position_after(&document_links[selected_link],
                                              link);
            if ((direction > 0 && !after) || (direction < 0 && !before))
                continue;
            if (best < 0 ||
                (direction > 0 &&
                 link_position_after(&document_links[best], link)) ||
                (direction < 0 &&
                 link_position_after(link, &document_links[best])))
                best = i;
        }
    }

    if (best < 0)
        best = edge;
    if (best < 0) {
        clear_link_selection();
        set_notice("No internal links on page %d", page + 1);
        return 0;
    }

    selected_link = best;
    if (document_links[best].line_idx < top ||
        document_links[best].line_idx >= top + body_h) {
        top = document_links[best].line_idx - body_h / 3;
        if (top < 0)
            top = 0;
    }
    link_revision++;
    chrome_dirty = 1;
    invalidate_body_cache();
    return 1;
}

static int follow_selected_link(void)
{
    if (selected_link < 0 || selected_link >= document_link_count) {
        set_notice("Use Shift-\u2191/\u2193 to choose a link");
        return 0;
    }

    DocumentLink *link = &document_links[selected_link];
    int target_line = link->target_line;
    int target_page = link->target_page;
    char label[160];
    snprintf(label, sizeof label, "%s", link->label);

    if (target_line < 0 && link->repair_target)
        target_line = find_heading_target_after(link->label,
                                                link->source_page,
                                                link->line_idx);
    if (target_line < 0 &&
        (target_page < 0 || target_page >= page_count)) {
        set_notice("That PDF destination is unavailable");
        return 0;
    }

    push_nav_history();
    if (target_line >= 0) {
        top = target_line;
        hscroll = 0;
        hscroll_user_set = 0;
        invalidate_body_cache();
    } else {
        go_to_page(target_page);
    }
    clear_link_selection();
    set_notice("Opened %.100s  \u2022  Backspace returns", label);
    return 1;
}

static int link_at_body_position(int body_y, int body_x)
{
    int line_idx = top + body_y;

    if (body_y < 0 || body_y >= body_win_h || body_x < 0 ||
        body_x >= body_win_w || line_idx < 0 || line_idx >= line_count)
        return -1;

    const char *text = lines[line_idx];
    int source_col = page_left_for_line(line_idx) + hscroll;
    if (source_col < 0)
        source_col = 0;
    const char *visible = utf8_ptr_at_col(text, source_col);
    int draw_col = 0;
    if (line_kinds[line_idx] == LINE_HEADING && source_col == 0) {
        int width = visual_len(text);
        if (width < body_win_w)
            draw_col = (body_win_w - width) / 2;
    }

    for (int i = 0; i < document_link_count; i++) {
        DocumentLink *link = &document_links[i];
        if (link->line_idx != line_idx || link->start_byte < 0 ||
            link->end_byte <= link->start_byte)
            continue;

        const char *start = text + link->start_byte;
        const char *end = text + link->end_byte;
        if (end <= visible)
            continue;
        int x = draw_col + utf8_width_between(visible, start);
        int width = utf8_width_between(start, end);
        if (body_x >= x && body_x < x + width)
            return i;
    }
    return -1;
}

static void load_document_text(const char *txtpath, int term_w, int term_h)
{
    if (epub_mode)
        load_epub_text(txtpath, term_w, term_h);
    else if (reading_mode)
        load_reading_text(txtpath, term_w);
    else
        load_layout_text(txtpath);

    rebuild_chapter_index();
}

static void reload_preserving_position(const char *txtpath, int term_w,
                                       int term_h)
{
    int old_page = page_for_line(top);
    int old_start = old_page < page_count ? pages[old_page].start : 0;
    int old_end = old_page < page_count ? pages[old_page].end : old_start + 1;
    int old_span = old_end - old_start;
    int old_offset = top - old_start;

    if (old_span < 1)
        old_span = 1;
    if (old_offset < 0)
        old_offset = 0;

    load_document_text(txtpath, term_w, term_h);

    if (old_page >= page_count)
        old_page = page_count - 1;
    if (old_page < 0)
        old_page = 0;

    int new_start = pages[old_page].start;
    int new_span = pages[old_page].end - new_start;
    if (new_span < 1)
        new_span = 1;
    top = new_start + old_offset * new_span / old_span;

    last_match = -1;
    hscroll = 0;
    hscroll_user_set = 0;
    force_full_redraw();
}

static void reveal_match(int hit)
{
    int context = body_height_for_term(LINES) / 3;

    top = hit - context;
    if (top < 0)
        top = 0;
    last_match = hit;
    invalidate_body_cache();
}

static int prompt_line(const char *label, char *out, size_t outsz)
{
    int row = LINES > 0 ? LINES - 1 : 0;
    int col = 2 + (int)strlen(label);

    attrset(body_attr());
    move(row, 0);
    clrtoeol();
    if (COLS > 4)
        draw_utf8_clipped(stdscr, row, 2, label, heading_attr(), COLS - 4);
    move(row, col < COLS ? col : COLS - 1);
    refresh();

    echo();
    curs_set(1);
    out[0] = 0;
    int rc = getnstr(out, (int)outsz - 1);
    noecho();
    curs_set(0);

    chrome_dirty = 1;
    invalidate_body_cache();
    return rc;
}

static int show_chapter_overlay(void)
{
    if (!epub_mode)
        ensure_pdf_links_for_page(page_for_line(top));

    if (chapter_count <= 0) {
        set_notice("No chapter headings found");
        return 0;
    }

    int term_h, term_w;
    getmaxyx(stdscr, term_h, term_w);
    int win_w = term_w - 6;
    if (win_w > 76)
        win_w = 76;
    int win_h = term_h - 4;
    if (win_h > 30)
        win_h = 30;
    if (win_h > chapter_count + 4)
        win_h = chapter_count + 4;
    if (win_h < 8 || win_w < 38) {
        set_notice("Resize the terminal for chapter navigation");
        return 0;
    }

    int y = (term_h - win_h) / 2;
    int x = (term_w - win_w) / 2;
    WINDOW *win = newwin(win_h, win_w, y, x);
    if (!win)
        return 0;
    keypad(win, TRUE);
    wbkgdset(win, (chtype)' ' | body_attr());

    int selected = 0;
    for (int i = 0; i < chapter_count; i++) {
        if (chapters[i].line_idx <= top)
            selected = i;
        else
            break;
    }

    int list_rows = win_h - 4;
    int list_top = selected - list_rows / 2;
    if (list_top < 0)
        list_top = 0;
    if (list_top > chapter_count - list_rows)
        list_top = chapter_count - list_rows;
    if (list_top < 0)
        list_top = 0;

    int chosen = -1;
    while (1) {
        werase(win);
        box(win, 0, 0);

        const char *heading = " Chapters ";
        int heading_col = (win_w - visual_len(heading)) / 2;
        draw_utf8_clipped(win, 0, heading_col, heading, heading_attr(),
                          win_w - heading_col - 1);

        for (int row = 0; row < list_rows; row++) {
            int index = list_top + row;
            int screen_y = row + 2;
            wattrset(win, body_attr());
            mvwhline(win, screen_y, 1, ' ', win_w - 2);
            if (index >= chapter_count)
                continue;

            char label[MAX_LINE];
            char status[32];
            int status_w;
            int label_w;
            attr_t attr = index == selected
                              ? body_attr() | A_REVERSE | A_BOLD
                              : body_attr();

            if (epub_mode) {
                int progress = line_count > 1
                                   ? chapters[index].line_idx * 100 /
                                         (line_count - 1)
                                   : 0;
                snprintf(status, sizeof status, "%d%%", progress);
            } else {
                snprintf(status, sizeof status, "page %d",
                         chapters[index].page + 1);
            }
            status_w = visual_len(status);
            label_w = win_w - status_w - 7;
            ellipsize_text(label, sizeof label, chapters[index].label,
                           label_w);

            draw_utf8_clipped(win, screen_y, 3, label, attr, label_w);
            draw_utf8_clipped(win, screen_y, win_w - status_w - 3,
                              status, index == selected ? attr : muted_attr(),
                              status_w);
        }

        const char *hint = "\u2191\u2193 move  PgUp/PgDn  Enter jump  Esc close";
        int hint_col = (win_w - visual_len(hint)) / 2;
        if (hint_col < 2)
            hint_col = 2;
        draw_utf8_clipped(win, win_h - 2, hint_col, hint, muted_attr(),
                          win_w - hint_col - 2);
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            chosen = selected;
            break;
        }
        if (ch == 27 || ch == 'q' || ch == 'o' || ch == KEY_BACKSPACE ||
            ch == 127 || ch == '\b')
            break;
        if (ch == KEY_RESIZE)
            break;
        if (ch == KEY_UP || ch == 'k')
            selected--;
        else if (ch == KEY_DOWN || ch == 'j')
            selected++;
        else if (ch == KEY_PPAGE)
            selected -= list_rows > 1 ? list_rows - 1 : 1;
        else if (ch == KEY_NPAGE)
            selected += list_rows > 1 ? list_rows - 1 : 1;
        else if (ch == KEY_HOME || ch == 'g')
            selected = 0;
        else if (ch == KEY_END || ch == 'G')
            selected = chapter_count - 1;

        if (selected < 0)
            selected = 0;
        if (selected >= chapter_count)
            selected = chapter_count - 1;
        if (selected < list_top)
            list_top = selected;
        if (selected >= list_top + list_rows)
            list_top = selected - list_rows + 1;
    }

    char chosen_label[160] = "";
    int chosen_line = -1;
    if (chosen >= 0 && chosen < chapter_count) {
        chosen_line = chapters[chosen].line_idx;
        snprintf(chosen_label, sizeof chosen_label, "%s",
                 chapters[chosen].label);
    }

    delwin(win);
    force_full_redraw();

    if (chosen_line >= 0) {
        push_nav_history();
        top = chosen_line;
        hscroll = 0;
        hscroll_user_set = 0;
        clear_link_selection();
        set_notice("Jumped to %.100s  \u2022  Backspace returns",
                   chosen_label);
        return 1;
    }
    return 0;
}

static void show_help_overlay(void)
{
    static const char *help[] = {
        "\u2191/\u2193 or j/k       scroll one line",
        "PgUp/PgDn, Space/b  scroll one screen",
        "Shift-\u2191 / Shift-\u2193 select PDF links",
        "Enter              open selected link",
        "Backspace          return after a jump",
        "o                  chapter navigator",
        "[ / ]              previous / next physical page",
        "g / G              beginning / end",
        "/ or f             find text; n/N repeats",
        "p                  go to page",
        "r                  reading / original layout",
        "\u2190/\u2192 or h/l       pan original layout",
        "i                  focus mode",
        "q or Esc           close",
    };
    int term_h, term_w;
    getmaxyx(stdscr, term_h, term_w);

    int count = (int)(sizeof help / sizeof help[0]);
    int win_h = count + 5;
    int win_w = term_w - 4;
    if (win_w > 58)
        win_w = 58;

    if (win_h > term_h - 2 || win_w < 32) {
        set_notice("Resize the terminal for the full shortcut guide");
        return;
    }

    int y = (term_h - win_h) / 2;
    int x = (term_w - win_w) / 2;
    WINDOW *help_win = newwin(win_h, win_w, y, x);
    if (!help_win)
        return;

    keypad(help_win, TRUE);
    wbkgdset(help_win, (chtype)' ' | body_attr());
    box(help_win, 0, 0);

    const char *heading = " SimplePDF shortcuts ";
    int heading_col = (win_w - visual_len(heading)) / 2;
    draw_utf8_clipped(help_win, 0, heading_col, heading, heading_attr(),
                      win_w - heading_col - 1);

    for (int i = 0; i < count; i++)
        draw_utf8_clipped(help_win, i + 2, 3, help[i], body_attr(), win_w - 6);

    const char *dismiss = "press any key";
    int dismiss_col = (win_w - visual_len(dismiss)) / 2;
    draw_utf8_clipped(help_win, win_h - 2, dismiss_col, dismiss,
                      muted_attr(), win_w - dismiss_col - 1);
    wrefresh(help_win);
    wgetch(help_win);
    delwin(help_win);
    force_full_redraw();
}

static void clamp_top(void)
{
    int h, w;
    getmaxyx(stdscr, h, w);

    int max_top = line_count - body_height_for_term(h);
    if (max_top < 0) max_top = 0;

    if (top < 0) top = 0;
    if (top > max_top) top = max_top;

    int left, body_w, max_hscroll;
    page_viewport(w, &left, &body_w, &max_hscroll);
    (void)left;
    (void)body_w;

    if (!hscroll_user_set)
        hscroll = 0;
    if (hscroll < 0) hscroll = 0;
    if (hscroll > max_hscroll) hscroll = max_hscroll;
}

static void ensure_visible_pdf_links(void)
{
    int body_h;
    int last_line;
    int first_page;
    int last_page;

    if (epub_mode || line_count <= 0)
        return;
    body_h = body_height_for_term(LINES);
    last_line = top + (body_h > 0 ? body_h - 1 : 0);
    if (last_line >= line_count)
        last_line = line_count - 1;
    first_page = page_for_line(top);
    last_page = page_for_line(last_line);

    /* Contents pages need live underlines before the first keypress. Ordinary
       prose pages almost never contain links, and synchronously asking
       pdftohtml about every newly visible page makes scrolling feel sticky.
       Inspect contiguous contents-page runs only; Shift-Up/Down and mouse
       clicks still inspect any unusual linked prose page on demand. */
    for (int page = first_page; page <= last_page; ) {
        int first_contents;

        while (page <= last_page) {
            int has_contents = 0;
            for (int line = pages[page].start; line < pages[page].end; line++) {
                if (line_kinds[line] == LINE_TOC) {
                    has_contents = 1;
                    break;
                }
            }
            if (has_contents)
                break;
            page++;
        }
        if (page > last_page)
            break;

        first_contents = page++;
        while (page <= last_page) {
            int has_contents = 0;
            for (int line = pages[page].start; line < pages[page].end; line++) {
                if (line_kinds[line] == LINE_TOC) {
                    has_contents = 1;
                    break;
                }
            }
            if (!has_contents)
                break;
            page++;
        }
        ensure_pdf_links_for_range(first_contents, page - 1);
    }
}

static void viewer_loop(const char *txtpath)
{
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    discover_pdf_keys();
    mousemask(BUTTON1_CLICKED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
    mouseinterval(0);

    int h, w;
    getmaxyx(stdscr, h, w);

    load_document_text(txtpath, w, h);

    for (int page = 0; page < page_count; page++) {
        if (pages[page].end > pages[page].start) {
            top = page_start_line(page);
            break;
        }
    }

    int ch;
    while (1) {
        clamp_top();
        ensure_visible_pdf_links();
        draw();

        ch = read_pdf_key();

        if (notice[0]) {
            notice[0] = 0;
            chrome_dirty = 1;
        }

        if (ch == 'q' || ch == 27) break;
        else if (ch == PDF_KEY_LINK_NEXT) select_link(1);
        else if (ch == PDF_KEY_LINK_PREV) select_link(-1);
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
            follow_selected_link();
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
            pop_nav_history();
        else if (ch == KEY_DOWN || ch == 'j') {
            clear_link_selection();
            top++;
        }
        else if (ch == KEY_UP || ch == 'k') {
            clear_link_selection();
            top--;
        }
        else if (ch == KEY_RIGHT || ch == 'l') {
            clear_link_selection();
            if (!epub_mode && !reading_mode) {
                hscroll += 4;
                hscroll_user_set = 1;
            }
        }
        else if (ch == KEY_LEFT || ch == 'h') {
            clear_link_selection();
            if (!epub_mode && !reading_mode) {
                hscroll -= 4;
                hscroll_user_set = 1;
            }
        }
        else if (ch == 'c' || ch == '0') {
            clear_link_selection();
            hscroll_user_set = 0;
            hscroll = 0;
        }
        else if (ch == 'i') {
            show_ui = !show_ui;
            force_full_redraw();
        }
        else if (ch == KEY_RESIZE) {
            getmaxyx(stdscr, h, w);
            if ((epub_mode || reading_mode) && reader_width_for_term(w) != wrap_width)
                reload_preserving_position(txtpath, w, h);
            else
                force_full_redraw();
        }
        else if (ch == 'f' || ch == '/') {
            clear_link_selection();
            char buf[sizeof search_term];
            int rc = prompt_line("Find: ", buf, sizeof buf);
            if (rc != ERR && buf[0]) {
                snprintf(search_term, sizeof search_term, "%s", buf);
                int hit = find_next_match(search_term, top + 1);
                if (hit >= 0) {
                    reveal_match(hit);
                    set_notice("Match on page %d", page_for_line(hit) + 1);
                } else
                    set_notice("No matches for \"%s\"", search_term);
            }
        }
        else if (ch == 'n') {
            if (search_term[0]) {
                int hit = find_next_match(search_term, last_match + 1);
                if (hit >= 0) {
                    reveal_match(hit);
                    set_notice("Match on page %d", page_for_line(hit) + 1);
                }
            } else
                set_notice("Use / to search first");
        }
        else if (ch == 'N') {
            if (search_term[0]) {
                int hit = find_prev_match(search_term, last_match - 1);
                if (hit >= 0) {
                    reveal_match(hit);
                    set_notice("Match on page %d", page_for_line(hit) + 1);
                }
            } else
                set_notice("Use / to search first");
        }
        else if (ch == KEY_NPAGE || ch == ' ') {
            clear_link_selection();
            int step = body_height_for_term(LINES) - 1;
            top += step > 0 ? step : 1;
        }
        else if (ch == KEY_PPAGE || ch == 'b') {
            clear_link_selection();
            int step = body_height_for_term(LINES) - 1;
            top -= step > 0 ? step : 1;
        }
        else if (ch == ']') {
            clear_link_selection();
            go_to_page(page_for_line(top) + 1);
        }
        else if (ch == '[') {
            clear_link_selection();
            int page = page_for_line(top);
            if (top <= page_start_line(page) + 1)
                page--;
            go_to_page(page);
        }
        else if (ch == 'p') {
            clear_link_selection();
            char buf[32];
            if (prompt_line("Go to page: ", buf, sizeof buf) != ERR && buf[0]) {
                char *end = NULL;
                long page = strtol(buf, &end, 10);
                if (end && *end == 0 && page >= 1 && page <= page_count)
                    go_to_page((int)page - 1);
                else
                    set_notice("Enter a page from 1 to %d", page_count);
            }
        }
        else if (ch == 'r') {
            clear_link_selection();
            if (epub_mode) {
                set_notice("EPUB is already in reading layout");
            } else {
                reading_mode = !reading_mode;
                getmaxyx(stdscr, h, w);
                reload_preserving_position(txtpath, w, h);
                set_notice(reading_mode ? "Reading layout" : "Original PDF layout");
            }
        }
        else if (ch == '?') {
            show_help_overlay();
        }
        else if (ch == 'o') {
            show_chapter_overlay();
        }
        else if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                if (event.bstate & BUTTON4_PRESSED) {
                    clear_link_selection();
                    top -= 3;
                }
                else if (event.bstate & BUTTON5_PRESSED) {
                    clear_link_selection();
                    top += 3;
                }
                else if (event.bstate & BUTTON1_CLICKED) {
                    int body_y = event.y - body_win_top;
                    int body_x = event.x - body_win_left;
                    int line = top + body_y;
                    if (!epub_mode && line >= 0 && line < line_count)
                        ensure_pdf_links_for_page(page_for_line(line));
                    int link = link_at_body_position(body_y, body_x);
                    if (link >= 0) {
                        selected_link = link;
                        link_revision++;
                        follow_selected_link();
                    }
                }
            }
        }
        else if (ch == KEY_HOME || ch == 'g') {
            clear_link_selection();
            top = 0;
        }
        else if (ch == KEY_END || ch == 'G') {
            clear_link_selection();
            top = line_count;
        }
    }

    destroy_body_window();
    endwin();
}

static const char *basename_simple(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void set_document_title(const char *infile)
{
    pdf_page_total = 0;
    snprintf(title, sizeof title, "%.*s", (int)sizeof title - 1,
             basename_simple(infile));
    char *extension = strrchr(title, '.');
    if (extension && (!strcasecmp(extension, ".pdf") ||
                      !strcasecmp(extension, ".epub")))
        *extension = 0;

    if (epub_mode)
        return;

    int output[2];
    if (pipe(output) != 0)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        close(output[0]);
        close(output[1]);
        return;
    }

    if (pid == 0) {
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0) {
            close(output[1]);
            _exit(127);
        }
        close(output[1]);
        execlp("pdfinfo", "pdfinfo", infile, NULL);
        close(STDOUT_FILENO);
        _exit(127);
    }

    close(output[1]);
    FILE *fp = fdopen(output[0], "r");
    char metadata_title[sizeof title] = "";

    if (fp) {
        char line[1024];
        while (fgets(line, sizeof line, fp)) {
            if (!strncmp(line, "Title:", 6)) {
                char *value = trim(line + 6);
                if (*value)
                    snprintf(metadata_title, sizeof metadata_title, "%.*s",
                             (int)sizeof metadata_title - 1, value);
            } else if (!strncmp(line, "Pages:", 6)) {
                char *value = trim(line + 6);
                char *end = NULL;
                long pages_total = strtol(value, &end, 10);
                if (end != value && pages_total > 0 &&
                    pages_total <= MAX_PAGES)
                    pdf_page_total = (int)pages_total;
            }
        }
        fclose(fp);
    } else {
        close(output[0]);
    }

    int status;
    if (waitpid(pid, &status, 0) > 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && metadata_title[0] &&
        strcasecmp(metadata_title, "untitled"))
        snprintf(title, sizeof title, "%s", metadata_title);
}



static int choose_file_tui(char *out, size_t outsz)
{
    const char *command =
        "for dir in \"$PWD\" \"$HOME/Downloads\" \"$HOME/Documents\" "
        "\"$HOME/Desktop\" \"$HOME/Books\" \"$HOME/writing\"; do "
        "[ -d \"$dir\" ] || continue; "
        "find \"$dir\" -type f \\( -iname '*.pdf' -o -iname '*.epub' \\) 2>/dev/null; "
        "done | awk '!seen[$0]++' | "
        "fzf --height=90% --border=rounded --prompt='Open > ' "
        "--header='Enter to open  |  Esc to cancel'";
    FILE *fp = popen(command, "r");
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

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "usage: simplepdf [--layout] [FILE.pdf|FILE.epub]\n"
            "\n"
            "PDFs open in a reflowed reading layout by default. Use --layout\n"
            "to preserve the source page layout, or press r while reading.\n");
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    char chosen[PATH_MAX];
    const char *infile = NULL;
    int requested_layout = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--layout") || !strcmp(argv[i], "-l")) {
            requested_layout = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(stdout);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "simplepdf: unknown option: %s\n", argv[i]);
            print_usage(stderr);
            return 2;
        } else if (infile) {
            fprintf(stderr, "simplepdf: open one document at a time\n");
            return 2;
        } else {
            infile = argv[i];
        }
    }

    if (!infile) {
        if (choose_file_tui(chosen, sizeof chosen) != 0)
            return 1;
        infile = chosen;
    }

    struct stat input_stat;
    if (stat(infile, &input_stat) != 0 || access(infile, R_OK) != 0) {
        fprintf(stderr, "simplepdf: cannot read %s: %s\n", infile,
                strerror(errno));
        return 1;
    }

    if (!has_ext(infile, ".pdf") && !has_ext(infile, ".epub")) {
        fprintf(stderr, "simplepdf: expected a PDF or EPUB file\n");
        return 2;
    }

    if (!realpath(infile, document_path))
        snprintf(document_path, sizeof document_path, "%s", infile);

    epub_mode = has_ext(infile, ".epub");
    reading_mode = epub_mode || !requested_layout;
    set_document_title(infile);

    ensure_line_capacity(INITIAL_LINE_CAPACITY);

    char txtpath[PATH_MAX];
    char workpath[PATH_MAX];
    int cache_hit = 0;
    int cache_available = cache_text_path(infile, txtpath, sizeof txtpath,
                                          &cache_hit) == 0;
    int temporary_text = 0;

    if (!cache_hit) {
        if (cache_available) {
            if (snprintf(workpath, sizeof workpath, "%s.tmp.XXXXXX", txtpath) >=
                (int)sizeof workpath)
                cache_available = 0;
        }
        if (!cache_available)
            snprintf(workpath, sizeof workpath, "/tmp/simplepdf-XXXXXX");

        int tmpfd = mkstemp(workpath);
        if (tmpfd < 0) {
            fprintf(stderr, "simplepdf: cannot create extraction file: %s\n",
                    strerror(errno));
            free(lines);
            free(line_kinds);
            free(line_pages);
            return 1;
        }
        close(tmpfd);

        show_loading_screen();
        int rc = run_extract_text(infile, workpath);
        endwin();
        if (rc != 0) {
            fprintf(stderr,
                    "simplepdf: text extraction failed; install poppler-utils "
                    "for PDF or unzip (with pandoc as fallback) for EPUB\n");
            unlink(workpath);
            free(lines);
            free(line_kinds);
            free(line_pages);
            return 1;
        }

        if (cache_available && rename(workpath, txtpath) == 0) {
            temporary_text = 0;
        } else {
            snprintf(txtpath, sizeof txtpath, "%s", workpath);
            temporary_text = 1;
        }
    }

    viewer_loop(txtpath);

    if (temporary_text)
        unlink(txtpath);
    free_document_links();
    free_chapter_index();
    free_epub_metadata();
    free_lines();
    free(body_row_cache);
    free(lines);
    free(line_kinds);
    free(line_pages);
    line_capacity = 0;

    return 0;
}
