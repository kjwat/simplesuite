#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#define _FILE_OFFSET_BITS 64
#include <ncurses.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "simpleproc.h"
#include "simpleui.h"

#define SC_VERSION "1.0.1"
#define SC_PATH 4096
#define SC_FORMATS 32
#define SC_FILES 4096
#define SC_LINE 32768

typedef enum { READY, RUNNING, DONE, FAILED, CANCELLED } JobState;
typedef enum { QUEUE, BROWSER, FORMATS, HELP, DETAILS, LOG_VIEW, INPUT } View;
typedef struct {
    char ext[12], category[20], label[160], reason[512];
    int enabled;
    uint32_t targets;
} Format;
typedef struct {
    char source[SC_PATH], output[SC_PATH], log[SC_PATH];
    char message[1024], route[256];
    int from, to, percent;
    int64_t started, elapsed;
    uint64_t bytes, output_bytes;
    JobState state;
} Job;
typedef struct { char *name; int directory; uint64_t bytes; } Entry;
typedef struct {
    Job *jobs;
    size_t count;
    int selected, top, active, batching, cancelling, got_done, got_error;
    pid_t worker;
    int output_fd;
    int64_t cancel_at;
    char protocol[SC_LINE];
    size_t protocol_used;
    Format formats[SC_FORMATS];
    int format_count, default_format;
    char engine[SC_PATH], output_dir[SC_PATH], message[1024];
    View view, before_input;
    int all_formats, format_category, format_selected, format_focus;
    char search[256];
    int searching;
    Entry *entries;
    size_t entry_count;
    char directory[SC_PATH];
    int browser_selected, browser_top, hidden;
    wchar_t input[SC_PATH];
    size_t input_length, input_cursor;
    int input_kind;
    char *log_text;
    int log_scroll;
    int quitting, quit_confirm;
} App;

static volatile sig_atomic_t stopped;
static void stop_signal(int sig) { stopped = sig; }
static const char *state_name(JobState state)
{
    static const char *names[] = {"READY", "CONVERTING", "DONE", "FAILED", "CANCELLED"};
    return names[state];
}
static void copy(char *dest, size_t size, const char *source)
{
    snprintf(dest, size, "%s", source ? source : "");
}
static void message(App *a, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(a->message, sizeof a->message, format, args);
    va_end(args);
}
static int join(char *dest, size_t size, const char *directory, const char *name)
{
    int n = snprintf(dest, size, "%s%s%s", directory, !strcmp(directory, "/") ? "" : "/", name);
    return n >= 0 && (size_t)n < size;
}
static const char *basename_of(const char *path)
{
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}
static void parent_of(char *path)
{
    char *last = strrchr(path, '/');
    if (!last || last == path) copy(path, SC_PATH, "/");
    else *last = 0;
}
static void size_text(uint64_t bytes, char *dest, size_t size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; unit++; }
    if (unit) snprintf(dest, size, "%.1f %s", value, units[unit]);
    else snprintf(dest, size, "%" PRIu64 " B", bytes);
}

/* Clip by terminal cells, keep UTF-8 whole, and never interpret controls. */
static void put(int y, int x, int width, const char *text, attr_t attr)
{
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || width <= 0) return;
    if (width > COLS - x - 1) width = COLS - x - 1;
    if (width <= 0) return;
    attron(attr);
    move(y, x);
    mbstate_t state = {0};
    int columns = 0;
    for (const char *p = text; *p;) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &state);
        if (n == (size_t)-1 || n == (size_t)-2) { wc = L'?'; n = 1; memset(&state, 0, sizeof state); }
        if (!n) break;
        if (iswcntrl(wc) || wc == 0x1b) wc = L'?';
        int cells = wcwidth(wc);
        if (cells < 0) { wc = L'?'; cells = 1; }
        if (columns + cells > width) break;
        wchar_t chars[2] = {wc, 0};
        addnwstr(chars, 1);
        columns += cells;
        p += n;
    }
    attroff(attr);
}
static void line(int y, int x, int width)
{
    if (y >= 0 && y < LINES && x >= 0 && x < COLS && width > 0)
        mvhline(y, x, ACS_HLINE, width < COLS - x ? width : COLS - x - 1);
}
static void frame(const char *title, const char *hint)
{
    erase();
    box(stdscr, 0, 0);
    put(0, 2, COLS - 6, title, A_BOLD);
    if (hint) put(LINES - 2, 2, COLS - 4, hint, A_DIM);
}
static int format_index(const App *a, const char *ext)
{
    for (int i = 0; i < a->format_count; i++) if (!strcasecmp(a->formats[i].ext, ext)) return i;
    return -1;
}
static int path_format(const App *a, const char *path)
{
    const char *ext = strrchr(basename_of(path), '.');
    if (!ext) return -1;
    ext++;
    if (!strcasecmp(ext, "htm") || !strcasecmp(ext, "xhtml")) ext = "html";
    else if (!strcasecmp(ext, "text")) ext = "txt";
    else if (!strcasecmp(ext, "markdown")) ext = "md";
    else if (!strcasecmp(ext, "azw") || !strcasecmp(ext, "prc")) ext = "mobi";
    else if (!strcasecmp(ext, "djv")) ext = "djvu";
    return format_index(a, ext);
}
static int supported(const App *a, int from, int to)
{
    return from >= 0 && to >= 0 && from < a->format_count && to < a->format_count &&
           (a->formats[from].targets & ((uint32_t)1 << to)) != 0;
}
static char *field(char **remaining)
{
    char *start = *remaining;
    if (!start) return "";
    char *end = strchr(start, '\t');
    if (end) { *end = 0; *remaining = end + 1; }
    else *remaining = NULL;
    return start;
}
static int load_catalog(App *a)
{
    char *args[] = {a->engine, "--catalog-all", NULL}, *output = NULL;
    if (!ssp_capture_argv(args, &output, 262144, 5000)) return 0;
    char *save = NULL;
    for (char *row = strtok_r(output, "\n", &save); row; row = strtok_r(NULL, "\n", &save)) {
        char *rest = row;
        char *kind = field(&rest), *ext = field(&rest);
        if (!strcmp(kind, "FORMAT") && a->format_count < SC_FORMATS) {
            Format *f = &a->formats[a->format_count++];
            copy(f->ext, sizeof f->ext, ext);
            copy(f->category, sizeof f->category, field(&rest));
            f->enabled = atoi(field(&rest));
            copy(f->label, sizeof f->label, field(&rest));
            copy(f->reason, sizeof f->reason, field(&rest));
        } else if (!strcmp(kind, "MATRIX")) {
            int index = format_index(a, ext);
            if (index >= 0) a->formats[index].targets = (uint32_t)strtoul(field(&rest), NULL, 10);
        }
    }
    free(output);
    return a->format_count > 0;
}
static int find_engine(char *dest, size_t size, const char *argv0)
{
    char path[SC_PATH] = "";
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n > 0) path[n] = 0;
#elif defined(__APPLE__)
    uint32_t length = sizeof path;
    if (_NSGetExecutablePath(path, &length) != 0) path[0] = 0;
#endif
    if (!path[0] && strchr(argv0, '/')) {
        if (!realpath(argv0, path)) path[0] = 0;
    }
    if (path[0]) {
        parent_of(path);
        if (join(dest, size, path, "simplechange-engine") && access(dest, X_OK) == 0) return 1;
    }
    const char *search = getenv("PATH");
    if (!search) search = "/usr/local/bin:/usr/bin:/bin";
    char *paths = strdup(search), *save = NULL;
    if (!paths) return 0;
    for (char *dir = strtok_r(paths, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (join(dest, size, dir, "simplechange-engine") && access(dest, X_OK) == 0) {
            free(paths); return 1;
        }
    }
    free(paths);
    return 0;
}
static void percent_decode(char *dest, size_t size, const char *source)
{
    size_t used = 0;
    for (const char *p = source; *p && used + 1 < size; p++) {
        if (*p == '%' && p[1] && p[2] && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            unsigned long value = strtoul(hex, NULL, 16);
            if (!value) break;
            dest[used++] = (char)value; p += 2;
        } else dest[used++] = *p;
    }
    dest[used] = 0;
}
static int add_file(App *a, const char *path)
{
    char resolved[SC_PATH];
    struct stat st;
    if (!realpath(path, resolved) || stat(resolved, &st) || !S_ISREG(st.st_mode)) {
        message(a, "Cannot read file: %s", path); return 0;
    }
    int from = path_format(a, resolved);
    if (from < 0) { message(a, "Unsupported file extension: %s", basename_of(path)); return 0; }
    for (size_t i = 0; i < a->count; i++) if (!strcmp(a->jobs[i].source, resolved)) {
        a->selected = (int)i; message(a, "Already in the queue: %s", basename_of(path)); return 0;
    }
    if (a->count >= SC_FILES) { message(a, "Queue is full (%d files).", SC_FILES); return 0; }
    Job *grown = realloc(a->jobs, (a->count + 1) * sizeof(*grown));
    if (!grown) { message(a, "Cannot add file: out of memory."); return 0; }
    a->jobs = grown;
    Job *job = &a->jobs[a->count];
    memset(job, 0, sizeof(*job));
    copy(job->source, sizeof job->source, resolved);
    job->from = from;
    job->to = supported(a, from, a->default_format) ? a->default_format : -1;
    job->bytes = (uint64_t)st.st_size;
    copy(job->message, sizeof job->message, job->to >= 0 ? "Ready to convert." : "Press f to choose an output format.");
    a->selected = (int)a->count++;
    message(a, "Added %s", basename_of(path));
    return 1;
}
static void free_entries(App *a)
{
    for (size_t i = 0; i < a->entry_count; i++) free(a->entries[i].name);
    free(a->entries); a->entries = NULL; a->entry_count = 0;
}
static int entry_compare(const void *left, const void *right)
{
    const Entry *a = left, *b = right;
    if (a->directory != b->directory) return b->directory - a->directory;
    return strcasecmp(a->name, b->name);
}
static int contains(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 1;
    for (const char *p = haystack; *p; p++) if (!strncasecmp(p, needle, n)) return 1;
    return 0;
}
static void scan_directory(App *a)
{
    free_entries(a);
    a->browser_selected = a->browser_top = 0;
    DIR *directory = opendir(a->directory);
    if (!directory) { message(a, "Cannot open directory: %s", strerror(errno)); return; }
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if ((!a->hidden && entry->d_name[0] == '.') || !contains(entry->d_name, a->search)) continue;
        char path[SC_PATH]; struct stat st;
        if (!join(path, sizeof path, a->directory, entry->d_name) || stat(path, &st)) continue;
        int dir = S_ISDIR(st.st_mode);
        if (!dir && (!S_ISREG(st.st_mode) || path_format(a, entry->d_name) < 0)) continue;
        if (a->entry_count >= 32768) { message(a, "Directory is large; type / to narrow the list."); break; }
        Entry *grown = realloc(a->entries, (a->entry_count + 1) * sizeof(*grown));
        if (!grown) break;
        a->entries = grown;
        Entry *item = &a->entries[a->entry_count];
        item->name = strdup(entry->d_name);
        if (!item->name) break;
        item->directory = dir; item->bytes = (uint64_t)st.st_size;
        a->entry_count++;
    }
    closedir(directory);
    if (a->entry_count) qsort(a->entries, a->entry_count, sizeof(*a->entries), entry_compare);
}
static void browser_open(App *a)
{
    a->view = BROWSER; a->search[0] = 0; a->searching = 0;
    scan_directory(a);
    message(a, "Enter opens a folder or adds a file. Space adds more files.");
}
static void input_open(App *a, int kind)
{
    a->input_kind = kind; a->before_input = a->view; a->view = INPUT;
    const char *initial = kind == 1 ? a->output_dir : a->directory;
    size_t n = mbstowcs(a->input, initial, SC_PATH - 1);
    if (n == (size_t)-1) n = 0;
    a->input[n] = 0; a->input_length = a->input_cursor = n;
}
static void input_accept(App *a)
{
    char text[SC_PATH], expanded[SC_PATH], resolved[SC_PATH];
    size_t n = wcstombs(text, a->input, sizeof text - 1);
    if (n == (size_t)-1 || n >= sizeof text - 1) { message(a, "The path is too long or cannot be encoded."); return; }
    text[n] = 0;
    if (a->input_kind == 1 && !text[0]) {
        a->output_dir[0] = 0; a->view = a->before_input;
        message(a, "Outputs will be saved beside each original."); return;
    }
    const char *path = text;
    if (text[0] == '~' && (!text[1] || text[1] == '/')) {
        const char *home = getenv("HOME");
        int written = snprintf(expanded, sizeof expanded, "%s%s", home ? home : "", text + 1);
        if (!home || written < 0 || (size_t)written >= sizeof expanded) { message(a, "Cannot expand this home path."); return; }
        path = expanded;
    }
    struct stat st;
    if (!realpath(path, resolved) || stat(resolved, &st) || !S_ISDIR(st.st_mode)) {
        message(a, "Choose an existing directory."); return;
    }
    if (a->input_kind == 1) {
        if (access(resolved, W_OK | X_OK)) { message(a, "The output directory is not writable."); return; }
        copy(a->output_dir, sizeof a->output_dir, resolved);
        message(a, "Output directory changed.");
    } else {
        copy(a->directory, sizeof a->directory, resolved);
        a->search[0] = 0; scan_directory(a);
    }
    a->view = a->before_input;
}
static int format_visible(const App *a, int *list)
{
    int count = 0;
    for (int i = 0; i < a->format_count; i++) {
        Format const *f = &a->formats[i];
        if (!a->all_formats && a->count && i == a->jobs[a->selected].from) continue;
        if (a->search[0]) {
            if (!contains(f->ext, a->search) && !contains(f->label, a->search)) continue;
        } else if (strcmp(f->category, a->format_category ? "EBook" : "Document")) continue;
        list[count++] = i;
    }
    return count;
}
static void format_open(App *a, int all)
{
    if (!a->count) { message(a, "Add files with a first."); return; }
    if (!all && a->jobs[a->selected].state == RUNNING) { message(a, "Cancel this conversion before changing its format."); return; }
    a->view = FORMATS; a->all_formats = all; a->search[0] = 0;
    a->format_focus = 1; a->format_selected = 0;
    int target = a->jobs[a->selected].to;
    a->format_category = target >= 0 && !strcmp(a->formats[target].category, "EBook");
    int list[SC_FORMATS], n = format_visible(a, list);
    for (int i = 0; i < n; i++) if (list[i] == target) a->format_selected = i;
}
static void format_choose(App *a)
{
    int list[SC_FORMATS], n = format_visible(a, list);
    if (!n || a->format_selected >= n) return;
    int target = list[a->format_selected], changed = 0, skipped = 0;
    for (size_t i = 0; i < a->count; i++) {
        if (!a->all_formats && (int)i != a->selected) continue;
        Job *job = &a->jobs[i];
        if (job->state == RUNNING || (a->all_formats && job->state == DONE)) continue;
        if (!supported(a, job->from, target)) { skipped++; continue; }
        job->to = target; job->state = READY; job->percent = 0;
        job->output[0] = job->log[0] = job->route[0] = 0;
        copy(job->message, sizeof job->message, "Ready to convert."); changed++;
    }
    if (!changed) { message(a, "This format is unavailable for the selected input. Check engines with --check."); return; }
    a->default_format = target; a->view = QUEUE;
    message(a, "Format set for %d file%s.%s", changed, changed == 1 ? "" : "s", skipped ? " Some inputs cannot use this format." : "");
}
static int start_worker(App *a, int index)
{
    int pipefd[2];
    if (pipe(pipefd)) { message(a, "Cannot start converter: %s", strerror(errno)); return 0; }
    Job *job = &a->jobs[index];
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGHUP, SIG_DFL);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO); close(pipefd[1]);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
        char *args[12]; int n = 0;
        args[n++] = a->engine; args[n++] = "--protocol"; args[n++] = "--to";
        args[n++] = a->formats[job->to].ext;
        if (a->output_dir[0]) { args[n++] = "--output-dir"; args[n++] = a->output_dir; }
        args[n++] = "--"; args[n++] = job->source; args[n] = NULL;
        execv(a->engine, args); perror("simplechange-engine"); _exit(127);
    }
    close(pipefd[1]);
    if (child < 0) { close(pipefd[0]); message(a, "Cannot start converter: %s", strerror(errno)); return 0; }
    a->worker = child; a->output_fd = pipefd[0]; a->active = index;
    fcntl(a->output_fd, F_SETFL, O_NONBLOCK);
    a->protocol_used = 0; a->got_done = a->got_error = a->cancelling = 0;
    job->state = RUNNING; job->percent = 0; job->started = sui_monotonic_ms();
    job->output[0] = job->log[0] = job->route[0] = 0;
    copy(job->message, sizeof job->message, "Starting converter...");
    message(a, "Converting. x cancels; the remaining queue will wait.");
    return 1;
}
static void cancel_worker(App *a)
{
    a->batching = 0;
    if (!a->worker || a->cancelling) return;
    a->cancelling = 1; a->cancel_at = sui_monotonic_ms();
    kill(a->worker, SIGTERM);
    message(a, "Cancelling conversion...");
}
static void parse_progress(App *a, char *row)
{
    if (a->active < 0) return;
    Job *job = &a->jobs[a->active];
    char *rest = row, *kind = field(&rest);
    if (!strcmp(kind, "PROGRESS")) {
        int percent = atoi(field(&rest));
        job->percent = percent < 0 ? 0 : percent > 99 ? 99 : percent;
        copy(job->message, sizeof job->message, field(&rest));
    } else if (!strcmp(kind, "DONE")) {
        percent_decode(job->output, sizeof job->output, field(&rest));
        job->output_bytes = strtoull(field(&rest), NULL, 10); a->got_done = 1;
    } else if (!strcmp(kind, "ERROR")) {
        copy(job->message, sizeof job->message, field(&rest)); a->got_error = 1;
    } else if (!strcmp(kind, "LOG")) percent_decode(job->log, sizeof job->log, field(&rest));
    else if (!strcmp(kind, "ROUTE")) copy(job->route, sizeof job->route, field(&rest));
    else if (!strcmp(kind, "WARNING")) message(a, "%s", field(&rest));
    else if (*kind) { copy(job->message, sizeof job->message, kind); }
}
static void drain_worker(App *a)
{
    char bytes[8192]; ssize_t n;
    if (a->output_fd < 0) return;
    while ((n = read(a->output_fd, bytes, sizeof bytes)) > 0) for (ssize_t i = 0; i < n; i++) {
        if (bytes[i] == '\n' || bytes[i] == '\r') {
            a->protocol[a->protocol_used] = 0;
            parse_progress(a, a->protocol); a->protocol_used = 0;
        } else if (a->protocol_used + 1 < sizeof a->protocol) a->protocol[a->protocol_used++] = bytes[i];
    }
}
static void poll_worker(App *a)
{
    if (a->worker) {
        drain_worker(a);
        if (a->cancelling && sui_monotonic_ms() - a->cancel_at > 5000) kill(a->worker, SIGKILL);
        int status = 0;
        pid_t got = waitpid(a->worker, &status, WNOHANG);
        if (got == a->worker || (got < 0 && errno != EINTR)) {
            drain_worker(a);
            if (a->protocol_used) { a->protocol[a->protocol_used] = 0; parse_progress(a, a->protocol); }
            close(a->output_fd); a->output_fd = -1;
            Job *job = &a->jobs[a->active];
            job->elapsed = sui_monotonic_ms() - job->started;
            int success = got > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && a->got_done && !a->got_error;
            if (success) {
                job->state = DONE; job->percent = 100;
                copy(job->message, sizeof job->message, "Converted and validated. Original kept.");
            } else if (a->cancelling) {
                job->state = CANCELLED; copy(job->message, sizeof job->message, "Cancelled. Original kept; no unfinished output published.");
            } else {
                job->state = FAILED;
                if (!a->got_error) copy(job->message, sizeof job->message, "Converter stopped unexpectedly. Press r to retry.");
            }
            a->worker = 0; a->active = -1; a->cancelling = 0;
            message(a, "%s: %s", state_name(job->state), basename_of(job->source));
        }
    }
    if (!a->worker && a->batching) {
        for (size_t i = 0; i < a->count; i++) if (a->jobs[i].state == READY && a->jobs[i].to >= 0) {
            if (!start_worker(a, (int)i)) a->batching = 0;
            return;
        }
        a->batching = 0;
        int done = 0, failed = 0;
        for (size_t i = 0; i < a->count; i++) { done += a->jobs[i].state == DONE; failed += a->jobs[i].state == FAILED; }
        message(a, "Queue finished: %d done, %d failed. Enter shows file details.", done, failed);
    }
}
static void draw_queue(App *a)
{
    frame(" simplechange ", "a add  f format  F all  c convert  o output  Enter details  ? help  q quit");
    int done = 0, ready = 0, failed = 0;
    for (size_t i = 0; i < a->count; i++) {
        done += a->jobs[i].state == DONE; ready += a->jobs[i].state == READY;
        failed += a->jobs[i].state == FAILED;
    }
    char summary[160];
    snprintf(summary, sizeof summary, "%zu file%s   %d ready   %d done%s", a->count, a->count == 1 ? "" : "s", ready, done, failed ? "   errors: Enter for details" : "");
    put(2, 3, COLS - 6, summary, A_NORMAL);
    int file_width = COLS - 45;
    put(4, 3, file_width, "FILE", A_DIM);
    put(4, file_width + 4, 15, "CONVERSION", A_DIM);
    put(4, COLS - 24, 12, "STATUS", A_DIM);
    put(4, COLS - 12, 9, "SIZE", A_DIM);
    line(5, 2, COLS - 4);
    int rows = LINES - 15;
    if (rows < 1) rows = 1;
    if (a->selected < a->top) a->top = a->selected;
    if (a->selected >= a->top + rows) a->top = a->selected - rows + 1;
    for (int row = 0; row < rows && (size_t)(a->top + row) < a->count; row++) {
        int index = a->top + row;
        Job *job = &a->jobs[index];
        int y = 6 + row;
        attr_t attr = index == a->selected ? A_REVERSE : A_NORMAL;
        if (index == a->selected) { attron(A_REVERSE); mvhline(y, 2, ' ', COLS - 4); attroff(A_REVERSE); }
        put(y, 3, file_width, basename_of(job->source), attr);
        char conversion[40], status[32], size[32];
        snprintf(conversion, sizeof conversion, "%s > %s", a->formats[job->from].ext, job->to >= 0 ? a->formats[job->to].ext : "...");
        if (job->state == RUNNING) snprintf(status, sizeof status, "%c %d%%", "|/-\\"[(sui_monotonic_ms() / 100) % 4], job->percent);
        else copy(status, sizeof status, state_name(job->state));
        size_text(job->state == DONE ? job->output_bytes : job->bytes, size, sizeof size);
        put(y, file_width + 4, 15, conversion, attr);
        put(y, COLS - 24, 12, status, attr | (job->state == FAILED ? A_BOLD : A_NORMAL));
        put(y, COLS - 12, 9, size, attr);
    }
    if (!a->count) {
        put(8, 5, COLS - 10, "Convert documents and ebooks. Keep the original.", A_BOLD);
        put(10, 5, COLS - 10, "Press a to add files, then f to choose a format.", A_NORMAL);
        put(12, 5, COLS - 10, "PDF  DOCX  EPUB  MOBI  AZW3  ODT  RTF  and more", A_DIM);
    }
    line(LINES - 8, 2, COLS - 4);
    if (a->worker) {
        Job *job = &a->jobs[a->active];
        char progress[160];
        snprintf(progress, sizeof progress, "%3d%%  %.120s", job->percent, basename_of(job->source));
        put(LINES - 7, 3, COLS - 15, progress, A_NORMAL);
        put(LINES - 7, COLS - 12, 9, "x cancel", A_BOLD);
        put(LINES - 6, 3, COLS - 6, job->message, A_NORMAL);
    } else if (a->count) {
        Job *job = &a->jobs[a->selected];
        put(LINES - 7, 3, COLS - 6, job->state == DONE ? job->output : job->source, A_DIM);
        put(LINES - 6, 3, COLS - 6, job->message, A_NORMAL);
    }
    char output[SC_PATH + 20];
    snprintf(output, sizeof output, "Save: %s", a->output_dir[0] ? a->output_dir : "beside each original");
    put(LINES - 5, 3, COLS - 6, output, A_DIM);
    put(LINES - 4, 3, COLS - 6, a->quit_confirm ? "Conversion running. Press q again to stop and quit; any other key keeps working." : a->message, a->quit_confirm ? A_BOLD : A_NORMAL);
}
static void draw_browser(App *a)
{
    frame(" Add files ", "Enter open/add  Space add more  / search  g path  Backspace parent  . hidden  Esc back");
    put(2, 3, COLS - 6, a->directory, A_BOLD);
    char search[300]; snprintf(search, sizeof search, "%s%s", a->searching ? "Search: " : "Filter: ", a->search);
    put(3, 3, COLS - 6, search, a->searching ? A_REVERSE : A_DIM);
    line(4, 2, COLS - 4);
    int rows = LINES - 9;
    if (a->browser_selected < a->browser_top) a->browser_top = a->browser_selected;
    if (a->browser_selected >= a->browser_top + rows) a->browser_top = a->browser_selected - rows + 1;
    for (int row = 0; row < rows && (size_t)(a->browser_top + row) < a->entry_count; row++) {
        int index = a->browser_top + row;
        Entry *item = &a->entries[index];
        attr_t attr = index == a->browser_selected ? A_REVERSE : A_NORMAL;
        if (index == a->browser_selected) { attron(attr); mvhline(row + 5, 2, ' ', COLS - 4); attroff(attr); }
        char path[SC_PATH]; int added = 0;
        if (join(path, sizeof path, a->directory, item->name)) {
            char resolved[SC_PATH];
            if (realpath(path, resolved)) for (size_t i = 0; i < a->count; i++) if (!strcmp(a->jobs[i].source, resolved)) { added = 1; break; }
        }
        put(row + 5, 3, 3, item->directory ? "/" : added ? "+" : " ", attr);
        put(row + 5, 6, COLS - 22, item->name, attr | (item->directory ? A_BOLD : A_NORMAL));
        char size[32]; size_text(item->bytes, size, sizeof size);
        put(row + 5, COLS - 14, 11, item->directory ? "directory" : size, attr);
    }
    if (!a->entry_count) put(7, 4, COLS - 8, "No matching documents or folders.", A_DIM);
    put(LINES - 3, 3, COLS - 6, a->message, A_NORMAL);
}
static void draw_formats(App *a)
{
    draw_queue(a);
    int width = COLS > 78 ? 74 : COLS - 4, height = LINES > 26 ? 22 : LINES - 4;
    int x = (COLS - width) / 2, y = (LINES - height) / 2;
    attron(A_NORMAL);
    for (int row = 0; row < height; row++) mvhline(y + row, x, ' ', width);
    mvhline(y, x, ACS_HLINE, width); mvhline(y + height - 1, x, ACS_HLINE, width);
    mvvline(y, x, ACS_VLINE, height); mvvline(y, x + width - 1, ACS_VLINE, height);
    mvaddch(y, x, ACS_ULCORNER); mvaddch(y, x + width - 1, ACS_URCORNER);
    mvaddch(y + height - 1, x, ACS_LLCORNER); mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    put(y, x + 2, width - 4, a->all_formats ? " Output format · all waiting files " : " Output format ", A_BOLD);
    char text[300]; snprintf(text, sizeof text, "Search: %s", a->search);
    put(y + 2, x + 3, width - 6, text, A_NORMAL);
    line(y + 3, x + 2, width - 4);
    put(y + 5, x + 3, 15, "Document", (!a->format_category && !a->search[0]) ? A_REVERSE : A_NORMAL);
    put(y + 7, x + 3, 15, "EBook", (a->format_category && !a->search[0]) ? A_REVERSE : A_NORMAL);
    int list[SC_FORMATS], count = format_visible(a, list);
    if (a->format_selected >= count) a->format_selected = count ? count - 1 : 0;
    int visible_rows = height - 10;
    if (visible_rows < 1) visible_rows = 1;
    int top = a->format_selected / 3 >= visible_rows ? a->format_selected / 3 - visible_rows + 1 : 0;
    int cell_width = (width - 23) / 3;
    for (int i = top * 3; i < count && i < (top + visible_rows) * 3; i++) {
        int target = list[i], enabled = 0;
        for (size_t j = 0; j < a->count; j++) {
            if (!a->all_formats && (int)j != a->selected) continue;
            if (supported(a, a->jobs[j].from, target)) { enabled = 1; break; }
        }
        attr_t attr = enabled ? A_NORMAL : A_DIM;
        if (i == a->format_selected && a->format_focus) attr |= A_REVERSE;
        char label[24]; snprintf(label, sizeof label, " %-6s ", a->formats[target].ext);
        for (char *p = label; *p; p++) *p = (char)toupper((unsigned char)*p);
        put(y + 5 + i / 3 - top, x + 21 + (i % 3) * cell_width, cell_width - 1, label, attr);
    }
    if (!count) put(y + 6, x + 21, width - 24, "No matching formats.", A_DIM);
    if (count) {
        Format *f = &a->formats[list[a->format_selected]];
        put(y + height - 4, x + 3, width - 6, f->label, A_NORMAL);
        if (!f->enabled) put(y + height - 3, x + 3, width - 6, "Unavailable: check conversion engines with simplechange --check", A_DIM);
        else if (!a->all_formats && !supported(a, a->jobs[a->selected].from, list[a->format_selected]))
            put(y + height - 3, x + 3, width - 6, "Unavailable for this input; check engines with simplechange --check", A_DIM);
        else put(y + height - 3, x + 3, width - 6, "Tab category  Arrows select  Type to filter  Enter choose  Esc back", A_DIM);
    }
    attroff(A_NORMAL);
}
static void draw_help(void)
{
    frame(" simplechange · keys ", "Esc or q back");
    const char *help[] = {
        "a             Add files with the built-in browser",
        "f / F         Choose a format for this file / all waiting files",
        "c             Convert all ready files; failures do not stop the queue",
        "x             Cancel the active conversion and pause the queue",
        "r             Reset the selected file for another attempt",
        "o             Set output folder; empty means beside each original",
        "Enter         File details, output path, and conversion route",
        "v / l         Open the completed file / read its diagnostic log",
        "b             Queue a completed output back into its original format",
        "d / Delete    Remove selected row; source and output stay on disk",
        "j k / arrows  Move through the queue; PgUp/PgDn scroll",
        "q             Quit (press twice while converting)",
        "",
        "Files stay local. Existing names receive (2), (3), and so on.",
        "Unavailable formats are dimmed. --check lists installed engines.",
        "PDF reflow can change layout; scanned pages need OCR first.",
        "Text-only formats cannot retain images or page layout.",
        "Spreadsheet conversion keeps tables as cells, prose as rows.",
        "DOCM changes the container; it does not create VBA macros.",
        "DRM-protected ebooks must be supplied in an unlocked format.",
    };
    for (size_t i = 0; i < sizeof help / sizeof help[0] && (int)i + 3 < LINES - 3; i++)
        put((int)i + 2, 3, COLS - 6, help[i], A_NORMAL);
}
static int wrap(int y, const char *label, const char *text)
{
    put(y++, 3, COLS - 6, label, A_DIM);
    /* A full path can be copied/read in the CLI; fit the detail pane safely. */
    put(y++, 5, COLS - 10, text, A_NORMAL);
    return y;
}
static void draw_details(App *a)
{
    frame(" File details ", "v open output  l log  b reverse  r retry  Esc back");
    if (!a->count) return;
    Job *job = &a->jobs[a->selected];
    int y = wrap(2, "Source", job->source);
    y = wrap(y + 1, "Output", job->output[0] ? job->output : "No output published yet.");
    char text[128], input_size[32], output_size[32];
    size_text(job->bytes, input_size, sizeof input_size); size_text(job->output_bytes, output_size, sizeof output_size);
    snprintf(text, sizeof text, "%s    %s > %s    %s%s%s", state_name(job->state), a->formats[job->from].ext,
             job->to >= 0 ? a->formats[job->to].ext : "...", input_size, job->output[0] ? " > " : "", job->output[0] ? output_size : "");
    y = wrap(y + 1, "Conversion", text);
    if (y + 3 < LINES - 3) y = wrap(y + 1, "Route", job->route[0] ? job->route : "Choose a format and press c.");
    if (y + 3 < LINES - 3) y = wrap(y + 1, "Status", job->message);
    put(LINES - 3, 3, COLS - 6, a->message, A_NORMAL);
}
static void draw_input(App *a)
{
    draw_queue(a);
    int y = LINES / 2 - 3;
    for (int row = 0; row < 7; row++) mvhline(y + row, 3, ' ', COLS - 6);
    line(y, 3, COLS - 6); line(y + 6, 3, COLS - 6);
    put(y + 1, 5, COLS - 10, a->input_kind == 1 ? "Output folder (empty = beside each original)" : "Go to folder", A_BOLD);
    int cells = 0; size_t start = a->input_cursor;
    while (start > 0) {
        int n = wcwidth(a->input[start - 1]); if (n < 1) n = 1;
        if (cells + n > COLS - 14) break;
        cells += n; start--;
    }
    char text[SC_PATH * 4];
    size_t n = wcstombs(text, a->input + start, sizeof text - 1);
    if (n == (size_t)-1) n = 0;
    text[n] = 0;
    put(y + 3, 5, COLS - 10, text, A_REVERSE);
    put(y + 5, 5, COLS - 10, "Enter use folder   Esc cancel   Ctrl-U clear", A_DIM);
    curs_set(1); move(y + 3, 5 + cells);
}
static void log_open(App *a)
{
    if (!a->count || !a->jobs[a->selected].log[0]) { message(a, "No diagnostic log for this file."); return; }
    FILE *file = fopen(a->jobs[a->selected].log, "rb");
    if (!file) { message(a, "Cannot open diagnostic log: %s", strerror(errno)); return; }
    free(a->log_text); a->log_text = calloc(262145, 1);
    if (!a->log_text) { fclose(file); message(a, "Out of memory."); return; }
    if (fseek(file, 0, SEEK_END) == 0) { long n = ftell(file); fseek(file, n > 262144 ? n - 262144 : 0, SEEK_SET); }
    size_t n = fread(a->log_text, 1, 262144, file); a->log_text[n] = 0; fclose(file);
    a->view = LOG_VIEW; a->log_scroll = 0;
}
static void draw_log(App *a)
{
    frame(" Conversion log ", "Arrows or PgUp/PgDn scroll  Esc back");
    const char *p = a->log_text ? a->log_text : "";
    for (int row = 0; row < a->log_scroll && *p; row++) { const char *next = strchr(p, '\n'); p = next ? next + 1 : p + strlen(p); }
    for (int y = 2; y < LINES - 3 && *p; y++) {
        const char *next = strchr(p, '\n');
        size_t n = next ? (size_t)(next - p) : strlen(p);
        char text[4096]; if (n >= sizeof text) n = sizeof text - 1;
        memcpy(text, p, n); text[n] = 0; put(y, 3, COLS - 6, text, A_NORMAL);
        p = next ? next + 1 : p + strlen(p);
    }
}
static void open_output(App *a)
{
    if (!a->count || a->jobs[a->selected].state != DONE) { message(a, "No completed output to open."); return; }
#ifdef __APPLE__
    char *command = "open";
#else
    char *command = "xdg-open";
#endif
    if (!ssp_command_available(command)) { message(a, "Install %s to open outputs from here.", command); return; }
    pid_t pid = fork();
    if (!pid) {
        pid_t child = fork();
        if (child < 0) _exit(1);
        if (child) _exit(0);
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        execlp(command, command, a->jobs[a->selected].output, (char *)NULL); _exit(127);
    }
    if (pid > 0) ssp_manage_deferred_child(pid);
    message(a, "Opening converted file.");
}
static void search_key(App *a, wint_t key, int special)
{
    size_t length = strlen(a->search);
    if ((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) {
        if (length) { do { length--; } while (length && ((unsigned char)a->search[length] & 0xc0) == 0x80); a->search[length] = 0; }
    } else if (!special && key >= 32 && iswprint((wint_t)key)) {
        mbstate_t state = {0}; char bytes[MB_LEN_MAX]; size_t n = wcrtomb(bytes, (wchar_t)key, &state);
        if (n != (size_t)-1 && length + n < sizeof a->search) { memcpy(a->search + length, bytes, n); a->search[length + n] = 0; }
    }
}
static void input_key(App *a, wint_t key, int special)
{
    if (!special && key == 27) { a->view = a->before_input; return; }
    if ((!special && (key == '\n' || key == '\r')) || (special && key == KEY_ENTER)) { input_accept(a); return; }
    if (special && key == KEY_LEFT && a->input_cursor) a->input_cursor--;
    else if (special && key == KEY_RIGHT && a->input_cursor < a->input_length) a->input_cursor++;
    else if ((special && key == KEY_HOME) || (!special && key == 1)) a->input_cursor = 0;
    else if ((special && key == KEY_END) || (!special && key == 5)) a->input_cursor = a->input_length;
    else if (!special && key == 21) { a->input_length = a->input_cursor = 0; a->input[0] = 0; }
    else if (((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) && a->input_cursor) {
        memmove(a->input + a->input_cursor - 1, a->input + a->input_cursor,
                (a->input_length - a->input_cursor + 1) * sizeof(wchar_t));
        a->input_cursor--; a->input_length--;
    } else if (special && key == KEY_DC && a->input_cursor < a->input_length) {
        memmove(a->input + a->input_cursor, a->input + a->input_cursor + 1,
                (a->input_length - a->input_cursor) * sizeof(wchar_t)); a->input_length--;
    } else if (!special && iswprint(key) && a->input_length + 1 < SC_PATH) {
        memmove(a->input + a->input_cursor + 1, a->input + a->input_cursor,
                (a->input_length - a->input_cursor + 1) * sizeof(wchar_t));
        a->input[a->input_cursor++] = (wchar_t)key; a->input_length++;
    }
}
static void browser_key(App *a, wint_t key, int special)
{
    if (!special && key == 27) {
        if (a->searching) { a->searching = 0; return; }
        a->view = QUEUE; return;
    }
    if (a->searching && !(special && (key == KEY_UP || key == KEY_DOWN || key == KEY_NPAGE || key == KEY_PPAGE))) {
        if ((!special && key == '\n') || (special && key == KEY_ENTER)) { a->searching = 0; return; }
        search_key(a, key, special);
        scan_directory(a); return;
    }
    if (key == KEY_UP || (!special && key == 'k')) { if (a->browser_selected > 0) a->browser_selected--; }
    else if (key == KEY_DOWN || (!special && key == 'j')) { if ((size_t)(a->browser_selected + 1) < a->entry_count) a->browser_selected++; }
    else if (key == KEY_PPAGE) { a->browser_selected -= LINES - 10; if (a->browser_selected < 0) a->browser_selected = 0; }
    else if (key == KEY_NPAGE) { a->browser_selected += LINES - 10; if ((size_t)a->browser_selected >= a->entry_count) a->browser_selected = a->entry_count ? (int)a->entry_count - 1 : 0; }
    else if (!special && key == '/') { a->searching = 1; }
    else if (!special && key == 'g') input_open(a, 2);
    else if (!special && key == '.') { a->hidden = !a->hidden; scan_directory(a); }
    else if (key == KEY_BACKSPACE || (!special && (key == 127 || key == 8 || key == 'h'))) {
        parent_of(a->directory); a->search[0] = 0; scan_directory(a);
    } else if (!special && key == '~') {
        const char *home = getenv("HOME"); if (home) { copy(a->directory, sizeof a->directory, home); a->search[0] = 0; scan_directory(a); }
    } else if ((key == KEY_ENTER || (!special && (key == '\n' || key == '\r' || key == ' '))) && a->entry_count) {
        Entry *item = &a->entries[a->browser_selected]; char path[SC_PATH];
        if (!join(path, sizeof path, a->directory, item->name)) { message(a, "Path is too long."); return; }
        if (item->directory && key != ' ') {
            copy(a->directory, sizeof a->directory, path); a->search[0] = 0; scan_directory(a);
        } else if (!item->directory) {
            add_file(a, path);
            if (key != ' ') a->view = QUEUE;
            else if ((size_t)(a->browser_selected + 1) < a->entry_count) a->browser_selected++;
        }
    }
}
static void format_key(App *a, wint_t key, int special)
{
    int list[SC_FORMATS], count = format_visible(a, list);
    if (!special && key == 27) { a->view = QUEUE; return; }
    if (!special && key == '\t') {
        a->format_category = !a->format_category; a->search[0] = 0; a->format_selected = 0; a->format_focus = 1; return;
    }
    if (special && key == KEY_LEFT) {
        if (a->format_selected % 3) a->format_selected--;
        else a->format_focus = 0;
    } else if (special && key == KEY_RIGHT) {
        if (!a->format_focus) a->format_focus = 1;
        else if (a->format_selected + 1 < count) a->format_selected++;
    } else if (special && (key == KEY_UP || key == KEY_DOWN)) {
        if (!a->format_focus) { a->format_category = !a->format_category; a->search[0] = 0; a->format_selected = 0; }
        else if (key == KEY_UP && a->format_selected >= 3) a->format_selected -= 3;
        else if (key == KEY_DOWN && a->format_selected + 3 < count) a->format_selected += 3;
    } else if (special && key == KEY_HOME) a->format_selected = 0;
    else if (special && key == KEY_END) a->format_selected = count ? count - 1 : 0;
    else if ((special && key == KEY_ENTER) || (!special && (key == '\n' || key == '\r'))) {
        if (!a->format_focus) a->format_focus = 1;
        else format_choose(a);
    } else if (!special || key == KEY_BACKSPACE) {
        search_key(a, key, special); a->format_selected = 0; a->format_focus = 1;
    }
}
static void retry(App *a)
{
    if (!a->count) return;
    Job *job = &a->jobs[a->selected];
    if (job->state == RUNNING) { message(a, "Cancel this conversion before retrying."); return; }
    job->state = READY; job->percent = 0;
    copy(job->message, sizeof job->message, "Ready to convert. Press c.");
    message(a, "File reset. Existing outputs will be kept.");
}
static void reverse_job(App *a)
{
    if (!a->count || a->jobs[a->selected].state != DONE) { message(a, "Complete a conversion before reversing it."); return; }
    Job *job = &a->jobs[a->selected];
    int from = job->from, to = job->to;
    if (!supported(a, to, from)) { message(a, "The original format is unavailable as an output for this file."); return; }
    char output[SC_PATH]; copy(output, sizeof output, job->output);
    if (add_file(a, output)) {
        a->jobs[a->selected].to = from;
        a->view = QUEUE;
        message(a, "Reverse conversion queued. Press c to convert; originals will be kept.");
    }
}
static void queue_key(App *a, wint_t key, int special)
{
    int confirm = a->quit_confirm; a->quit_confirm = 0;
    if (key == KEY_UP || (!special && key == 'k')) { if (a->selected > 0) a->selected--; }
    else if (key == KEY_DOWN || (!special && key == 'j')) { if ((size_t)(a->selected + 1) < a->count) a->selected++; }
    else if (key == KEY_HOME) a->selected = 0;
    else if (key == KEY_END) a->selected = a->count ? (int)a->count - 1 : 0;
    else if (key == KEY_PPAGE) { a->selected -= LINES - 15; if (a->selected < 0) a->selected = 0; }
    else if (key == KEY_NPAGE) { a->selected += LINES - 15; if ((size_t)a->selected >= a->count) a->selected = a->count ? (int)a->count - 1 : 0; }
    else if (!special && key == 'a') browser_open(a);
    else if (!special && (key == 'f' || key == 'F')) format_open(a, key == 'F');
    else if (!special && key == 'o') input_open(a, 1);
    else if (!special && key == '?') a->view = HELP;
    else if (!special && key == 'v') open_output(a);
    else if (!special && key == 'l') log_open(a);
    else if (!special && key == 'r') retry(a);
    else if (!special && key == 'b') reverse_job(a);
    else if (!special && key == 'x') cancel_worker(a);
    else if (!special && key == 'c') {
        if (!a->count) message(a, "Add files with a first.");
        else if (!a->worker) { a->batching = 1; poll_worker(a); }
    } else if ((key == KEY_DC || (!special && key == 'd')) && a->count) {
        if (a->jobs[a->selected].state == RUNNING) { message(a, "Cancel this conversion before removing its row."); return; }
        if (a->active > a->selected) a->active--;
        memmove(a->jobs + a->selected, a->jobs + a->selected + 1, (a->count - (size_t)a->selected - 1) * sizeof(Job));
        a->count--; if ((size_t)a->selected >= a->count) a->selected = a->count ? (int)a->count - 1 : 0;
        message(a, "Removed row. Files on disk kept.");
    } else if ((key == KEY_ENTER || (!special && (key == '\n' || key == '\r'))) && a->count) a->view = DETAILS;
    else if (!special && key == 'q') {
        if (a->worker && !confirm) a->quit_confirm = 1;
        else { cancel_worker(a); a->quitting = 1; }
    }
}
static void mouse_key(App *a)
{
    MEVENT event;
    if (getmouse(&event) != OK) return;
    if (event.bstate & BUTTON4_PRESSED) {
        if (a->view == BROWSER) browser_key(a, KEY_UP, 1);
        else if (a->view == QUEUE) queue_key(a, KEY_UP, 1);
        return;
    }
#ifdef BUTTON5_PRESSED
    if (event.bstate & BUTTON5_PRESSED) {
        if (a->view == BROWSER) browser_key(a, KEY_DOWN, 1);
        else if (a->view == QUEUE) queue_key(a, KEY_DOWN, 1);
        return;
    }
#endif
    if (!(event.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON1_PRESSED))) return;
    if (a->view == QUEUE && event.y >= 6 && event.y < LINES - 9) {
        int index = a->top + event.y - 6;
        if (index >= 0 && (size_t)index < a->count) {
            a->selected = index;
            if (event.bstate & BUTTON1_DOUBLE_CLICKED) a->view = DETAILS;
        }
    } else if (a->view == BROWSER && event.y >= 5 && event.y < LINES - 4) {
        int index = a->browser_top + event.y - 5;
        if (index >= 0 && (size_t)index < a->entry_count) {
            a->browser_selected = index;
            if (event.bstate & BUTTON1_DOUBLE_CLICKED) browser_key(a, KEY_ENTER, 1);
        }
    } else if (a->view == FORMATS) {
        int width = COLS > 78 ? 74 : COLS - 4, height = LINES > 26 ? 22 : LINES - 4;
        int x = (COLS - width) / 2, y = (LINES - height) / 2;
        if (event.x >= x + 3 && event.x < x + 18) {
            if (event.y == y + 5 || event.y == y + 7) { a->format_category = event.y == y + 7; a->format_selected = 0; a->search[0] = 0; }
        } else if (event.x >= x + 21 && event.y >= y + 5 && event.y < y + height - 5) {
            int top = a->format_selected / 3 >= height - 10 ? a->format_selected / 3 - (height - 10) + 1 : 0;
            int index = (event.y - y - 5 + top) * 3 + (event.x - x - 21) / ((width - 23) / 3);
            int list[SC_FORMATS], count = format_visible(a, list);
            if (index >= 0 && index < count) { a->format_selected = index; a->format_focus = 1; if (event.bstate & BUTTON1_DOUBLE_CLICKED) format_choose(a); }
        }
    }
}
static int run_ui(App *a)
{
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    set_escdelay(SUI_ESCAPE_DELAY_MS);
    mouseinterval(180); mousemask(ALL_MOUSE_EVENTS, NULL);
    int dirty = 1;
    while ((!a->quitting || a->worker) && !stopped) {
        pid_t old_worker = a->worker;
        poll_worker(a); ssp_manage_deferred_child(0);
        if (a->worker || old_worker) dirty = 1;
        if (dirty) {
            curs_set(0);
            if (COLS < 68 || LINES < 20) {
                erase(); put(0, 0, COLS - 1, "simplechange: resize to at least 68 x 20. q quits.", A_NORMAL);
            } else switch (a->view) {
                case QUEUE: draw_queue(a); break;
                case BROWSER: draw_browser(a); break;
                case FORMATS: draw_formats(a); break;
                case HELP: draw_help(); break;
                case DETAILS: draw_details(a); break;
                case LOG_VIEW: draw_log(a); break;
                case INPUT: draw_input(a); break;
            }
            wnoutrefresh(stdscr); doupdate(); dirty = 0;
        }
        timeout(a->worker ? 70 : 1000);
        wint_t key = 0;
        int got = get_wch(&key);
        if (got == ERR) continue;
        dirty = 1;
        int special = got == KEY_CODE_YES;
        if (special && key == KEY_RESIZE) continue;
        if (COLS < 68 || LINES < 20) {
            if (!special && key == 'q') { cancel_worker(a); a->quitting = 1; }
            else if (!special && key == 'x') cancel_worker(a);
            continue;
        }
        if (special && key == KEY_MOUSE) { mouse_key(a); continue; }
        switch (a->view) {
            case QUEUE: queue_key(a, key, special); break;
            case BROWSER: browser_key(a, key, special); break;
            case FORMATS: format_key(a, key, special); break;
            case INPUT: input_key(a, key, special); break;
            case HELP: a->view = QUEUE; break;
            case DETAILS:
                if (!special && key == 'v') open_output(a);
                else if (!special && key == 'l') log_open(a);
                else if (!special && key == 'r') retry(a);
                else if (!special && key == 'b') reverse_job(a);
                else if (!special && (key == 27 || key == 'q' || key == '\n')) a->view = QUEUE;
                break;
            case LOG_VIEW:
                if (key == KEY_UP && a->log_scroll > 0) a->log_scroll--;
                else if (key == KEY_DOWN) a->log_scroll++;
                else if (key == KEY_PPAGE) { a->log_scroll -= LINES - 5; if (a->log_scroll < 0) a->log_scroll = 0; }
                else if (key == KEY_NPAGE) a->log_scroll += LINES - 5;
                else if (key == KEY_HOME) a->log_scroll = 0;
                else if (!special && (key == 27 || key == 'q')) a->view = QUEUE;
                break;
        }
    }
    if (a->worker) {
        cancel_worker(a);
        int64_t deadline = sui_monotonic_ms() + 5000;
        while (a->worker && sui_monotonic_ms() < deadline) { poll_worker(a); sui_sleep_ms(20); }
        if (a->worker) { kill(a->worker, SIGKILL); while (waitpid(a->worker, NULL, 0) < 0 && errno == EINTR) {} }
    }
    if (a->output_fd >= 0) close(a->output_fd);
    endwin();
    return stopped ? 128 + stopped : 0;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    App *a = calloc(1, sizeof(*a));
    if (!a) { fputs("simplechange: out of memory\n", stderr); return 1; }
    a->output_fd = a->active = -1;
    if (!find_engine(a->engine, sizeof a->engine, argv[0])) {
        fputs("simplechange: simplechange-engine must be installed beside the program or on PATH\n", stderr); free(a); return 1;
    }
    int direct = 0, first = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { first = i + 1; break; }
        if (argv[i][0] == '-') direct = 1;
    }
    if (direct) {
        char **args = calloc((size_t)argc + 1, sizeof(*args));
        if (!args) { free(a); return 1; }
        args[0] = a->engine;
        for (int i = 1; i < argc; i++) args[i] = argv[i];
        execv(a->engine, args); perror("simplechange-engine"); free(args); free(a); return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("simplechange: run in a terminal, or use --to FORMAT [--output-dir DIR] FILE...\n", stderr); free(a); return 2;
    }
    if (!load_catalog(a)) { fputs("simplechange: cannot read formats; check Python 3 and simplechange-engine --check\n", stderr); free(a); return 1; }
    a->default_format = format_index(a, "pdf");
    if (!getcwd(a->directory, sizeof a->directory)) copy(a->directory, sizeof a->directory, getenv("HOME") ? getenv("HOME") : "/");
    copy(a->message, sizeof a->message, "Files stay local. Existing outputs are kept.");
    for (int i = first; i < argc; i++) add_file(a, argv[i]);
    struct sigaction action = {0}; action.sa_handler = stop_signal; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL); sigaction(SIGHUP, &action, NULL);
    int result = run_ui(a);
    free_entries(a); free(a->jobs); free(a->log_text); free(a);
    return result;
}
