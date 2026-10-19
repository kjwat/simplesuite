#define _XOPEN_SOURCE 700
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <ncurses.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include "simpleui.h"

#define QUERY_CHARS 512
#define QUERY_BYTES (QUERY_CHARS * 4)
#define DEBOUNCE_MS 65
#define FRAME_MS 16
#define CURSOR_RESTORE_MS 160
#define RESULT_ROW 6
#define EXCERPT_ROWS 4
#define RESULT_HEIGHT (EXCERPT_ROWS + 2)
#define EXCERPT_BYTES 8192

extern char **environ;

typedef struct {
    wchar_t chars[QUERY_CHARS];
    size_t length;
} Pattern;

typedef struct {
    char *path;
    char *text;
    size_t hit;                    /* byte offset in the excerpt */
    size_t hit_length;
    unsigned long line;
    unsigned long column;           /* one-based byte column in the file */
    int cropped;
} Match;

typedef struct {
    Match *items;
    size_t count, capacity;
    unsigned long generation;
    Pattern pattern;
    unsigned int unreadable;
    char error[128];
} Results;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    atomic_ulong generation;
    atomic_int quit;
    unsigned long requested;
    unsigned long failed;
    int pending;
    char query[QUERY_BYTES];
    char root[PATH_MAX];
    Results *ready;
} Search;

typedef struct {
    wchar_t query[QUERY_CHARS];
    size_t length, cursor;
    char bytes[QUERY_BYTES];
    Results *results;
    size_t selected, top;
    int scheduled, waiting, open_when_ready;
    int64_t due;
    int64_t cursor_hidden_until;
    char message[160];
} View;

static volatile sig_atomic_t quitting;
static attr_t accent, muted, highlight;
static void request_quit(int sig) { (void)sig; quitting = 1; }

static size_t decode(const char *text, wchar_t *wc)
{
    if ((unsigned char)*text < 128) { *wc = (unsigned char)*text; return 1; }
    mbstate_t state = {0};
    size_t n = mbrtowc(wc, text, MB_CUR_MAX, &state);
    if (n == (size_t)-1 || n == (size_t)-2) { *wc = L'\ufffd'; return 1; }
    return n ? n : 1;
}

static Pattern make_pattern(const char *query)
{
    Pattern p = {0};
    while (*query && p.length < QUERY_CHARS) {
        wchar_t wc; query += decode(query, &wc);
        p.chars[p.length++] = towlower(wc);
    }
    return p;
}

static const char *find_phrase(const char *text, const Pattern *p, size_t *length)
{
    if (!p->length) return NULL;
    while (*text) {
        wchar_t wc;
        size_t step = decode(text, &wc);
        if (towlower(wc) == (wint_t)p->chars[0]) {
            const char *end = text + step;
            size_t i = 1;
            while (i < p->length && *end) {
                size_t n = decode(end, &wc);
                if (towlower(wc) != (wint_t)p->chars[i]) break;
                end += n; i++;
            }
            if (i == p->length) { *length = (size_t)(end - text); return text; }
        }
        text += step;
    }
    return NULL;
}

static void free_results(Results *r)
{
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) { free(r->items[i].path); free(r->items[i].text); }
    free(r->items); free(r);
}

static int cancelled(Search *s, const Results *r)
{
    return atomic_load(&s->quit) || atomic_load(&s->generation) != r->generation;
}

static int add_match(Results *r, const char *path, const char *line,
                     unsigned long number, const char *hit, size_t length)
{
    if (r->count == r->capacity) {
        size_t next = r->capacity ? r->capacity * 2 : 128;
        Match *items = realloc(r->items, next * sizeof *items);
        if (!items) return 0;
        r->items = items; r->capacity = next;
    }
    const char *start = hit - line > 160 ? hit - 160 : line;
    while (((unsigned char)*start & 0xc0) == 0x80) start++;
    size_t available = strlen(start);
    size_t keep = (size_t)(hit - start) + length + 2048;
    if (keep > available) keep = available;
    while (keep && ((unsigned char)start[keep] & 0xc0) == 0x80) keep--;
    Match m = {.line = number, .column = (unsigned long)(hit - line) + 1,
               .hit = (size_t)(hit - start), .hit_length = length, .cropped = start != line};
    m.path = strdup(path); m.text = strndup(start, keep);
    if (!m.path || !m.text) { free(m.path); free(m.text); return 0; }
    r->items[r->count++] = m;
    return 1;
}

static int add_context(Match *m, const char *line, int before)
{
    size_t old = strlen(m->text), length = strlen(line);
    if (old + 1 >= EXCERPT_BYTES) return 1;
    if (length > EXCERPT_BYTES - old - 2) length = EXCERPT_BYTES - old - 2;
    while (length && ((unsigned char)line[length] & 0xc0) == 0x80) length--;
    char *text = realloc(m->text, old + length + 2);
    if (!text) return 0;
    m->text = text;
    if (before) {
        memmove(text + length + 1, text, old + 1);
        memcpy(text, line, length); text[length] = '\n';
        m->hit += length + 1;
    } else {
        text[old] = '\n'; memcpy(text + old + 1, line, length);
        text[old + length + 1] = 0;
    }
    return 1;
}

static void scan_file(Search *s, Results *r, const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) { r->unreadable++; return; }
    unsigned char probe[8192];
    size_t n = fread(probe, 1, sizeof probe, file);
    if (memchr(probe, 0, n)) { fclose(file); return; }
    rewind(file);
    char *line = NULL; size_t capacity = 0; ssize_t bytes;
    unsigned long number = 0; size_t before = r->count;
    char previous[161] = {0}; int previous_cropped = 0;
    while (!cancelled(s, r) && !*r->error && (bytes = getline(&line, &capacity, file)) >= 0) {
        number++;
        if (memchr(line, 0, (size_t)bytes)) {
            /* A late NUL makes the entire file binary, including earlier lines. */
            while (r->count > before) {
                Match *m = &r->items[--r->count]; free(m->path); free(m->text);
            }
            break;
        }
        while (bytes && (line[bytes - 1] == '\n' || line[bytes - 1] == '\r')) line[--bytes] = 0;
        /* Complete preceding hits with the next two source lines. */
        for (size_t i = r->count; i > before; ) {
            Match *m = &r->items[--i];
            if (number - m->line > 2) break;
            if (!add_context(m, line, 0))
                snprintf(r->error, sizeof r->error, "Not enough memory to finish searching");
        }
        size_t length;
        const char *hit = find_phrase(line, &r->pattern, &length);
        if (hit) {
            if (!add_match(r, path, line, number, hit, length))
                snprintf(r->error, sizeof r->error, "Not enough memory to finish searching");
            else {
                Match *m = &r->items[r->count - 1];
                if (number > 1 && !m->cropped) {
                    if (!add_context(m, previous, 1))
                        snprintf(r->error, sizeof r->error, "Not enough memory to finish searching");
                    m->cropped = previous_cropped;
                }
            }
        }
        const char *tail = bytes > 160 ? line + bytes - 160 : line;
        while (((unsigned char)*tail & 0xc0) == 0x80) tail++;
        snprintf(previous, sizeof previous, "%s", tail);
        previous_cropped = tail != line;
    }
    if (ferror(file)) r->unreadable++;
    free(line); fclose(file);
}

typedef struct {
    struct dirent *entry;
    int priority;
    struct timespec modified;
} RootEntry;

static int compare_root_entries(const void *a, const void *b)
{
    const RootEntry *left = a, *right = b;
    if (left->priority != right->priority)
        return left->priority < right->priority ? -1 : 1;
    if (left->modified.tv_sec != right->modified.tv_sec)
        return left->modified.tv_sec < right->modified.tv_sec ? -1 : 1;
    if (left->modified.tv_nsec != right->modified.tv_nsec)
        return left->modified.tv_nsec < right->modified.tv_nsec ? -1 : 1;
    return strcmp(left->entry->d_name, right->entry->d_name);
}

static void scan_tree(Search *s, Results *r, const char *path, unsigned int depth)
{
    if (cancelled(s, r) || *r->error) return;
    struct stat st;
    if (lstat(path, &st)) { r->unreadable++; return; }
    if (S_ISREG(st.st_mode)) { scan_file(s, r, path); return; }
    if (!S_ISDIR(st.st_mode) || depth > 128) return;
    struct dirent **entries;
    int count = scandir(path, &entries, NULL, alphasort);
    if (count < 0) { r->unreadable++; return; }
    /* Keep the two primary writing folders first, then oldest to newest.
       Descendants retain their filename order (including year/date names). */
    if (!depth && count) {
        RootEntry *ordered = calloc((size_t)count, sizeof *ordered);
        if (!ordered) {
            for (int i = 0; i < count; i++) free(entries[i]);
            free(entries);
            snprintf(r->error, sizeof r->error, "Not enough memory to order folders");
            return;
        }
        for (int i = 0; i < count; i++) {
            ordered[i].entry = entries[i];
            ordered[i].priority = 2;
            char child[PATH_MAX];
            int n = snprintf(child, sizeof child, "%s/%s", path, entries[i]->d_name);
            if (n > 0 && (size_t)n < sizeof child && !lstat(child, &st)) {
#ifdef __APPLE__
                ordered[i].modified = st.st_mtimespec;
#else
                ordered[i].modified = st.st_mtim;
#endif
                if (S_ISDIR(st.st_mode)) {
                    if (!strcmp(entries[i]->d_name, "moleskin")) ordered[i].priority = 0;
                    else if (!strcmp(entries[i]->d_name, "notes")) ordered[i].priority = 1;
                }
            }
        }
        qsort(ordered, (size_t)count, sizeof *ordered, compare_root_entries);
        for (int i = 0; i < count; i++) entries[i] = ordered[i].entry;
        free(ordered);
    }
    for (int i = 0; i < count; i++) {
        const char *name = entries[i]->d_name;
        if (!cancelled(s, r) && strcmp(name, ".") && strcmp(name, "..") && strcmp(name, ".git")) {
            char child[PATH_MAX];
            int n = snprintf(child, sizeof child, "%s/%s", path, name);
            if (n > 0 && (size_t)n < sizeof child) scan_tree(s, r, child, depth + 1);
        }
        free(entries[i]);
    }
    free(entries);
}

/* This worker never touches curses, stdout, or terminal signal handlers. */
static void *search_worker(void *arg)
{
    Search *s = arg;
    pthread_mutex_lock(&s->mutex);
    while (!atomic_load(&s->quit)) {
        while (!s->pending && !atomic_load(&s->quit)) pthread_cond_wait(&s->wake, &s->mutex);
        if (atomic_load(&s->quit)) break;
        char query[QUERY_BYTES]; memcpy(query, s->query, sizeof query);
        unsigned long generation = s->requested;
        s->pending = 0;
        pthread_mutex_unlock(&s->mutex);
        Results *r = calloc(1, sizeof *r);
        if (r) {
            r->generation = generation; r->pattern = make_pattern(query);
            if (r->pattern.length) scan_tree(s, r, s->root, 0);
        }
        pthread_mutex_lock(&s->mutex);
        if (r && !cancelled(s, r)) { free_results(s->ready); s->ready = r; }
        else free_results(r);
        if (!r) s->failed = generation;
    }
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

static void submit(Search *s, View *v)
{
    pthread_mutex_lock(&s->mutex);
    memcpy(s->query, v->bytes, sizeof s->query);
    s->requested = atomic_load(&s->generation); s->pending = 1;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->mutex);
    v->scheduled = 0; v->waiting = 1;
}

static int take_results(Search *s, View *v)
{
    pthread_mutex_lock(&s->mutex);
    Results *r = s->ready; s->ready = NULL;
    unsigned long failed = s->failed; s->failed = 0;
    pthread_mutex_unlock(&s->mutex);
    if (failed && failed == atomic_load(&s->generation)) {
        free_results(r);
        v->waiting = v->open_when_ready = 0;
        snprintf(v->message, sizeof v->message, "Not enough memory to search");
        return 1;
    }
    if (!r) return 0;
    if (r->generation != atomic_load(&s->generation)) { free_results(r); return 0; }
    size_t selected = 0;
    if (v->results && v->results->count) {
        Match *old = &v->results->items[v->selected];
        for (size_t i = 0; i < r->count; i++)
            if (r->items[i].line == old->line && !strcmp(r->items[i].path, old->path)) { selected = i; break; }
    }
    free_results(v->results); v->results = r; v->selected = selected;
    v->waiting = 0;
    return 1;
}

static void query_changed(Search *s, View *v)
{
    v->cursor_hidden_until = 0;
    wcstombs(v->bytes, v->query, sizeof v->bytes - 1);
    v->bytes[sizeof v->bytes - 1] = 0;
    atomic_fetch_add(&s->generation, 1);
    v->open_when_ready = 0; v->message[0] = 0;
    v->due = sui_monotonic_ms() + DEBOUNCE_MS;
    v->scheduled = v->length != 0; v->waiting = v->length != 0;
    if (!v->length) { free_results(v->results); v->results = NULL; v->selected = v->top = 0; }
}

static int glyph_width(wchar_t wc) { int n = wcwidth(wc); return n < 0 ? 1 : n; }

static int text_width(const char *start, const char *end)
{
    int width = 0;
    while (start < end && *start) { wchar_t wc; start += decode(start, &wc); width += glyph_width(wc); }
    return width;
}

static int draw_text(int row, int col, const char *text, int width, attr_t attr)
{
    if (width <= 0) return 0;
    int used = 0; move(row, col); attrset(attr);
    while (*text) {
        wchar_t wc; size_t n = decode(text, &wc);
        int cells = glyph_width(wc);
        if (used + cells > width) break;
        if (wcwidth(wc) < 0) wc = L' ';
        wchar_t str[] = {wc, 0}; addwstr(str);
        used += cells; text += n;
    }
    attrset(A_NORMAL); return used;
}

static const char *wrap_end(const char *text, int width)
{
    const char *start = text, *space = NULL;
    int used = 0;
    while (*text && *text != '\n') {
        wchar_t wc; size_t n = decode(text, &wc);
        int cells = glyph_width(wc);
        if (used + cells > width)
            return space ? space : text == start ? text + n : text;
        used += cells; text += n;
        if (iswspace(wc)) space = text;
    }
    return text;
}

static void draw_excerpt(int row, int col, int width, int height, const Match *m, const Pattern *p)
{
    if (width < 4 || height < 1) return;
    const char *start = m->text;
    int context = text_width(start, start + m->hit);
    int before = width / 2;
    while (context > before && start < m->text + m->hit) {
        wchar_t wc; size_t n = decode(start, &wc); context -= glyph_width(wc); start += n;
    }
    if (start != m->text) {
        /* Begin a cropped excerpt on a word boundary when possible. */
        while (start < m->text + m->hit) {
            wchar_t wc; size_t n = decode(start, &wc);
            if (iswspace(wc)) break;
            start += n;
        }
    }
    while (start < m->text + m->hit && isspace((unsigned char)*start)) start++;
    size_t length = 0;
    const char *hit = find_phrase(start, p, &length);
    const char *end = hit ? hit + length : NULL;
    const char *text = start;
    for (int i = 0; i < height && *text; i++) {
        int used = 0;
        if (!i && (m->cropped || start != m->text))
            used = draw_text(row, col, "… ", width, muted);
        int limit = width - used - (i == height - 1);
        const char *stop = wrap_end(text, limit);
        move(row + i, col + used);
        while (text < stop) {
            if (end && text >= end) { hit = find_phrase(text, p, &length); end = hit ? hit + length : NULL; }
            wchar_t wc; size_t n = decode(text, &wc);
            attrset(hit && text >= hit && text < end ? highlight : A_NORMAL);
            if (wcwidth(wc) < 0) wc = L' ';
            wchar_t str[] = {wc, 0}; addwstr(str);
            used += glyph_width(wc); text += n;
        }
        if (*text == '\n') text++;
        if (i == height - 1 && *text)
            draw_text(row + i, col + used, "…", width - used, muted);
    }
    attrset(A_NORMAL);
}

static int visible_results(int rows)
{
    int visible = (rows - RESULT_ROW - 2) / RESULT_HEIGHT;
    return visible > 0 ? visible : 1;
}

static void draw_view(View *v, const char *root)
{
    /* The hardware cursor must never expose intermediate paint positions. */
    curs_set(0);
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int left = cols > 112 ? (cols - 108) / 2 : 2;
    int width = cols - left * 2;
    erase();
    if (rows < 9 || width < 20) {
        draw_text(0, 0, "simplesearch", cols, A_BOLD);
        if (rows > 1) draw_text(1, 0, "Resize terminal to search", cols, A_NORMAL);
        wnoutrefresh(stdscr); doupdate(); return;
    }
    draw_text(0, left, "simplesearch", width, A_BOLD);
    const char *domain = "~/writing";
    draw_text(0, left + width - (int)strlen(domain), domain, (int)strlen(domain), muted);
    draw_text(2, left, "> ", width, accent);
    int query_width = width - 3;
    size_t start = 0; int cursor_col = 0;
    for (size_t i = 0; i < v->cursor; i++) cursor_col += glyph_width(v->query[i]);
    while (cursor_col >= query_width && start < v->cursor) cursor_col -= glyph_width(v->query[start++]);
    char query[QUERY_BYTES]; wcstombs(query, v->query + start, sizeof query);
    draw_text(2, left + 2, query, query_width, A_NORMAL);
    char label[240];
    size_t count = v->results ? v->results->count : 0;
    if (*v->message) snprintf(label, sizeof label, "%s", v->message);
    else if (v->results && *v->results->error) snprintf(label, sizeof label, "%s", v->results->error);
    else if (v->waiting) snprintf(label, sizeof label, "Searching…");
    else if (count) snprintf(label, sizeof label, "%zu %s  ·  %zu/%zu%s", count, count == 1 ? "match" : "matches", v->selected + 1, count,
                            v->results->unreadable ? "  ·  some files unreadable" : "");
    else snprintf(label, sizeof label, "%s", v->length ? "No matches" : "Search inside your writing");
    draw_text(4, left, label, width, muted);
    int visible = visible_results(rows);
    if (v->selected < v->top) v->top = v->selected;
    if (v->selected >= v->top + (size_t)visible) v->top = v->selected - visible + 1;
    if (count <= (size_t)visible) v->top = 0;
    for (int i = 0; i < visible && v->top + (size_t)i < count; i++) {
        size_t index = v->top + (size_t)i;
        int y = RESULT_ROW + i * RESULT_HEIGHT;
        if (y + 1 >= rows - 2) break;
        Match *m = &v->results->items[index];
        char suffix[48]; snprintf(suffix, sizeof suffix, " :%lu", m->line);
        const char *path = m->path + strlen(root) + 1;
        int suffix_width = (int)strlen(suffix);
        int path_width = width - 3 - suffix_width;
        if (index == v->selected) draw_text(y, left, "›", 1, accent);
        int drawn = draw_text(y, left + 2, path, path_width, index == v->selected ? accent : muted);
        draw_text(y, left + 2 + drawn, suffix, suffix_width, muted);
        int height = rows - 2 - (y + 1);
        if (height > EXCERPT_ROWS) height = EXCERPT_ROWS;
        draw_excerpt(y + 1, left + 2, width - 2, height, m, &v->results->pattern);
    }
    const char *help = width >= 82 ? "↑↓ move   PgUp/PgDn page   Enter open   Ctrl-U clear   Ctrl-R refresh   Esc quit" :
                       width >= 50 ? "↑↓ move  Enter open  Ctrl-U clear  Esc quit" : "↑↓ move  Enter open  Esc quit";
    draw_text(rows - 1, left, help, width, muted);
    move(2, left + 2 + cursor_col);
    /* Prevent ncurses from moving whole terminal rows to reuse old results.
     * Invalidate only the unused padding column, as SimpleNote does for its
     * panes. The prompt, footer, and unchanged excerpts keep their pixels. */
    WINDOW *padding = derwin(stdscr, rows, 1, 0, cols - 1);
    if (padding) { wredrawln(padding, 0, rows); delwin(padding); }
    wnoutrefresh(stdscr); doupdate();
    if (!v->cursor_hidden_until) curs_set(1);
}

static int open_match(const Match *m)
{
    char location[96];
    snprintf(location, sizeof location, "%lu:%lu:%zu", m->line, m->column, m->hit_length);
    char *args[] = {"simplewords", "--at", location, "--", m->path, NULL};
    /* Keep the editor in this terminal so exiting it returns to these results,
     * even if a different terminal already owns the SimpleWords workspace. */
    size_t count = 0;
    while (environ[count]) count++;
    char **environment = malloc((count + 2) * sizeof *environment);
    if (!environment) return 0;
    size_t used = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(environ[i], "SIMPLEWORDS_SEARCH_SESSION=",
                    sizeof "SIMPLEWORDS_SEARCH_SESSION=" - 1) != 0)
            environment[used++] = environ[i];
    environment[used++] = "SIMPLEWORDS_SEARCH_SESSION=1";
    environment[used] = NULL;
    def_prog_mode(); endwin(); fflush(stdout);
    pid_t child;
    int error = posix_spawnp(&child, args[0], NULL, NULL, args, environment);
    free(environment);
    int result = 0;
    if (!error) while (waitpid(child, &result, 0) < 0) { if (errno != EINTR) { error = errno; break; } }
    reset_prog_mode(); clearok(stdscr, TRUE);
    return !error && WIFEXITED(result) && WEXITSTATUS(result) == 0;
}

static void move_selection(View *v, long delta)
{
    size_t count = v->results ? v->results->count : 0;
    if (!count) return;
    v->cursor_hidden_until = sui_monotonic_ms() + CURSOR_RESTORE_MS;
    if (delta < 0) v->selected = v->selected < (size_t)-delta ? 0 : v->selected - (size_t)-delta;
    else v->selected = v->selected + (size_t)delta < count ? v->selected + (size_t)delta : count - 1;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    if (argc > 1) {
        printf("Usage: %s\nLive, case-insensitive phrase search in ~/writing.\nEnter opens SimpleWords at the highlighted occurrence.\n", argv[0]);
        return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 1;
    }
    Search s = {.mutex = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};
    const char *home = getenv("HOME");
    if (!home || snprintf(s.root, sizeof s.root, "%s/writing", home) >= (int)sizeof s.root) return 1;
    struct stat st;
    if (stat(s.root, &st) || !S_ISDIR(st.st_mode) || access(s.root, R_OK | X_OK)) {
        fprintf(stderr, "simplesearch: cannot read %s\n", s.root); return 1;
    }
    int error = pthread_create(&s.thread, NULL, search_worker, &s);
    if (error) { fprintf(stderr, "simplesearch: %s\n", strerror(error)); return 1; }
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); set_escdelay(25);
    intrflush(stdscr, FALSE); scrollok(stdscr, FALSE); idlok(stdscr, FALSE); idcok(stdscr, FALSE);
    leaveok(stdscr, FALSE);
    /* Finish each frame, including the final prompt cursor move, even when
     * held navigation keys are waiting. Input is batched between frames. */
    typeahead(-1);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(1, COLOR_CYAN, -1); init_pair(2, COLOR_YELLOW, -1);
        accent = A_BOLD | COLOR_PAIR(1); highlight = A_BOLD | A_UNDERLINE | COLOR_PAIR(2);
    } else { accent = A_BOLD; highlight = A_BOLD | A_REVERSE; }
    muted = A_DIM;
    mouseinterval(180);
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON4_PRESSED
#ifdef BUTTON5_PRESSED
              | BUTTON5_PRESSED
#endif
              , NULL);
    signal(SIGINT, request_quit); signal(SIGTERM, request_quit);
    View v = {0}; int dirty = 1; int64_t last_draw = 0;
    while (!quitting) {
        int64_t now = sui_monotonic_ms();
        if (v.cursor_hidden_until && now >= v.cursor_hidden_until) {
            v.cursor_hidden_until = 0;
            dirty = 1;
        }
        if (v.scheduled && sui_monotonic_ms() >= v.due) submit(&s, &v);
        if (take_results(&s, &v)) dirty = 1;
        if (v.open_when_ready && !v.waiting) {
            v.open_when_ready = 0;
            if (v.results && v.results->count) {
                if (open_match(&v.results->items[v.selected])) {
                    query_changed(&s, &v); if (v.length) submit(&s, &v);
                } else snprintf(v.message, sizeof v.message, "Could not open SimpleWords");
                dirty = 1;
            }
        }
        if (dirty && now >= last_draw + FRAME_MS) {
            draw_view(&v, s.root); dirty = 0;
            last_draw = sui_monotonic_ms();
        }
        int wait = v.waiting ? FRAME_MS : 100;
        if (dirty) wait = sui_ms_until(last_draw + FRAME_MS, wait);
        if (v.cursor_hidden_until) {
            int remaining = sui_ms_until(v.cursor_hidden_until, wait);
            if (remaining < wait) wait = remaining;
        }
        timeout(wait);
        wint_t key; int kind = get_wch(&key);
        if (kind == ERR) continue;
        int changed = 0, enter = 0;
        dirty = 1;
        if (kind == KEY_CODE_YES) {
            if (key == KEY_UP) move_selection(&v, -1);
            else if (key == KEY_DOWN) move_selection(&v, 1);
            else if (key == KEY_PPAGE) move_selection(&v, -visible_results(LINES));
            else if (key == KEY_NPAGE) move_selection(&v, visible_results(LINES));
            else if (key == KEY_LEFT) { if (v.cursor) v.cursor--; v.cursor_hidden_until = 0; }
            else if (key == KEY_RIGHT) { if (v.cursor < v.length) v.cursor++; v.cursor_hidden_until = 0; }
            else if (key == KEY_HOME) { v.cursor = 0; v.cursor_hidden_until = 0; }
            else if (key == KEY_END) { v.cursor = v.length; v.cursor_hidden_until = 0; }
            else if (key == KEY_BACKSPACE) key = 127, kind = OK;
            else if (key == KEY_DC && v.cursor < v.length) {
                memmove(v.query + v.cursor, v.query + v.cursor + 1, (v.length - v.cursor) * sizeof *v.query);
                v.length--; changed = 1;
            } else if (key == KEY_ENTER) enter = 1;
            else if (key == KEY_MOUSE) {
                MEVENT event;
                if (getmouse(&event) == OK) {
                    if (event.bstate & BUTTON4_PRESSED) move_selection(&v, -3);
#ifdef BUTTON5_PRESSED
                    else if (event.bstate & BUTTON5_PRESSED) move_selection(&v, 3);
#endif
                    else if (event.y >= RESULT_ROW && event.y < LINES - 2 && v.results) {
                        size_t index = v.top + (event.y - RESULT_ROW) / RESULT_HEIGHT;
                        if (index < v.results->count) {
                            v.selected = index;
                            v.cursor_hidden_until = sui_monotonic_ms() + CURSOR_RESTORE_MS;
                            enter = !!(event.bstate & BUTTON1_DOUBLE_CLICKED);
                        }
                    }
                }
            }
        }
        if (kind != KEY_CODE_YES) {
            if (key == 27 || key == 3 || key == 4) break;
            else if (key == L'\n' || key == L'\r') enter = 1;
            else if (key == 21) { v.length = v.cursor = 0; v.query[0] = 0; changed = 1; }
            else if (key == 18) { query_changed(&s, &v); if (v.length) submit(&s, &v); }
            else if (key == 1) { v.cursor = 0; v.cursor_hidden_until = 0; }
            else if (key == 5) { v.cursor = v.length; v.cursor_hidden_until = 0; }
            else if (key == 14) move_selection(&v, 1);
            else if (key == 16) move_selection(&v, -1);
            else if ((key == 127 || key == 8) && v.cursor) {
                memmove(v.query + v.cursor - 1, v.query + v.cursor, (v.length - v.cursor + 1) * sizeof *v.query);
                v.cursor--; v.length--; changed = 1;
            } else if (key == 23 && v.cursor) {
                size_t end = v.cursor;
                while (v.cursor && iswspace(v.query[v.cursor - 1])) v.cursor--;
                while (v.cursor && !iswspace(v.query[v.cursor - 1])) v.cursor--;
                memmove(v.query + v.cursor, v.query + end, (v.length - end + 1) * sizeof *v.query);
                v.length -= end - v.cursor; changed = 1;
            } else if (iswprint(key) && v.length + 1 < QUERY_CHARS) {
                memmove(v.query + v.cursor + 1, v.query + v.cursor, (v.length - v.cursor + 1) * sizeof *v.query);
                v.query[v.cursor++] = key; v.length++; changed = 1;
            }
        }
        if (changed) query_changed(&s, &v);
        if (enter && v.length) { if (v.scheduled) submit(&s, &v); v.open_when_ready = 1; }
    }
    atomic_store(&s.quit, 1);
    pthread_mutex_lock(&s.mutex); pthread_cond_signal(&s.wake); pthread_mutex_unlock(&s.mutex);
    pthread_join(s.thread, NULL);
    free_results(s.ready); free_results(v.results);
    pthread_mutex_destroy(&s.mutex); pthread_cond_destroy(&s.wake);
    endwin(); return 0;
}
