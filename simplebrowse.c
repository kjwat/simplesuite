#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE_EXTENDED 1

#include <ncurses.h>
#include <curl/curl.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "simplerender.h"

#define URL_MAX 4096
#define RESPONSE_LIMIT (16u * 1024u * 1024u)
#define MAX_HISTORY 128
#define CTRL_KEY(ch) ((ch) & 0x1f)
#define SIMPLEBROWSE_VERSION "4.0.0"
#define SIMPLEBROWSE_UA "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"
#define WEBKITD_HELPER "simplebrowse-webkitd"
#define WEBKITD_RESPONSE_HEADER "SIMPLEBROWSE_WEBKITD_RESPONSE_V1"
#define JS_RESPONSE_LIMIT (32u * 1024u * 1024u)

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum {
    NAV_REPLACE,
    NAV_NORMAL,
    NAV_BACK,
    NAV_FORWARD
};

enum {
    MODE_PAGE,
    MODE_URL,
    MODE_FIND,
    MODE_BOOKMARKS,
    MODE_FIELD
};

enum {
    STOP_LINK,
    STOP_CONTROL
};

enum {
    SB_PAIR_LINK = 1,
    SB_PAIR_VISITED_LINK,
    SB_PAIR_SELECTED_LINK,
    SB_PAIR_SELECTED_CONTROL,
    SB_PAIR_MATCH
};

enum {
    CONTROL_TEXT,
    CONTROL_SEARCH,
    CONTROL_PASSWORD,
    CONTROL_EMAIL,
    CONTROL_URL,
    CONTROL_NUMBER,
    CONTROL_TEXTAREA,
    CONTROL_SELECT,
    CONTROL_CHECKBOX,
    CONTROL_RADIO,
    CONTROL_SUBMIT,
    CONTROL_BUTTON
};

enum {
    BROWSE_KEY_WORD_LEFT = 100001,
    BROWSE_KEY_WORD_RIGHT,
    BROWSE_KEY_SELECT_WORD_LEFT,
    BROWSE_KEY_SELECT_WORD_RIGHT
};

typedef struct {
    int code;
    int action;
} KeyMapping;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct {
    char *label;
    char *url;
    size_t marker_offset;
} Link;

typedef struct {
    int first_link;
    int last_link;
    int kind;
    int control_index;
    size_t start;
    size_t end;
    int start_line;
    int end_line;
} LinkStop;

typedef struct {
    char *label;
    char *name;
    char *value;
    char *form_action;
    char *form_method;
    char *form_enctype;
    char **options;
    char **option_values;
    size_t option_count;
    size_t option_cap;
    size_t marker_offset;
    size_t display_len;
    int type;
    int form_index;
    int checked;
    int selected;
    int disabled;
    int button_submits;
} FormControl;

typedef struct {
    char *title;
    char *url;
} Bookmark;

typedef struct {
    size_t start;
    size_t len;
    int link;
} DisplayLine;

typedef struct {
    char *text;
    char *url;
    char *title;
    char *meta;
    Link *links;
    size_t link_count;
    size_t link_cap;
    FormControl *controls;
    size_t control_count;
    size_t control_cap;
    LinkStop *stops;
    size_t stop_count;
    size_t stop_cap;
    DisplayLine *display;
    size_t display_count;
    size_t display_cap;
    int layout_width;
} Page;

typedef struct {
    char *url;
    Page page;
    int top;
    int selected_link;
    int selected_control;
    int js_mode_active;
    long code;
} PageSnapshot;

typedef struct {
    Page page;
    char url_bar[URL_MAX];
    int url_len;
    int url_pos;
    char current_url[URL_MAX];
    char status[512];
    char *back_stack[MAX_HISTORY];
    int back_count;
    char *forward_stack[MAX_HISTORY];
    int forward_count;
    char *visited_urls[MAX_HISTORY];
    int visited_count;
    PageSnapshot *page_cache;
    int page_cache_count;
    int page_cache_cap;
    Bookmark *bookmarks;
    size_t bookmark_count;
    size_t bookmark_cap;
    int selected_bookmark;
    int bookmark_top;
    char bookmark_path[PATH_MAX];
    char number_buf[16];
    int number_len;
    char find_query[256];
    int find_len;
    int find_pos;
    size_t match_offset;
    size_t match_len;
    int has_match;
    int top;
    int selected_link;
    int selected_control;
    int editing_control;
    int field_cursor;
    int field_select_anchor;
    int field_select_active;
    int field_undo_cursor;
    int field_redo_cursor;
    char *field_undo_value;
    char *field_redo_value;
    char *field_clipboard;
    int url_focus;
    int find_focus;
    int bookmark_mode;
    int mode;
    int running;
    int js_mode_active;
    int initial_js;
} App;

typedef struct {
    Buffer out;
    int pending_space;
} TextBuilder;

typedef struct {
    Buffer text;
    int pending_space;
} AnchorBuilder;

typedef struct {
    long code;
    CURLcode curl_code;
    int network_error;
    char reason[64];
    char effective[URL_MAX];
    char title[512];
    char error[512];
} FetchResult;

typedef struct {
    pid_t pid;
    int in_fd;
    int out_fd;
    int err_fd;
    off_t err_pos;
} WebKitDaemon;

typedef struct {
    char *html;
    char effective[URL_MAX];
    long code;
    int used_js;
} CacheEntry;

typedef struct {
    const char *start;
    const char *end;
    int found;
    int listing;
    long score;
} ReaderRegion;

static int sb_has_color = 0;

static void finish_navigation(App *a, int mode, const char *old_url,
                              const char *new_url, int success);
static void remember_visited_url(App *a, const char *url);
static int browse_read_width(int screen_w);

typedef struct {
    char *site;
    char *author;
    char *published;
    char *updated;
    char *comments;
} ReaderMeta;

typedef struct {
    char tag[16];
    const char *start;
    long base_score;
    long text_chars;
    long link_chars;
    long punctuation;
    long paragraphs;
    long headings;
    long list_items;
    long images;
    long child_articles;
    int clutter;
} RegionFrame;

static void set_status(App *a, const char *msg);
static void ensure_selected_visible(App *a, int body_h, int body_w);
static void clamp_top(App *a, int body_h, int body_w);
static void set_selected_link_status(App *a, int link_index);
static void set_selected_control_status(App *a, int control_index);
static int stop_is_visible(App *a, int stop_index, int body_h);
static int visible_stop_candidate(App *a, int dir, int body_h);
static int offscreen_stop_candidate(App *a, int dir, int body_h, int *line_out);
static int field_selection_bounds(App *a, int *start, int *end);
static size_t page_meaningful_chars(Page *p);
static int html_contains_ci(const char *html, size_t len, const char *needle);

static WebKitDaemon webkitd = { -1, -1, -1, -1, 0 };

static void die(const char *msg)
{
    endwin();
    perror(msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("malloc");
    return p;
}

static void *xrealloc(void *ptr, size_t n)
{
    void *p = realloc(ptr, n ? n : 1);
    if (!p) die("realloc");
    return p;
}

static char *xstrndup_local(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *xstrdup_local(const char *s)
{
    return xstrndup_local(s ? s : "", strlen(s ? s : ""));
}

static int buf_reserve(Buffer *b, size_t need)
{
    size_t n;
    char *p;

    if (need <= b->cap) return 1;

    n = b->cap ? b->cap : 256;
    while (n < need) {
        if (n > SIZE_MAX / 2) return 0;
        n *= 2;
    }

    p = realloc(b->data, n);
    if (!p) return 0;

    b->data = p;
    b->cap = n;
    return 1;
}

static int buf_addn(Buffer *b, const char *s, size_t n)
{
    if (!n) return 1;
    if (!buf_reserve(b, b->len + n + 1)) return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return 1;
}

static int buf_addc(Buffer *b, char c)
{
    return buf_addn(b, &c, 1);
}

static void buf_clear(Buffer *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static char *trim_copy(const char *s)
{
    const char *start = s ? s : "";
    const char *end;

    while (*start && isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    return xstrndup_local(start, (size_t)(end - start));
}

static int snprintf_ok(int n, size_t size)
{
    return n >= 0 && (size_t)n < size;
}

static void set_status(App *a, const char *msg);

static KeyMapping runtime_keys[24];
static int runtime_key_count;
static int field_clip_warned;
static int loose_html_parse;

static void add_terminfo_key(const char *capability, int action)
{
    const char *sequence = tigetstr((char *)capability);
    int code;

    if (!sequence || sequence == (char *)-1 || !*sequence)
        return;
    code = key_defined(sequence);
    if (code <= 0)
        return;
    if (runtime_key_count >= (int)(sizeof(runtime_keys) / sizeof(runtime_keys[0])))
        return;
    runtime_keys[runtime_key_count].code = code;
    runtime_keys[runtime_key_count].action = action;
    runtime_key_count++;
}

static void discover_browser_keys(void)
{
    runtime_key_count = 0;
    add_terminfo_key("kLFT", KEY_SLEFT);
    add_terminfo_key("kRIT", KEY_SRIGHT);
    add_terminfo_key("kLFT5", BROWSE_KEY_WORD_LEFT);
    add_terminfo_key("kRIT5", BROWSE_KEY_WORD_RIGHT);
    add_terminfo_key("kLFT6", BROWSE_KEY_SELECT_WORD_LEFT);
    add_terminfo_key("kRIT6", BROWSE_KEY_SELECT_WORD_RIGHT);
}

static int normalize_browser_key(int ch)
{
    int i;

    for (i = 0; i < runtime_key_count; i++)
        if (runtime_keys[i].code == ch)
            return runtime_keys[i].action;
    return ch;
}

static int key_modifier_has_shift(int modifier)
{
    return modifier > 0 && ((modifier - 1) & 1) != 0;
}

static int key_modifier_has_ctrl(int modifier)
{
    return modifier > 0 && ((modifier - 1) & 4) != 0;
}

static int parse_browser_csi(const char *sequence)
{
    int first;
    int modifier;
    char final;

    if (!strcmp(sequence, "[A") || !strcmp(sequence, "OA")) return KEY_UP;
    if (!strcmp(sequence, "[B") || !strcmp(sequence, "OB")) return KEY_DOWN;
    if (!strcmp(sequence, "[C") || !strcmp(sequence, "OC")) return KEY_RIGHT;
    if (!strcmp(sequence, "[D") || !strcmp(sequence, "OD")) return KEY_LEFT;

    if (sscanf(sequence, "[1;%d%c", &modifier, &final) == 2 ||
        sscanf(sequence, "O1;%d%c", &modifier, &final) == 2) {
        if (final == 'C') {
            if (key_modifier_has_ctrl(modifier) && key_modifier_has_shift(modifier))
                return BROWSE_KEY_SELECT_WORD_RIGHT;
            if (key_modifier_has_ctrl(modifier))
                return BROWSE_KEY_WORD_RIGHT;
            if (key_modifier_has_shift(modifier))
                return KEY_SRIGHT;
        } else if (final == 'D') {
            if (key_modifier_has_ctrl(modifier) && key_modifier_has_shift(modifier))
                return BROWSE_KEY_SELECT_WORD_LEFT;
            if (key_modifier_has_ctrl(modifier))
                return BROWSE_KEY_WORD_LEFT;
            if (key_modifier_has_shift(modifier))
                return KEY_SLEFT;
        }
    }

    if (sscanf(sequence, "[%d;%d%c", &first, &modifier, &final) == 3) {
        (void)first;
        if (final == 'C') {
            if (key_modifier_has_ctrl(modifier) && key_modifier_has_shift(modifier))
                return BROWSE_KEY_SELECT_WORD_RIGHT;
            if (key_modifier_has_ctrl(modifier))
                return BROWSE_KEY_WORD_RIGHT;
            if (key_modifier_has_shift(modifier))
                return KEY_SRIGHT;
        } else if (final == 'D') {
            if (key_modifier_has_ctrl(modifier) && key_modifier_has_shift(modifier))
                return BROWSE_KEY_SELECT_WORD_LEFT;
            if (key_modifier_has_ctrl(modifier))
                return BROWSE_KEY_WORD_LEFT;
            if (key_modifier_has_shift(modifier))
                return KEY_SLEFT;
        }
    }

    return 0;
}

static int read_browser_key(void)
{
    char sequence[32];
    int len = 0;
    int ch = getch();

    if (ch != 27)
        return normalize_browser_key(ch);

    timeout(25);
    ch = getch();
    if (ch != '[' && ch != 'O') {
        if (ch != ERR)
            ungetch(ch);
        timeout(-1);
        return 27;
    }

    sequence[len++] = (char)ch;
    while (len < (int)sizeof(sequence) - 1) {
        ch = getch();
        if (ch == ERR)
            break;
        if (ch < 0 || ch > UCHAR_MAX)
            break;
        sequence[len++] = (char)ch;
        if ((ch >= '@' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '~')
            break;
    }
    sequence[len] = 0;
    timeout(-1);

    ch = parse_browser_csi(sequence);
    return ch ? ch : ERR;
}

static int command_exists(const char *cmd)
{
    char probe[256];

    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", cmd);
    return system(probe) == 0;
}

enum {
    CLIP_BACKEND_NONE,
    CLIP_BACKEND_WL,
    CLIP_BACKEND_XCLIP,
    CLIP_BACKEND_XSEL
};

static int detect_clipboard_backend(void)
{
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *x11 = getenv("DISPLAY");

    if (wayland && *wayland &&
        command_exists("wl-copy") && command_exists("wl-paste"))
        return CLIP_BACKEND_WL;
    if (x11 && *x11 && command_exists("xclip"))
        return CLIP_BACKEND_XCLIP;
    if (x11 && *x11 && command_exists("xsel"))
        return CLIP_BACKEND_XSEL;
    return CLIP_BACKEND_NONE;
}

static void warn_no_field_clipboard_once(App *a)
{
    if (!field_clip_warned) {
        set_status(a, "No system clipboard tool; using field-local clipboard");
        field_clip_warned = 1;
    }
}

static int write_system_clipboard(App *a, const char *text)
{
    char tmpname[] = "/tmp/simplebrowse-clip-XXXXXX";
    char cmd[PATH_MAX + 256];
    int fd;
    FILE *fp;

    if (!text)
        return 0;
    fd = mkstemp(tmpname);
    if (fd < 0) {
        warn_no_field_clipboard_once(a);
        return 0;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmpname);
        warn_no_field_clipboard_once(a);
        return 0;
    }
    fputs(text, fp);
    fclose(fp);

    switch (detect_clipboard_backend()) {
    case CLIP_BACKEND_WL:
        snprintf(cmd, sizeof(cmd),
                 "wl-copy --type text/plain < '%s' 2>/dev/null", tmpname);
        break;
    case CLIP_BACKEND_XCLIP:
        snprintf(cmd, sizeof(cmd),
                 "xclip -selection clipboard < '%s' 2>/dev/null", tmpname);
        break;
    case CLIP_BACKEND_XSEL:
        snprintf(cmd, sizeof(cmd),
                 "xsel --clipboard --input < '%s' 2>/dev/null", tmpname);
        break;
    default:
        unlink(tmpname);
        warn_no_field_clipboard_once(a);
        return 0;
    }

    {
        int rc = system(cmd);
        unlink(tmpname);
        return rc == 0;
    }
}

static char *read_system_clipboard(App *a)
{
    FILE *fp = NULL;
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    int ch;

    switch (detect_clipboard_backend()) {
    case CLIP_BACKEND_WL:
        fp = popen("wl-paste --no-newline 2>/dev/null", "r");
        break;
    case CLIP_BACKEND_XCLIP:
        fp = popen("xclip -selection clipboard -o 2>/dev/null", "r");
        break;
    case CLIP_BACKEND_XSEL:
        fp = popen("xsel --clipboard --output 2>/dev/null", "r");
        break;
    default:
        warn_no_field_clipboard_once(a);
        return NULL;
    }

    if (!fp)
        return NULL;

    while ((ch = fgetc(fp)) != EOF) {
        if (len + 2 > cap) {
            size_t new_cap = cap ? cap * 2 : 4096;
            char *grown = realloc(buf, new_cap);

            if (!grown) {
                free(buf);
                pclose(fp);
                return NULL;
            }
            buf = grown;
            cap = new_cap;
        }
        buf[len++] = (char)ch;
    }
    pclose(fp);

    if (!buf)
        return NULL;
    buf[len] = 0;
    return buf;
}

static int home_path(char *out, size_t outsz, const char *suffix)
{
    const char *home = getenv("HOME");
    int n;

    if (!home || !*home) return 0;
    n = snprintf(out, outsz, "%s/%s", home, suffix);
    return snprintf_ok(n, outsz);
}

static int mkdir_one(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    char *p;

    if (strlen(path) >= sizeof(tmp)) return 0;
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (!mkdir_one(tmp)) return 0;
            *p = '/';
        }
    }
    return mkdir_one(tmp);
}

static int cache_base_path(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    int n;

    if (xdg && *xdg) {
        n = snprintf(out, outsz, "%s/simplebrowse/pages", xdg);
        return snprintf_ok(n, outsz);
    }
    return home_path(out, outsz, ".cache/simplebrowse/pages");
}

static unsigned long long cache_hash_key(const char *mode, const char *url)
{
    const unsigned char *p;
    unsigned long long h = 1469598103934665603ULL;

    for (p = (const unsigned char *)(mode ? mode : ""); *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    h ^= '|';
    h *= 1099511628211ULL;
    for (p = (const unsigned char *)(url ? url : ""); *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static int cache_file_path(char *out, size_t outsz, const char *mode,
                           const char *url)
{
    char dir[PATH_MAX];
    unsigned long long key;
    int n;

    if (!cache_base_path(dir, sizeof(dir))) return 0;
    if (!mkdir_p(dir)) return 0;
    key = cache_hash_key(mode, url);
    n = snprintf(out, outsz, "%s/%016llx.cache", dir, key);
    return snprintf_ok(n, outsz);
}

static void cache_entry_clear(CacheEntry *entry)
{
    if (!entry) return;
    free(entry->html);
    memset(entry, 0, sizeof(*entry));
}

static int cache_read_exact(FILE *fp, void *data, size_t n)
{
    return n == 0 || fread(data, 1, n, fp) == n;
}

static int cache_read_entry(const char *mode, const char *url, CacheEntry *entry)
{
    char path[PATH_MAX];
    FILE *fp;
    char magic[8];
    uint32_t effective_len;
    uint64_t html_len;
    int64_t code;
    int32_t used_js;
    char *html;

    memset(entry, 0, sizeof(*entry));
    if (!cache_file_path(path, sizeof(path), mode, url)) return 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;

    if (!cache_read_exact(fp, magic, sizeof(magic)) ||
        memcmp(magic, "SBCACHE2", sizeof(magic)) ||
        !cache_read_exact(fp, &code, sizeof(code)) ||
        !cache_read_exact(fp, &used_js, sizeof(used_js)) ||
        !cache_read_exact(fp, &effective_len, sizeof(effective_len)) ||
        !cache_read_exact(fp, &html_len, sizeof(html_len)) ||
        effective_len >= URL_MAX ||
        html_len > JS_RESPONSE_LIMIT) {
        fclose(fp);
        return 0;
    }

    if (!cache_read_exact(fp, entry->effective, effective_len)) {
        fclose(fp);
        return 0;
    }
    entry->effective[effective_len] = 0;
    html = malloc((size_t)html_len + 1);
    if (!html) {
        fclose(fp);
        return 0;
    }
    if (!cache_read_exact(fp, html, (size_t)html_len)) {
        free(html);
        fclose(fp);
        return 0;
    }
    html[html_len] = 0;
    fclose(fp);

    entry->html = html;
    entry->code = (long)code;
    entry->used_js = used_js != 0;
    return 1;
}

static void cache_write_entry(const char *mode, const char *url,
                              const char *effective, long code, int used_js,
                              const char *html)
{
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    FILE *fp;
    uint32_t effective_len;
    uint64_t html_len;
    int64_t stored_code = code;
    int32_t stored_used_js = used_js ? 1 : 0;

    if (!html || !*html) return;
    if (!cache_file_path(path, sizeof(path), mode, url)) return;
    if (!snprintf_ok(snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()),
                     sizeof(tmp)))
        return;

    effective = effective && *effective ? effective : url;
    effective_len = (uint32_t)strlen(effective);
    html_len = (uint64_t)strlen(html);
    if (effective_len >= URL_MAX || html_len > JS_RESPONSE_LIMIT) return;

    fp = fopen(tmp, "wb");
    if (!fp) return;
    if (fwrite("SBCACHE2", 1, 8, fp) != 8 ||
        fwrite(&stored_code, sizeof(stored_code), 1, fp) != 1 ||
        fwrite(&stored_used_js, sizeof(stored_used_js), 1, fp) != 1 ||
        fwrite(&effective_len, sizeof(effective_len), 1, fp) != 1 ||
        fwrite(&html_len, sizeof(html_len), 1, fp) != 1 ||
        fwrite(effective, 1, effective_len, fp) != effective_len ||
        fwrite(html, 1, (size_t)html_len, fp) != (size_t)html_len ||
        fclose(fp) != 0) {
        unlink(tmp);
        return;
    }
    rename(tmp, path);
}

static int clear_browser_cache(size_t *removed_out)
{
    char dir[PATH_MAX];
    DIR *dp;
    struct dirent *de;
    size_t removed = 0;

    if (!cache_base_path(dir, sizeof(dir))) return 0;
    dp = opendir(dir);
    if (!dp) {
        if (errno == ENOENT) {
            if (removed_out) *removed_out = 0;
            return 1;
        }
        return 0;
    }
    while ((de = readdir(dp)) != NULL) {
        char path[PATH_MAX];
        size_t len = strlen(de->d_name);

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (len < 7 || strcmp(de->d_name + len - 6, ".cache"))
            continue;
        if (!snprintf_ok(snprintf(path, sizeof(path), "%s/%s", dir, de->d_name),
                         sizeof(path)))
            continue;
        if (unlink(path) == 0) removed++;
    }
    closedir(dp);
    if (removed_out) *removed_out = removed;
    return 1;
}

static int ascii_eqn(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static const char *ci_find(const char *s, const char *end, const char *needle)
{
    size_t n;

    if (!s || !end || !needle) return NULL;
    n = strlen(needle);
    if (!n) return s;

    while (s < end && (size_t)(end - s) >= n) {
        if (ascii_eqn(s, needle, n)) return s;
        s++;
    }
    return NULL;
}

static int wordish_char(int ch)
{
    return isalnum((unsigned char)ch) || ch == '_';
}

static int attr_contains_wordish(const char *value, const char *term)
{
    const char *end;
    const char *p;
    size_t n;

    if (!value || !term || !*term) return 0;
    end = value + strlen(value);
    n = strlen(term);
    p = value;
    while ((p = ci_find(p, end, term)) != NULL) {
        int before_ok = p == value || !wordish_char((unsigned char)p[-1]);
        int after_ok = p + n >= end || !wordish_char((unsigned char)p[n]);

        if (before_ok && after_ok) return 1;
        p++;
    }
    return 0;
}

static int text_contains_any(const char *value, const char * const *terms)
{
    const char *end;
    int i;

    if (!value || !*value) return 0;
    end = value + strlen(value);
    for (i = 0; terms[i]; i++) {
        if (ci_find(value, end, terms[i])) return 1;
    }
    return 0;
}

static int starts_http_url(const char *s)
{
    return !strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8);
}

static int starts_https_url_trimmed(const char *s)
{
    if (!s)
        return 0;
    while (*s && isspace((unsigned char)*s))
        s++;
    return !strncasecmp(s, "https://", 8);
}

static int utf8_put(Buffer *b, unsigned long cp)
{
    char x[4];
    size_t n;

    if (cp <= 0x7f) {
        x[0] = (char)cp;
        n = 1;
    } else if (cp <= 0x7ff) {
        x[0] = (char)(0xc0 | (cp >> 6));
        x[1] = (char)(0x80 | (cp & 63));
        n = 2;
    } else if (cp <= 0xffff) {
        x[0] = (char)(0xe0 | (cp >> 12));
        x[1] = (char)(0x80 | ((cp >> 6) & 63));
        x[2] = (char)(0x80 | (cp & 63));
        n = 3;
    } else if (cp <= 0x10ffff) {
        x[0] = (char)(0xf0 | (cp >> 18));
        x[1] = (char)(0x80 | ((cp >> 12) & 63));
        x[2] = (char)(0x80 | ((cp >> 6) & 63));
        x[3] = (char)(0x80 | (cp & 63));
        n = 4;
    } else {
        return 0;
    }

    return buf_addn(b, x, n);
}

static int decode_entity(Buffer *b, const char *s, size_t n)
{
    if (n == 3 && !memcmp(s, "amp", 3)) return buf_addc(b, '&');
    if (n == 2 && !memcmp(s, "lt", 2)) return buf_addc(b, '<');
    if (n == 2 && !memcmp(s, "gt", 2)) return buf_addc(b, '>');
    if (n == 4 && !memcmp(s, "quot", 4)) return buf_addc(b, '"');
    if (n == 4 && !memcmp(s, "apos", 4)) return buf_addc(b, '\'');
    if (n == 4 && !memcmp(s, "nbsp", 4)) return buf_addc(b, ' ');

    if (n > 1 && s[0] == '#') {
        char tmp[32];
        char *end = NULL;
        unsigned long cp;

        if (n >= sizeof(tmp)) return 0;
        memcpy(tmp, s + 1, n - 1);
        tmp[n - 1] = 0;
        cp = strtoul((tmp[0] == 'x' || tmp[0] == 'X') ? tmp + 1 : tmp,
                     &end, (tmp[0] == 'x' || tmp[0] == 'X') ? 16 : 10);
        if (end && *end == 0 && cp) return utf8_put(b, cp);
    }

    buf_addc(b, '&');
    buf_addn(b, s, n);
    return buf_addc(b, ';');
}

static int append_decoded_char(Buffer *dst, const char *s, size_t n, size_t *used)
{
    const char *semi;

    if (n && s[0] == '&') {
        semi = memchr(s + 1, ';', n - 1);
        if (semi && (size_t)(semi - s) <= 16) {
            *used = (size_t)(semi - s) + 1;
            return decode_entity(dst, s + 1, (size_t)(semi - s - 1));
        }
    }

    *used = 1;
    return buf_addc(dst, s[0]);
}

static void tb_trim_space(TextBuilder *tb)
{
    while (tb->out.len && tb->out.data[tb->out.len - 1] == ' ')
        tb->out.data[--tb->out.len] = 0;
}

static void tb_space(TextBuilder *tb)
{
    if (!tb->out.len) return;
    if (tb->out.data[tb->out.len - 1] == '\n') return;
    tb->pending_space = 1;
}

static void tb_newline(TextBuilder *tb)
{
    tb_trim_space(tb);
    if (!tb->out.len || tb->out.data[tb->out.len - 1] == '\n') {
        tb->pending_space = 0;
        return;
    }
    buf_addc(&tb->out, '\n');
    tb->pending_space = 0;
}

static void tb_br(TextBuilder *tb)
{
    tb_trim_space(tb);
    if (!tb->out.len) {
        tb->pending_space = 0;
        return;
    }
    if (tb->out.data[tb->out.len - 1] != '\n') {
        buf_addc(&tb->out, '\n');
    } else if (tb->out.len < 2 || tb->out.data[tb->out.len - 2] != '\n') {
        buf_addc(&tb->out, '\n');
    }
    tb->pending_space = 0;
}

static void tb_block(TextBuilder *tb)
{
    tb_trim_space(tb);
    if (!tb->out.len) {
        tb->pending_space = 0;
        return;
    }
    if (tb->out.data[tb->out.len - 1] != '\n')
        buf_addc(&tb->out, '\n');
    if (tb->out.len < 2 || tb->out.data[tb->out.len - 2] != '\n')
        buf_addc(&tb->out, '\n');
    tb->pending_space = 0;
}

static void tb_text_bytes(TextBuilder *tb, const char *s, size_t n)
{
    size_t i = 0;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t used = 1;
        Buffer one = {0};

        if (isspace(c)) {
            tb_space(tb);
            i++;
            continue;
        }

        if (!append_decoded_char(&one, s + i, n - i, &used)) {
            buf_clear(&one);
            i++;
            continue;
        }

        if (one.len == 1 && one.data[0] == ' ') {
            tb_space(tb);
        } else {
            if (tb->pending_space &&
                !(one.len == 1 && strchr(".,;:!?)]}", one.data[0]))) {
                buf_addc(&tb->out, ' ');
            }
            tb->pending_space = 0;
            buf_addn(&tb->out, one.data, one.len);
        }

        buf_clear(&one);
        i += used;
    }
}

static void tb_pre_bytes(TextBuilder *tb, const char *s, size_t n)
{
    size_t i = 0;

    if (tb->pending_space) {
        buf_addc(&tb->out, ' ');
        tb->pending_space = 0;
    }

    while (i < n) {
        size_t used = 1;
        Buffer one = {0};

        if (!append_decoded_char(&one, s + i, n - i, &used)) {
            buf_clear(&one);
            i++;
            continue;
        }
        buf_addn(&tb->out, one.data, one.len);
        buf_clear(&one);
        i += used;
    }
}

static void ab_space(AnchorBuilder *ab)
{
    if (!ab->text.len) return;
    ab->pending_space = 1;
}

static void ab_text_bytes(AnchorBuilder *ab, const char *s, size_t n)
{
    size_t i = 0;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t used = 1;
        Buffer one = {0};

        if (isspace(c)) {
            ab_space(ab);
            i++;
            continue;
        }

        if (!append_decoded_char(&one, s + i, n - i, &used)) {
            buf_clear(&one);
            i++;
            continue;
        }

        if (one.len == 1 && one.data[0] == ' ') {
            ab_space(ab);
        } else {
            if (ab->pending_space) {
                buf_addc(&ab->text, ' ');
                ab->pending_space = 0;
            }
            buf_addn(&ab->text, one.data, one.len);
        }

        buf_clear(&one);
        i += used;
    }
}

static void page_free(Page *p)
{
    size_t i;

    free(p->text);
    free(p->url);
    free(p->title);
    free(p->meta);
    for (i = 0; i < p->link_count; i++) {
        free(p->links[i].label);
        free(p->links[i].url);
    }
    for (i = 0; i < p->control_count; i++) {
        size_t j;

        free(p->controls[i].label);
        free(p->controls[i].name);
        free(p->controls[i].value);
        free(p->controls[i].form_action);
        free(p->controls[i].form_method);
        free(p->controls[i].form_enctype);
        for (j = 0; j < p->controls[i].option_count; j++) {
            free(p->controls[i].options[j]);
            free(p->controls[i].option_values[j]);
        }
        free(p->controls[i].options);
        free(p->controls[i].option_values);
    }
    free(p->links);
    free(p->controls);
    free(p->stops);
    free(p->display);
    memset(p, 0, sizeof(*p));
    p->layout_width = -1;
}

static int page_add_link(Page *p, const char *label, const char *url, size_t marker_offset)
{
    Link *links;
    size_t n;

    if (!url || !*url) return 0;
    if (p->link_count == p->link_cap) {
        n = p->link_cap ? p->link_cap * 2 : 32;
        links = realloc(p->links, n * sizeof(*links));
        if (!links) return 0;
        p->links = links;
        p->link_cap = n;
    }

    p->links[p->link_count].label = xstrdup_local(label && *label ? label : url);
    p->links[p->link_count].url = xstrdup_local(url);
    p->links[p->link_count].marker_offset = marker_offset;
    p->link_count++;
    return 1;
}

static void control_free(FormControl *c)
{
    size_t i;

    free(c->label);
    free(c->name);
    free(c->value);
    free(c->form_action);
    free(c->form_method);
    free(c->form_enctype);
    for (i = 0; i < c->option_count; i++) {
        free(c->options[i]);
        free(c->option_values[i]);
    }
    free(c->options);
    free(c->option_values);
    memset(c, 0, sizeof(*c));
}

static int control_add_option(FormControl *c, const char *label, const char *value)
{
    size_t n;

    if (c->option_count == c->option_cap) {
        n = c->option_cap ? c->option_cap * 2 : 8;
        c->options = xrealloc(c->options, n * sizeof(*c->options));
        c->option_values = xrealloc(c->option_values, n * sizeof(*c->option_values));
        c->option_cap = n;
    }
    c->options[c->option_count] = xstrdup_local(label && *label ? label : value);
    c->option_values[c->option_count] = xstrdup_local(value && *value ? value : label);
    c->option_count++;
    return 1;
}

static int page_add_control(Page *p, const FormControl *src)
{
    FormControl *controls;
    FormControl *dst;
    size_t n;
    size_t i;

    if (!src || src->disabled) return 0;
    if (p->control_count == p->control_cap) {
        n = p->control_cap ? p->control_cap * 2 : 16;
        controls = realloc(p->controls, n * sizeof(*controls));
        if (!controls) return 0;
        p->controls = controls;
        p->control_cap = n;
    }

    dst = &p->controls[p->control_count];
    memset(dst, 0, sizeof(*dst));
    dst->label = xstrdup_local(src->label && *src->label ? src->label : "Field");
    dst->name = xstrdup_local(src->name ? src->name : "");
    dst->value = xstrdup_local(src->value ? src->value : "");
    dst->form_action = xstrdup_local(src->form_action ? src->form_action : "");
    dst->form_method = xstrdup_local(src->form_method ? src->form_method : "GET");
    dst->form_enctype = xstrdup_local(src->form_enctype ? src->form_enctype :
                                      "application/x-www-form-urlencoded");
    dst->marker_offset = src->marker_offset;
    dst->display_len = src->display_len;
    dst->type = src->type;
    dst->form_index = src->form_index;
    dst->checked = src->checked;
    dst->selected = src->selected;
    dst->disabled = src->disabled;
    dst->button_submits = src->button_submits;
    for (i = 0; i < src->option_count; i++)
        control_add_option(dst, src->options[i], src->option_values[i]);
    p->control_count++;
    return 1;
}

static int page_clone(Page *dst, const Page *src)
{
    size_t i;

    memset(dst, 0, sizeof(*dst));
    dst->text = xstrdup_local(src->text ? src->text : "");
    dst->url = xstrdup_local(src->url ? src->url : "");
    dst->title = xstrdup_local(src->title ? src->title : "");
    dst->meta = xstrdup_local(src->meta ? src->meta : "");
    dst->layout_width = src->layout_width;

    for (i = 0; i < src->link_count; i++) {
        if (!page_add_link(dst, src->links[i].label, src->links[i].url,
                           src->links[i].marker_offset)) {
            page_free(dst);
            return 0;
        }
    }
    for (i = 0; i < src->control_count; i++) {
        if (!page_add_control(dst, &src->controls[i])) {
            page_free(dst);
            return 0;
        }
    }
    if (src->stop_count) {
        dst->stops = malloc(sizeof(*dst->stops) * src->stop_count);
        if (!dst->stops) {
            page_free(dst);
            return 0;
        }
        memcpy(dst->stops, src->stops, sizeof(*dst->stops) * src->stop_count);
        dst->stop_count = src->stop_count;
        dst->stop_cap = src->stop_count;
    }
    if (src->display_count) {
        dst->display = malloc(sizeof(*dst->display) * src->display_count);
        if (!dst->display) {
            page_free(dst);
            return 0;
        }
        memcpy(dst->display, src->display, sizeof(*dst->display) * src->display_count);
        dst->display_count = src->display_count;
        dst->display_cap = src->display_count;
    }
    return 1;
}

static const char *tag_name_start(const char *tag, const char *end, int *closing)
{
    const char *p = tag;

    *closing = 0;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p < end && *p == '/') {
        *closing = 1;
        p++;
    }
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static size_t tag_name_len(const char *p, const char *end)
{
    const char *start = p;

    while (p < end &&
           (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ':'))
        p++;
    return (size_t)(p - start);
}

static int tag_is(const char *name, size_t n, const char *want)
{
    size_t wn = strlen(want);

    return n == wn && ascii_eqn(name, want, wn);
}

static int tag_is_block(const char *name, size_t n)
{
    static const char *tags[] = {
        "address", "article", "aside", "blockquote", "body", "dd", "div",
        "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
        "header", "hr", "li", "main", "nav", "ol", "p", "pre", "section",
        "table", "tbody", "td", "tfoot", "th", "thead", "tr", "ul", NULL
    };
    int i;

    for (i = 0; tags[i]; i++) {
        if (tag_is(name, n, tags[i])) return 1;
    }
    return 0;
}

static int tag_is_heading(const char *name, size_t n)
{
    return n == 2 && (name[0] == 'h' || name[0] == 'H') &&
           name[1] >= '1' && name[1] <= '6';
}

static int tag_is_stripped_block(const char *name, size_t n)
{
    static const char *tags[] = {
        "head", "script", "style", "nav", "footer", "aside", "menu",
        "dialog", "template", "iframe", "canvas", "object",
        "embed", "svg", "noscript", NULL
    };
    int i;

    for (i = 0; tags[i]; i++) {
        if (tag_is(name, n, tags[i])) return 1;
    }
    return 0;
}

static const char *find_tag_end(const char *p, const char *end)
{
    char quote = 0;

    for (; p < end; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '>') {
            return p;
        }
    }
    return NULL;
}

static char *decode_attr_value(const char *s, size_t n)
{
    Buffer b = {0};
    size_t i = 0;

    while (i < n) {
        size_t used = 1;
        if (!append_decoded_char(&b, s + i, n - i, &used)) break;
        i += used;
    }
    return b.data ? b.data : xstrdup_local("");
}

static char *attr_value(const char *tag, const char *end, const char *name)
{
    const char *p;
    size_t want_len = strlen(name);

    p = tag;
    while (p < end) {
        const char *key;
        const char *value;
        size_t key_len;
        char quote = 0;

        while (p < end && (isspace((unsigned char)*p) || *p == '/')) p++;
        key = p;
        while (p < end &&
               (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ':'))
            p++;
        key_len = (size_t)(p - key);
        if (!key_len) {
            p++;
            continue;
        }

        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != '=') {
            continue;
        }
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;

        if (*p == '\'' || *p == '"') quote = *p++;
        value = p;
        while (p < end && (quote ? *p != quote :
               !isspace((unsigned char)*p) && *p != '/' && *p != '>'))
            p++;

        if (key_len == want_len && ascii_eqn(key, name, want_len))
            return decode_attr_value(value, (size_t)(p - value));

        if (quote && p < end && *p == quote) p++;
    }

    return NULL;
}

static int tag_is_clutter_candidate(const char *name, size_t n)
{
    static const char *tags[] = {
        "div", "section", "header", "ul", "ol", "table", NULL
    };
    int i;

    for (i = 0; tags[i]; i++) {
        if (tag_is(name, n, tags[i])) return 1;
    }
    return 0;
}

static int attr_value_is_clutter(const char *value)
{
    static const char *substring_terms[] = {
        "breadcrumb", "pagination", "toolbar", "cookie", "newsletter",
        "subscribe", "subscription", "related", "recommend", "recirc",
        "advert", "advertisement", "sponsor", "sponsored", "promo",
        "contentinfo", "complementary", "outbrain", "taboola", "paywall",
        "modal", "popup", "overlay", "login", "signin", "signup",
        "sign-in", "sign-up", "more-from", "also-read", "people-also",
        "author-bio", "bio-box", "tag-cloud", "affiliate",
        "language-selection", "infobox", "navbox", "ambox", "metadata",
        "vertical-navbox", "sidebar", "toc", "hatnote", "shortdescription",
        "mw-editsection", NULL
    };
    static const char *word_terms[] = {
        "nav", "menu", "sidebar", "rail", "social", "share", "sharing",
        "comment", "comments", "widget", "masthead", "footer", "ad",
        "ads", "bio", "links", "tags", NULL
    };
    int i;

    if (!value || !*value) return 0;
    if (text_contains_any(value, substring_terms)) return 1;
    for (i = 0; word_terms[i]; i++) {
        if (attr_contains_wordish(value, word_terms[i])) return 1;
    }
    return 0;
}

static int tag_has_clutter_attrs(const char *name, size_t n,
                                 const char *attrs, const char *end)
{
    static const char *attr_names[] = {
        "role", "class", "id", "aria-label", "data-block", NULL
    };
    int i;

    if (!tag_is_clutter_candidate(name, n)) return 0;

    for (i = 0; attr_names[i]; i++) {
        char *value = attr_value(attrs, end, attr_names[i]);
        int clutter = attr_value_is_clutter(value);

        free(value);
        if (clutter) return 1;
    }
    return 0;
}

static int tag_self_closing(const char *lt, const char *gt)
{
    const char *p = gt;

    while (p > lt && isspace((unsigned char)p[-1])) p--;
    return p > lt && p[-1] == '/';
}

static int tag_is_void_element(const char *name, size_t n)
{
    static const char *tags[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    int i;

    for (i = 0; tags[i]; i++) {
        if (tag_is(name, n, tags[i])) return 1;
    }
    return 0;
}

static char *site_prefix(const char *url)
{
    const char *scheme = strstr(url, "://");
    const char *host;
    const char *end;
    Buffer b = {0};

    if (!scheme) return NULL;
    host = scheme + 3;
    end = host + strcspn(host, "/?#");
    buf_addn(&b, url, (size_t)(end - url));
    return b.data;
}

static char *normalize_path_url(const char *url)
{
    const char *scheme = strstr(url, "://");
    const char *host;
    const char *path;
    const char *tail;
    Buffer out = {0};
    char **parts = NULL;
    size_t part_count = 0, part_cap = 0;
    char *path_copy;
    char *save = NULL;
    char *token;
    size_t i;

    if (!scheme) return xstrdup_local(url);
    host = scheme + 3;
    path = host + strcspn(host, "/?#");
    tail = path + strcspn(path, "?#");

    buf_addn(&out, url, (size_t)(path - url));
    if (path == tail) {
        buf_addc(&out, '/');
    } else {
        path_copy = xstrndup_local(path, (size_t)(tail - path));
        token = strtok_r(path_copy, "/", &save);
        while (token) {
            if (!strcmp(token, "..")) {
                if (part_count) part_count--;
            } else if (strcmp(token, ".") && *token) {
                if (part_count == part_cap) {
                    part_cap = part_cap ? part_cap * 2 : 16;
                    parts = xrealloc(parts, part_cap * sizeof(*parts));
                }
                parts[part_count++] = token;
            }
            token = strtok_r(NULL, "/", &save);
        }

        buf_addc(&out, '/');
        for (i = 0; i < part_count; i++) {
            if (i) buf_addc(&out, '/');
            buf_addn(&out, parts[i], strlen(parts[i]));
        }
        if (tail > path && tail[-1] == '/' && out.data[out.len - 1] != '/')
            buf_addc(&out, '/');

        free(parts);
        free(path_copy);
    }

    buf_addn(&out, tail, strlen(tail));
    return out.data;
}

static int ignored_href(const char *href)
{
    return !href || !*href || href[0] == '#' ||
           !strncasecmp(href, "javascript:", 11) ||
           !strncasecmp(href, "mailto:", 7) ||
           !strncasecmp(href, "tel:", 4) ||
           !strncasecmp(href, "data:", 5);
}

static long attr_long_value(const char *attrs, const char *end, const char *name)
{
    char *value = attr_value(attrs, end, name);
    char *tail = NULL;
    long n = 0;

    if (value && *value)
        n = strtol(value, &tail, 10);
    if (!value || tail == value)
        n = 0;
    free(value);
    return n;
}

static int attr_int_value_default(const char *attrs, const char *end,
                                  const char *name, int fallback)
{
    char *value = attr_value(attrs, end, name);
    char *tail = NULL;
    long n;

    if (!value || !*value) {
        free(value);
        return fallback;
    }
    n = strtol(value, &tail, 10);
    free(value);
    if (tail == value)
        return fallback;
    if (n < INT_MIN)
        return INT_MIN;
    if (n > INT_MAX)
        return INT_MAX;
    return (int)n;
}

static int image_attrs_are_clutter(const char *attrs, const char *end)
{
    static const char *bad_terms[] = {
        "avatar", "author", "bio", "icon", "logo", "sprite", "share",
        "social", "tracking", "pixel", "spacer", "advert", "ad-", "ads",
        "promo", "sponsor", "newsletter", NULL
    };
    char *class_value = attr_value(attrs, end, "class");
    char *id_value = attr_value(attrs, end, "id");
    long width = attr_long_value(attrs, end, "width");
    long height = attr_long_value(attrs, end, "height");
    int clutter = 0;

    if ((width > 0 && width <= 80 && height > 0 && height <= 80) ||
        text_contains_any(class_value, bad_terms) ||
        text_contains_any(id_value, bad_terms))
        clutter = 1;

    free(class_value);
    free(id_value);
    return clutter;
}

static char *clean_image_link_label(const char *label)
{
    const char *start;
    const char *end;
    char *trimmed = trim_copy(label);
    size_t len = strlen(trimmed);
    const char *suffix;

    if (!len) return trimmed;

    suffix = ci_find(trimmed, trimmed + len, " primary image");
    if (suffix && suffix + strlen(" primary image") == trimmed + len) {
        char *clean;

        *((char *)suffix) = 0;
        clean = trim_copy(trimmed);
        free(trimmed);
        trimmed = clean;
        len = strlen(trimmed);
    }

    start = ci_find(trimmed, trimmed + len, " with the text ");
    if (start) {
        const char *quote = strchr(start, '"');
        const char *quote_end;

        if (quote && (quote_end = strchr(quote + 1, '"')) != NULL) {
            char *quoted = xstrndup_local(quote + 1,
                                          (size_t)(quote_end - quote - 1));
            char *clean = trim_copy(quoted);

            free(quoted);
            free(trimmed);
            return clean;
        }
    }

    end = trimmed + len;
    if (len > 48 ||
        ci_find(trimmed, end, "an image of") ||
        ci_find(trimmed, end, "image of") ||
        ci_find(trimmed, end, "super imposed") ||
        ci_find(trimmed, end, "logo") ||
        ci_find(trimmed, end, "photo of") ||
        ci_find(trimmed, end, "picture of")) {
        trimmed[0] = 0;
    }
    return trimmed;
}

static void append_image_marker(TextBuilder *tb, AnchorBuilder *ab, int in_anchor,
                                const char *attrs, const char *end)
{
    char *alt;
    char *title;
    char *label;
    char marker[512];

    if (image_attrs_are_clutter(attrs, end)) return;

    alt = attr_value(attrs, end, "alt");
    title = attr_value(attrs, end, "title");
    label = trim_copy(alt && *alt ? alt : (title ? title : ""));

    if (!*label) {
        free(alt);
        free(title);
        free(label);
        return;
    }

    if (in_anchor) {
        char *clean = clean_image_link_label(label);

        if (*clean)
            ab_text_bytes(ab, clean, strlen(clean));
        free(clean);
    } else {
        snprintf(marker, sizeof(marker), "[Image: %.460s]", label);
        tb_block(tb);
        tb_text_bytes(tb, marker, strlen(marker));
        tb_block(tb);
    }

    free(alt);
    free(title);
    free(label);
}

static char *absolute_url(const char *base, const char *href)
{
    const char *scheme;
    const char *host;
    const char *base_end;
    const char *slash;
    char *prefix;
    char *raw;
    char *normalized;
    Buffer b = {0};

    if (ignored_href(href)) return NULL;
    if (starts_http_url(href)) return normalize_path_url(href);

    scheme = strstr(base, "://");
    if (!scheme) return NULL;

    if (href[0] == '/' && href[1] == '/') {
        buf_addn(&b, base, (size_t)(scheme - base));
        buf_addc(&b, ':');
        buf_addn(&b, href, strlen(href));
        normalized = normalize_path_url(b.data);
        free(b.data);
        return normalized;
    }

    prefix = site_prefix(base);
    if (!prefix) return NULL;

    if (href[0] == '/') {
        buf_addn(&b, prefix, strlen(prefix));
        buf_addn(&b, href, strlen(href));
        free(prefix);
        normalized = normalize_path_url(b.data);
        free(b.data);
        return normalized;
    }

    if (href[0] == '?') {
        base_end = base + strcspn(base, "?#");
        buf_addn(&b, base, (size_t)(base_end - base));
        buf_addn(&b, href, strlen(href));
        free(prefix);
        return b.data;
    }

    if (href[0] == '#') {
        base_end = base + strcspn(base, "#");
        buf_addn(&b, base, (size_t)(base_end - base));
        buf_addn(&b, href, strlen(href));
        free(prefix);
        return b.data;
    }

    host = scheme + 3;
    base_end = base + strcspn(base, "?#");
    slash = base_end;
    while (slash > host && slash[-1] != '/') slash--;

    if (slash <= host) {
        buf_addn(&b, prefix, strlen(prefix));
        buf_addc(&b, '/');
        raw = b.data;
        memset(&b, 0, sizeof(b));
    } else {
        raw = xstrndup_local(base, (size_t)(slash - base));
    }
    buf_addn(&b, raw, strlen(raw));
    buf_addn(&b, href, strlen(href));
    normalized = normalize_path_url(b.data);
    free(raw);
    free(prefix);
    free(b.data);
    return normalized;
}

static int anchor_label_is_clutter(const char *label)
{
    static const char *short_terms[] = {
        "advertisement", "comments", "comment", "facebook", "linkedin",
        "newsletter", "pinterest", "print", "reddit", "search", "share",
        "sign in", "sign up", "subscribe", "threads", "tweet", "twitter",
        "whatsapp", "x", "#", "¶", "permalink", "anchor", "section link",
        "open form target",
        NULL
    };
    char *trimmed = trim_copy(label);
    size_t len = strlen(trimmed);
    int clutter = 0;
    int i;

    if (!len) {
        free(trimmed);
        return 1;
    }

    if (len <= 80) {
        for (i = 0; short_terms[i]; i++) {
            if (!strcasecmp(trimmed, short_terms[i]) ||
                attr_contains_wordish(trimmed, short_terms[i])) {
                clutter = 1;
                break;
            }
        }
    }

    free(trimmed);
    return clutter;
}

static int anchor_href_is_clutter(const char *href)
{
    static const char *terms[] = {
        "/account", "/login", "/print",
        "/register", "/share", "/signin", "/sign-in", "/signup", "/sign-up",
        "/subscribe", "facebook.com/sharer", "linkedin.com/share",
        "pinterest.com/pin", "reddit.com/submit", "twitter.com/intent",
        "x.com/intent", "whatsapp://", "mailto:?subject", NULL
    };

    return text_contains_any(href, terms);
}

static void finish_anchor(Page *page, TextBuilder *tb, AnchorBuilder *ab,
                          char **href, const char *base_url)
{
    char *label;
    char *resolved;
    size_t marker_offset;

    while (ab->text.len && ab->text.data[ab->text.len - 1] == ' ')
        ab->text.data[--ab->text.len] = 0;

    resolved = *href ? absolute_url(base_url, *href) : NULL;
    label = trim_copy(ab->text.data && *ab->text.data ? ab->text.data : "");

    if (*label && !anchor_label_is_clutter(label) &&
        !anchor_href_is_clutter(*href ? *href : "") &&
        !anchor_href_is_clutter(resolved ? resolved : "")) {
        if (tb->pending_space) {
            buf_addc(&tb->out, ' ');
            tb->pending_space = 0;
        }
        marker_offset = tb->out.len;
        buf_addn(&tb->out, label, strlen(label));
        if (resolved)
            page_add_link(page, label, resolved, marker_offset);
        tb_space(tb);
    }

    free(label);
    free(resolved);
    free(*href);
    *href = NULL;
    buf_clear(&ab->text);
    ab->pending_space = 0;
}

static char *plain_text_from_html_fragment(const char *html, size_t len);

static int tag_has_attr(const char *tag, const char *end, const char *name)
{
    const char *p = tag;
    size_t want_len = strlen(name);

    while (p < end) {
        const char *key;
        size_t key_len;

        while (p < end && (isspace((unsigned char)*p) || *p == '/' || *p == '<')) p++;
        key = p;
        while (p < end &&
               (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ':'))
            p++;
        key_len = (size_t)(p - key);
        if (key_len == want_len && ascii_eqn(key, name, want_len))
            return 1;
        while (p < end && !isspace((unsigned char)*p) && *p != '>') {
            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                while (p < end && *p != quote) p++;
                if (p < end) p++;
            } else {
                p++;
            }
        }
    }
    return 0;
}

static char *first_nonempty_attr(const char *attrs, const char *end,
                                 const char *a, const char *b,
                                 const char *c, const char *d,
                                 const char *e, const char *f)
{
    const char *names[] = { a, b, c, d, e, f, NULL };
    int i;

    for (i = 0; names[i]; i++) {
        char *value = attr_value(attrs, end, names[i]);
        char *trimmed;

        if (!value) continue;
        trimmed = trim_copy(value);
        free(value);
        if (*trimmed)
            return trimmed;
        free(trimmed);
    }
    return xstrdup_local("");
}

static int input_type_from_attrs(const char *attrs, const char *end)
{
    char *type = attr_value(attrs, end, "type");
    int out = CONTROL_TEXT;

    if (!type || !*type || !strcasecmp(type, "text"))
        out = CONTROL_TEXT;
    else if (!strcasecmp(type, "search"))
        out = CONTROL_SEARCH;
    else if (!strcasecmp(type, "password"))
        out = CONTROL_PASSWORD;
    else if (!strcasecmp(type, "email"))
        out = CONTROL_EMAIL;
    else if (!strcasecmp(type, "url"))
        out = CONTROL_URL;
    else if (!strcasecmp(type, "number"))
        out = CONTROL_NUMBER;
    else if (!strcasecmp(type, "checkbox"))
        out = CONTROL_CHECKBOX;
    else if (!strcasecmp(type, "radio"))
        out = CONTROL_RADIO;
    else if (!strcasecmp(type, "submit"))
        out = CONTROL_SUBMIT;
    else if (!strcasecmp(type, "button"))
        out = CONTROL_BUTTON;
    else
        out = -1;

    free(type);
    return out;
}

static const char *control_type_name(const FormControl *c)
{
    switch (c ? c->type : CONTROL_TEXT) {
    case CONTROL_SEARCH: return "Search";
    case CONTROL_PASSWORD: return "Password";
    case CONTROL_EMAIL: return "Email";
    case CONTROL_URL: return "URL";
    case CONTROL_NUMBER: return "Number";
    case CONTROL_TEXTAREA: return "Text";
    case CONTROL_SELECT: return "Select";
    case CONTROL_CHECKBOX: return "Checkbox";
    case CONTROL_RADIO: return "Radio";
    case CONTROL_SUBMIT: return "Submit";
    case CONTROL_BUTTON: return "Button";
    default: return "Text";
    }
}

static int control_is_textual(const FormControl *c)
{
    if (!c) return 0;
    return c->type == CONTROL_TEXT || c->type == CONTROL_SEARCH ||
           c->type == CONTROL_PASSWORD || c->type == CONTROL_EMAIL ||
           c->type == CONTROL_URL || c->type == CONTROL_NUMBER ||
           c->type == CONTROL_TEXTAREA;
}

static int control_is_checkable(const FormControl *c)
{
    return c && (c->type == CONTROL_CHECKBOX || c->type == CONTROL_RADIO);
}

static char *control_label_from_attrs(const char *attrs, const char *end,
                                      const char *fallback)
{
    char *label = first_nonempty_attr(attrs, end, "data-simplebrowse-label",
                                     "aria-label", "placeholder", "title",
                                     "value", "name");

    if (!*label) {
        free(label);
        label = first_nonempty_attr(attrs, end, "id", NULL, NULL, NULL, NULL, NULL);
    }
    if (!*label && fallback) {
        free(label);
        label = xstrdup_local(fallback);
    }
    return label;
}

static int control_label_is_noise(const char *label)
{
    char *trimmed = trim_copy(label);
    size_t len = strlen(trimmed);
    int noise = 0;

    if (!len)
        noise = 1;
    if (!noise && len > 140)
        noise = 1;
    if (!noise && ((trimmed[0] == '_' && (trimmed[1] == 'R' || trimmed[1] == 'r') &&
                    trimmed[2] == '_') ||
                   !strncasecmp(trimmed, "radix-", 6) ||
                   ci_find(trimmed, trimmed + len, "{transition") ||
                   ci_find(trimmed, trimmed + len, "var(--") ||
                   ci_find(trimmed, trimmed + len, "clip-path:") ||
                   ci_find(trimmed, trimmed + len, "display:flex") ||
                   ci_find(trimmed, trimmed + len, "scroll carousel") ||
                   ci_find(trimmed, trimmed + len, "open column options")))
        noise = 1;
    free(trimmed);
    return noise;
}

static void lower_ascii_inplace(char *s)
{
    while (s && *s) {
        *s = (char)tolower((unsigned char)*s);
        s++;
    }
}

static char *form_attr_url(const char *attrs, const char *end, const char *base_url)
{
    char *action = attr_value(attrs, end, "action");
    char *resolved = NULL;

    if (action && *action)
        resolved = absolute_url(base_url, action);
    if (!resolved)
        resolved = xstrdup_local(base_url);
    free(action);
    return resolved;
}

static char *form_attr_token(const char *attrs, const char *end, const char *name,
                             const char *fallback)
{
    char *value = attr_value(attrs, end, name);
    char *trimmed;

    if (!value || !*value) {
        free(value);
        return xstrdup_local(fallback);
    }
    trimmed = trim_copy(value);
    free(value);
    if (!*trimmed) {
        free(trimmed);
        return xstrdup_local(fallback);
    }
    lower_ascii_inplace(trimmed);
    return trimmed;
}

static void control_set_form_defaults(FormControl *c, const char *action,
                                      const char *method, const char *enctype,
                                      int form_index)
{
    c->form_action = xstrdup_local(action && *action ? action : "");
    c->form_method = xstrdup_local(method && *method ? method : "get");
    c->form_enctype = xstrdup_local(enctype && *enctype ? enctype :
                                    "application/x-www-form-urlencoded");
    c->form_index = form_index;
}

static void append_control_marker(Page *page, TextBuilder *tb, FormControl *c)
{
    Buffer line = {0};
    const char *label = c->label && *c->label ? c->label : control_type_name(c);
    const char *selected = "";

    if (control_label_is_noise(label))
        return;

    tb_block(tb);
    c->marker_offset = tb->out.len;

    if (c->type == CONTROL_CHECKBOX || c->type == CONTROL_RADIO) {
        buf_addn(&line, c->checked ? "[x] " : "[ ] ", 4);
        buf_addn(&line, label, strlen(label));
    } else if (c->type == CONTROL_SELECT) {
        if (c->option_count) {
            size_t idx = c->selected >= 0 &&
                         (size_t)c->selected < c->option_count ?
                         (size_t)c->selected : 0;
            selected = c->options[idx] ? c->options[idx] : "";
        }
        buf_addn(&line, label, strlen(label));
        buf_addn(&line, ": [", 3);
        buf_addn(&line, *selected ? selected : "select", strlen(*selected ? selected : "select"));
        buf_addc(&line, ']');
    } else if (c->type == CONTROL_SUBMIT || c->type == CONTROL_BUTTON) {
        buf_addn(&line, "[", 1);
        buf_addn(&line, label, strlen(label));
        buf_addn(&line, "]", 1);
    } else {
        buf_addn(&line, label, strlen(label));
        buf_addn(&line, ": [", 3);
        buf_addn(&line, control_type_name(c), strlen(control_type_name(c)));
        buf_addc(&line, ']');
    }

    c->display_len = line.len;
    buf_addn(&tb->out, line.data, line.len);
    tb->pending_space = 0;
    tb_newline(tb);
    page_add_control(page, c);
    buf_clear(&line);
}

static void init_input_control(FormControl *c, const char *attrs, const char *end,
                               const char *action, const char *method,
                               const char *enctype, int form_index)
{
    char *name = attr_value(attrs, end, "name");
    char *value = attr_value(attrs, end, "value");
    int type = input_type_from_attrs(attrs, end);

    memset(c, 0, sizeof(*c));
    c->type = type;
    c->name = name ? name : xstrdup_local("");
    c->value = value ? value : xstrdup_local("");
    c->label = control_label_from_attrs(attrs, end, control_type_name(c));
    c->checked = tag_has_attr(attrs, end, "checked");
    c->disabled = tag_has_attr(attrs, end, "disabled");
    c->selected = -1;
    c->button_submits = type == CONTROL_SUBMIT;
    control_set_form_defaults(c, action, method, enctype, form_index);
}

static void skip_element_content(const char **cursor, const char *end, const char *name)
{
    const char *p = *cursor;
    size_t want_len = strlen(name);
    int depth = 1;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *gt;
        const char *tag_name;
        size_t tag_len;
        int closing;

        if (!lt) break;
        if (end - lt >= 4 && !memcmp(lt, "<!--", 4)) {
            const char *comment_end = ci_find(lt + 4, end, "-->");
            p = comment_end ? comment_end + 3 : end;
            continue;
        }
        gt = find_tag_end(lt + 1, end);
        if (!gt) break;
        tag_name = tag_name_start(lt + 1, gt, &closing);
        tag_len = tag_name_len(tag_name, gt);
        if (tag_len == want_len && ascii_eqn(tag_name, name, want_len)) {
            if (closing) {
                depth--;
                if (depth <= 0) {
                    *cursor = gt + 1;
                    return;
                }
            } else if (!tag_self_closing(lt, gt) &&
                       !tag_is_void_element(tag_name, tag_len)) {
                depth++;
            }
        }
        p = gt + 1;
    }

    *cursor = end;
}

static void text_stats(const char *s, size_t n, long *chars, long *punct)
{
    size_t i;
    int in_space = 1;

    *chars = 0;
    *punct = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (isspace(c)) {
            in_space = 1;
            continue;
        }
        if (c == '<') break;
        if (in_space && *chars > 0)
            (*chars)++;
        in_space = 0;
        (*chars)++;
        if (c == '.' || c == '?' || c == '!' || c == ';')
            (*punct)++;
    }
}

static void region_add_text(RegionFrame *stack, int stack_count,
                            const char *s, size_t n, int in_link)
{
    long chars;
    long punct;
    int i;

    text_stats(s, n, &chars, &punct);
    if (chars <= 0) return;

    for (i = 0; i < stack_count; i++) {
        stack[i].text_chars += chars;
        stack[i].punctuation += punct;
        if (in_link)
            stack[i].link_chars += chars;
    }
}

static void region_add_to_open_frames(RegionFrame *stack, int stack_count,
                                      const char *name, size_t name_len)
{
    int i;

    for (i = 0; i < stack_count; i++) {
        if (tag_is(name, name_len, "p"))
            stack[i].paragraphs++;
        else if (tag_is_heading(name, name_len))
            stack[i].headings++;
        else if (tag_is(name, name_len, "li"))
            stack[i].list_items++;
        else if (tag_is(name, name_len, "img"))
            stack[i].images++;
    }
}

static void region_note_child_article(RegionFrame *stack, int stack_count)
{
    int i;

    for (i = 0; i < stack_count; i++) {
        if (strcmp(stack[i].tag, "article"))
            stack[i].child_articles++;
    }
}

static int tag_is_region_candidate(const char *name, size_t n)
{
    return tag_is(name, n, "article") ||
           tag_is(name, n, "main") ||
           tag_is(name, n, "section") ||
           tag_is(name, n, "div") ||
           tag_is(name, n, "body");
}

static long region_attr_score(const char *name, size_t name_len,
                              const char *attrs, const char *end,
                              int *clutter)
{
    static const char *positive_terms[] = {
        "article", "article-body", "article-content", "article__body",
        "article__content", "entry-content", "post-content", "story-body",
        "story-content", "main-content", "page-content", "body-content",
        "articlebody", "articleBody", "field-body", "rich-text",
        "markdown", "prose", "documentwrapper", "bodywrapper", "essay",
        "story", "post", "mw-parser-output", "mw-body-content",
        "mw-content-text", NULL
    };
    char *role = attr_value(attrs, end, "role");
    char *class_value = attr_value(attrs, end, "class");
    char *id_value = attr_value(attrs, end, "id");
    char *itemprop = attr_value(attrs, end, "itemprop");
    long score = 0;

    *clutter = 0;

    if (tag_is(name, name_len, "article"))
        score += 8000;
    else if (tag_is(name, name_len, "main"))
        score += 8000;
    else if (tag_is(name, name_len, "section"))
        score += 150;
    else if (tag_is(name, name_len, "body"))
        score -= 700;

    if ((role && (attr_contains_wordish(role, "main") ||
                  attr_contains_wordish(role, "article"))) ||
        (itemprop && text_contains_any(itemprop, positive_terms))) {
        score += 8000;
    }
    if ((class_value && attr_contains_wordish(class_value, "main")) ||
        (id_value && attr_contains_wordish(id_value, "main")))
        score += 8000;

    if (class_value && text_contains_any(class_value, positive_terms))
        score += 900;
    if (class_value && attr_contains_wordish(class_value, "mw-parser-output"))
        score += 12000;
    if (id_value && text_contains_any(id_value, positive_terms))
        score += 700;
    if (id_value && attr_contains_wordish(id_value, "mw-content-text"))
        score += 12000;
    if ((class_value && attr_contains_wordish(class_value, "body")) ||
        (id_value && attr_contains_wordish(id_value, "body")))
        score += 800;
    if ((class_value && attr_contains_wordish(class_value, "content")) ||
        (id_value && attr_contains_wordish(id_value, "content")))
        score += 350;

    if (attr_value_is_clutter(role) ||
        attr_value_is_clutter(class_value) ||
        attr_value_is_clutter(id_value)) {
        score -= 5000;
        *clutter = 1;
    }

    free(role);
    free(class_value);
    free(id_value);
    free(itemprop);
    return score;
}

static long score_region(const RegionFrame *f)
{
    long score;
    long link_pct = 0;

    if (f->text_chars <= 0) return -1000000;
    if (f->text_chars < 160 && f->paragraphs < 2 && f->headings < 1)
        return -1000000;

    score = f->base_score +
            f->text_chars +
            f->paragraphs * 520 +
            f->headings * 120 +
            f->punctuation * 28 +
            f->images * 40;

    if (f->paragraphs >= 3)
        score += f->paragraphs * 900;
    if (f->paragraphs == 0 && f->list_items + f->headings > 3)
        score -= 4000;

    if (f->child_articles >= 2 &&
        (!strcmp(f->tag, "main") || f->base_score >= 8000)) {
        score += f->child_articles * 3200;
        if (f->headings >= f->child_articles)
            score += f->child_articles * 800;
    }

    if (f->text_chars > 0)
        link_pct = (f->link_chars * 100) / f->text_chars;

    if (link_pct > 12)
        score -= (link_pct - 12) * 85;
    if (link_pct > 45)
        score -= 5000;
    if (f->list_items > f->paragraphs * 4 + 8 && link_pct > 15)
        score -= 3200;
    if (f->clutter)
        score -= 5000;

    return score;
}

static int frame_is_semantic_region(const RegionFrame *f)
{
    return !strcmp(f->tag, "main") || !strcmp(f->tag, "article") ||
           f->base_score >= 8000;
}

static void update_reader_best(ReaderRegion *best, ReaderRegion *semantic_best,
                               const RegionFrame *f, const char *end)
{
    long score = score_region(f);
    int listing = f->child_articles >= 2 &&
                  (!strcmp(f->tag, "main") || f->base_score >= 8000);

    if (score > best->score) {
        best->start = f->start;
        best->end = end;
        best->listing = listing;
        best->score = score;
        best->found = 1;
    }
    if (frame_is_semantic_region(f) && score > semantic_best->score) {
        semantic_best->start = f->start;
        semantic_best->end = end;
        semantic_best->listing = listing;
        semantic_best->score = score;
        semantic_best->found = 1;
    }
}

static void close_region_frame(ReaderRegion *best, ReaderRegion *semantic_best,
                               RegionFrame *stack, int *stack_count,
                               const char *name, size_t name_len,
                               const char *end)
{
    int i;

    for (i = *stack_count - 1; i >= 0; i--) {
        if (strlen(stack[i].tag) != name_len ||
            !ascii_eqn(stack[i].tag, name, name_len))
            continue;

        while (*stack_count - 1 >= i) {
            update_reader_best(best, semantic_best, &stack[*stack_count - 1], end);
            (*stack_count)--;
        }
        return;
    }
}

static ReaderRegion select_reader_region(const char *html, size_t len)
{
    const char *p = html;
    const char *end = html + len;
    ReaderRegion best = { html, end, 0, 0, -1000000 };
    ReaderRegion semantic_best = { html, end, 0, 0, -1000000 };
    RegionFrame stack[128];
    int stack_count = 0;
    int link_depth = 0;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *gt;
        const char *name;
        size_t name_len;
        int closing;

        if (!lt) {
            region_add_text(stack, stack_count, p, (size_t)(end - p),
                            link_depth > 0);
            break;
        }

        if (lt > p)
            region_add_text(stack, stack_count, p, (size_t)(lt - p),
                            link_depth > 0);

        if (end - lt >= 4 && !memcmp(lt, "<!--", 4)) {
            const char *comment_end = ci_find(lt + 4, end, "-->");
            p = comment_end ? comment_end + 3 : end;
            continue;
        }

        gt = find_tag_end(lt + 1, end);
        if (!gt) break;

        name = tag_name_start(lt + 1, gt, &closing);
        name_len = tag_name_len(name, gt);
        if (!name_len || *name == '!' || *name == '?') {
            p = gt + 1;
            continue;
        }

        if (closing) {
            if (tag_is(name, name_len, "a") && link_depth > 0)
                link_depth--;
            if (tag_is_region_candidate(name, name_len))
                close_region_frame(&best, &semantic_best, stack, &stack_count, name,
                                   name_len, gt + 1);
            p = gt + 1;
            continue;
        }

        if (tag_is_stripped_block(name, name_len) ||
            tag_has_clutter_attrs(name, name_len, name + name_len, gt)) {
            char tag[32];
            size_t copy_len = name_len < sizeof(tag) - 1 ? name_len : sizeof(tag) - 1;

            memcpy(tag, name, copy_len);
            tag[copy_len] = 0;
            p = gt + 1;
            if (!tag_self_closing(lt, gt) &&
                !tag_is_void_element(name, name_len))
                skip_element_content(&p, end, tag);
            continue;
        }

        if (tag_is_region_candidate(name, name_len) &&
            !tag_self_closing(lt, gt) &&
            !tag_is_void_element(name, name_len) &&
            stack_count < (int)(sizeof(stack) / sizeof(stack[0]))) {
            RegionFrame *f = &stack[stack_count++];
            int clutter = 0;
            size_t copy_len = name_len < sizeof(f->tag) - 1 ? name_len : sizeof(f->tag) - 1;

            memset(f, 0, sizeof(*f));
            memcpy(f->tag, name, copy_len);
            f->tag[copy_len] = 0;
            f->start = lt;
            f->base_score = region_attr_score(name, name_len, name + name_len,
                                              gt, &clutter);
            f->clutter = clutter;
        }

        if (tag_is(name, name_len, "article"))
            region_note_child_article(stack, stack_count);

        region_add_to_open_frames(stack, stack_count, name, name_len);

        if (tag_is(name, name_len, "a") &&
            !tag_self_closing(lt, gt) &&
            !tag_is_void_element(name, name_len))
            link_depth++;

        p = gt + 1;
    }

    while (stack_count > 0) {
        update_reader_best(&best, &semantic_best, &stack[stack_count - 1], end);
        stack_count--;
    }

    if (semantic_best.found && semantic_best.score > 500)
        return semantic_best;

    if (!best.found || best.score < 300)
        best = (ReaderRegion){ html, end, 1, 0, 0 };

    return best;
}

static Page parse_html_fragment(const char *html, size_t len, const char *base_url)
{
    Page page = {0};
    TextBuilder tb = {0};
    AnchorBuilder anchor = {0};
    AnchorBuilder textarea_text = {0};
    AnchorBuilder option_text = {0};
    char *anchor_href = NULL;
    char *form_action = NULL;
    char *form_method = NULL;
    char *form_enctype = NULL;
    char *option_value = NULL;
    const char *p = html;
    const char *end = html + len;
    int in_anchor = 0;
    int in_textarea = 0;
    int in_select = 0;
    int in_option = 0;
    int option_selected = 0;
    int form_depth = 0;
    int current_form_index = -1;
    int next_form_index = 0;
    int pre_depth = 0;
    FormControl textarea_control = {0};
    FormControl select_control = {0};

    page.layout_width = -1;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *gt;
        const char *name;
        size_t name_len;
        int closing;

        if (!lt) {
            if (in_option) ab_text_bytes(&option_text, p, (size_t)(end - p));
            else if (in_textarea) ab_text_bytes(&textarea_text, p, (size_t)(end - p));
            else if (in_anchor) ab_text_bytes(&anchor, p, (size_t)(end - p));
            else if (pre_depth) tb_pre_bytes(&tb, p, (size_t)(end - p));
            else tb_text_bytes(&tb, p, (size_t)(end - p));
            break;
        }

        if (lt > p) {
            if (in_option) ab_text_bytes(&option_text, p, (size_t)(lt - p));
            else if (in_textarea) ab_text_bytes(&textarea_text, p, (size_t)(lt - p));
            else if (in_anchor) ab_text_bytes(&anchor, p, (size_t)(lt - p));
            else if (pre_depth) tb_pre_bytes(&tb, p, (size_t)(lt - p));
            else tb_text_bytes(&tb, p, (size_t)(lt - p));
        }

        if (end - lt >= 4 && !memcmp(lt, "<!--", 4)) {
            const char *comment_end = ci_find(lt + 4, end, "-->");
            p = comment_end ? comment_end + 3 : end;
            continue;
        }

        gt = find_tag_end(lt + 1, end);
        if (!gt) break;

        name = tag_name_start(lt + 1, gt, &closing);
        name_len = tag_name_len(name, gt);
        if (!name_len || *name == '!' || *name == '?') {
            p = gt + 1;
            continue;
        }

        if (tag_is(name, name_len, "form")) {
            if (closing) {
                if (form_depth > 0) form_depth--;
                if (form_depth == 0) {
                    free(form_action);
                    free(form_method);
                    free(form_enctype);
                    form_action = NULL;
                    form_method = NULL;
                    form_enctype = NULL;
                    current_form_index = -1;
                }
                tb_block(&tb);
            } else {
                int browser_form_index = attr_int_value_default(name + name_len, gt,
                                                                "data-simplebrowse-form-index",
                                                                -1);
                free(form_action);
                free(form_method);
                free(form_enctype);
                form_action = form_attr_url(name + name_len, gt, base_url);
                form_method = form_attr_token(name + name_len, gt, "method", "get");
                form_enctype = form_attr_token(name + name_len, gt, "enctype",
                                               "application/x-www-form-urlencoded");
                if (browser_form_index >= 0) {
                    current_form_index = browser_form_index;
                    if (browser_form_index >= next_form_index)
                        next_form_index = browser_form_index + 1;
                } else {
                    current_form_index = next_form_index++;
                }
                form_depth++;
                tb_block(&tb);
            }
            p = gt + 1;
            continue;
        }

        if (!closing &&
            (tag_is_stripped_block(name, name_len) ||
             (!loose_html_parse &&
              tag_has_clutter_attrs(name, name_len, name + name_len, gt)))) {
            char tag[32];
            size_t copy_len = name_len < sizeof(tag) - 1 ? name_len : sizeof(tag) - 1;

            memcpy(tag, name, copy_len);
            tag[copy_len] = 0;
            p = gt + 1;
            if (!tag_self_closing(lt, gt) &&
                !tag_is_void_element(name, name_len))
                skip_element_content(&p, end, tag);
            continue;
        }

        if (tag_is(name, name_len, "input") && !closing) {
            FormControl c;
            int type = input_type_from_attrs(name + name_len, gt);

            if (type >= 0) {
                init_input_control(&c, name + name_len, gt,
                                   form_action ? form_action : base_url,
                                   form_method ? form_method : "get",
                                   form_enctype ? form_enctype :
                                   "application/x-www-form-urlencoded",
                                   current_form_index);
                if (c.type >= 0 && !c.disabled)
                    append_control_marker(&page, &tb, &c);
                control_free(&c);
            }
        } else if (tag_is(name, name_len, "textarea")) {
            if (closing) {
                char *value;

                if (in_textarea) {
                    value = trim_copy(textarea_text.text.data ? textarea_text.text.data : "");
                    textarea_control.value = value;
                    append_control_marker(&page, &tb, &textarea_control);
                    control_free(&textarea_control);
                    buf_clear(&textarea_text.text);
                    textarea_text.pending_space = 0;
                    in_textarea = 0;
                }
            } else {
                memset(&textarea_control, 0, sizeof(textarea_control));
                textarea_control.type = CONTROL_TEXTAREA;
                textarea_control.name = attr_value(name + name_len, gt, "name");
                if (!textarea_control.name) textarea_control.name = xstrdup_local("");
                textarea_control.value = xstrdup_local("");
                textarea_control.label = control_label_from_attrs(name + name_len, gt, "Text");
                textarea_control.selected = -1;
                textarea_control.disabled = tag_has_attr(name + name_len, gt, "disabled");
                control_set_form_defaults(&textarea_control,
                                          form_action ? form_action : base_url,
                                          form_method ? form_method : "get",
                                          form_enctype ? form_enctype :
                                          "application/x-www-form-urlencoded",
                                          current_form_index);
                buf_clear(&textarea_text.text);
                textarea_text.pending_space = 0;
                in_textarea = 1;
            }
        } else if (tag_is(name, name_len, "select")) {
            if (closing) {
                if (in_select) {
                    if (select_control.selected < 0 && select_control.option_count)
                        select_control.selected = 0;
                    if (select_control.option_count &&
                        (size_t)select_control.selected < select_control.option_count) {
                        free(select_control.value);
                        select_control.value = xstrdup_local(
                            select_control.option_values[select_control.selected]);
                    }
                    append_control_marker(&page, &tb, &select_control);
                    control_free(&select_control);
                    in_select = 0;
                }
            } else {
                memset(&select_control, 0, sizeof(select_control));
                select_control.type = CONTROL_SELECT;
                select_control.name = attr_value(name + name_len, gt, "name");
                if (!select_control.name) select_control.name = xstrdup_local("");
                select_control.value = xstrdup_local("");
                select_control.label = control_label_from_attrs(name + name_len, gt, "Select");
                select_control.selected = -1;
                select_control.disabled = tag_has_attr(name + name_len, gt, "disabled");
                control_set_form_defaults(&select_control,
                                          form_action ? form_action : base_url,
                                          form_method ? form_method : "get",
                                          form_enctype ? form_enctype :
                                          "application/x-www-form-urlencoded",
                                          current_form_index);
                in_select = 1;
            }
        } else if (tag_is(name, name_len, "option") && in_select) {
            if (closing) {
                char *label;

                if (in_option) {
                    label = trim_copy(option_text.text.data ? option_text.text.data : "");
                    control_add_option(&select_control, label,
                                       option_value && *option_value ? option_value : label);
                    if (option_selected)
                        select_control.selected = (int)select_control.option_count - 1;
                    free(label);
                    free(option_value);
                    option_value = NULL;
                    buf_clear(&option_text.text);
                    option_text.pending_space = 0;
                    option_selected = 0;
                    in_option = 0;
                }
            } else {
                free(option_value);
                option_value = attr_value(name + name_len, gt, "value");
                if (!option_value) option_value = xstrdup_local("");
                option_selected = tag_has_attr(name + name_len, gt, "selected");
                buf_clear(&option_text.text);
                option_text.pending_space = 0;
                in_option = 1;
            }
        } else if (tag_is(name, name_len, "button") && !closing) {
            FormControl c;
            char *type = form_attr_token(name + name_len, gt, "type", "submit");
            char *label = control_label_from_attrs(name + name_len, gt, "");
            const char *after = gt + 1;
            const char *close = ci_find(after, end, "</button");

            if (!*label && close) {
                free(label);
                label = plain_text_from_html_fragment(after, (size_t)(close - after));
            }
            memset(&c, 0, sizeof(c));
            c.type = !strcasecmp(type, "button") ? CONTROL_BUTTON : CONTROL_SUBMIT;
            c.name = attr_value(name + name_len, gt, "name");
            if (!c.name) c.name = xstrdup_local("");
            c.value = attr_value(name + name_len, gt, "value");
            if (!c.value) c.value = xstrdup_local(label);
            c.label = *label ? label : xstrdup_local(c.type == CONTROL_BUTTON ? "Button" : "Submit");
            c.selected = -1;
            c.disabled = tag_has_attr(name + name_len, gt, "disabled");
            c.button_submits = c.type == CONTROL_SUBMIT;
            control_set_form_defaults(&c, form_action ? form_action : base_url,
                                      form_method ? form_method : "get",
                                      form_enctype ? form_enctype :
                                      "application/x-www-form-urlencoded",
                                      current_form_index);
            if (!c.disabled)
                append_control_marker(&page, &tb, &c);
            control_free(&c);
            free(type);
            if (close) {
                const char *button_gt = find_tag_end(close + 2, end);
                p = button_gt ? button_gt + 1 : end;
                continue;
            }
        } else if (tag_is(name, name_len, "br")) {
            if (in_anchor) ab_space(&anchor);
            else tb_br(&tb);
        } else if (tag_is(name, name_len, "img") && !closing) {
            append_image_marker(&tb, &anchor, in_anchor, name + name_len, gt);
        } else if (tag_is(name, name_len, "a")) {
            if (closing) {
                if (in_anchor) {
                    finish_anchor(&page, &tb, &anchor, &anchor_href, base_url);
                    in_anchor = 0;
                }
            } else if (!in_anchor) {
                anchor_href = attr_value(name + name_len, gt, "href");
                in_anchor = 1;
            }
        } else if (tag_is(name, name_len, "pre")) {
            if (closing) {
                if (pre_depth > 0) pre_depth--;
                tb_block(&tb);
            } else {
                tb_block(&tb);
                pre_depth++;
            }
        } else if (tag_is(name, name_len, "blockquote")) {
            if (!closing) {
                tb_block(&tb);
                tb_text_bytes(&tb, "> ", 2);
            } else {
                tb_block(&tb);
            }
        } else if (tag_is_heading(name, name_len)) {
            if (closing) tb_block(&tb);
            else tb_block(&tb);
        } else if (tag_is_block(name, name_len)) {
            if (tag_is(name, name_len, "li") && !closing) {
                tb_newline(&tb);
                tb_text_bytes(&tb, "- ", 2);
            } else {
                tb_block(&tb);
            }
        }

        p = gt + 1;
    }

    if (in_anchor)
        finish_anchor(&page, &tb, &anchor, &anchor_href, base_url);
    if (in_textarea) {
        textarea_control.value = trim_copy(textarea_text.text.data ? textarea_text.text.data : "");
        append_control_marker(&page, &tb, &textarea_control);
        control_free(&textarea_control);
    }
    if (in_select) {
        if (select_control.selected < 0 && select_control.option_count)
            select_control.selected = 0;
        if (select_control.option_count &&
            (size_t)select_control.selected < select_control.option_count) {
            free(select_control.value);
            select_control.value = xstrdup_local(
                select_control.option_values[select_control.selected]);
        }
        append_control_marker(&page, &tb, &select_control);
        control_free(&select_control);
    }

    tb_trim_space(&tb);
    while (tb.out.len && tb.out.data[tb.out.len - 1] == '\n')
        tb.out.data[--tb.out.len] = 0;
    if (!tb.out.data)
        tb_text_bytes(&tb, "(empty page)", 12);

    page.text = tb.out.data ? tb.out.data : xstrdup_local("(empty page)");
    page.url = xstrdup_local(base_url);
    buf_clear(&anchor.text);
    buf_clear(&textarea_text.text);
    buf_clear(&option_text.text);
    free(anchor_href);
    free(form_action);
    free(form_method);
    free(form_enctype);
    free(option_value);
    return page;
}

static char *plain_text_from_html_fragment(const char *html, size_t len)
{
    TextBuilder tb = {0};
    const char *p = html;
    const char *end = html + len;
    char *out;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *gt;

        if (!lt) {
            tb_text_bytes(&tb, p, (size_t)(end - p));
            break;
        }
        if (lt > p)
            tb_text_bytes(&tb, p, (size_t)(lt - p));
        gt = find_tag_end(lt + 1, end);
        if (!gt) break;
        p = gt + 1;
    }

    tb_trim_space(&tb);
    out = trim_copy(tb.out.data ? tb.out.data : "");
    buf_clear(&tb.out);
    return out;
}

static char *extract_first_tag_text(const char *html, size_t len, const char *tag)
{
    const char *p = html;
    const char *end = html + len;
    size_t tag_len = strlen(tag);

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *gt;
        const char *name;
        size_t name_len;
        int closing;

        if (!lt) break;
        gt = find_tag_end(lt + 1, end);
        if (!gt) break;
        name = tag_name_start(lt + 1, gt, &closing);
        name_len = tag_name_len(name, gt);
        if (!closing && name_len == tag_len && ascii_eqn(name, tag, tag_len)) {
            char closing_tag[32];
            const char *close;

            snprintf(closing_tag, sizeof(closing_tag), "</%s", tag);
            close = ci_find(gt + 1, end, closing_tag);
            if (!close) return plain_text_from_html_fragment(gt + 1,
                                                             (size_t)(end - gt - 1));
            return plain_text_from_html_fragment(gt + 1,
                                                 (size_t)(close - gt - 1));
        }
        p = gt + 1;
    }

    return xstrdup_local("");
}

static char *extract_meta_title(const char *html, size_t len)
{
    const char *p = html;
    const char *end = html + len;

    while (p < end) {
        const char *lt = ci_find(p, end, "<meta");
        const char *gt;
        char *property;
        char *name;
        char *content;
        int match = 0;

        if (!lt) break;
        gt = find_tag_end(lt + 1, end);
        if (!gt) break;

        property = attr_value(lt + 5, gt, "property");
        name = attr_value(lt + 5, gt, "name");
        content = attr_value(lt + 5, gt, "content");
        if (content && *content) {
            match = (property && (!strcasecmp(property, "og:title") ||
                                  !strcasecmp(property, "article:title"))) ||
                    (name && (!strcasecmp(name, "twitter:title") ||
                              !strcasecmp(name, "title")));
        }

        free(property);
        free(name);
        if (match)
            return content;
        free(content);
        p = gt + 1;
    }

    return xstrdup_local("");
}

static int attr_value_matches_any(const char *value, const char *const *keys)
{
    int i;

    if (!value || !*value) return 0;
    for (i = 0; keys[i]; i++) {
        if (!strcasecmp(value, keys[i])) return 1;
    }
    return 0;
}

static char *extract_meta_content_any(const char *html, size_t len,
                                      const char *const *keys)
{
    const char *p = html;
    const char *end = html + len;

    while (p < end) {
        const char *lt = ci_find(p, end, "<meta");
        const char *gt;
        char *property;
        char *name;
        char *itemprop;
        char *content;
        int match = 0;

        if (!lt) break;
        gt = find_tag_end(lt + 1, end);
        if (!gt) break;

        property = attr_value(lt + 5, gt, "property");
        name = attr_value(lt + 5, gt, "name");
        itemprop = attr_value(lt + 5, gt, "itemprop");
        content = attr_value(lt + 5, gt, "content");
        if (content && *content) {
            match = attr_value_matches_any(property, keys) ||
                    attr_value_matches_any(name, keys) ||
                    attr_value_matches_any(itemprop, keys);
        }

        free(property);
        free(name);
        free(itemprop);
        if (match)
            return content;
        free(content);
        p = gt + 1;
    }

    return xstrdup_local("");
}

static char *decode_json_string(const char *s, const char *end)
{
    Buffer b = {0};

    while (s < end) {
        unsigned char c = (unsigned char)*s++;

        if (c == '"') break;
        if (c == '\\' && s < end) {
            c = (unsigned char)*s++;
            switch (c) {
            case '"':
            case '\\':
            case '/':
                buf_addc(&b, (char)c);
                break;
            case 'b':
                buf_addc(&b, '\b');
                break;
            case 'f':
                buf_addc(&b, '\f');
                break;
            case 'n':
                buf_addc(&b, '\n');
                break;
            case 'r':
                buf_addc(&b, '\r');
                break;
            case 't':
                buf_addc(&b, '\t');
                break;
            case 'u':
                if ((size_t)(end - s) >= 4) s += 4;
                break;
            default:
                buf_addc(&b, (char)c);
                break;
            }
        } else {
            buf_addc(&b, (char)c);
        }
    }

    return b.data ? b.data : xstrdup_local("");
}

static char *extract_json_string_field_from(const char *start, const char *end,
                                            const char *field)
{
    char pattern[128];
    const char *key;
    const char *p;

    if (!snprintf_ok(snprintf(pattern, sizeof(pattern), "\"%s\"", field),
                     sizeof(pattern)))
        return xstrdup_local("");

    key = ci_find(start, end, pattern);
    if (!key) return xstrdup_local("");

    p = key + strlen(pattern);
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != ':') return xstrdup_local("");
    p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '"') return xstrdup_local("");
    return decode_json_string(p + 1, end);
}

static char *extract_json_string_field(const char *html, size_t len,
                                       const char *field)
{
    return extract_json_string_field_from(html, html + len, field);
}

static char *extract_json_author_name(const char *html, size_t len)
{
    const char *end = html + len;
    const char *p = html;
    const char *pattern = "\"author\"";

    while (p < end) {
        const char *key = ci_find(p, end, pattern);
        const char *value;
        const char *window_end;
        char *name;

        if (!key) break;
        value = key + strlen(pattern);
        while (value < end && isspace((unsigned char)*value)) value++;
        if (value >= end || *value != ':') {
            p = key + strlen(pattern);
            continue;
        }
        value++;
        while (value < end && isspace((unsigned char)*value)) value++;
        if (value < end && *value == '"')
            return decode_json_string(value + 1, end);

        window_end = (size_t)(end - value) > 1600 ? value + 1600 : end;
        name = extract_json_string_field_from(value, window_end, "name");
        if (*name) return name;
        free(name);
        p = value;
    }

    return xstrdup_local("");
}

static char *extract_json_number_field(const char *html, size_t len,
                                       const char *field)
{
    char pattern[128];
    const char *end = html + len;
    const char *key;
    const char *p;
    const char *start;

    if (!snprintf_ok(snprintf(pattern, sizeof(pattern), "\"%s\"", field),
                     sizeof(pattern)))
        return xstrdup_local("");

    key = ci_find(html, end, pattern);
    if (!key) return xstrdup_local("");

    p = key + strlen(pattern);
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != ':') return xstrdup_local("");
    p++;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p < end && *p == '"') p++;
    start = p;
    while (p < end && isdigit((unsigned char)*p)) p++;
    if (p == start) return xstrdup_local("");
    return xstrndup_local(start, (size_t)(p - start));
}

static char *clean_metadata_value(char *value)
{
    char *trimmed;

    if (!value) return xstrdup_local("");
    trimmed = trim_copy(value);
    free(value);
    return trimmed;
}

static int metadata_value_is_urlish(const char *value)
{
    return value && (starts_http_url(value) || strstr(value, "://"));
}

static char *first_metadata_value(const char *html, size_t len,
                                  const char *const *keys,
                                  const char *json_field)
{
    char *value = extract_meta_content_any(html, len, keys);

    value = clean_metadata_value(value);
    if (!*value && json_field) {
        free(value);
        value = clean_metadata_value(extract_json_string_field(html, len,
                                                               json_field));
    }
    return value;
}

static int parse_iso_date(const char *value, int *year, int *month, int *day)
{
    int y;
    int m;
    int d;

    if (!value) return 0;
    if (sscanf(value, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    if (y < 1900 || y > 3000 || m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    *year = y;
    *month = m;
    *day = d;
    return 1;
}

static char *format_date_value(const char *value)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int year;
    int month;
    int day;
    char out[64];

    if (parse_iso_date(value, &year, &month, &day)) {
        snprintf(out, sizeof(out), "%s %d, %d", months[month - 1], day, year);
        return xstrdup_local(out);
    }
    return trim_copy(value);
}

static int same_iso_date(const char *a, const char *b)
{
    int ay;
    int am;
    int ad;
    int by;
    int bm;
    int bd;

    return parse_iso_date(a, &ay, &am, &ad) &&
           parse_iso_date(b, &by, &bm, &bd) &&
           ay == by && am == bm && ad == bd;
}

static char *format_comment_count(const char *value)
{
    char *trimmed = trim_copy(value);
    char *end = NULL;
    long n;
    char out[64];

    if (!*trimmed) return trimmed;
    n = strtol(trimmed, &end, 10);
    if (end == trimmed) return trimmed;
    free(trimmed);

    if (n <= 0) return xstrdup_local("Comment");
    if (n == 1) return xstrdup_local("1 comment");
    snprintf(out, sizeof(out), "%ld comments", n);
    return xstrdup_local(out);
}

static void reader_meta_free(ReaderMeta *meta)
{
    free(meta->site);
    free(meta->author);
    free(meta->published);
    free(meta->updated);
    free(meta->comments);
    memset(meta, 0, sizeof(*meta));
}

static void add_metadata_piece(Buffer *line, const char *piece)
{
    if (!piece || !*piece) return;
    if (line->len) buf_addn(line, " | ", 3);
    buf_addn(line, piece, strlen(piece));
}

static char *extract_reader_meta_line(const char *html, size_t len)
{
    static const char *site_keys[] = {
        "og:site_name", "application-name", "twitter:site", NULL
    };
    static const char *author_keys[] = {
        "author", "article:author", "byl", "dc.creator", "dcterms.creator",
        "parsely-author", "sailthru.author", "citation_author", NULL
    };
    static const char *published_keys[] = {
        "article:published_time", "datePublished", "datepublished",
        "pubdate", "publishdate", "date", "dc.date", "dc.date.issued",
        "dcterms.created", "parsely-pub-date", "sailthru.date",
        "citation_publication_date", NULL
    };
    static const char *updated_keys[] = {
        "article:modified_time", "dateModified", "datemodified",
        "lastmod", "last-modified", "modified", "dcterms.modified", NULL
    };
    ReaderMeta meta = {0};
    Buffer line = {0};
    char *published = NULL;
    char *updated = NULL;

    meta.site = first_metadata_value(html, len, site_keys, "publisher");
    meta.author = first_metadata_value(html, len, author_keys, NULL);
    if (metadata_value_is_urlish(meta.author))
        meta.author[0] = 0;
    if (!*meta.author) {
        free(meta.author);
        meta.author = clean_metadata_value(extract_json_author_name(html, len));
    }
    meta.published = first_metadata_value(html, len, published_keys,
                                          "datePublished");
    meta.updated = first_metadata_value(html, len, updated_keys,
                                        "dateModified");
    meta.comments = clean_metadata_value(extract_json_number_field(html, len,
                                                                   "commentCount"));

    if (meta.site && meta.site[0] == '@')
        memmove(meta.site, meta.site + 1, strlen(meta.site));
    if (metadata_value_is_urlish(meta.site))
        meta.site[0] = 0;

    if (meta.author && *meta.author) {
        char author_piece[256];

        if (!strncasecmp(meta.author, "by ", 3)) {
            add_metadata_piece(&line, meta.author);
        } else {
            snprintf(author_piece, sizeof(author_piece), "by %.240s",
                     meta.author);
            add_metadata_piece(&line, author_piece);
        }
    }
    add_metadata_piece(&line, meta.site);

    if (meta.published && *meta.published)
        published = format_date_value(meta.published);
    if (meta.updated && *meta.updated &&
        !same_iso_date(meta.published, meta.updated))
        updated = format_date_value(meta.updated);

    add_metadata_piece(&line, published);
    if (updated && *updated) {
        char updated_piece[96];

        snprintf(updated_piece, sizeof(updated_piece), "Updated %.80s",
                 updated);
        add_metadata_piece(&line, updated_piece);
    }
    if (meta.comments && *meta.comments) {
        char *comments = format_comment_count(meta.comments);

        add_metadata_piece(&line, comments);
        free(comments);
    }

    free(published);
    free(updated);
    reader_meta_free(&meta);
    return line.data ? line.data : xstrdup_local("");
}

static void clean_title_in_place(char *title)
{
    static const char *separators[] = { " | ", " - ", " :: ", NULL };
    char *trimmed;
    int i;

    if (!title) return;
    trimmed = trim_copy(title);
    snprintf(title, strlen(title) + 1, "%s", trimmed);
    free(trimmed);

    for (i = 0; separators[i]; i++) {
        char *sep = strstr(title, separators[i]);
        size_t left_len;
        size_t right_len;

        if (!sep) continue;
        left_len = (size_t)(sep - title);
        right_len = strlen(sep + strlen(separators[i]));
        if (left_len >= 8 && right_len <= 70) {
            *sep = 0;
            break;
        }
    }

    trimmed = trim_copy(title);
    snprintf(title, strlen(title) + 1, "%s", trimmed);
    free(trimmed);
    if (strlen(title) >= 2) {
        char *pilcrow = strstr(title, "\xC2\xB6");

        if (pilcrow) {
            char *tail = pilcrow + 2;

            while (*tail && isspace((unsigned char)*tail)) tail++;
            if (!*tail) {
                *pilcrow = 0;
                trimmed = trim_copy(title);
                snprintf(title, strlen(title) + 1, "%s", trimmed);
                free(trimmed);
            }
        }
    }
}

static char *extract_reader_title(const char *html, size_t len,
                                  ReaderRegion region)
{
    char *title;

    title = extract_first_tag_text(region.start,
                                   (size_t)(region.end - region.start), "h1");
    if (*title) {
        clean_title_in_place(title);
        return title;
    }
    free(title);

    title = extract_first_tag_text(html, len, "h1");
    if (*title) {
        clean_title_in_place(title);
        return title;
    }
    free(title);

    title = extract_meta_title(html, len);
    if (*title) {
        clean_title_in_place(title);
        return title;
    }
    free(title);

    title = extract_first_tag_text(html, len, "title");
    clean_title_in_place(title);
    return title;
}

static int line_range_trim(const char *text, size_t start, size_t len,
                           size_t *trim_start, size_t *trim_len)
{
    size_t s = start;
    size_t e = start + len;

    while (s < e && isspace((unsigned char)text[s])) s++;
    while (e > s && isspace((unsigned char)text[e - 1])) e--;
    *trim_start = s;
    *trim_len = e - s;
    return *trim_len > 0;
}

static int range_contains_ci(const char *text, size_t start, size_t len,
                             const char *needle)
{
    return ci_find(text + start, text + start + len, needle) != NULL;
}

static int range_equals_ci(const char *text, size_t start, size_t len,
                           const char *needle)
{
    size_t n = strlen(needle);

    return len == n && ascii_eqn(text + start, needle, n);
}

static int line_is_reader_junk(const char *text, size_t start, size_t len)
{
    static const char *phrases[] = {
        "advertisement", "skip to content", "skip navigation",
        "privacy choices", "cookie", "cookies", "all rights reserved",
        "share this", "listen to this article", "sign up for",
        "sign in", "log in", "subscribe now", "subscribe to",
        "newsletter", "comment count", "comments are closed",
        "click here to share", "add al jazeera on google",
        "getting the conversation ready", "posts from this topic",
        "text settings", "subscribers only", "minimize to nav",
        "browser version with limited support", "without styles and javascript",
        "view author publications", "search author on", "published on",
        "published:", "updated ", "listen &middot;", "listen ·",
        "see all ", "hide caption", "toggle caption",
        "view image in fullscreen", "may earn a commission",
        "ethics statement", "size small standard", "links standard",
        "previously worked at", "is a news editor", "figure caption",
        "image caption", "image source", "orcid:", "show authors",
        "metrics details", "access through your institution",
        "subscription content", "rent or buy", "prices vary",
        "cite this article", "legal disclaimers", "unedited version",
        "accesses", "altmetric", "from wikipedia, the free encyclopedia",
        "baseline widely available", "see full compatibility",
        "open more actions menu", "read more", "readmore",
        NULL
    };
    static const char *exact[] = {
        "ad", "ads", "advertising", "advertisement", "share", "shares",
        "comments", "print", "email", "read more", "more", "save",
        "hide caption", "toggle caption", "view image in fullscreen",
        "toggle more options", "download", "embed", "follow", "close",
        "navigation drawer", "comments drawer", "loading comments",
        "lightsystemdark", "text settings", "learn more", "published",
        "updated", "access options", "author information", "author notes",
        "subjects", "metrics details", NULL
    };
    size_t s;
    size_t n;
    int i;

    if (!line_range_trim(text, start, len, &s, &n)) return 0;
    if (n > 2 && text[s] == '-' && isspace((unsigned char)text[s + 1])) {
        s += 2;
        n -= 2;
        while (n && isspace((unsigned char)text[s])) {
            s++;
            n--;
        }
    }
    if (n <= 2) return 1;
    if (n <= 30 && isdigit((unsigned char)text[s]) &&
        range_contains_ci(text, s, n, " languages"))
        return 1;
    if (range_contains_ci(text, s, n, "hide caption") ||
        range_contains_ci(text, s, n, "toggle caption") ||
        range_contains_ci(text, s, n, "view image in fullscreen") ||
        range_contains_ci(text, s, n, "unedited version") ||
        range_contains_ci(text, s, n, "legal disclaimers") ||
        range_contains_ci(text, s, n, "subscription content"))
        return 1;
    if (n <= 90 && range_contains_ci(text, s, n, " for npr"))
        return 1;

    for (i = 0; exact[i]; i++) {
        if (range_equals_ci(text, s, n, exact[i])) return 1;
    }
    if (n <= 160) {
        for (i = 0; phrases[i]; i++) {
            if (range_contains_ci(text, s, n, phrases[i])) return 1;
        }
    }
    return 0;
}

static int line_is_trailing_junk_heading(const char *text, size_t start, size_t len)
{
    static const char *terms[] = {
        "also read", "around the web", "author bio", "comments",
        "from our partners", "latest stories", "latest news", "more from",
        "more in", "most popular", "newsletter", "people also read",
        "popular", "read next", "recommended", "related", "related stories",
        "sponsored", "tags", "topics", "trending", "you may also like",
        "access through", "access options", "rent or buy", "from$",
        "to$", "prices may", "author information", "authors and affiliations",
        "authors", NULL
    };
    size_t s;
    size_t n;
    int i;

    if (!line_range_trim(text, start, len, &s, &n)) return 0;
    if (n > 120) return 0;
    for (i = 0; terms[i]; i++) {
        if (range_contains_ci(text, s, n, terms[i])) return 1;
    }
    return 0;
}

static int line_is_metadata_date(const char *text, size_t start, size_t len)
{
    static const char *months[] = {
        "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december",
        "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept",
        "oct", "nov", "dec", NULL
    };
    size_t s;
    size_t n;
    int i;

    if (!line_range_trim(text, start, len, &s, &n)) return 0;
    if (range_equals_ci(text, s, n, "published") ||
        range_equals_ci(text, s, n, "updated"))
        return 1;
    if (n > 70) return 0;
    if (range_contains_ci(text, s, n, " ago")) return 1;
    if (range_contains_ci(text, s, n, "updated ")) return 1;
    if (range_contains_ci(text, s, n, "published ")) return 1;
    if (range_contains_ci(text, s, n, " bst") ||
        range_contains_ci(text, s, n, " et") ||
        range_contains_ci(text, s, n, " utc"))
        return 1;
    for (i = 0; months[i]; i++) {
        if (range_contains_ci(text, s, n, months[i]) &&
            (n <= 25 || isdigit((unsigned char)text[s])))
            return 1;
    }
    return 0;
}

static int range_has_month_name(const char *text, size_t start, size_t len)
{
    static const char *months[] = {
        "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december",
        "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept",
        "oct", "nov", "dec", NULL
    };
    int i;

    for (i = 0; months[i]; i++) {
        if (range_contains_ci(text, start, len, months[i])) return 1;
    }
    return 0;
}

static int range_has_digit(const char *text, size_t start, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (isdigit((unsigned char)text[start + i])) return 1;
    }
    return 0;
}

static int line_is_compact_article_metadata(const char *text, size_t start,
                                            size_t len)
{
    size_t s;
    size_t n;
    unsigned char last;

    if (!line_range_trim(text, start, len, &s, &n)) return 0;
    if (n > 160) return 0;
    last = (unsigned char)text[s + n - 1];
    if (range_contains_ci(text, s, n, " min read") ||
        range_contains_ci(text, s, n, " minute read"))
        return 1;
    if (range_has_month_name(text, s, n) && range_has_digit(text, s, n) &&
        (n <= 90 || range_contains_ci(text, s, n, " | ")) &&
        last != '.' && last != '?' && last != '!' && last != ':' &&
        last != ';' && last != '"' && last != '\'' && last != ')')
        return 1;
    if (n <= 120 && range_contains_ci(text, s, n, " updated ") &&
        last != '.' && last != '?' && last != '!' && last != ':' &&
        last != ';' && last != '"' && last != '\'' && last != ')')
        return 1;
    return 0;
}

static int next_nonblank_line_is_metadata_date(const char *text, size_t pos)
{
    size_t total = strlen(text ? text : "");

    while (pos <= total) {
        size_t line_start = pos;
        size_t line_len;
        const char *nl = memchr(text + pos, '\n', total - pos);
        size_t trim_start;
        size_t trim_len;

        line_len = nl ? (size_t)(nl - (text + pos)) : total - pos;
        if (line_range_trim(text, line_start, line_len, &trim_start, &trim_len))
            return line_is_metadata_date(text, line_start, line_len);
        if (!nl) break;
        pos = (size_t)(nl - text) + 1;
    }
    return 0;
}

static int line_lacks_sentence_end(const char *text, size_t start, size_t len)
{
    size_t s;
    size_t n;
    unsigned char last;

    if (!line_range_trim(text, start, len, &s, &n)) return 0;
    last = (unsigned char)text[s + n - 1];
    return last != '.' && last != '?' && last != '!' &&
           last != ':' && last != ';' && last != '"' &&
           last != '\'' && last != ')';
}

static int first_nonblank_line(const char *text, size_t *start, size_t *len)
{
    size_t pos = 0;
    size_t total = strlen(text ? text : "");

    while (pos <= total) {
        size_t line_start = pos;
        size_t line_len;
        const char *nl;

        nl = memchr(text + pos, '\n', total - pos);
        line_len = nl ? (size_t)(nl - (text + pos)) : total - pos;
        if (line_range_trim(text, line_start, line_len, start, len))
            return 1;
        if (!nl) break;
        pos = (size_t)(nl - text) + 1;
    }
    return 0;
}

static int range_matches_title(const char *title, const char *text,
                               size_t start, size_t len)
{
    size_t title_len;
    const char *line_start;
    const char *line_end;
    char *line;
    char *trimmed;
    int match = 0;

    if (!title || !*title || !text) return 0;
    title_len = strlen(title);
    if (title_len < 4) return 0;
    if (!line_range_trim(text, start, len, &start, &len)) return 0;

    /*
     * This is for detecting duplicate page-title/header lines only.
     * It must NOT match real article prose like:
     * "Captain James Cook (...) was a British Royal Navy officer..."
     */
    if (len > title_len + 80)
        return 0;
    if (!line_lacks_sentence_end(text, start, len))
        return 0;

    line_start = text + start;
    line_end = line_start + len;

    line = xstrndup_local(line_start, len);
    trimmed = trim_copy(line);

    if (!strcasecmp(trimmed, title))
        match = 1;
    else if (len <= title_len + 30 && ci_find(line_start, line_end, title))
        match = 1;
    else if (len <= title_len + 30 && ci_find(title, title + title_len, trimmed))
        match = 1;

    free(trimmed);
    free(line);
    return match;
}

static int title_matches_line(const char *title, const char *text)
{
    size_t start;
    size_t len;

    if (!title || !*title || !text) return 1;
    if (strlen(title) < 4) return 1;
    if (!first_nonblank_line(text, &start, &len)) return 0;
    return range_matches_title(title, text, start, len);
}

static int find_title_line_end(const char *title, const char *text, size_t *line_end)
{
    size_t pos = 0;
    size_t total = strlen(text ? text : "");
    int lines = 0;

    if (!title || !*title || strlen(title) < 4) return 0;
    while (pos <= total && pos < 2500 && lines < 18) {
        size_t line_start = pos;
        size_t line_len;
        const char *nl = memchr(text + pos, '\n', total - pos);

        line_len = nl ? (size_t)(nl - (text + pos)) : total - pos;
        if (range_matches_title(title, text, line_start, line_len) &&
            line_lacks_sentence_end(text, line_start, line_len)) {
            *line_end = nl ? (size_t)(nl - text) + 1 : total;
            return 1;
        }
        if (!nl) break;
        pos = (size_t)(nl - text) + 1;
        lines++;
    }
    return 0;
}

static void append_reader_header(Buffer *out, const char *title,
                                 const char *meta_line)
{
    if (title && *title) {
        buf_addn(out, title, strlen(title));
        buf_addn(out, "\n\n", 2);
    }
    if (meta_line && *meta_line) {
        buf_addn(out, meta_line, strlen(meta_line));
        buf_addn(out, "\n\n", 2);
    }
    if (out->len) {
        const char *rule =
            "------------------------------------------------------------------------\n\n";

        buf_addn(out, rule, strlen(rule));
    }
}

static int range_matches_metadata_piece(const char *meta_line, const char *text,
                                        size_t start, size_t len)
{
    char *line;
    char *trimmed;
    int match = 0;

    if (!meta_line || !*meta_line || !text) return 0;
    if (!line_range_trim(text, start, len, &start, &len)) return 0;
    if (len < 3 || len > 120) return 0;

    line = xstrndup_local(text + start, len);
    trimmed = trim_copy(line);
    if (*trimmed)
        match = ci_find(meta_line, meta_line + strlen(meta_line), trimmed) != NULL;
    free(trimmed);
    free(line);
    return match;
}

static void filtered_page_add_link(Page *dst, Link *src, size_t marker_offset)
{
    page_add_link(dst, src->label, src->url, marker_offset);
}

static void filtered_page_add_control(Page *dst, FormControl *src, size_t marker_offset)
{
    FormControl copy = *src;

    copy.marker_offset = marker_offset;
    page_add_control(dst, &copy);
}

static void reader_filter_page(Page *p, const char *title,
                               const char *meta_line)
{
    const char *text = p->text ? p->text : "";
    size_t total = strlen(text);
    size_t pos = 0;
    Buffer out = {0};
    Page links_page = {0};
    long content_chars = 0;
    int pending_blank = 0;
    int wrote_text = 0;
    char last_kept_line[512] = "";
    size_t link_i;
    size_t control_i;

    links_page.layout_width = -1;

    append_reader_header(&out, title, meta_line);
    wrote_text = out.len > 0;

    if (title && *title) {
        size_t title_line_end;

        if (find_title_line_end(title, text, &title_line_end))
            pos = title_line_end;
    }

    while (pos <= total) {
        size_t line_start = pos;
        size_t line_len;
        size_t trim_start;
        size_t trim_len;
        const char *nl = memchr(text + pos, '\n', total - pos);
        int has_text;
        int drop;
        size_t next_pos;

        line_len = nl ? (size_t)(nl - (text + pos)) : total - pos;
        next_pos = nl ? (size_t)(nl - text) + 1 : total;
        has_text = line_range_trim(text, line_start, line_len,
                                   &trim_start, &trim_len);

        if (has_text && content_chars > 600 &&
            line_is_trailing_junk_heading(text, line_start, line_len))
            break;

        drop = has_text && line_is_reader_junk(text, line_start, line_len);
        if (!drop && has_text && line_is_metadata_date(text, line_start, line_len))
            drop = 1;
        if (!drop && has_text && content_chars < 300 &&
            line_is_compact_article_metadata(text, line_start, line_len))
            drop = 1;
        if (!drop && has_text && content_chars < 300 &&
            range_matches_metadata_piece(meta_line, text, line_start, line_len))
            drop = 1;
        if (!drop && has_text && content_chars < 150 && trim_len > 2 &&
            text[trim_start] == '-' &&
            isspace((unsigned char)text[trim_start + 1]))
            drop = 1;
        if (!drop && has_text && content_chars > 200 && trim_len <= 140 &&
            line_lacks_sentence_end(text, line_start, line_len) &&
            next_nonblank_line_is_metadata_date(text, next_pos))
            drop = 1;
        if (!drop && has_text && title && *title && wrote_text &&
            content_chars < 800 &&
            range_matches_title(title, text, line_start, line_len)) {
            size_t title_len = strlen(title);

            /*
             * Only drop actual duplicate title/header lines.
             * Do NOT drop the opening paragraph just because it begins
             * with the article title, e.g. Wikipedia:
             * "Captain James Cook (...) was a British Royal Navy officer..."
             */
            if (line_lacks_sentence_end(text, line_start, line_len))
                drop = 1;
        }
        if (!drop && has_text && trim_len < sizeof(last_kept_line)) {
            char *line_copy = xstrndup_local(text + trim_start, trim_len);

            if (last_kept_line[0] && !strcasecmp(line_copy, last_kept_line))
                drop = 1;
            free(line_copy);
        }
        if (!drop && has_text) {
            size_t out_line_start;

            if (pending_blank && out.len && out.data[out.len - 1] != '\n')
                buf_addc(&out, '\n');
            if (pending_blank && out.len &&
                (out.len < 2 || out.data[out.len - 2] != '\n'))
                buf_addc(&out, '\n');
            pending_blank = 0;

            out_line_start = out.len;
            buf_addn(&out, text + line_start, line_len);
            buf_addc(&out, '\n');
            wrote_text = 1;
            content_chars += (long)trim_len;
            if (trim_len < sizeof(last_kept_line)) {
                memcpy(last_kept_line, text + trim_start, trim_len);
                last_kept_line[trim_len] = 0;
            } else {
                last_kept_line[0] = 0;
            }

            for (link_i = 0; link_i < p->link_count; link_i++) {
                size_t off = p->links[link_i].marker_offset;

                if (off >= line_start && off < line_start + line_len) {
                    filtered_page_add_link(&links_page, &p->links[link_i],
                                           out_line_start + (off - line_start));
                }
            }
            for (control_i = 0; control_i < p->control_count; control_i++) {
                size_t off = p->controls[control_i].marker_offset;

                if (off >= line_start && off < line_start + line_len) {
                    filtered_page_add_control(&links_page, &p->controls[control_i],
                                              out_line_start + (off - line_start));
                }
            }
        } else if (wrote_text && !drop) {
            pending_blank = 1;
        }

        if (!nl) break;
        pos = next_pos;
    }

    while (out.len && (out.data[out.len - 1] == '\n' ||
                       out.data[out.len - 1] == ' '))
        out.data[--out.len] = 0;

    if (!out.len)
        buf_addn(&out, text, strlen(text));

    free(p->text);
    for (link_i = 0; link_i < p->link_count; link_i++) {
        free(p->links[link_i].label);
        free(p->links[link_i].url);
    }
    for (control_i = 0; control_i < p->control_count; control_i++) {
        size_t j;

        free(p->controls[control_i].label);
        free(p->controls[control_i].name);
        free(p->controls[control_i].value);
        free(p->controls[control_i].form_action);
        free(p->controls[control_i].form_method);
        free(p->controls[control_i].form_enctype);
        for (j = 0; j < p->controls[control_i].option_count; j++) {
            free(p->controls[control_i].options[j]);
            free(p->controls[control_i].option_values[j]);
        }
        free(p->controls[control_i].options);
        free(p->controls[control_i].option_values);
    }
    free(p->links);
    free(p->controls);
    p->text = out.data ? out.data : xstrdup_local("");
    p->links = links_page.links;
    p->link_count = links_page.link_count;
    p->link_cap = links_page.link_cap;
    p->controls = links_page.controls;
    p->control_count = links_page.control_count;
    p->control_cap = links_page.control_cap;
    p->display_count = 0;
    p->display_cap = 0;
    free(p->display);
    p->display = NULL;
    p->layout_width = -1;
}

static size_t line_end_after_offset(const char *text, size_t total, size_t offset)
{
    const char *nl;

    if (offset > total) offset = total;
    nl = memchr(text + offset, '\n', total - offset);
    if (!nl) return total;
    return (size_t)(nl - text) + 1;
}

static size_t skip_blank_text_lines(const char *text, size_t total, size_t pos)
{
    while (pos < total) {
        size_t line_start = pos;
        size_t line_end = line_end_after_offset(text, total, pos);
        size_t trim_start;
        size_t trim_len;

        if (line_end > line_start && text[line_end - 1] == '\n')
            line_end--;
        if (line_range_trim(text, line_start, line_end - line_start,
                            &trim_start, &trim_len))
            break;
        pos = line_end < total ? line_end + 1 : total;
    }
    return pos;
}

static int text_line_equals_literal(const char *text, size_t start, size_t len,
                                    const char *literal)
{
    size_t trim_start;
    size_t trim_len;
    size_t literal_len = strlen(literal);

    if (!line_range_trim(text, start, len, &trim_start, &trim_len))
        return literal_len == 0;
    return trim_len == literal_len &&
           ascii_eqn(text + trim_start, literal, literal_len);
}

static int browser_snapshot_links_section(Page *p, size_t *start_out,
                                          size_t *remove_end_out)
{
    const char *text = p->text ? p->text : "";
    size_t total = strlen(text);
    size_t first = total;
    size_t pos;
    size_t scan_floor;
    size_t section_start = total;
    size_t remove_end;
    size_t i;

    if (!p->link_count || !total) return 0;
    for (i = 0; i < p->link_count; i++) {
        if (p->links[i].marker_offset < first)
            first = p->links[i].marker_offset;
    }
    if (first >= total) return 0;

    pos = first;
    scan_floor = first > 4096 ? first - 4096 : 0;
    while (pos >= scan_floor) {
        size_t line_start = pos;
        size_t line_end;

        while (line_start > 0 && text[line_start - 1] != '\n')
            line_start--;
        line_end = line_end_after_offset(text, total, line_start);
        if (line_end > line_start && text[line_end - 1] == '\n')
            line_end--;
        if (text_line_equals_literal(text, line_start, line_end - line_start,
                                     "Links")) {
            section_start = line_start;
            break;
        }
        if (line_start == 0 || line_start <= scan_floor)
            break;
        pos = line_start - 1;
    }
    if (section_start >= total) return 0;

    remove_end = section_start;
    for (i = 0; i < p->link_count; i++) {
        size_t off = p->links[i].marker_offset;

        if (off >= section_start && off < total) {
            size_t end = line_end_after_offset(text, total, off);

            if (end > remove_end) remove_end = end;
        }
    }
    remove_end = skip_blank_text_lines(text, total, remove_end);
    if (remove_end <= section_start) return 0;

    *start_out = section_start;
    *remove_end_out = remove_end;
    return 1;
}

static int find_visible_snapshot_label(const char *text, size_t limit,
                                       const char *label, size_t start_hint,
                                       size_t *offset_out)
{
    const char *end;
    const char *match;

    if (!text || !label || !*label || !limit) return 0;
    if (strlen(label) > limit) return 0;
    if (start_hint > limit) start_hint = 0;

    end = text + limit;
    match = ci_find(text + start_hint, end, label);
    if (!match && start_hint)
        match = ci_find(text, end, label);
    if (!match) return 0;

    *offset_out = (size_t)(match - text);
    return 1;
}

static void remove_text_range(Page *p, size_t start, size_t end)
{
    const char *old = p->text ? p->text : "";
    size_t total = strlen(old);
    size_t delta;
    char *text;
    size_t i;
    size_t out_i;

    if (start >= end || end > total) return;
    delta = end - start;
    text = xmalloc(total - delta + 1);
    memcpy(text, old, start);
    memcpy(text + start, old + end, total - end);
    text[total - delta] = 0;
    free(p->text);
    p->text = text;

    out_i = 0;
    for (i = 0; i < p->link_count; i++) {
        Link *link = &p->links[i];

        if (link->marker_offset >= start && link->marker_offset < end) {
            free(link->label);
            free(link->url);
            continue;
        }
        if (link->marker_offset >= end)
            link->marker_offset -= delta;
        if (out_i != i)
            p->links[out_i] = p->links[i];
        out_i++;
    }
    p->link_count = out_i;

    out_i = 0;
    for (i = 0; i < p->control_count; i++) {
        FormControl *control = &p->controls[i];

        if (control->marker_offset >= start && control->marker_offset < end) {
            control_free(control);
            continue;
        }
        if (control->marker_offset >= end)
            control->marker_offset -= delta;
        if (out_i != i)
            p->controls[out_i] = p->controls[i];
        out_i++;
    }
    p->control_count = out_i;

    free(p->display);
    p->display = NULL;
    p->display_count = 0;
    p->display_cap = 0;
    free(p->stops);
    p->stops = NULL;
    p->stop_count = 0;
    p->stop_cap = 0;
    p->layout_width = -1;
}

static int browser_snapshot_line_looks_like_script(const char *text,
                                                  size_t start, size_t len)
{
    const char *s = text + start;
    const char *e = s + len;
    int score = 0;

    if (len < 140) return 0;

    if (ci_find(s, e, "function(")) score++;
    if (ci_find(s, e, "=>")) score++;
    if (ci_find(s, e, "document.")) score++;
    if (ci_find(s, e, "window.")) score++;
    if (ci_find(s, e, "localStorage")) score++;
    if (ci_find(s, e, "JSON.parse")) score++;
    if (ci_find(s, e, "classList")) score++;
    if (ci_find(s, e, "querySelector")) score++;
    if (ci_find(s, e, "getElementById")) score++;
    if (ci_find(s, e, ".addEventListener")) score++;
    if (ci_find(s, e, "return(")) score++;
    if (ci_find(s, e, "let ")) score++;
    if (ci_find(s, e, "const ")) score++;

    return score >= 2 || (len > 500 && score >= 1);
}

static void browser_snapshot_remove_script_lines(Page *p)
{
    size_t pos = 0;

    while (p->text && p->text[pos]) {
        const char *text = p->text;
        size_t total = strlen(text);
        size_t line_start = pos;
        const char *nl = memchr(text + pos, '\n', total - pos);
        size_t line_end = nl ? (size_t)(nl - text) : total;
        size_t remove_end = nl ? line_end + 1 : line_end;
        size_t trim_start;
        size_t trim_len;

        line_range_trim(text, line_start, line_end - line_start,
                        &trim_start, &trim_len);

        if (browser_snapshot_line_looks_like_script(text, trim_start, trim_len)) {
            remove_text_range(p, line_start, remove_end);
            pos = line_start;
            continue;
        }

        if (!nl) break;
        pos = line_end + 1;
    }
}

static void browser_snapshot_promote_link_list(Page *p)
{
    const char *text = p->text ? p->text : "";
    size_t section_start;
    size_t remove_end;
    size_t *offsets;
    unsigned char *mapped;
    size_t cursor = 0;
    size_t mapped_count = 0;
    size_t i;
    size_t out_i;

    if (!browser_snapshot_links_section(p, &section_start, &remove_end))
        return;

    offsets = xmalloc(p->link_count * sizeof(*offsets));
    mapped = xmalloc(p->link_count * sizeof(*mapped));
    memset(mapped, 0, p->link_count * sizeof(*mapped));

    for (i = 0; i < p->link_count; i++) {
        size_t offset;

        if (find_visible_snapshot_label(text, section_start,
                                        p->links[i].label, cursor, &offset)) {
            offsets[i] = offset;
            mapped[i] = 1;
            mapped_count++;
            cursor = offset + strlen(p->links[i].label);
            if (cursor > section_start) cursor = section_start;
        }
    }

    if (!mapped_count || mapped_count * 2 < p->link_count) {
        free(offsets);
        free(mapped);
        return;
    }

    out_i = 0;
    for (i = 0; i < p->link_count; i++) {
        if (!mapped[i]) {
            free(p->links[i].label);
            free(p->links[i].url);
            continue;
        }
        p->links[i].marker_offset = offsets[i];
        if (out_i != i)
            p->links[out_i] = p->links[i];
        out_i++;
    }
    p->link_count = out_i;

    remove_text_range(p, section_start, remove_end);
    free(offsets);
    free(mapped);
}

static int looks_like_html_document(const char *data, size_t len)
{
    const char *end = data + (len < 4096 ? len : 4096);

    return ci_find(data, end, "<!doctype") ||
           ci_find(data, end, "<html") ||
           ci_find(data, end, "<head") ||
           ci_find(data, end, "<body") ||
           ci_find(data, end, "<title") ||
           ci_find(data, end, "<meta") ||
           ci_find(data, end, "<article") ||
           ci_find(data, end, "<main");
}

static Page parse_html(const char *html, size_t len, const char *base_url)
{
    int browser_snapshot =
        html_contains_ci(html, len, "data-simplebrowse-snapshot=\"visible\"") ||
        html_contains_ci(html, len, "data-simplebrowse-snapshot='visible'");

    if (!looks_like_html_document(html, len)) {
        Page page = {0};
        const char *plain = html;
        size_t plain_len = len;

        if (plain_len >= 3 &&
            (unsigned char)plain[0] == 0xef &&
            (unsigned char)plain[1] == 0xbb &&
            (unsigned char)plain[2] == 0xbf) {
            plain += 3;
            plain_len -= 3;
        }
        while (plain_len && (*plain == '\r' || *plain == '\n')) {
            plain++;
            plain_len--;
        }
        page.text = xstrndup_local(plain, plain_len);
        page.url = xstrdup_local(base_url);
        page.title = xstrdup_local(base_url);
        page.meta = xstrdup_local("");
        page.layout_width = -1;
        return page;
    }

    ReaderRegion region = select_reader_region(html, len);
    char *title = extract_reader_title(html, len, region);
    char *meta_line = extract_reader_meta_line(html, len);
    Page page = parse_html_fragment(region.start,
                                    (size_t)(region.end - region.start),
                                    base_url);

    page.title = xstrdup_local(title);
    page.meta = xstrdup_local(meta_line);
    if (browser_snapshot) {
        browser_snapshot_promote_link_list(&page);
        browser_snapshot_remove_script_lines(&page);
    }
    if (!region.listing && !browser_snapshot)
        reader_filter_page(&page, title, meta_line);
    if (page_meaningful_chars(&page) < 300) {
        Page full;
        loose_html_parse = 1;
        full = parse_html_fragment(html, len, base_url);
        loose_html_parse = 0;

        if (page_meaningful_chars(&full) > page_meaningful_chars(&page) ||
            full.control_count > page.control_count) {
            free(full.title);
            free(full.meta);
            full.title = xstrdup_local(title);
            full.meta = xstrdup_local(meta_line);
            page_free(&page);
            page = full;
        } else {
            page_free(&full);
        }
    }
    free(title);
    free(meta_line);
    return page;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    Buffer *b = userdata;
    size_t n;

    if (size && nmemb > SIZE_MAX / size) return 0;
    n = size * nmemb;
    if (b->len + n > RESPONSE_LIMIT) return 0;
    if (!buf_addn(b, ptr, n)) return 0;
    return n;
}

static const char *http_reason_phrase(long code)
{
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 418: return "I'm a teapot";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:
        if (code >= 100 && code < 200) return "Informational";
        if (code >= 200 && code < 300) return "Success";
        if (code >= 300 && code < 400) return "Redirect";
        if (code >= 400 && code < 500) return "Client Error";
        if (code >= 500 && code < 600) return "Server Error";
        return "No Response";
    }
}

static void format_http_status(long code, char *out, size_t outsz)
{
    snprintf(out, outsz, "%ld %s", code, http_reason_phrase(code));
}

static char *fetch_url(const char *url, FetchResult *result)
{
    CURL *curl;
    CURLcode rc;
    Buffer b = {0};
    char curl_error[CURL_ERROR_SIZE] = "";
    char *effective_url = NULL;

    memset(result, 0, sizeof(*result));
    snprintf(result->effective, sizeof(result->effective), "%s", url);

    curl = curl_easy_init();
    if (!curl) {
        snprintf(result->error, sizeof(result->error), "curl initialization failed");
        result->network_error = 1;
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, SIMPLEBROWSE_UA);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);

    rc = curl_easy_perform(curl);
    result->curl_code = rc;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->code);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    snprintf(result->effective, sizeof(result->effective), "%s",
             effective_url ? effective_url : url);
    curl_easy_cleanup(curl);
    snprintf(result->reason, sizeof(result->reason), "%s",
             http_reason_phrase(result->code));

    if (rc != CURLE_OK) {
        result->network_error = 1;
        snprintf(result->error, sizeof(result->error), "%s",
                 curl_error[0] ? curl_error : curl_easy_strerror(rc));
        free(b.data);
        return NULL;
    }
    if (result->code < 200 || result->code >= 300) {
        format_http_status(result->code, result->error, sizeof(result->error));
        free(b.data);
        return NULL;
    }
    if (!b.data) {
        snprintf(result->error, sizeof(result->error), "empty response");
        return NULL;
    }

    return b.data;
}

static char *fetch_url_post(const char *url, const char *body,
                            const char *content_type, FetchResult *result)
{
    CURL *curl;
    CURLcode rc;
    Buffer b = {0};
    char curl_error[CURL_ERROR_SIZE] = "";
    char *effective_url = NULL;
    struct curl_slist *headers = NULL;
    char header[256];

    memset(result, 0, sizeof(*result));
    snprintf(result->effective, sizeof(result->effective), "%s", url);

    curl = curl_easy_init();
    if (!curl) {
        snprintf(result->error, sizeof(result->error), "curl initialization failed");
        result->network_error = 1;
        return NULL;
    }

    snprintf(header, sizeof(header), "Content-Type: %s",
             content_type && *content_type ? content_type :
             "application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, SIMPLEBROWSE_UA);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)strlen(body ? body : ""));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    rc = curl_easy_perform(curl);
    result->curl_code = rc;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->code);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    if (effective_url)
        snprintf(result->effective, sizeof(result->effective), "%s", effective_url);
    snprintf(result->reason, sizeof(result->reason), "%s",
             http_reason_phrase(result->code));

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        snprintf(result->error, sizeof(result->error), "%s",
                 curl_error[0] ? curl_error : curl_easy_strerror(rc));
        result->network_error = 1;
        buf_clear(&b);
        return NULL;
    }

    if (result->code >= 400) {
        format_http_status(result->code, result->error, sizeof(result->error));
        buf_clear(&b);
        return NULL;
    }

    if (!b.data)
        return xstrdup_local("");
    return b.data;
}

static void copy_error_excerpt(char *out, size_t outsz, const char *s)
{
    size_t i = 0;

    if (!outsz) return;
    if (!s || !*s) {
        snprintf(out, outsz, "WebKit backend failed");
        return;
    }

    while (*s && i + 1 < outsz) {
        unsigned char c = (unsigned char)*s++;

        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (c < 32)
            continue;
        out[i++] = (char)c;
    }
    out[i] = 0;
}

static int read_fd_limited(int fd, Buffer *b, size_t limit, pid_t child)
{
    char chunk[8192];
    ssize_t n;

    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        if (b->len + (size_t)n > limit) {
            if (child > 0) kill(child, SIGKILL);
            return 0;
        }
        if (!buf_addn(b, chunk, (size_t)n)) {
            if (child > 0) kill(child, SIGKILL);
            return 0;
        }
    }
    return n == 0;
}

static int write_full(int fd, const char *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (n == 0)
            return 0;
        off += (size_t)n;
    }
    return 1;
}

static int read_line_limited(int fd, Buffer *line, size_t limit)
{
    char ch;

    line->len = 0;
    if (line->data)
        line->data[0] = 0;

    while (1) {
        ssize_t n = read(fd, &ch, 1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (n == 0)
            return 0;
        if (ch == '\n')
            return 1;
        if (line->len + 1 > limit)
            return 0;
        if (!buf_addc(line, ch))
            return 0;
    }
}

static char *read_exact_alloc(int fd, size_t len, size_t limit)
{
    char *data;
    size_t off = 0;

    if (len > limit)
        return NULL;
    data = xmalloc(len + 1);
    while (off < len) {
        ssize_t n = read(fd, data + off, len - off);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(data);
            return NULL;
        }
        if (n == 0) {
            free(data);
            return NULL;
        }
        off += (size_t)n;
    }
    data[len] = 0;
    return data;
}

static int parse_size_header(const char *line, const char *prefix, size_t *out)
{
    const char *value;
    char *tail = NULL;
    unsigned long long n;
    size_t prefix_len = strlen(prefix);

    if (strncmp(line, prefix, prefix_len) != 0)
        return 0;
    value = line + prefix_len;
    if (!*value)
        return 0;
    errno = 0;
    n = strtoull(value, &tail, 10);
    if (errno || !tail || *tail || n > SIZE_MAX)
        return 0;
    *out = (size_t)n;
    return 1;
}

static int parse_code_header(const char *line, long *out)
{
    const char prefix[] = "CODE ";
    char *tail = NULL;
    long n;

    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0)
        return 0;
    errno = 0;
    n = strtol(line + sizeof(prefix) - 1, &tail, 10);
    if (errno || !tail || *tail)
        return 0;
    *out = n;
    return 1;
}

static void webkitd_reset(void)
{
    if (webkitd.in_fd >= 0)
        close(webkitd.in_fd);
    if (webkitd.out_fd >= 0)
        close(webkitd.out_fd);
    if (webkitd.err_fd >= 0)
        close(webkitd.err_fd);
    if (webkitd.pid > 0)
        waitpid(webkitd.pid, NULL, WNOHANG);
    webkitd.pid = -1;
    webkitd.in_fd = -1;
    webkitd.out_fd = -1;
    webkitd.err_fd = -1;
    webkitd.err_pos = 0;
}

static void webkitd_stop(void)
{
    if (webkitd.pid > 0)
        kill(webkitd.pid, SIGTERM);
    webkitd_reset();
}

static int webkitd_is_running(void)
{
    if (webkitd.pid <= 0)
        return 0;
    if (kill(webkitd.pid, 0) == 0 || errno == EPERM)
        return 1;
    webkitd_reset();
    return 0;
}

static void webkitd_read_stderr(char *out, size_t outsz, const char *fallback)
{
    Buffer err = {0};
    off_t end;

    if (webkitd.err_fd >= 0) {
        end = lseek(webkitd.err_fd, 0, SEEK_END);
        if (end >= 0 && lseek(webkitd.err_fd, webkitd.err_pos, SEEK_SET) >= 0) {
            read_fd_limited(webkitd.err_fd, &err, 65536, 0);
            webkitd.err_pos = end;
        }
    }
    copy_error_excerpt(out, outsz,
                       err.data && *err.data ? err.data : fallback);
    buf_clear(&err);
}

static int webkitd_start(FetchResult *result)
{
    int to_child[2] = { -1, -1 };
    int from_child[2] = { -1, -1 };
    int errfd = -1;
    pid_t pid;
    char err_template[] = "/tmp/simplebrowse-webkitd.XXXXXX";

    if (webkitd_is_running())
        return 1;

    webkitd_reset();
    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        snprintf(result->error, sizeof(result->error),
                 "pipe failed: %s", strerror(errno));
        goto fail;
    }
    errfd = mkstemp(err_template);
    if (errfd >= 0)
        unlink(err_template);

    pid = fork();
    if (pid < 0) {
        snprintf(result->error, sizeof(result->error),
                 "fork failed: %s", strerror(errno));
        goto fail;
    }
    if (pid == 0) {
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        if (errfd >= 0)
            dup2(errfd, STDERR_FILENO);
        close(to_child[0]);
        close(from_child[1]);
        if (errfd >= 0)
            close(errfd);

        setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);

        execlp(WEBKITD_HELPER, WEBKITD_HELPER, (char *)NULL);
        execl("./" WEBKITD_HELPER, WEBKITD_HELPER, (char *)NULL);
        dprintf(STDERR_FILENO, "%s: %s\n", WEBKITD_HELPER, strerror(errno));
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    webkitd.pid = pid;
    webkitd.in_fd = to_child[1];
    webkitd.out_fd = from_child[0];
    webkitd.err_fd = errfd;
    webkitd.err_pos = 0;
    return 1;

fail:
    if (to_child[0] >= 0) close(to_child[0]);
    if (to_child[1] >= 0) close(to_child[1]);
    if (from_child[0] >= 0) close(from_child[0]);
    if (from_child[1] >= 0) close(from_child[1]);
    if (errfd >= 0) close(errfd);
    return 0;
}

static int webkitd_send_load(const char *url, const char *payload, FetchResult *result)
{
    char header[128];
    size_t url_len = strlen(url ? url : "");
    size_t payload_len = strlen(payload ? payload : "");
    int n;

    if (payload) {
        n = snprintf(header, sizeof(header), "SUBMIT %zu %zu\n",
                     url_len, payload_len);
    } else {
        n = snprintf(header, sizeof(header), "LOAD %zu\n", url_len);
    }
    if (!snprintf_ok(n, sizeof(header))) {
        snprintf(result->error, sizeof(result->error), "WebKit command too large");
        return 0;
    }
    if (!write_full(webkitd.in_fd, header, (size_t)n) ||
        !write_full(webkitd.in_fd, url ? url : "", url_len) ||
        !write_full(webkitd.in_fd, "\n", 1))
        return 0;
    if (payload &&
        (!write_full(webkitd.in_fd, payload, payload_len) ||
         !write_full(webkitd.in_fd, "\n", 1)))
        return 0;
    return 1;
}

static int webkitd_read_response(FetchResult *result, char **html_out)
{
    Buffer line = {0};
    char status[32] = "";
    char *url = NULL;
    char *title = NULL;
    char *error = NULL;
    char *html = NULL;
    size_t url_len = 0;
    size_t title_len = 0;
    size_t error_len = 0;
    size_t html_len = 0;
    long code = 0;
    int ok = 0;

    *html_out = NULL;
    if (!read_line_limited(webkitd.out_fd, &line, 256) ||
        strcmp(line.data ? line.data : "", WEBKITD_RESPONSE_HEADER)) {
        snprintf(result->error, sizeof(result->error),
                 "invalid WebKit daemon response");
        goto done;
    }
    if (!read_line_limited(webkitd.out_fd, &line, 256) ||
        strncmp(line.data ? line.data : "", "STATUS ", 7)) {
        snprintf(result->error, sizeof(result->error),
                 "missing WebKit daemon status");
        goto done;
    }
    snprintf(status, sizeof(status), "%s", line.data + 7);
    if (!read_line_limited(webkitd.out_fd, &line, 256) ||
        !parse_code_header(line.data ? line.data : "", &code) ||
        !read_line_limited(webkitd.out_fd, &line, 256) ||
        !parse_size_header(line.data ? line.data : "", "URL_LENGTH ", &url_len) ||
        !read_line_limited(webkitd.out_fd, &line, 256) ||
        !parse_size_header(line.data ? line.data : "", "TITLE_LENGTH ", &title_len) ||
        !read_line_limited(webkitd.out_fd, &line, 256) ||
        !parse_size_header(line.data ? line.data : "", "ERROR_LENGTH ", &error_len) ||
        !read_line_limited(webkitd.out_fd, &line, 256) ||
        !parse_size_header(line.data ? line.data : "", "HTML_LENGTH ", &html_len) ||
        !read_line_limited(webkitd.out_fd, &line, 1) ||
        (line.data && *line.data)) {
        snprintf(result->error, sizeof(result->error),
                 "malformed WebKit daemon response");
        goto done;
    }

    url = read_exact_alloc(webkitd.out_fd, url_len, URL_MAX * 4u);
    title = read_exact_alloc(webkitd.out_fd, title_len, 65536u);
    error = read_exact_alloc(webkitd.out_fd, error_len, 65536u);
    html = read_exact_alloc(webkitd.out_fd, html_len, JS_RESPONSE_LIMIT);
    if (!url || !title || !error || !html) {
        snprintf(result->error, sizeof(result->error),
                 "truncated WebKit daemon response");
        goto done;
    }

    result->code = code;
    snprintf(result->reason, sizeof(result->reason), "%s",
             code == 200 ? "OK" : "WebKit");
    snprintf(result->effective, sizeof(result->effective), "%s", url);
    snprintf(result->title, sizeof(result->title), "%s", title);

    if (strcmp(status, "OK") != 0) {
        snprintf(result->error, sizeof(result->error), "%s",
                 *error ? error : "WebKit load failed");
        goto done;
    }
    if (!html_len) {
        snprintf(result->error, sizeof(result->error),
                 "empty WebKit daemon snapshot");
        goto done;
    }

    *html_out = html;
    html = NULL;
    result->network_error = 0;
    ok = 1;

done:
    free(url);
    free(title);
    free(error);
    free(html);
    buf_clear(&line);
    return ok ? 1 : (status[0] || code ? 0 : -1);
}

static char *fetch_url_webkit(const char *url, const char *payload,
                              FetchResult *result)
{
    char *html = NULL;
    int attempt;

    memset(result, 0, sizeof(*result));
    snprintf(result->effective, sizeof(result->effective), "%s", url);
    result->network_error = 1;

    for (attempt = 0; attempt < 2; attempt++) {
        if (!webkitd_start(result))
            return NULL;
        if (!webkitd_send_load(url, payload, result)) {
            webkitd_read_stderr(result->error, sizeof(result->error),
                                "failed to write to WebKit daemon");
            webkitd_stop();
            continue;
        }
        {
            int response = webkitd_read_response(result, &html);

            if (response > 0)
                return html;
            if (response == 0)
                return NULL;
        }
        if (html) {
            free(html);
            html = NULL;
        }
        webkitd_read_stderr(result->error, sizeof(result->error),
                            "failed to read from WebKit daemon");
        webkitd_stop();
    }
    return NULL;
}

static char *fetch_url_js(const char *url, FetchResult *result)
{
    return fetch_url_webkit(url, NULL, result);
}

static char *fetch_url_js_submit(const char *url, const char *payload,
                                 FetchResult *result)
{
    return fetch_url_webkit(url, payload ? payload : "{}", result);
}

static char *http_fallback_url(const char *url)
{
    Buffer b = {0};

    if (strncasecmp(url, "https://", 8)) return NULL;
    buf_addn(&b, "http://", 7);
    buf_addn(&b, url + 8, strlen(url + 8));
    return b.data;
}

static int url_is_https(const char *url)
{
    return url && !strncasecmp(url, "https://", 8);
}

static int failed_http_fallback_returned_to_https(const FetchResult *retry)
{
    return retry && retry->network_error && retry->code >= 300 &&
           retry->code < 400 && url_is_https(retry->effective);
}

static int curl_error_should_retry_js(CURLcode code)
{
    return code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR ||
           code == CURLE_SSL_CONNECT_ERROR ||
           code == CURLE_WEIRD_SERVER_REPLY ||
           code == CURLE_HTTP2;
}

static char *normalize_input_url(const char *input)
{
    char *trimmed = trim_copy(input);
    char *out;
    Buffer b = {0};

    if (!*trimmed) return trimmed;
    if (starts_http_url(trimmed)) return trimmed;

    buf_addn(&b, "https://", 8);
    buf_addn(&b, trimmed, strlen(trimmed));
    out = b.data;
    free(trimmed);
    return out;
}

static size_t page_meaningful_chars(Page *p)
{
    const char *s = p && p->text ? p->text : "";
    size_t n = 0;

    while (*s) {
        if (isalnum((unsigned char)*s))
            n++;
        s++;
    }
    return n;
}

static int html_contains_ci(const char *html, size_t len, const char *needle)
{
    return ci_find(html, html + len, needle) != NULL;
}

static int html_count_ci(const char *html, size_t len, const char *needle)
{
    const char *p = html;
    const char *end = html + len;
    size_t needle_len = strlen(needle);
    int count = 0;

    if (!needle_len) return 0;
    while ((p = ci_find(p, end, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static int html_looks_browser_rejected(const char *html, size_t html_len)
{
    int mentions_captcha;

    if (!html || !html_len)
        return 0;
    if (html_contains_ci(html, html_len, "checking your browser") ||
        html_contains_ci(html, html_len, "just a moment") ||
        html_contains_ci(html, html_len, "cf-browser-verification") ||
        html_contains_ci(html, html_len, "cf-challenge") ||
        html_contains_ci(html, html_len, "cloudflare challenge") ||
        html_contains_ci(html, html_len, "browser check") ||
        html_contains_ci(html, html_len, "bot challenge") ||
        html_contains_ci(html, html_len, "unusual traffic") ||
        html_contains_ci(html, html_len, "our systems have detected unusual traffic") ||
        html_contains_ci(html, html_len, "sorry/index") ||
        html_contains_ci(html, html_len, "/sorry/"))
        return 1;

    mentions_captcha = html_contains_ci(html, html_len, "captcha");
    if (mentions_captcha &&
        (html_contains_ci(html, html_len, "complete the captcha") ||
         html_contains_ci(html, html_len, "solve the captcha") ||
         html_contains_ci(html, html_len, "verify you are human") ||
         html_contains_ci(html, html_len, "prove you are human") ||
         html_contains_ci(html, html_len, "human verification") ||
         html_contains_ci(html, html_len, "recaptcha challenge") ||
         html_contains_ci(html, html_len, "hcaptcha challenge")))
        return 1;

    return 0;
}

static int url_should_retry_js(const char *url)
{
    const char *end = url ? url + strlen(url) : "";

    return url && *url &&
           (ci_find(url, end, "/sorry/") ||
            ci_find(url, end, "consent.google.") ||
            ci_find(url, end, "challenge-platform") ||
            ci_find(url, end, "cdn-cgi/challenge-platform"));
}

static int url_host_copy(const char *url, char *host, size_t hostsz)
{
    const char *p;
    const char *end;
    size_t i;
    size_t len;

    if (!hostsz)
        return 0;
    host[0] = 0;
    if (!url)
        return 0;
    if (!strncasecmp(url, "https://", 8))
        p = url + 8;
    else if (!strncasecmp(url, "http://", 7))
        p = url + 7;
    else
        return 0;

    end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#' && *end != ':')
        end++;
    len = (size_t)(end - p);
    if (!len)
        return 0;
    if (len >= hostsz)
        len = hostsz - 1;
    for (i = 0; i < len; i++)
        host[i] = (char)tolower((unsigned char)p[i]);
    host[len] = 0;
    return 1;
}

static int host_is_google(const char *host)
{
    if (!host)
        return 0;
    if (!strncmp(host, "www.", 4))
        host += 4;
    return !strncmp(host, "google.", 7) || strstr(host, ".google.") != NULL;
}

static int webkit_first_url(const char *url)
{
    char host[256];

    if (url_should_retry_js(url))
        return 1;
    if (!url_host_copy(url, host, sizeof(host)))
        return 0;
    if (host_is_google(host))
        return 1;
    return !strcmp(host, "duckduckgo.com") ||
           !strcmp(host, "www.duckduckgo.com") ||
           !strcmp(host, "html.duckduckgo.com") ||
           !strcmp(host, "lite.duckduckgo.com") ||
           !strcmp(host, "bing.com") ||
           !strcmp(host, "www.bing.com") ||
           !strcmp(host, "search.yahoo.com") ||
           !strcmp(host, "search.brave.com") ||
           !strcmp(host, "reddit.com") ||
           !strcmp(host, "www.reddit.com") ||
           !strcmp(host, "old.reddit.com");
}

static const char *webkit_retry_target(const char *original_url,
                                       const char *effective_url)
{
    if (!effective_url || !*effective_url)
        return original_url;
    if (url_should_retry_js(effective_url))
        return original_url;
    return effective_url;
}

static int webkit_snapshot_rejected(const char *url, const char *html,
                                    size_t html_len,
                                    const FetchResult *result)
{
    if (url_should_retry_js(url))
        return 1;
    if (result && result->code == 429)
        return 1;
    if (html_looks_browser_rejected(html, html_len))
        return 1;
    return 0;
}

static int static_page_should_retry_js(const char *url, const char *html,
                                       size_t html_len, Page *p)
{
    size_t chars = page_meaningful_chars(p);
    int scripts;

    if (!html || !html_len) return 0;

    if (url_should_retry_js(url))
        return 1;

    if (html_contains_ci(html, html_len, "enable javascript") ||
        html_contains_ci(html, html_len, "requires javascript") ||
        html_contains_ci(html, html_len, "javascript is disabled") ||
        html_contains_ci(html, html_len, "please turn on javascript") ||
        html_contains_ci(html, html_len, "please enable js") ||
        html_contains_ci(html, html_len, "turn on javascript"))
        return 1;

    if (html_looks_browser_rejected(html, html_len))
        return 1;

    if (chars < 80 && html_len > 4096 &&
        (html_contains_ci(html, html_len, "<body") ||
         html_contains_ci(html, html_len, "<main") ||
         html_contains_ci(html, html_len, "<article") ||
         html_contains_ci(html, html_len, "<div")))
        return 1;

    if (chars < 180 &&
        (html_contains_ci(html, html_len, "__NEXT_DATA__") ||
         html_contains_ci(html, html_len, "__NUXT__") ||
         html_contains_ci(html, html_len, "data-reactroot") ||
         html_contains_ci(html, html_len, "id=\"root\"") ||
         html_contains_ci(html, html_len, "id='root'") ||
         html_contains_ci(html, html_len, "id=\"app\"") ||
         html_contains_ci(html, html_len, "id='app'")))
        return 1;

    scripts = html_count_ci(html, html_len, "<script");
    if (chars < 350 && scripts >= 8)
        return 1;

    if (chars < 120 && html_len > 20000 && scripts >= 3)
        return 1;

    return 0;
}

static int http_status_should_retry_js(long code)
{
    return code == 400 || code == 401 || code == 403 || code == 429 || code == 503;
}

static int url_host_matches(const char *url, const char *host)
{
    const char *p;
    size_t host_len = strlen(host);

    if (!strncasecmp(url, "https://", 8))
        p = url + 8;
    else if (!strncasecmp(url, "http://", 7))
        p = url + 7;
    else
        return 0;

    return !strncasecmp(p, host, host_len) &&
           (p[host_len] == '/' || p[host_len] == ':' ||
            p[host_len] == '?' || p[host_len] == '#' ||
            p[host_len] == '\0');
}

static char *url_host_label(const char *url)
{
    const char *p;
    const char *end;

    if (!strncasecmp(url, "https://", 8))
        p = url + 8;
    else if (!strncasecmp(url, "http://", 7))
        p = url + 7;
    else
        return xstrdup_local("");

    end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#' && *end != ':')
        end++;
    if (end - p > 4 && !strncasecmp(p, "www.", 4))
        p += 4;
    return xstrndup_local(p, (size_t)(end - p));
}

static int text_is_numericish(const char *s)
{
    int saw_digit = 0;

    while (*s) {
        unsigned char c = (unsigned char)*s++;

        if (isdigit(c)) {
            saw_digit = 1;
            continue;
        }
        if (isspace(c) || c == '.' || c == ',' || c == 'k' || c == 'K' ||
            c == 'm' || c == 'M' || c == '+' || c == '-')
            continue;
        return 0;
    }
    return saw_digit;
}

static int text_is_domainish(const char *s)
{
    int has_dot = 0;
    int has_space = 0;
    int has_alpha = 0;

    while (*s) {
        unsigned char c = (unsigned char)*s++;

        if (isspace(c)) has_space = 1;
        if (isalpha(c)) has_alpha = 1;
        if (c == '.') has_dot = 1;
    }
    return has_alpha && has_dot && !has_space;
}

static int reddit_label_is_noise(const char *label)
{
    static const char *exact[] = {
        "about this ad", "advertise", "blog", "careers", "chat", "close",
        "comments", "give award", "hide", "join", "learn more", "loading",
        "loading...", "log in", "login", "message the mods", "next",
        "permalink", "popular", "preferences", "prev", "privacy policy",
        "report", "reply", "save", "share", "sign up", "source",
        "submit a new link", "submit a new text post", "terms", "wiki",
        NULL
    };
    char *trimmed = trim_copy(label);
    size_t len = strlen(trimmed);
    int i;
    int noise = 0;

    if (len < 4 || text_is_numericish(trimmed) || text_is_domainish(trimmed))
        noise = 1;
    if (!noise && !strncasecmp(trimmed, "self.", 5))
        noise = 1;
    if (!noise && ci_find(trimmed, trimmed + len, "everywhere argentina"))
        noise = 1;
    if (!noise && ci_find(trimmed, trimmed + len, "alaska alabama"))
        noise = 1;
    for (i = 0; !noise && exact[i]; i++) {
        if (!strcasecmp(trimmed, exact[i]))
            noise = 1;
    }

    free(trimmed);
    return noise;
}

static int reddit_url_is_noise(const char *url)
{
    const char *end = url + strlen(url);

    return ci_find(url, end, "geo_filter=") ||
           ci_find(url, end, "/domain/") ||
           ci_find(url, end, "/login") ||
           ci_find(url, end, "/message/") ||
           ci_find(url, end, "/prefs") ||
           ci_find(url, end, "/report") ||
           ci_find(url, end, "/search") ||
           ci_find(url, end, "/submit") ||
           ci_find(url, end, "/user/");
}

static int reddit_link_seen(Page *p, const char *label, const char *url)
{
    size_t i;

    for (i = 0; i < p->link_count; i++) {
        if (!strcmp(p->links[i].url, url) &&
            !strcasecmp(p->links[i].label, label))
            return 1;
    }
    return 0;
}

static void reddit_heading_from_url(const char *url, char *out, size_t outsz)
{
    const char *p;
    const char *name;
    const char *end;

    snprintf(out, outsz, "Reddit");
    if (!url)
        return;
    p = ci_find(url, url + strlen(url), "old.reddit.com/r/");
    if (!p)
        return;
    name = p + strlen("old.reddit.com/r/");
    end = name;
    while (*end && *end != '/' && *end != '?' && *end != '#')
        end++;
    if (end > name && (size_t)(end - name) < outsz - 3)
        snprintf(out, outsz, "r/%.*s", (int)(end - name), name);
}

static void normalize_reddit_listing_page(Page *p)
{
    Buffer out = {0};
    Page links_page = {0};
    size_t i;
    size_t kept = 0;
    char heading_buf[128];
    const char *heading = heading_buf;
    char *new_title;

    if (!p || !p->url || !url_host_matches(p->url, "old.reddit.com"))
        return;

    links_page.layout_width = -1;
    reddit_heading_from_url(p->url, heading_buf, sizeof(heading_buf));
    if (!strcmp(heading_buf, "Reddit") &&
        p->title && *p->title && strcasecmp(p->title, "reddit") &&
        !ci_find(p->title, p->title + strlen(p->title), "about this ad"))
        heading = p->title;
    buf_addn(&out, heading, strlen(heading));
    buf_addn(&out, "\n\n", 2);
    buf_addn(&out, "------------------------------------------------------------------------\n\n",
             strlen("------------------------------------------------------------------------\n\n"));

    for (i = 0; i < p->link_count && kept < 80; i++) {
        Link *link = &p->links[i];
        char *label;
        char *host;
        size_t marker;

        if (!link->url || !*link->url || !link->label || !*link->label)
            continue;
        if (reddit_url_is_noise(link->url) || reddit_label_is_noise(link->label))
            continue;
        if (reddit_link_seen(&links_page, link->label, link->url))
            continue;

        label = trim_copy(link->label);
        host = url_host_label(link->url);
        buf_addn(&out, "- ", 2);
        marker = out.len;
        buf_addn(&out, label, strlen(label));
        if (*host && strcasecmp(host, "old.reddit.com")) {
            buf_addn(&out, " (", 2);
            buf_addn(&out, host, strlen(host));
            buf_addc(&out, ')');
        }
        buf_addn(&out, "\n\n", 2);
        page_add_link(&links_page, label, link->url, marker);
        kept++;
        free(host);
        free(label);
    }

    if (kept == 0) {
        page_free(&links_page);
        buf_clear(&out);
        return;
    }

    new_title = xstrdup_local(heading);
    free(p->text);
    free(p->title);
    for (i = 0; i < p->link_count; i++) {
        free(p->links[i].label);
        free(p->links[i].url);
    }
    for (i = 0; i < p->control_count; i++)
        control_free(&p->controls[i]);
    free(p->links);
    free(p->controls);
    free(p->stops);
    free(p->display);
    p->text = out.data ? out.data : xstrdup_local("");
    p->title = new_title;
    p->links = links_page.links;
    p->link_count = links_page.link_count;
    p->link_cap = links_page.link_cap;
    p->controls = NULL;
    p->control_count = 0;
    p->control_cap = 0;
    p->stops = NULL;
    p->stop_count = 0;
    p->stop_cap = 0;
    p->display = NULL;
    p->display_count = 0;
    p->display_cap = 0;
    p->layout_width = -1;
}

static void add_display_line(Page *p, size_t start, size_t len)
{
    size_t i;
    int link = -1;

    if (p->display_count == p->display_cap) {
        p->display_cap = p->display_cap ? p->display_cap * 2 : 128;
        p->display = xrealloc(p->display, p->display_cap * sizeof(*p->display));
    }

    for (i = 0; i < p->link_count; i++) {
        if (p->links[i].marker_offset >= start &&
            p->links[i].marker_offset < start + len) {
            link = (int)i;
            break;
        }
    }

    p->display[p->display_count].start = start;
    p->display[p->display_count].len = len;
    p->display[p->display_count].link = link;
    p->display_count++;
}

static void clear_link_stops(Page *p)
{
    free(p->stops);
    p->stops = NULL;
    p->stop_count = 0;
    p->stop_cap = 0;
}

static void page_add_stop(Page *p, int link_index, size_t start, size_t end,
                          int start_line, int end_line)
{
    if (p->stop_count == p->stop_cap) {
        p->stop_cap = p->stop_cap ? p->stop_cap * 2 : 32;
        p->stops = xrealloc(p->stops, p->stop_cap * sizeof(*p->stops));
    }

    p->stops[p->stop_count].first_link = link_index;
    p->stops[p->stop_count].last_link = link_index;
    p->stops[p->stop_count].kind = STOP_LINK;
    p->stops[p->stop_count].control_index = -1;
    p->stops[p->stop_count].start = start;
    p->stops[p->stop_count].end = end;
    p->stops[p->stop_count].start_line = start_line;
    p->stops[p->stop_count].end_line = end_line;
    p->stop_count++;
}

static void page_add_control_stop(Page *p, int control_index, size_t start, size_t end,
                                  int start_line, int end_line)
{
    if (p->stop_count == p->stop_cap) {
        p->stop_cap = p->stop_cap ? p->stop_cap * 2 : 32;
        p->stops = xrealloc(p->stops, p->stop_cap * sizeof(*p->stops));
    }

    p->stops[p->stop_count].first_link = -1;
    p->stops[p->stop_count].last_link = -1;
    p->stops[p->stop_count].kind = STOP_CONTROL;
    p->stops[p->stop_count].control_index = control_index;
    p->stops[p->stop_count].start = start;
    p->stops[p->stop_count].end = end;
    p->stops[p->stop_count].start_line = start_line;
    p->stops[p->stop_count].end_line = end_line;
    p->stop_count++;
}

static int text_has_ascii_word(const char *s)
{
    while (s && *s) {
        if (isalnum((unsigned char)*s)) return 1;
        s++;
    }
    return 0;
}

static int text_has_visible_nonspace(const char *s)
{
    while (s && *s) {
        unsigned char c = (unsigned char)*s;

        if (!isspace(c) && c != 0xc2 && c != 0xb6) return 1;
        s++;
    }
    return 0;
}

static int link_label_is_navigation_noise(const char *label)
{
    static const char *exact[] = {
        "#", "¶", "permalink", "anchor", "section link", "edit",
        "open form target",
        "profile", "canonical", "me", "amphtml", NULL
    };
    char *trimmed = trim_copy(label);
    size_t len = strlen(trimmed);
    int noise = 0;
    int i;

    if (!len || !text_has_visible_nonspace(trimmed)) {
        free(trimmed);
        return 1;
    }

    for (i = 0; exact[i]; i++) {
        if (!strcasecmp(trimmed, exact[i])) {
            noise = 1;
            break;
        }
    }
    if (!noise && len <= 3 && !text_has_ascii_word(trimmed))
        noise = 1;

    free(trimmed);
    return noise;
}

static size_t url_without_fragment_len(const char *url)
{
    return strcspn(url ? url : "", "#");
}

static int link_is_same_page_fragment(Page *p, Link *link)
{
    size_t page_len;
    size_t link_len;

    if (!link->url || !strchr(link->url, '#')) return 0;
    if (!p->url || !*p->url) return 0;

    page_len = url_without_fragment_len(p->url);
    link_len = url_without_fragment_len(link->url);
    return page_len == link_len && ascii_eqn(p->url, link->url, page_len);
}

static int display_line_for_offset(Page *p, size_t offset, int *line_out)
{
    size_t i;

    for (i = 0; i < p->display_count; i++) {
        DisplayLine *line = &p->display[i];

        if (!line->len) continue;
        if (offset >= line->start && offset < line->start + line->len) {
            if (line_out) *line_out = (int)i;
            return 1;
        }
    }
    return 0;
}

static void compact_link_visible_range(Page *p, size_t full_start,
                                       size_t full_end, size_t *start_out,
                                       size_t *end_out);

static int link_visible_range(Page *p, int link_index, size_t *start_out,
                              size_t *end_out, int *start_line_out,
                              int *end_line_out)
{
    Link *link;
    const char *text = p->text ? p->text : "";
    size_t text_len = strlen(text);
    size_t start;
    size_t end;
    int start_line;
    int end_line;

    if (link_index < 0 || (size_t)link_index >= p->link_count)
        return 0;
    link = &p->links[link_index];
    if (link_label_is_navigation_noise(link->label)) return 0;
    if (link_is_same_page_fragment(p, link)) return 0;
    if (link->marker_offset >= text_len) return 0;

    start = link->marker_offset;
    end = start + strlen(link->label ? link->label : "");
    if (end > text_len) end = text_len;
    while (start < end && isspace((unsigned char)text[start])) start++;
    while (end > start && isspace((unsigned char)text[end - 1])) end--;
    if (end <= start) return 0;
    compact_link_visible_range(p, start, end, &start, &end);
    if (end <= start) return 0;
    if (!display_line_for_offset(p, start, &start_line)) return 0;
    if (!display_line_for_offset(p, end - 1, &end_line)) return 0;

    *start_out = start;
    *end_out = end;
    *start_line_out = start_line;
    *end_line_out = end_line;
    return 1;
}

static int control_visible_range(Page *p, int control_index, size_t *start_out,
                                 size_t *end_out, int *start_line_out,
                                 int *end_line_out)
{
    FormControl *control;
    const char *text = p->text ? p->text : "";
    size_t text_len = strlen(text);
    size_t start;
    size_t end;
    int start_line;
    int end_line;

    if (control_index < 0 || (size_t)control_index >= p->control_count)
        return 0;
    control = &p->controls[control_index];
    if (control->marker_offset >= text_len) return 0;
    start = control->marker_offset;
    end = start + (control->display_len ? control->display_len :
                   strlen(control->label ? control->label : ""));
    if (end > text_len) end = text_len;
    while (start < end && isspace((unsigned char)text[start])) start++;
    while (end > start && isspace((unsigned char)text[end - 1])) end--;
    if (end <= start) return 0;
    if (!display_line_for_offset(p, start, &start_line)) return 0;
    if (!display_line_for_offset(p, end - 1, &end_line)) return 0;

    *start_out = start;
    *end_out = end;
    *start_line_out = start_line;
    *end_line_out = end_line;
    return 1;
}

static int link_gap_is_joiner(Page *p, size_t start, size_t end)
{
    const char *text = p->text ? p->text : "";
    size_t i;

    if (end < start) return 1;
    if (end - start > 4) return 0;
    for (i = start; i < end; i++) {
        unsigned char c = (unsigned char)text[i];

        if (isalnum(c)) return 0;
        if (c >= 0x80) continue;
        if (!isspace(c) && !strchr("/-_.:|+,&", c)) return 0;
    }
    return 1;
}

static void trim_text_range(Page *p, size_t *start, size_t *end)
{
    const char *text = p->text ? p->text : "";

    while (*start < *end && isspace((unsigned char)text[*start]))
        (*start)++;
    while (*end > *start && isspace((unsigned char)text[*end - 1]))
        (*end)--;
}

static int text_range_has_alpha(Page *p, size_t start, size_t end)
{
    const char *text = p->text ? p->text : "";

    while (start < end) {
        if (isalpha((unsigned char)text[start])) return 1;
        start++;
    }
    return 0;
}

static int text_range_contains_ci(Page *p, size_t start, size_t end,
                                  const char *needle)
{
    const char *text = p->text ? p->text : "";

    return ci_find(text + start, text + end, needle) != NULL;
}

static int text_range_quoted_tail(Page *p, size_t start, size_t end,
                                  size_t *quote_start, size_t *quote_end)
{
    const char *text = p->text ? p->text : "";
    size_t first = end;
    size_t last = end;
    size_t i;

    if (!text_range_contains_ci(p, start, end, " with the text "))
        return 0;
    for (i = start; i < end; i++) {
        if (text[i] == '"') {
            first = i;
            break;
        }
    }
    if (first == end) return 0;
    for (i = first + 1; i < end; i++) {
        if (text[i] == '"') {
            last = i;
            break;
        }
    }
    if (last <= first + 1) return 0;
    *quote_start = first + 1;
    *quote_end = last;
    return 1;
}

static int link_label_candidate_score(Page *p, size_t start, size_t end)
{
    size_t len;
    int score;

    trim_text_range(p, &start, &end);
    if (end <= start) return -100000;
    len = end - start;
    score = (int)(len > 80 ? 80 : len);
    if (!text_range_has_alpha(p, start, end)) score -= 200;
    if (len < 2) score -= 100;
    if (len > 120) score -= (int)(len - 120);
    if (text_range_contains_ci(p, start, end, "an image of")) score -= 180;
    if (text_range_contains_ci(p, start, end, "super imposed")) score -= 180;
    if (text_range_contains_ci(p, start, end, "primary image")) score -= 160;
    if (text_range_contains_ci(p, start, end, "autocomplete")) score -= 160;
    if (text_range_contains_ci(p, start, end, "allfor youtoday")) score -= 160;
    if (text_range_contains_ci(p, start, end, "cookie")) score -= 120;
    if (text_range_contains_ci(p, start, end, "privacy")) score -= 80;
    return score;
}

static void compact_link_visible_range(Page *p, size_t full_start, size_t full_end,
                                       size_t *start_out, size_t *end_out)
{
    int start_line;
    int end_line;
    int best_score = -100000;
    size_t best_start = full_start;
    size_t best_end = full_end;
    int i;

    trim_text_range(p, &full_start, &full_end);
    if (full_end <= full_start) {
        *start_out = full_start;
        *end_out = full_end;
        return;
    }

    if (!display_line_for_offset(p, full_start, &start_line) ||
        !display_line_for_offset(p, full_end - 1, &end_line)) {
        *start_out = full_start;
        *end_out = full_end;
        return;
    }

    if (start_line == end_line && full_end - full_start <= 100 &&
        link_label_candidate_score(p, full_start, full_end) > -50) {
        *start_out = full_start;
        *end_out = full_end;
        return;
    }

    for (i = start_line; i <= end_line; i++) {
        DisplayLine *line = &p->display[i];
        size_t cand_start = line->start > full_start ? line->start : full_start;
        size_t cand_end = line->start + line->len < full_end ?
                          line->start + line->len : full_end;
        size_t quote_start;
        size_t quote_end;
        int score;

        trim_text_range(p, &cand_start, &cand_end);
        if (cand_end <= cand_start) continue;
        if (text_range_quoted_tail(p, cand_start, cand_end,
                                   &quote_start, &quote_end)) {
            cand_start = quote_start;
            cand_end = quote_end;
        }
        score = link_label_candidate_score(p, cand_start, cand_end);
        if (score > best_score) {
            best_score = score;
            best_start = cand_start;
            best_end = cand_end;
        }
    }

    *start_out = best_start;
    *end_out = best_end;
}

static int link_stops_should_merge(Page *p, LinkStop *stop, Link *link,
                                   size_t start, size_t end, int start_line)
{
    if (start < stop->end &&
        end > stop->start &&
        start_line == stop->end_line)
        return 1;

    if (start_line == stop->end_line &&
        link_gap_is_joiner(p, stop->end, start))
        return 1;

    if (start_line == stop->end_line &&
        start - stop->end <= 24 &&
        link->url &&
        p->links[stop->first_link].url &&
        !strcmp(link->url, p->links[stop->first_link].url))
        return 1;

    (void)end;
    return 0;
}

static int text_ranges_equal_ci(Page *p, size_t a_start, size_t a_end,
                                size_t b_start, size_t b_end)
{
    const char *text = p->text ? p->text : "";

    trim_text_range(p, &a_start, &a_end);
    trim_text_range(p, &b_start, &b_end);
    if (a_end - a_start != b_end - b_start) return 0;
    return ascii_eqn(text + a_start, text + b_start, a_end - a_start);
}

static int recent_duplicate_link_stop(Page *p, int link_index,
                                      size_t start, size_t end,
                                      int start_line)
{
    int i;

    if (!p->links[link_index].url) return 0;
    for (i = (int)p->stop_count - 1; i >= 0; i--) {
        LinkStop *stop = &p->stops[i];
        Link *prev = &p->links[stop->first_link];

        if (stop->kind != STOP_LINK || stop->first_link < 0) continue;
        if (start_line - stop->end_line > 12) break;
        if (!prev->url || strcmp(prev->url, p->links[link_index].url))
            continue;

        /* Search result pages often repeat the same href on the title,
           display URL, and snippet. Treat nearby repeats as one stop so
           Down moves result-to-result instead of title-url-snippet. */
        if (start_line - stop->end_line <= 8)
            return 1;

        if (text_ranges_equal_ci(p, stop->start, stop->end, start, end))
            return 1;
    }
    return 0;
}

static int compare_stops(const void *a, const void *b)
{
    const LinkStop *sa = a;
    const LinkStop *sb = b;

    if (sa->start < sb->start) return -1;
    if (sa->start > sb->start) return 1;
    if (sa->kind < sb->kind) return -1;
    if (sa->kind > sb->kind) return 1;
    return 0;
}

static void build_link_stops(Page *p)
{
    size_t i;

    clear_link_stops(p);
    for (i = 0; i < p->link_count; i++) {
        size_t start;
        size_t end;
        int start_line;
        int end_line;

        if (!link_visible_range(p, (int)i, &start, &end, &start_line, &end_line))
            continue;

        if (p->stop_count) {
            LinkStop *last = &p->stops[p->stop_count - 1];

            if (link_stops_should_merge(p, last, &p->links[i], start, end,
                                        start_line)) {
                last->last_link = (int)i;
                if (start < last->start) {
                    last->start = start;
                    last->start_line = start_line;
                }
                if (end > last->end) {
                    last->end = end;
                    last->end_line = end_line;
                }
                continue;
            }
        }

        if (recent_duplicate_link_stop(p, (int)i, start, end, start_line))
            continue;

        page_add_stop(p, (int)i, start, end, start_line, end_line);
    }

    for (i = 0; i < p->control_count; i++) {
        size_t start;
        size_t end;
        int start_line;
        int end_line;

        if (!control_visible_range(p, (int)i, &start, &end, &start_line, &end_line))
            continue;
        page_add_control_stop(p, (int)i, start, end, start_line, end_line);
    }

    if (p->stop_count > 1)
        qsort(p->stops, p->stop_count, sizeof(*p->stops), compare_stops);
}

static size_t utf8_safe_prefix(const char *s, size_t n)
{
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
    return n ? n : 1;
}

static void page_layout(Page *p, int width)
{
    const char *text;
    size_t len;
    size_t pos = 0;

    if (width < 20) width = 20;
    if (p->layout_width == width) return;

    free(p->display);
    p->display = NULL;
    p->display_count = 0;
    p->display_cap = 0;
    clear_link_stops(p);
    p->layout_width = width;

    text = p->text ? p->text : "";
    len = strlen(text);
    if (!len) {
        add_display_line(p, 0, 0);
        return;
    }

    while (pos < len) {
        size_t line_end = pos;
        size_t hard_end;
        size_t remaining;

        while (pos < len && text[pos] == ' ') pos++;
        if (pos < len && text[pos] == '\n') {
            add_display_line(p, pos, 0);
            pos++;
            continue;
        }

        hard_end = pos;
        while (hard_end < len && text[hard_end] != '\n') hard_end++;
        remaining = hard_end - pos;
        if (remaining <= (size_t)width) {
            line_end = hard_end;
        } else {
            size_t limit = pos + (size_t)width;
            size_t break_at = limit;
            size_t s;

            for (s = limit; s > pos; s--) {
                if (isspace((unsigned char)text[s])) {
                    break_at = s;
                    break;
                }
            }
            if (break_at == limit)
                break_at = pos + utf8_safe_prefix(text + pos, (size_t)width);
            line_end = break_at;
        }

        while (line_end > pos && text[line_end - 1] == ' ') line_end--;
        add_display_line(p, pos, line_end - pos);

        pos = line_end;
        while (pos < len && text[pos] == ' ') pos++;
        if (pos < len && text[pos] == '\n') pos++;
    }

    build_link_stops(p);
}

static int line_for_offset(Page *p, size_t offset)
{
    int line;

    if (display_line_for_offset(p, offset, &line)) return line;
    return 0;
}

static size_t top_text_offset(App *a)
{
    if (!a->page.display_count || a->top < 0 ||
        (size_t)a->top >= a->page.display_count)
        return 0;
    return a->page.display[a->top].start;
}

static int find_match_from(const char *text, const char *query,
                           size_t start, int dir, size_t *offset)
{
    size_t len;
    size_t qlen;
    const char *begin;
    const char *end;
    const char *p;
    const char *match;
    const char *last = NULL;

    if (!text || !query || !*query) return 0;
    len = strlen(text);
    qlen = strlen(query);
    if (!len || qlen > len) return 0;
    if (start > len) start = len;

    begin = text;
    end = text + len;

    if (dir >= 0) {
        match = ci_find(begin + start, end, query);
        if (!match && start > 0)
            match = ci_find(begin, begin + start, query);
        if (!match) return 0;
        *offset = (size_t)(match - begin);
        return 1;
    }

    p = begin;
    while (p < begin + start && (match = ci_find(p, begin + start, query))) {
        last = match;
        p = match + 1;
    }
    if (!last) {
        p = begin + start;
        while (p < end && (match = ci_find(p, end, query))) {
            last = match;
            p = match + 1;
        }
    }
    if (!last) return 0;
    *offset = (size_t)(last - begin);
    return 1;
}

static void jump_to_match(App *a, size_t offset)
{
    int h, w;
    int body_h;
    int line;

    getmaxyx(stdscr, h, w);
    body_h = h - 3;
    w = browse_read_width(w);
    page_layout(&a->page, w);
    line = line_for_offset(&a->page, offset);
    if (line < 0) line = 0;
    if (body_h < 1) body_h = 1;
    if (line < a->top || line >= a->top + body_h)
        a->top = line > body_h / 2 ? line - body_h / 2 : 0;
    a->selected_link = -1;
    a->selected_control = -1;
}

static int search_page(App *a, int dir, size_t start)
{
    size_t offset;

    if (!a->find_query[0]) {
        set_status(a, "No search term");
        return 0;
    }

    if (!find_match_from(a->page.text, a->find_query, start, dir, &offset)) {
        snprintf(a->status, sizeof(a->status), "Not found: %s", a->find_query);
        a->has_match = 0;
        return 0;
    }

    a->match_offset = offset;
    a->match_len = strlen(a->find_query);
    a->has_match = 1;
    jump_to_match(a, offset);
    snprintf(a->status, sizeof(a->status), "Found: %s", a->find_query);
    return 1;
}

static int link_line(Page *p, int link_index)
{
    if (link_index < 0 || (size_t)link_index >= p->link_count)
        return 0;
    if (!p->display_count)
        return 0;
    return line_for_offset(p, p->links[link_index].marker_offset);
}

static int page_stop_index_for_link(Page *p, int link_index)
{
    size_t i;

    if (link_index < 0 || (size_t)link_index >= p->link_count)
        return -1;
    for (i = 0; i < p->stop_count; i++) {
        LinkStop *stop = &p->stops[i];
        size_t offset = p->links[link_index].marker_offset;

        if (stop->kind != STOP_LINK) continue;
        if (offset >= stop->start && offset < stop->end)
            return (int)i;
        if (link_index == stop->first_link)
            return (int)i;
    }
    return -1;
}

static int page_stop_index_for_control(Page *p, int control_index)
{
    size_t i;

    if (control_index < 0 || (size_t)control_index >= p->control_count)
        return -1;
    for (i = 0; i < p->stop_count; i++) {
        LinkStop *stop = &p->stops[i];

        if (stop->kind == STOP_CONTROL && stop->control_index == control_index)
            return (int)i;
    }
    return -1;
}

static int selected_stop_index(Page *p, int selected_link)
{
    return page_stop_index_for_link(p, selected_link);
}

static int selected_page_stop_index(App *a)
{
    if (a->selected_control >= 0)
        return page_stop_index_for_control(&a->page, a->selected_control);
    return page_stop_index_for_link(&a->page, a->selected_link);
}

static int link_on_line(Page *p, int link_index, int line_index)
{
    DisplayLine *line;
    size_t offset;

    if (link_index < 0 || line_index < 0 ||
        (size_t)link_index >= p->link_count ||
        (size_t)line_index >= p->display_count)
        return 0;

    line = &p->display[line_index];
    offset = p->links[link_index].marker_offset;
    return offset >= line->start && offset < line->start + line->len;
}

static int line_has_link(Page *p, int line_index)
{
    size_t i;

    if (line_index < 0 || (size_t)line_index >= p->display_count)
        return 0;
    for (i = 0; i < p->link_count; i++) {
        if (link_on_line(p, (int)i, line_index)) return 1;
    }
    return 0;
}

static void next_link_stop(App *a, int dir, int body_h, int body_w)
{
    int stop_index;
    int line = 0;
    int selected_stop;

    if (body_h < 1) body_h = 1;
    page_layout(&a->page, body_w);
    clamp_top(a, body_h, body_w);

    if (!a->page.stop_count) {
        set_status(a, "No visible links or fields");
        return;
    }

    selected_stop = selected_page_stop_index(a);
    if (selected_stop >= 0) {
        stop_index = selected_stop + (dir > 0 ? 1 : -1);
        if (stop_index < 0 || (size_t)stop_index >= a->page.stop_count) {
            set_status(a, dir > 0 ? "No next link or field" :
                                     "No previous link or field");
            return;
        }
    } else {
        stop_index = visible_stop_candidate(a, dir, body_h);
    }

    if (stop_index < 0) {
        stop_index = offscreen_stop_candidate(a, dir, body_h, &line);
        if (stop_index < 0) {
            set_status(a, dir > 0 ? "No next link or field" :
                                     "No previous link or field");
            return;
        }
        a->top = line;
        clamp_top(a, body_h, body_w);
    }

    if (a->page.stops[stop_index].kind == STOP_CONTROL) {
        set_selected_control_status(a, a->page.stops[stop_index].control_index);
        return;
    }
    set_selected_link_status(a, a->page.stops[stop_index].first_link);
    ensure_selected_visible(a, body_h, body_w);
}

static void set_status(App *a, const char *msg)
{
    snprintf(a->status, sizeof(a->status), "%s", msg ? msg : "");
}

static void stack_push(char **stack, int *count, const char *url)
{
    if (!url || !*url) return;
    if (*count == MAX_HISTORY) {
        free(stack[0]);
        memmove(&stack[0], &stack[1], sizeof(stack[0]) * (MAX_HISTORY - 1));
        *count = MAX_HISTORY - 1;
    }
    stack[(*count)++] = xstrdup_local(url);
}

static char *stack_pop(char **stack, int *count)
{
    char *url;

    if (*count <= 0) return NULL;
    url = stack[--(*count)];
    stack[*count] = NULL;
    return url;
}

static const char *stack_peek(char **stack, int count)
{
    return count > 0 ? stack[count - 1] : NULL;
}

static void stack_clear(char **stack, int *count)
{
    while (*count > 0)
        free(stack_pop(stack, count));
}

static void page_snapshot_free(PageSnapshot *snap)
{
    if (!snap) return;
    free(snap->url);
    page_free(&snap->page);
    memset(snap, 0, sizeof(*snap));
}

static int page_cache_find(App *a, const char *url)
{
    int i;

    if (!url || !*url) return -1;
    for (i = 0; i < a->page_cache_count; i++) {
        if (a->page_cache[i].url && !strcmp(a->page_cache[i].url, url))
            return i;
    }
    return -1;
}

static void page_cache_store_current(App *a, const char *url)
{
    PageSnapshot snap;
    int index;

    if (!url || !*url || !a->page.text) return;
    memset(&snap, 0, sizeof(snap));
    snap.url = xstrdup_local(url);
    if (!page_clone(&snap.page, &a->page)) {
        free(snap.url);
        return;
    }
    snap.top = a->top;
    snap.selected_link = a->selected_link;
    snap.selected_control = a->selected_control;
    snap.js_mode_active = a->js_mode_active;
    snap.code = 200;

    index = page_cache_find(a, url);
    if (index >= 0) {
        page_snapshot_free(&a->page_cache[index]);
        a->page_cache[index] = snap;
        return;
    }
    if (a->page_cache_count == a->page_cache_cap) {
        int new_cap = a->page_cache_cap ? a->page_cache_cap * 2 : 16;
        PageSnapshot *items = realloc(a->page_cache,
                                      (size_t)new_cap * sizeof(*items));

        if (!items) {
            page_snapshot_free(&snap);
            return;
        }
        a->page_cache = items;
        a->page_cache_cap = new_cap;
    }
    if (a->page_cache_count == MAX_HISTORY) {
        page_snapshot_free(&a->page_cache[0]);
        memmove(&a->page_cache[0], &a->page_cache[1],
                sizeof(a->page_cache[0]) * (MAX_HISTORY - 1));
        a->page_cache_count = MAX_HISTORY - 1;
    }
    a->page_cache[a->page_cache_count++] = snap;
}

static int page_cache_restore(App *a, const char *url, int nav_mode)
{
    int index = page_cache_find(a, url);
    Page restored;
    PageSnapshot *snap;
    char *old_url;
    int top;
    int selected_link;
    int selected_control;
    int js_mode_active;

    if (index < 0) return 0;
    snap = &a->page_cache[index];
    if (!page_clone(&restored, &snap->page)) return 0;

    old_url = xstrdup_local(a->current_url);
    top = snap->top;
    selected_link = snap->selected_link;
    selected_control = snap->selected_control;
    js_mode_active = snap->js_mode_active;

    finish_navigation(a, nav_mode, old_url, url, 1);
    page_free(&a->page);
    a->page = restored;
    snprintf(a->current_url, sizeof(a->current_url), "%s", url);
    remember_visited_url(a, url);
    snprintf(a->url_bar, sizeof(a->url_bar), "%s", url);
    a->url_len = (int)strlen(a->url_bar);
    a->url_pos = a->url_len;
    a->top = top;
    a->selected_link = selected_link;
    a->selected_control = selected_control;
    a->editing_control = -1;
    a->has_match = 0;
    a->js_mode_active = js_mode_active;
    a->mode = MODE_PAGE;
    a->url_focus = 0;
    a->find_focus = 0;
    a->bookmark_mode = 0;
    snprintf(a->status, sizeof(a->status), "History cache | %zu links | %zu fields | %s",
             a->page.link_count, a->page.control_count, url);
    free(old_url);
    return 1;
}

static void page_cache_clear(App *a)
{
    int i;

    for (i = 0; i < a->page_cache_count; i++)
        page_snapshot_free(&a->page_cache[i]);
    free(a->page_cache);
    a->page_cache = NULL;
    a->page_cache_count = 0;
    a->page_cache_cap = 0;
}

static void clear_number_entry(App *a)
{
    a->number_len = 0;
    a->number_buf[0] = 0;
}

static void append_number_entry(App *a, int ch)
{
    if (a->number_len >= (int)sizeof(a->number_buf) - 1) return;
    a->number_buf[a->number_len++] = (char)ch;
    a->number_buf[a->number_len] = 0;
    snprintf(a->status, sizeof(a->status), "Open link %s with Enter", a->number_buf);
}

static void free_bookmarks(App *a)
{
    size_t i;

    for (i = 0; i < a->bookmark_count; i++) {
        free(a->bookmarks[i].title);
        free(a->bookmarks[i].url);
    }
    free(a->bookmarks);
    a->bookmarks = NULL;
    a->bookmark_count = 0;
    a->bookmark_cap = 0;
}

static int add_bookmark_memory(App *a, const char *title, const char *url)
{
    Bookmark *items;
    size_t n;

    if (!url || !*url) return 0;
    if (a->bookmark_count == a->bookmark_cap) {
        n = a->bookmark_cap ? a->bookmark_cap * 2 : 32;
        items = realloc(a->bookmarks, n * sizeof(*items));
        if (!items) return 0;
        a->bookmarks = items;
        a->bookmark_cap = n;
    }

    a->bookmarks[a->bookmark_count].title = xstrdup_local(title && *title ? title : url);
    a->bookmarks[a->bookmark_count].url = xstrdup_local(url);
    a->bookmark_count++;
    return 1;
}

static int bookmark_exists(App *a, const char *url)
{
    size_t i;

    for (i = 0; i < a->bookmark_count; i++) {
        if (!strcmp(a->bookmarks[i].url, url)) return 1;
    }
    return 0;
}

static char *current_page_title(App *a)
{
    const char *text = a->page.text ? a->page.text : "";
    const char *end = strchr(text, '\n');
    char *title;
    char *trimmed;
    char *p;

    if (a->page.title && *a->page.title)
        return xstrdup_local(a->page.title);

    if (!end) end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    title = xstrndup_local(text, (size_t)(end - text));
    p = title;
    while (*p) {
        if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
        p++;
    }
    trimmed = trim_copy(title);
    if (!*trimmed) {
        free(title);
        free(trimmed);
        return xstrdup_local(a->current_url);
    }
    free(trimmed);
    return title;
}

static void init_bookmarks(App *a)
{
    char dir[PATH_MAX];
    FILE *fp;
    char line[URL_MAX + 512];

    if (!home_path(dir, sizeof(dir), ".config/simplebrowse")) return;
    mkdir_p(dir);
    if (!snprintf_ok(snprintf(a->bookmark_path, sizeof(a->bookmark_path),
                              "%s/bookmarks", dir), sizeof(a->bookmark_path)))
        return;

    fp = fopen(a->bookmark_path, "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        char *tab;
        char *url;
        char *title;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;

        tab = strchr(line, '\t');
        if (tab) {
            *tab++ = 0;
            url = trim_copy(line);
            title = trim_copy(tab);
        } else {
            url = trim_copy(line);
            title = xstrdup_local(url);
        }
        if (*url) add_bookmark_memory(a, *title ? title : url, url);
        free(url);
        free(title);
    }

    fclose(fp);
}

static void bookmark_current_page(App *a)
{
    FILE *fp;
    char *title;

    if (!a->current_url[0]) {
        set_status(a, "No page to bookmark");
        return;
    }
    if (bookmark_exists(a, a->current_url)) {
        set_status(a, "Already bookmarked");
        return;
    }
    if (!a->bookmark_path[0])
        init_bookmarks(a);
    if (!a->bookmark_path[0]) {
        set_status(a, "Cannot find HOME for bookmarks");
        return;
    }

    title = current_page_title(a);
    fp = fopen(a->bookmark_path, "a");
    if (!fp) {
        snprintf(a->status, sizeof(a->status), "Bookmark failed: %s", strerror(errno));
        free(title);
        return;
    }
    fprintf(fp, "%s\t%s\n", a->current_url, title);
    fclose(fp);
    add_bookmark_memory(a, title, a->current_url);
    snprintf(a->status, sizeof(a->status), "Bookmarked: %s", title);
    free(title);
}

static void open_bookmark_list(App *a)
{
    a->mode = MODE_BOOKMARKS;
    a->url_focus = 0;
    a->find_focus = 0;
    a->bookmark_mode = 1;
    if (a->selected_bookmark < 0 && a->bookmark_count)
        a->selected_bookmark = 0;
    set_status(a, a->bookmark_count ? "Bookmarks" : "No bookmarks");
}

static int next_available_page_path(char *out, size_t outsz)
{
    char dir[PATH_MAX];
    int i;

    if (!home_path(dir, sizeof(dir), "Downloads")) return 0;
    mkdir_p(dir);
    if (snprintf_ok(snprintf(out, outsz, "%s/simplebrowse-page.txt", dir), outsz) &&
        access(out, F_OK) != 0)
        return 1;

    for (i = 1; i < 10000; i++) {
        if (!snprintf_ok(snprintf(out, outsz, "%s/simplebrowse-page-%d.txt", dir, i), outsz))
            return 0;
        if (access(out, F_OK) != 0) return 1;
    }
    return 0;
}

static void save_page_text(App *a)
{
    char path[PATH_MAX];
    FILE *fp;

    if (!a->page.text) {
        set_status(a, "No page text to save");
        return;
    }
    if (!next_available_page_path(path, sizeof(path))) {
        set_status(a, "Cannot create save path");
        return;
    }

    fp = fopen(path, "w");
    if (!fp) {
        snprintf(a->status, sizeof(a->status), "Save failed: %s", strerror(errno));
        return;
    }
    fputs(a->page.text, fp);
    fputc('\n', fp);
    if (fclose(fp) != 0) {
        snprintf(a->status, sizeof(a->status), "Save failed: %s", strerror(errno));
        return;
    }
    snprintf(a->status, sizeof(a->status), "Saved %s", path);
}

static void open_external(App *a, const char *url)
{
    pid_t pid;
    int status;

    if (!url || !*url) {
        set_status(a, "No URL to open");
        return;
    }

    pid = fork();
    if (pid < 0) {
        snprintf(a->status, sizeof(a->status), "xdg-open failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(a->status, sizeof(a->status), "xdg-open failed: %s", strerror(errno));
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(a->status, sizeof(a->status), "xdg-open failed for %s", url);
        return;
    }
    snprintf(a->status, sizeof(a->status), "Opened externally: %s", url);
}

static void ensure_selected_visible(App *a, int body_h, int body_w)
{
    int stop_index;
    LinkStop *stop;

    if ((a->selected_link < 0 && a->selected_control < 0) || body_h < 1) return;
    page_layout(&a->page, body_w);
    stop_index = selected_page_stop_index(a);
    if (stop_index < 0) return;
    stop = &a->page.stops[stop_index];
    if (stop->start_line < a->top) a->top = stop->start_line;
    if (stop->end_line >= a->top + body_h)
        a->top = stop->end_line - body_h + 1;
}

static int page_max_top(Page *p, int body_h)
{
    if (body_h < 1) body_h = 1;
    if (p->display_count <= (size_t)body_h) return 0;
    return (int)p->display_count - body_h;
}

static void clamp_top(App *a, int body_h, int body_w)
{
    int max_top;

    page_layout(&a->page, body_w);
    max_top = page_max_top(&a->page, body_h);
    if (a->top < 0) a->top = 0;
    if (a->top > max_top) a->top = max_top;
}

static void scroll_page(App *a, int delta, int body_h, int body_w)
{
    a->selected_link = -1;
    a->selected_control = -1;
    a->top += delta;
    clamp_top(a, body_h, body_w);
}

static int stop_is_visible(App *a, int stop_index, int body_h)
{
    LinkStop *stop;

    if (stop_index < 0 || (size_t)stop_index >= a->page.stop_count)
        return 0;
    stop = &a->page.stops[stop_index];
    return stop->end_line >= a->top && stop->start_line < a->top + body_h;
}

static void set_selected_link_status(App *a, int link_index)
{
    a->selected_link = link_index;
    a->selected_control = -1;
    if (link_index >= 0 && (size_t)link_index < a->page.link_count)
        a->status[0] = 0;
}

static void set_selected_control_status(App *a, int control_index)
{
    a->selected_control = control_index;
    a->selected_link = -1;
    if (control_index >= 0 && (size_t)control_index < a->page.control_count)
        a->status[0] = 0;
}

static int visible_stop_candidate(App *a, int dir, int body_h)
{
    int count = (int)a->page.stop_count;
    int view_start = a->top;
    int view_end = a->top + body_h - 1;
    int i;

    if (dir > 0) {
        for (i = 0; i < count; i++) {
            int line = a->page.stops[i].start_line;

            if (line < view_start || line > view_end) continue;
            return i;
        }
    } else {
        for (i = count - 1; i >= 0; i--) {
            int line = a->page.stops[i].end_line;

            if (line < view_start || line > view_end) continue;
            return i;
        }
    }

    return -1;
}

static int offscreen_stop_candidate(App *a, int dir, int body_h, int *line_out)
{
    int count = (int)a->page.stop_count;
    int boundary = dir > 0 ? a->top + body_h : a->top - 1;
    int best = -1;
    int best_line = dir > 0 ? INT_MAX : -1;
    int i;

    for (i = 0; i < count; i++) {
        LinkStop *stop = &a->page.stops[i];
        int line = dir > 0 ? stop->start_line : stop->end_line;

        if (dir > 0) {
            if (line < boundary) continue;
            if (line < best_line) {
                best = i;
                best_line = line;
            }
        } else {
            if (line > boundary) continue;
            if (line > best_line || (line == best_line && i > best)) {
                best = i;
                best_line = line;
            }
        }
    }

    if (best >= 0 && line_out) *line_out = best_line;
    return best;
}

static void jump_visible_link(App *a, int dir)
{
    int h, w;
    int body_h;
    int body_w;

    getmaxyx(stdscr, h, w);
    body_h = h - 3;
    body_w = browse_read_width(w);
    next_link_stop(a, dir, body_h, body_w);
}

static int display_line_trim(Page *p, int line_index, size_t *start, size_t *len)
{
    DisplayLine *line;
    size_t s;
    size_t e;

    if (line_index < 0 || (size_t)line_index >= p->display_count)
        return 0;

    line = &p->display[line_index];
    s = line->start;
    e = line->start + line->len;
    while (s < e && isspace((unsigned char)p->text[s])) s++;
    while (e > s && isspace((unsigned char)p->text[e - 1])) e--;
    *start = s;
    *len = e - s;
    return *len > 0;
}

static int line_contains_ci(Page *p, int line_index, const char *needle)
{
    size_t start;
    size_t len;

    if (!display_line_trim(p, line_index, &start, &len)) return 0;
    return ci_find(p->text + start, p->text + start + len, needle) != NULL;
}

static int line_looks_clutter(Page *p, int line_index)
{
    size_t start;
    size_t len;

    if (!display_line_trim(p, line_index, &start, &len)) return 1;
    if (line_has_link(p, line_index)) return 1;
    if (len <= 3) return 1;
    return line_contains_ci(p, line_index, "menu") ||
           line_contains_ci(p, line_index, "navigation") ||
           line_contains_ci(p, line_index, "skip to") ||
           line_contains_ci(p, line_index, "search") ||
           line_contains_ci(p, line_index, "sign in") ||
           line_contains_ci(p, line_index, "log in");
}

static int first_content_line(Page *p)
{
    size_t limit = p->display_count < 120 ? p->display_count : 120;
    size_t i;

    for (i = 0; i < limit; i++) {
        if (!line_looks_clutter(p, (int)i)) return (int)i;
    }
    for (i = limit; i < p->display_count; i++) {
        size_t start;
        size_t len;

        if (display_line_trim(p, (int)i, &start, &len) && !line_has_link(p, (int)i))
            return (int)i;
    }
    return 0;
}

static int line_looks_heading(Page *p, int line_index)
{
    size_t start;
    size_t len;
    int prev_blank;
    int next_blank;
    char last;

    if (!display_line_trim(p, line_index, &start, &len)) return 0;
    if (line_has_link(p, line_index)) return 0;
    if (len < 4 || len > 120) return 0;
    if (line_contains_ci(p, line_index, "menu") ||
        line_contains_ci(p, line_index, "navigation") ||
        line_contains_ci(p, line_index, "subscribe") ||
        line_contains_ci(p, line_index, "newsletter"))
        return 0;
    last = p->text[start + len - 1];
    if (last == '.' || last == ',' || last == ';' || last == ':')
        return 0;

    prev_blank = line_index == 0 ||
                 !display_line_trim(p, line_index - 1, &start, &len);
    next_blank = (size_t)(line_index + 1) >= p->display_count ||
                 !display_line_trim(p, line_index + 1, &start, &len);
    return prev_blank || next_blank;
}

static void jump_past_top_navigation(App *a)
{
    int h, w;
    int body_h;
    int body_w;

    getmaxyx(stdscr, h, w);
    body_h = h - 3;
    if (body_h < 1) body_h = 1;
    body_w = browse_read_width(w);
    page_layout(&a->page, body_w);
    a->selected_link = -1;
    a->selected_control = -1;
    a->top = first_content_line(&a->page);
    clamp_top(a, body_h, body_w);
    set_status(a, "Past top navigation");
}

static void jump_to_article_heading(App *a)
{
    int h, w;
    int body_h;
    int body_w;
    int start;
    size_t i;

    getmaxyx(stdscr, h, w);
    body_h = h - 3;
    if (body_h < 1) body_h = 1;
    body_w = browse_read_width(w);
    page_layout(&a->page, body_w);
    a->selected_link = -1;
    a->selected_control = -1;

    start = first_content_line(&a->page);
    for (i = (size_t)start; i < a->page.display_count; i++) {
        if (line_looks_heading(&a->page, (int)i)) {
            a->top = (int)i;
            clamp_top(a, body_h, body_w);
            set_status(a, "Article heading");
            return;
        }
    }

    a->top = start;
    clamp_top(a, body_h, body_w);
    set_status(a, "Likely content");
}

static int browse_read_width(int screen_w)
{
    int width = screen_w - 4;

    if (width > 80)
        width = 80;
    if (width < 20)
        width = screen_w > 1 ? screen_w - 1 : 1;
    if (width < 1)
        width = 1;
    return width;
}

static int browse_read_left(int screen_w, int width)
{
    int left = (screen_w - width) / 2;

    return left > 0 ? left : 0;
}

static int browse_visual_width_n(const char *s, size_t n)
{
    int col = 0;

    for (size_t i = 0; i < n; ) {
        if (s[i] == '\t') {
            col += SIMPLERENDER_TAB_WIDTH - (col % SIMPLERENDER_TAB_WIDTH);
            i++;
        } else {
            wchar_t wc;
            int used = 1;
            int width = ssr_utf8_decode_n(s + i, (int)(n - i), &wc, &used);

            col += width;
            i += (size_t)used;
        }
    }
    return col;
}

static void draw_attr_slice(int y, int x, const char *s, size_t n, attr_t attrs)
{
    int col = 0;
    short pair = PAIR_NUMBER(attrs);
    attr_t text_attrs = attrs & ~A_COLOR;

    if (y < 0 || y >= LINES || x >= COLS || n == 0)
        return;

    for (size_t i = 0; i < n && x + col < COLS; ) {
        if (s[i] == '\t') {
            int spaces = SIMPLERENDER_TAB_WIDTH - (col % SIMPLERENDER_TAB_WIDTH);

            for (int k = 0; k < spaces && x + col + k < COLS; k++)
                mvaddch(y, x + col + k, ' ' | attrs);
            col += spaces;
            i++;
        } else {
            wchar_t wc;
            wchar_t text[2];
            cchar_t cell;
            int used = 1;
            int width = ssr_utf8_decode_n(s + i, (int)(n - i), &wc, &used);

            if (x + col + width > COLS)
                break;
            text[0] = wc;
            text[1] = L'\0';
            setcchar(&cell, text, text_attrs, pair, NULL);
            mvadd_wch(y, x + col, &cell);
            col += width;
            i += (size_t)used;
        }
    }
}

static int url_was_visited(App *a, const char *url)
{
    int i;

    if (!url || !*url) return 0;
    for (i = 0; i < a->visited_count; i++) {
        if (a->visited_urls[i] && !strcmp(a->visited_urls[i], url))
            return 1;
    }
    return 0;
}

static void remember_visited_url(App *a, const char *url)
{
    if (!url || !*url || url_was_visited(a, url)) return;
    if (a->visited_count == MAX_HISTORY) {
        free(a->visited_urls[0]);
        memmove(&a->visited_urls[0], &a->visited_urls[1],
                sizeof(a->visited_urls[0]) * (MAX_HISTORY - 1));
        a->visited_count = MAX_HISTORY - 1;
    }
    a->visited_urls[a->visited_count++] = xstrdup_local(url);
}

static int link_index_was_visited(App *a, int link_index)
{
    if (link_index < 0 || (size_t)link_index >= a->page.link_count)
        return 0;
    return url_was_visited(a, a->page.links[link_index].url);
}

static int selected_stop_range_on_line(App *a, int line_index,
                                       size_t *start_out, size_t *end_out)
{
    int stop_index;
    DisplayLine *line;
    LinkStop *stop;
    size_t line_start;
    size_t line_end;
    size_t start;
    size_t end;

    if (line_index < 0 || (size_t)line_index >= a->page.display_count)
        return 0;
    stop_index = selected_page_stop_index(a);
    if (stop_index < 0) return 0;

    line = &a->page.display[line_index];
    if (!line->len) return 0;
    stop = &a->page.stops[stop_index];
    line_start = line->start;
    line_end = line->start + line->len;
    if (stop->end <= line_start || stop->start >= line_end) return 0;

    start = stop->start > line_start ? stop->start : line_start;
    end = stop->end < line_end ? stop->end : line_end;
    if (end <= start) return 0;
    *start_out = start;
    *end_out = end;
    return 1;
}

static int page_stop_at_offset(App *a, size_t cursor, size_t line_end,
                               LinkStop **stop_out, size_t *next_out)
{
    size_t i;
    size_t next = line_end;
    LinkStop *found = NULL;

    for (i = 0; i < a->page.stop_count; i++) {
        LinkStop *stop = &a->page.stops[i];

        if (stop->end <= cursor) continue;
        if (stop->start > cursor) {
            if (stop->start < next) next = stop->start;
            continue;
        }
        found = stop;
        if (stop->end < next) next = stop->end;
        break;
    }

    if (next_out) *next_out = next;
    if (stop_out) *stop_out = found;
    return found != NULL;
}

static void status_append(char *out, size_t outsz, const char *fmt, ...)
{
    va_list ap;
    size_t len = strlen(out);

    if (len >= outsz) return;
    va_start(ap, fmt);
    vsnprintf(out + len, outsz - len, fmt, ap);
    va_end(ap);
}

static void make_status_line(App *a, int body_h, char *out, size_t outsz)
{
    const char *help = "i search field | Up/Down links/fields | Enter open/edit | Backspace back | q quit";
    size_t total;
    size_t first;
    size_t last;

    if (a->bookmark_mode) {
        snprintf(out, outsz, "%s", a->status[0] ? a->status : "Bookmarks");
        if (a->bookmark_count) {
            status_append(out, outsz, " | bookmark %d/%zu",
                          a->selected_bookmark + 1, a->bookmark_count);
        } else {
            status_append(out, outsz, " | no bookmarks");
        }
        return;
    }

    if (a->mode == MODE_FIELD &&
        a->editing_control >= 0 &&
        a->editing_control < (int)a->page.control_count) {
        FormControl *c = &a->page.controls[a->editing_control];

        snprintf(out, outsz, "Field %d/%zu | %s: \"%s\" | Esc done | Shift-arrows select | C-y paste",
                 a->editing_control + 1, a->page.control_count,
                 c->label && *c->label ? c->label : control_type_name(c),
                 c->type == CONTROL_PASSWORD ? "********" : (c->value ? c->value : ""));
    } else if (a->selected_control >= 0 &&
               a->selected_control < (int)a->page.control_count) {
        FormControl *c = &a->page.controls[a->selected_control];
        int stop_index = page_stop_index_for_control(&a->page, a->selected_control);

        if (a->status[0])
            snprintf(out, outsz, "%s | ", a->status);
        else
            out[0] = 0;
        status_append(out, outsz, "Field %d/%zu | %s: \"%s\" | Enter edit/activate",
                      a->selected_control + 1, a->page.control_count,
                      c->label && *c->label ? c->label : control_type_name(c),
                      c->type == CONTROL_PASSWORD ? "********" : (c->value ? c->value : ""));
        if (stop_index >= 0)
            status_append(out, outsz, " | [%d/%zu]", stop_index + 1,
                          a->page.stop_count);
    } else if (a->selected_link >= 0 && a->selected_link < (int)a->page.link_count) {
        int stop_index = selected_stop_index(&a->page, a->selected_link);
        size_t stop_total = a->page.stop_count ? a->page.stop_count : a->page.link_count;

        if (a->status[0])
            snprintf(out, outsz, "%s | ", a->status);
        else
            out[0] = 0;
        status_append(out, outsz, "[%d/%zu] %s | Enter open",
                      stop_index >= 0 ? stop_index + 1 : a->selected_link + 1,
                      stop_total,
                      a->page.links[a->selected_link].url);
    } else {
        snprintf(out, outsz, "%s", help);
        if (a->status[0])
            status_append(out, outsz, " | %s", a->status);
    }

    total = a->page.display_count;
    if (total) {
        first = (size_t)a->top + 1;
        if (first > total) first = total;
        last = (size_t)a->top + (body_h > 0 ? (size_t)body_h : 1);
        if (last > total) last = total;
        status_append(out, outsz, " | lines %zu-%zu/%zu", first, last, total);
    }
    if (a->has_match && a->find_query[0])
        status_append(out, outsz, " | /%s", a->find_query);
    if (a->js_mode_active)
        status_append(out, outsz, " | WebKit");
}

static void draw_text_line(App *a, int y, int left, DisplayLine *line)
{
    int line_index = (int)(line - a->page.display);
    size_t line_start = line->start;
    size_t line_end = line->start + line->len;
    size_t cursor;
    size_t selected_start = 0;
    size_t selected_end = 0;
    size_t match_start = 0;
    size_t match_end = 0;
    int has_selected = selected_stop_range_on_line(a, line_index,
                                                   &selected_start,
                                                   &selected_end);
    int has_line_match = a->has_match && a->match_len > 0 &&
                         a->match_offset >= line_start &&
                         a->match_offset < line_end;

    if (has_line_match) {
        match_start = a->match_offset > line_start ? a->match_offset : line_start;
        match_end = a->match_offset + a->match_len;
        if (match_end > line_end) match_end = line_end;
    }

    cursor = line_start;
    while (cursor < line_end) {
        size_t next = line_end;
        int attrs = 0;
        LinkStop *stop = NULL;

        if (page_stop_at_offset(a, cursor, line_end, &stop, &next)) {
            if (stop->kind == STOP_LINK) {
                int link_index = stop->first_link;

                if (sb_has_color) {
                    attrs |= COLOR_PAIR(link_index_was_visited(a, link_index) ?
                                        SB_PAIR_VISITED_LINK : SB_PAIR_LINK);
                }
            }
        }
        if (has_selected) {
            if (cursor < selected_start && selected_start < next)
                next = selected_start;
            else if (cursor >= selected_start && cursor < selected_end) {
                if (sb_has_color) {
                    attrs &= ~A_COLOR;
                    attrs |= COLOR_PAIR(a->selected_control >= 0 ?
                                        SB_PAIR_SELECTED_CONTROL :
                                        SB_PAIR_SELECTED_LINK);
                } else {
                    attrs |= A_REVERSE;
                }
                if (selected_end < next) next = selected_end;
            }
        }
        if (has_line_match) {
            if (cursor < match_start && match_start < next)
                next = match_start;
            else if (cursor >= match_start && cursor < match_end) {
                if (sb_has_color) {
                    attrs &= ~A_COLOR;
                    attrs |= COLOR_PAIR(SB_PAIR_MATCH);
                } else {
                    attrs |= A_UNDERLINE;
                }
                if (match_end < next) next = match_end;
            }
        }
        if (next <= cursor) next = cursor + 1;
        draw_attr_slice(y,
                        left + browse_visual_width_n(a->page.text + line_start,
                                                     cursor - line_start),
                        a->page.text + cursor,
                        next - cursor, attrs);
        cursor = next;
    }
}

static void draw_bookmarks(App *a, int body_top, int body_h, int w)
{
    int row;

    if (!a->bookmark_count) {
        mvaddnstr(body_top, 0, "No bookmarks.", w - 1);
        return;
    }

    if (a->selected_bookmark < 0) a->selected_bookmark = 0;
    if (a->selected_bookmark >= (int)a->bookmark_count)
        a->selected_bookmark = (int)a->bookmark_count - 1;
    if (a->selected_bookmark < a->bookmark_top)
        a->bookmark_top = a->selected_bookmark;
    if (a->selected_bookmark >= a->bookmark_top + body_h)
        a->bookmark_top = a->selected_bookmark - body_h + 1;
    if (a->bookmark_top < 0) a->bookmark_top = 0;

    for (row = 0; row < body_h; row++) {
        int index = a->bookmark_top + row;
        char line[URL_MAX + 512];

        if (index >= (int)a->bookmark_count) break;
        snprintf(line, sizeof(line), "%3d  %s  %s", index + 1,
                 a->bookmarks[index].title, a->bookmarks[index].url);
        if (index == a->selected_bookmark) attron(A_REVERSE);
        mvaddnstr(body_top + row, 0, line, w - 1);
        if (index == a->selected_bookmark) attroff(A_REVERSE);
    }
}

static void draw_top_bar(App *a, int w)
{
    const char *app_name = "SimpleBrowse";
    const char *title = a->page.title && *a->page.title ? a->page.title : "";
    int app_len = (int)strlen(app_name);
    int max_title = w - app_len - 5;

    attron(A_REVERSE);
    mvhline(0, 0, ' ', w);
    mvaddnstr(0, 1, app_name, w - 2);
    if (*title && max_title >= 12) {
        char title_buf[512];
        size_t title_len = strlen(title);
        const char *display = title;
        int display_len;

        if ((int)title_len > max_title) {
            size_t keep = (size_t)(max_title - 3);

            if (keep >= sizeof(title_buf)) keep = sizeof(title_buf) - 4;
            memcpy(title_buf, title, keep);
            memcpy(title_buf + keep, "...", 4);
            display = title_buf;
        }
        display_len = (int)strlen(display);
        mvaddnstr(0, w - display_len - 1, display, display_len);
    }
    attroff(A_REVERSE);
}

static void draw_screen(App *a)
{
    int h, w;
    int body_top = 2;
    int status_row;
    int body_h;
    int body_w;
    int body_left;
    size_t i;
    char prompt[16];
    char status_line[2048];

    getmaxyx(stdscr, h, w);
    if (h < 4 || w < 20) {
        erase();
        mvaddstr(0, 0, "SimpleBrowse");
        mvaddstr(1, 0, "terminal too small");
        refresh();
        return;
    }

    status_row = h - 1;
    body_h = status_row - body_top;
    body_w = browse_read_width(w);
    body_left = browse_read_left(w, body_w);
    if (!a->bookmark_mode) {
        page_layout(&a->page, body_w);
        if (a->selected_link >= 0 || a->selected_control >= 0)
            ensure_selected_visible(a, body_h, body_w);
    }

    erase();

    draw_top_bar(a, w);

    if (a->mode == MODE_FIELD)
        snprintf(prompt, sizeof(prompt), "Field: ");
    else
        snprintf(prompt, sizeof(prompt), "%s", a->find_focus ? "Find: " : "URL: ");
    mvhline(1, 0, ' ', w);
    mvaddnstr(1, 0, prompt, w);
    if (a->mode == MODE_FIELD &&
        a->editing_control >= 0 &&
        a->editing_control < (int)a->page.control_count &&
        w > (int)strlen(prompt)) {
        FormControl *c = &a->page.controls[a->editing_control];
        const char *value = c->value ? c->value : "";
        int prompt_len = (int)strlen(prompt);
        int max_cols = w - prompt_len - 1;
        int sel_start = 0;
        int sel_end = 0;
        int has_sel = field_selection_bounds(a, &sel_start, &sel_end);
        int i;
        int col = 0;

        if (max_cols < 0) max_cols = 0;
        for (i = 0; value[i] && col < max_cols; i++, col++) {
            unsigned char raw = (unsigned char)value[i];
            chtype out = c->type == CONTROL_PASSWORD ? '*' :
                         (raw == '\t' || raw == '\r' || raw == '\n' ? ' ' :
                          (isprint(raw) ? raw : '?'));

            if (has_sel && i >= sel_start && i < sel_end)
                attron(A_REVERSE);
            mvaddch(1, prompt_len + col, out);
            if (has_sel && i >= sel_start && i < sel_end)
                attroff(A_REVERSE);
        }
    } else if (a->find_focus && w > (int)strlen(prompt)) {
        mvaddnstr(1, (int)strlen(prompt), a->find_query,
                  w - (int)strlen(prompt) - 1);
    } else if (w > (int)strlen(prompt)) {
        mvaddnstr(1, (int)strlen(prompt), a->url_bar, w - (int)strlen(prompt) - 1);
    }

    if (a->bookmark_mode) {
        draw_bookmarks(a, body_top, body_h, w);
    } else {
        for (i = 0; i < (size_t)body_h; i++) {
            size_t idx = (size_t)a->top + i;
            DisplayLine *line;

            if (idx >= a->page.display_count) break;
            line = &a->page.display[idx];
            draw_text_line(a, body_top + (int)i, body_left, line);
        }
    }

    make_status_line(a, body_h, status_line, sizeof(status_line));
    attron(A_REVERSE);
    mvhline(status_row, 0, ' ', w);
    mvaddnstr(status_row, 1, status_line, w - 2);
    attroff(A_REVERSE);

    if (a->mode == MODE_FIELD &&
        a->editing_control >= 0 &&
        a->editing_control < (int)a->page.control_count) {
        int cursor_x = (int)strlen(prompt) + a->field_cursor;
        if (cursor_x >= w) cursor_x = w - 1;
        move(1, cursor_x);
        curs_set(1);
    } else if (a->url_focus) {
        int cursor_x = (int)strlen(prompt) + a->url_pos;
        if (cursor_x >= w) cursor_x = w - 1;
        move(1, cursor_x);
        curs_set(1);
    } else if (a->find_focus) {
        int cursor_x = (int)strlen(prompt) + a->find_pos;
        if (cursor_x >= w) cursor_x = w - 1;
        move(1, cursor_x);
        curs_set(1);
    } else {
        curs_set(0);
    }

    refresh();
}

static void init_browser_colors(void)
{
    short link_yellow = COLORS >= 256 ? 226 : COLOR_YELLOW;
    short visited = COLORS >= 256 ? 130 : COLOR_MAGENTA;
    short selected_fg = COLOR_BLACK;
    short selected_bg = COLORS >= 256 ? 15 : COLOR_WHITE;

    if (!has_colors()) return;
    if (start_color() == ERR) return;
    use_default_colors();
    if (init_pair(SB_PAIR_LINK, link_yellow, -1) == ERR) return;
    init_pair(SB_PAIR_VISITED_LINK, visited, -1);
    init_pair(SB_PAIR_SELECTED_LINK, selected_fg, selected_bg);
    init_pair(SB_PAIR_SELECTED_CONTROL, COLOR_GREEN, -1);
    init_pair(SB_PAIR_MATCH, COLOR_YELLOW, -1);
    sb_has_color = 1;
}

static void make_error_page(App *a, const char *url, const FetchResult *result)
{
    Page p = {0};
    Buffer b = {0};
    char status_text[96];

    buf_addn(&b, "Could not load page.\n\n", 22);
    buf_addn(&b, "Final URL: ", 11);
    buf_addn(&b, url, strlen(url));
    buf_addn(&b, "\n\n", 2);
    if (result && result->code > 0) {
        format_http_status(result->code, status_text, sizeof(status_text));
        buf_addn(&b, "HTTP status: ", 13);
        buf_addn(&b, status_text, strlen(status_text));
        buf_addn(&b, "\n", 1);
    }
    buf_addn(&b, result && result->network_error ? "Network error: " : "Error: ",
             result && result->network_error ? 15 : 7);
    buf_addn(&b, result && result->error[0] ? result->error : "request failed",
             strlen(result && result->error[0] ? result->error : "request failed"));

    page_free(&a->page);
    p.text = b.data;
    p.url = xstrdup_local(url);
    p.title = xstrdup_local("Could not load page");
    p.meta = xstrdup_local("");
    p.layout_width = -1;
    a->page = p;
}

static void finish_navigation(App *a, int mode, const char *old_url,
                              const char *new_url, int success)
{
    int same = old_url && *old_url && new_url && !strcmp(old_url, new_url);
    char *discard;

    if (success && old_url && *old_url)
        page_cache_store_current(a, old_url);

    if (mode == NAV_NORMAL) {
        if (old_url && *old_url && !same) {
            stack_push(a->back_stack, &a->back_count, old_url);
            stack_clear(a->forward_stack, &a->forward_count);
        }
    } else if (success && mode == NAV_BACK) {
        discard = stack_pop(a->back_stack, &a->back_count);
        free(discard);
        if (old_url && *old_url && !same)
            stack_push(a->forward_stack, &a->forward_count, old_url);
    } else if (success && mode == NAV_FORWARD) {
        discard = stack_pop(a->forward_stack, &a->forward_count);
        free(discard);
        if (old_url && *old_url && !same)
            stack_push(a->back_stack, &a->back_count, old_url);
    }
}

static int load_url_mode(App *a, const char *input, int nav_mode, int force_js)
{
    char *url = normalize_input_url(input);
    char *old_url = xstrdup_local(a->current_url);
    char *fallback = NULL;
    char *html = NULL;
    char original_url[URL_MAX];
    char final_url[URL_MAX];
    char status_text[96];
    char js_warning[256] = "";
    FetchResult result;
    FetchResult retry;
    FetchResult js_result;
    CacheEntry cache;
    const char *cache_mode = force_js ? "js" : "auto";
    Page p;
    int used_js = 0;
    int parsed = 0;
    int cached = 0;
    int explicit_https_input = starts_https_url_trimmed(input);

    memset(&result, 0, sizeof(result));
    memset(&retry, 0, sizeof(retry));
    memset(&js_result, 0, sizeof(js_result));
    memset(&cache, 0, sizeof(cache));

    if (!*url) {
        free(url);
        free(old_url);
        set_status(a, "Enter a URL");
        return 0;
    }
    snprintf(original_url, sizeof(original_url), "%s", url);

    if ((nav_mode == NAV_BACK || nav_mode == NAV_FORWARD) &&
        page_cache_restore(a, url, nav_mode)) {
        free(url);
        free(fallback);
        free(old_url);
        return 1;
    }

    clear_number_entry(a);
    a->mode = MODE_PAGE;
    a->url_focus = 0;
    a->find_focus = 0;
    a->bookmark_mode = 0;
    snprintf(a->url_bar, sizeof(a->url_bar), "%s", url);
    a->url_len = (int)strlen(a->url_bar);
    a->url_pos = a->url_len;
    snprintf(a->status, sizeof(a->status), "Loading%s %s",
             force_js ? " with WebKit" : "", url);
    draw_screen(a);

    if (cache_read_entry(cache_mode, url, &cache)) {
        html = cache.html;
        cache.html = NULL;
        snprintf(final_url, sizeof(final_url), "%s",
                 cache.effective[0] ? cache.effective : url);
        result.code = cache.code;
        snprintf(result.effective, sizeof(result.effective), "%s", final_url);
        snprintf(result.reason, sizeof(result.reason), "%s",
                 http_reason_phrase(result.code));
        used_js = cache.used_js;
        cached = 1;
    }
    cache_entry_clear(&cache);

    if (!cached && !html && (force_js || webkit_first_url(url))) {
        if (!force_js) {
            snprintf(a->status, sizeof(a->status),
                     "Using WebKit first for browser-sensitive site %s", url);
            draw_screen(a);
        }
        html = fetch_url_js(url, &result);
        used_js = html != NULL;
    } else if (!cached && !html) {
        html = fetch_url(url, &result);
        if (!html && result.network_error) {
            fallback = explicit_https_input ? NULL : http_fallback_url(url);
            if (fallback) {
                snprintf(a->status, sizeof(a->status), "HTTPS failed; trying %s", fallback);
                draw_screen(a);

                html = fetch_url(fallback, &retry);
                if (html) {
                    free(url);
                    url = fallback;
                    fallback = NULL;
                    result = retry;
                } else if (failed_http_fallback_returned_to_https(&retry)) {
                    char original_error[sizeof(result.error)];

                    snprintf(original_error, sizeof(original_error), "%s",
                             result.error[0] ? result.error : "request failed");
                    snprintf(result.error, sizeof(result.error),
                             "%.240s; HTTP fallback redirected back to HTTPS and failed: %.180s",
                             original_error,
                             retry.error[0] ? retry.error : "request failed");
                } else {
                    char combined[512];

                    snprintf(combined, sizeof(combined),
                             "HTTPS failed: %.180s; HTTP failed: %.180s",
                             result.error, retry.error);
                    snprintf(result.error, sizeof(result.error), "%s", combined);
                    if (retry.effective[0])
                        snprintf(result.effective, sizeof(result.effective), "%s", retry.effective);
                    result.code = retry.code;
                    snprintf(result.reason, sizeof(result.reason), "%s", retry.reason);
                    result.network_error = retry.network_error;
                }
            } else if (explicit_https_input) {
                snprintf(a->status, sizeof(a->status),
                         "HTTPS failed; not retrying with plain HTTP");
            }
        }
    }

    if (!cached)
        snprintf(final_url, sizeof(final_url), "%s", result.effective[0] ? result.effective : url);

    if (!cached && !force_js && !html && result.network_error &&
        curl_error_should_retry_js(result.curl_code)) {
        const char *retry_url = webkit_retry_target(original_url, final_url);

        snprintf(a->status, sizeof(a->status),
                 "Static network load failed; retrying with WebKit");
        draw_screen(a);

        memset(&js_result, 0, sizeof(js_result));
        html = fetch_url_js(retry_url, &js_result);
        if (html) {
            result = js_result;
            snprintf(final_url, sizeof(final_url), "%s",
                     result.effective[0] ? result.effective : retry_url);
            used_js = 1;
        } else {
            char combined[512];

            snprintf(combined, sizeof(combined),
                     "static failed: %.180s; WebKit failed: %.180s",
                     result.error[0] ? result.error : "request failed",
                     js_result.error[0] ? js_result.error : "backend failed");
            snprintf(result.error, sizeof(result.error), "%s", combined);
        }
    }

    if (!cached && !force_js && !used_js && http_status_should_retry_js(result.code)) {
        const char *retry_url = webkit_retry_target(original_url, final_url);
        char *old_html = html;

        snprintf(a->status, sizeof(a->status),
                 "Static load returned %ld; retrying with WebKit", result.code);
        draw_screen(a);

        memset(&js_result, 0, sizeof(js_result));
        html = fetch_url_js(retry_url, &js_result);
        if (html) {
            free(old_html);
            result = js_result;
            snprintf(final_url, sizeof(final_url), "%s",
                     result.effective[0] ? result.effective : retry_url);
            used_js = 1;
        } else {
            html = old_html;
            char combined[512];

            snprintf(combined, sizeof(combined),
                     "static failed: %.180s; WebKit failed: %.180s",
                     result.error[0] ? result.error : "request failed",
                     js_result.error[0] ? js_result.error : "backend failed");
            snprintf(result.error, sizeof(result.error), "%s", combined);
        }
    }

    if (!cached && html && used_js &&
        webkit_snapshot_rejected(final_url, html, strlen(html), &result)) {
        free(html);
        html = NULL;
        result.network_error = 0;
        if (!result.code)
            result.code = 429;
        snprintf(result.reason, sizeof(result.reason), "%s",
                 http_reason_phrase(result.code));
        snprintf(result.error, sizeof(result.error),
                 "site rejected the WebKit browser session");
    }

    if (!html) {
        finish_navigation(a, nav_mode, old_url, final_url, 1);
        make_error_page(a, final_url, &result);
        snprintf(a->current_url, sizeof(a->current_url), "%s", final_url);
        remember_visited_url(a, final_url);
        snprintf(a->url_bar, sizeof(a->url_bar), "%s", final_url);
        a->url_len = (int)strlen(a->url_bar);
        a->url_pos = a->url_len;
        snprintf(a->status, sizeof(a->status), "Load failed: %s | %s",
                 result.error[0] ? result.error : "request failed", final_url);
        free(url);
        free(fallback);
        free(old_url);
        a->top = 0;
        a->selected_link = -1;
        a->selected_control = -1;
        a->editing_control = -1;
        a->has_match = 0;
        a->js_mode_active = 0;
        return 0;
    }

    memset(&p, 0, sizeof(p));
    p = parse_html(html, strlen(html), final_url);
    parsed = 1;

    if (!cached && !force_js && static_page_should_retry_js(final_url, html, strlen(html), &p)) {
        snprintf(a->status, sizeof(a->status), "Static page needs browser behavior; retrying with WebKit");
        draw_screen(a);

        if (result.effective[0])
            snprintf(final_url, sizeof(final_url), "%s", result.effective);
        page_free(&p);
        parsed = 0;

        memset(&js_result, 0, sizeof(js_result));
        {
            const char *retry_url = webkit_retry_target(original_url, final_url);
            char *js_html = fetch_url_js(retry_url, &js_result);

            if (js_html) {
                if (webkit_snapshot_rejected(js_result.effective[0] ?
                                             js_result.effective : retry_url,
                                             js_html, strlen(js_html),
                                             &js_result)) {
                    free(js_html);
                    snprintf(js_warning, sizeof(js_warning),
                             " | WebKit rejected: %.200s",
                             js_result.error[0] ? js_result.error :
                             "site rejected browser session");
                    p = parse_html(html, strlen(html), final_url);
                    parsed = 1;
                } else {
                    free(html);
                    html = js_html;
                    result = js_result;
                    snprintf(final_url, sizeof(final_url), "%s",
                             result.effective[0] ? result.effective : retry_url);
                    used_js = 1;
                    p = parse_html(html, strlen(html), final_url);
                    parsed = 1;
                }
            } else {
                snprintf(js_warning, sizeof(js_warning), " | WebKit unavailable: %.200s",
                         js_result.error[0] ? js_result.error : "backend failed");
                p = parse_html(html, strlen(html), final_url);
                parsed = 1;
            }
        }
    }

    normalize_reddit_listing_page(&p);

    if (!cached) {
        cache_write_entry(cache_mode, url, final_url, result.code, used_js, html);
        if (strcmp(url, final_url) != 0)
            cache_write_entry(cache_mode, final_url, final_url, result.code, used_js, html);
        if (used_js && strcmp(cache_mode, "auto") != 0)
            cache_write_entry("auto", final_url, final_url, result.code, used_js, html);
    }

    finish_navigation(a, nav_mode, old_url, final_url, 1);

    if (!parsed)
        p = parse_html(html, strlen(html), final_url);
    free(html);
    page_free(&a->page);
    a->page = p;
    snprintf(a->current_url, sizeof(a->current_url), "%s", final_url);
    remember_visited_url(a, final_url);
    snprintf(a->url_bar, sizeof(a->url_bar), "%s", final_url);
    a->url_len = (int)strlen(a->url_bar);
    a->url_pos = a->url_len;
    a->top = 0;
    a->selected_link = -1;
    a->selected_control = -1;
    a->editing_control = -1;
    a->has_match = 0;
    a->js_mode_active = used_js;
    format_http_status(result.code, status_text, sizeof(status_text));
    snprintf(a->status, sizeof(a->status), "%s%s | %zu links | %zu fields | %s%s",
             cached ? "Cache " : (used_js ? "WebKit " : ""), status_text, a->page.link_count,
             a->page.control_count,
             final_url, js_warning);
    free(url);
    free(fallback);
    free(old_url);
    return 1;
}

static int load_url(App *a, const char *input, int nav_mode)
{
    return load_url_mode(a, input, nav_mode, 0);
}

static int load_url_js(App *a, const char *input, int nav_mode)
{
    return load_url_mode(a, input, nav_mode, 1);
}

static void form_urlencode_append(Buffer *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";

    while (s && *s) {
        unsigned char c = (unsigned char)*s++;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            buf_addc(b, (char)c);
        } else if (c == ' ') {
            buf_addc(b, '+');
        } else {
            char enc[3];

            enc[0] = '%';
            enc[1] = hex[c >> 4];
            enc[2] = hex[c & 15];
            buf_addn(b, enc, sizeof(enc));
        }
    }
}

static void json_string_append(Buffer *b, const char *s)
{
    buf_addc(b, '"');
    while (s && *s) {
        unsigned char c = (unsigned char)*s++;

        switch (c) {
        case '\\': buf_addn(b, "\\\\", 2); break;
        case '"': buf_addn(b, "\\\"", 2); break;
        case '\b': buf_addn(b, "\\b", 2); break;
        case '\f': buf_addn(b, "\\f", 2); break;
        case '\n': buf_addn(b, "\\n", 2); break;
        case '\r': buf_addn(b, "\\r", 2); break;
        case '\t': buf_addn(b, "\\t", 2); break;
        default:
            if (c < 32) {
                char esc[7];

                snprintf(esc, sizeof(esc), "\\u%04x", c);
                buf_addn(b, esc, strlen(esc));
            } else {
                buf_addc(b, (char)c);
            }
            break;
        }
    }
    buf_addc(b, '"');
}

static const char *control_submit_value(FormControl *c)
{
    if (!c) return "";
    if (c->type == CONTROL_SELECT && c->option_count) {
        size_t idx = c->selected >= 0 && (size_t)c->selected < c->option_count ?
                     (size_t)c->selected : 0;
        return c->option_values[idx] ? c->option_values[idx] : "";
    }
    return c->value ? c->value : "";
}

static int control_is_successful(FormControl *c, int index, int submit_index,
                                 int form_index)
{
    if (!c || c->disabled || !c->name || !*c->name) return 0;
    if (form_index >= 0 && c->form_index != form_index) return 0;
    if (form_index < 0 && index != submit_index && c->form_index != form_index) return 0;

    if (c->type == CONTROL_CHECKBOX || c->type == CONTROL_RADIO)
        return c->checked;
    if (c->type == CONTROL_SUBMIT || c->type == CONTROL_BUTTON)
        return index == submit_index && c->button_submits;
    return 1;
}

static char *build_urlencoded_form(Page *p, int form_index, int submit_index)
{
    Buffer b = {0};
    size_t i;
    int first = 1;

    for (i = 0; i < p->control_count; i++) {
        FormControl *c = &p->controls[i];

        if (!control_is_successful(c, (int)i, submit_index, form_index))
            continue;
        if (!first) buf_addc(&b, '&');
        first = 0;
        form_urlencode_append(&b, c->name);
        buf_addc(&b, '=');
        form_urlencode_append(&b, control_submit_value(c));
    }
    return b.data ? b.data : xstrdup_local("");
}

static char *build_form_json(Page *p, int form_index, int submit_index)
{
    Buffer b = {0};
    size_t i;
    int first = 1;
    FormControl *submit = submit_index >= 0 &&
                          submit_index < (int)p->control_count ?
                          &p->controls[submit_index] : NULL;

    buf_addn(&b, "{\"form_index\":", strlen("{\"form_index\":"));
    {
        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%d", form_index >= 0 ? form_index : 0);
        buf_addn(&b, nbuf, strlen(nbuf));
    }
    if (submit && submit->name && *submit->name) {
        buf_addn(&b, ",\"submit_name\":", strlen(",\"submit_name\":"));
        json_string_append(&b, submit->name);
        buf_addn(&b, ",\"submit_value\":", strlen(",\"submit_value\":"));
        json_string_append(&b, submit->value ? submit->value : "");
    }
    buf_addn(&b, ",\"controls\":[", strlen(",\"controls\":["));
    for (i = 0; i < p->control_count; i++) {
        FormControl *c = &p->controls[i];
        char nbuf[64];

        if (form_index >= 0 && c->form_index != form_index)
            continue;
        if (!c->name || !*c->name)
            continue;
        if (!first) buf_addc(&b, ',');
        first = 0;
        buf_addc(&b, '{');
        buf_addn(&b, "\"name\":", strlen("\"name\":"));
        json_string_append(&b, c->name);
        buf_addn(&b, ",\"type\":", strlen(",\"type\":"));
        json_string_append(&b, control_type_name(c));
        buf_addn(&b, ",\"value\":", strlen(",\"value\":"));
        json_string_append(&b, control_submit_value(c));
        snprintf(nbuf, sizeof(nbuf), ",\"checked\":%s", c->checked ? "true" : "false");
        buf_addn(&b, nbuf, strlen(nbuf));
        snprintf(nbuf, sizeof(nbuf), ",\"selected\":%d", c->selected);
        buf_addn(&b, nbuf, strlen(nbuf));
        snprintf(nbuf, sizeof(nbuf), ",\"active\":%s",
                 (int)i == submit_index ? "true" : "false");
        buf_addn(&b, nbuf, strlen(nbuf));
        buf_addc(&b, '}');
    }
    buf_addn(&b, "]}", strlen("]}"));
    return b.data ? b.data : xstrdup_local("{}");
}

static char *build_multipart_form(Page *p, int form_index, int submit_index,
                                  const char *boundary)
{
    Buffer b = {0};
    size_t i;

    for (i = 0; i < p->control_count; i++) {
        FormControl *c = &p->controls[i];

        if (!control_is_successful(c, (int)i, submit_index, form_index))
            continue;
        buf_addn(&b, "--", 2);
        buf_addn(&b, boundary, strlen(boundary));
        buf_addn(&b, "\r\nContent-Disposition: form-data; name=\"",
                 strlen("\r\nContent-Disposition: form-data; name=\""));
        buf_addn(&b, c->name, strlen(c->name));
        buf_addn(&b, "\"\r\n\r\n", 5);
        buf_addn(&b, control_submit_value(c), strlen(control_submit_value(c)));
        buf_addn(&b, "\r\n", 2);
    }
    buf_addn(&b, "--", 2);
    buf_addn(&b, boundary, strlen(boundary));
    buf_addn(&b, "--\r\n", 4);
    return b.data ? b.data : xstrdup_local("");
}

static char *form_url_with_query(const char *action, const char *query)
{
    Buffer b = {0};
    size_t len = strlen(action ? action : "");

    buf_addn(&b, action ? action : "", len);
    if (query && *query) {
        if (!strchr(action ? action : "", '?'))
            buf_addc(&b, '?');
        else if (len && action[len - 1] != '?' && action[len - 1] != '&')
            buf_addc(&b, '&');
        buf_addn(&b, query, strlen(query));
    }
    return b.data ? b.data : xstrdup_local(action ? action : "");
}

static int load_submitted_html(App *a, const char *fallback_url,
                               FetchResult *result, char *html)
{
    char final_url[URL_MAX];
    char status_text[96];
    char *old_url = xstrdup_local(a->current_url);
    Page p;

    snprintf(final_url, sizeof(final_url), "%s",
             result->effective[0] ? result->effective : fallback_url);
    if (html && webkit_snapshot_rejected(final_url, html, strlen(html), result)) {
        free(html);
        html = NULL;
        result->network_error = 0;
        if (!result->code)
            result->code = 429;
        snprintf(result->reason, sizeof(result->reason), "%s",
                 http_reason_phrase(result->code));
        snprintf(result->error, sizeof(result->error),
                 "site rejected the WebKit browser session");
    }
    if (!html) {
        finish_navigation(a, NAV_NORMAL, old_url, final_url, 1);
        make_error_page(a, final_url, result);
        snprintf(a->current_url, sizeof(a->current_url), "%s", final_url);
        remember_visited_url(a, final_url);
        snprintf(a->url_bar, sizeof(a->url_bar), "%s", final_url);
        a->url_len = (int)strlen(a->url_bar);
        a->url_pos = a->url_len;
        a->top = 0;
        a->selected_link = -1;
        a->selected_control = -1;
        a->editing_control = -1;
        snprintf(a->status, sizeof(a->status), "Submit failed: %s | %s",
                 result->error[0] ? result->error : "request failed", final_url);
        free(old_url);
        return 0;
    }

    p = parse_html(html, strlen(html), final_url);
    normalize_reddit_listing_page(&p);
    finish_navigation(a, NAV_NORMAL, old_url, final_url, 1);
    page_free(&a->page);
    a->page = p;
    snprintf(a->current_url, sizeof(a->current_url), "%s", final_url);
    remember_visited_url(a, final_url);
    snprintf(a->url_bar, sizeof(a->url_bar), "%s", final_url);
    a->url_len = (int)strlen(a->url_bar);
    a->url_pos = a->url_len;
    a->top = 0;
    a->selected_link = -1;
    a->selected_control = -1;
    a->editing_control = -1;
    a->mode = MODE_PAGE;
    a->url_focus = 0;
    a->find_focus = 0;
    a->has_match = 0;
    format_http_status(result->code, status_text, sizeof(status_text));
    snprintf(a->status, sizeof(a->status), "Submitted | %s | %zu links | %zu fields | %s",
             status_text, a->page.link_count, a->page.control_count, final_url);
    free(html);
    free(old_url);
    return 1;
}

static int submit_control(App *a, int control_index)
{
    FormControl *c;
    int form_index;
    char *body;
    char *target;
    char *html = NULL;
    FetchResult result;

    if (control_index < 0 || control_index >= (int)a->page.control_count) {
        set_status(a, "No selected field");
        return 0;
    }
    c = &a->page.controls[control_index];
    if (c->type == CONTROL_BUTTON && !c->button_submits) {
        set_status(a, "Button has no static submit action");
        return 0;
    }

    form_index = c->form_index;
    body = build_urlencoded_form(&a->page, form_index, control_index);
    target = c->form_action && *c->form_action ?
             xstrdup_local(c->form_action) : xstrdup_local(a->current_url);

    if (a->js_mode_active) {
        char *payload = build_form_json(&a->page, form_index, control_index);
        FetchResult js_result;
        char *js_html;

        snprintf(a->status, sizeof(a->status), "Submitting with WebKit %s",
                 a->current_url);
        draw_screen(a);
        js_html = fetch_url_js_submit(a->current_url, payload, &js_result);
        free(payload);
        if (js_html) {
            free(body);
            free(target);
            return load_submitted_html(a, js_result.effective[0] ?
                                       js_result.effective : a->current_url,
                                       &js_result, js_html);
        }
        snprintf(a->status, sizeof(a->status),
                 "WebKit submit failed: %.180s; using HTTP submit",
                 js_result.error[0] ? js_result.error : "backend failed");
        draw_screen(a);
    }

    if (!strcasecmp(c->form_method ? c->form_method : "get", "post")) {
        const char *enctype = c->form_enctype && *c->form_enctype ?
                              c->form_enctype :
                              "application/x-www-form-urlencoded";
        char content_type[256];

        if (!strcasecmp(enctype, "multipart/form-data")) {
            const char *boundary = "----SimpleBrowseFormBoundary4";
            char *multipart = build_multipart_form(&a->page, form_index,
                                                   control_index, boundary);

            snprintf(content_type, sizeof(content_type),
                     "multipart/form-data; boundary=%s", boundary);
            free(body);
            body = multipart;
        } else {
            snprintf(content_type, sizeof(content_type),
                     "application/x-www-form-urlencoded");
        }
        snprintf(a->status, sizeof(a->status), "Submitting POST %s", target);
        draw_screen(a);
        html = fetch_url_post(target, body, content_type, &result);
        free(body);
        {
            int ok = load_submitted_html(a, target, &result, html);
            free(target);
            return ok;
        }
    }

    {
        char *url = form_url_with_query(target, body);

        free(body);
        free(target);
        {
            int ok = load_url(a, url, NAV_NORMAL);
            free(url);
            return ok;
        }
    }
}

static void go_back(App *a)
{
    const char *url;

    url = stack_peek(a->back_stack, a->back_count);
    if (!url) {
        set_status(a, "No back history");
        return;
    }

    load_url(a, url, NAV_BACK);
}

static void go_forward(App *a)
{
    const char *url;

    url = stack_peek(a->forward_stack, a->forward_count);
    if (!url) {
        set_status(a, "No forward history");
        return;
    }

    load_url(a, url, NAV_FORWARD);
}

static void next_link(App *a, int dir)
{
    jump_visible_link(a, dir);
}

static void activate_selected_control(App *a);

static void open_selected_link(App *a)
{
    if (a->selected_link < 0 || a->selected_link >= (int)a->page.link_count) {
        set_status(a, "No selected link");
        return;
    }
    load_url(a, a->page.links[a->selected_link].url, NAV_NORMAL);
}

static void open_numbered_link(App *a)
{
    long n;
    char *end = NULL;
    int h;
    int w;

    if (!a->number_len) {
        open_selected_link(a);
        return;
    }

    n = strtol(a->number_buf, &end, 10);
    getmaxyx(stdscr, h, w);
    (void)h;
    page_layout(&a->page, browse_read_width(w));
    if (!end || *end || n < 1 || n > (long)a->page.stop_count) {
        snprintf(a->status, sizeof(a->status), "No link %s", a->number_buf);
        clear_number_entry(a);
        return;
    }

    a->selected_link = a->page.stops[n - 1].first_link;
    a->selected_control = -1;
    if (a->page.stops[n - 1].kind == STOP_CONTROL) {
        a->selected_link = -1;
        a->selected_control = a->page.stops[n - 1].control_index;
    }
    clear_number_entry(a);
    if (a->selected_control >= 0)
        activate_selected_control(a);
    else
        open_selected_link(a);
}

static void remember_field_undo(App *a, FormControl *c)
{
    if (a->field_undo_value) return;
    a->field_undo_value = xstrdup_local(c->value ? c->value : "");
    a->field_undo_cursor = a->field_cursor;
    free(a->field_redo_value);
    a->field_redo_value = NULL;
}

static void clear_field_undo(App *a)
{
    free(a->field_undo_value);
    free(a->field_redo_value);
    a->field_undo_value = NULL;
    a->field_redo_value = NULL;
    a->field_undo_cursor = 0;
    a->field_redo_cursor = 0;
}

static void begin_field_edit(App *a, int control_index)
{
    FormControl *c;

    if (control_index < 0 || control_index >= (int)a->page.control_count)
        return;
    c = &a->page.controls[control_index];
    if (!control_is_textual(c)) {
        set_status(a, "Field is not editable text");
        return;
    }
    a->mode = MODE_FIELD;
    a->editing_control = control_index;
    a->selected_control = control_index;
    a->selected_link = -1;
    a->url_focus = 0;
    a->find_focus = 0;
    a->field_cursor = (int)strlen(c->value ? c->value : "");
    a->field_select_anchor = a->field_cursor;
    a->field_select_active = 0;
    clear_field_undo(a);
    set_status(a, "Editing field");
}

static void finish_field_edit(App *a)
{
    a->mode = MODE_PAGE;
    a->editing_control = -1;
    a->field_select_active = 0;
    clear_field_undo(a);
    set_status(a, "Field updated");
}

static void jump_to_first_text_field(App *a)
{
    size_t i;
    int found = -1;
    int h, w;

    for (i = 0; i < a->page.control_count; i++) {
        if (a->page.controls[i].type == CONTROL_SEARCH) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) {
        for (i = 0; i < a->page.control_count; i++) {
            if (control_is_textual(&a->page.controls[i])) {
                found = (int)i;
                break;
            }
        }
    }
    if (found < 0) {
        set_status(a, "No search field on this page");
        return;
    }

    a->selected_link = -1;
    a->selected_control = found;
    getmaxyx(stdscr, h, w);
    ensure_selected_visible(a, h - 3, browse_read_width(w));
    begin_field_edit(a, found);
}

static int field_selection_bounds(App *a, int *start, int *end)
{
    int a0;
    int b0;

    if (!a->field_select_active || a->field_select_anchor == a->field_cursor)
        return 0;
    a0 = a->field_select_anchor;
    b0 = a->field_cursor;
    if (a0 > b0) {
        int tmp = a0;
        a0 = b0;
        b0 = tmp;
    }
    if (start) *start = a0;
    if (end) *end = b0;
    return b0 > a0;
}

static void field_clear_selection(App *a)
{
    a->field_select_active = 0;
    a->field_select_anchor = a->field_cursor;
}

static void field_move_cursor(App *a, FormControl *c, int pos, int extend)
{
    int len = (int)strlen(c && c->value ? c->value : "");

    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    if (extend) {
        if (!a->field_select_active)
            a->field_select_anchor = a->field_cursor;
        a->field_select_active = 1;
    }
    a->field_cursor = pos;
    if (!extend || a->field_select_anchor == a->field_cursor)
        field_clear_selection(a);
}

static int field_prev_word(const char *value, int pos)
{
    const char *s = value ? value : "";

    while (pos > 0 && isspace((unsigned char)s[pos - 1])) pos--;
    while (pos > 0 && !isspace((unsigned char)s[pos - 1])) pos--;
    return pos;
}

static int field_next_word(const char *value, int pos)
{
    const char *s = value ? value : "";
    int len = (int)strlen(s);

    while (pos < len && !isspace((unsigned char)s[pos])) pos++;
    while (pos < len && isspace((unsigned char)s[pos])) pos++;
    return pos;
}

static void field_delete_selection(App *a, FormControl *c)
{
    int start;
    int end;
    size_t len;

    if (!field_selection_bounds(a, &start, &end) || !c->value)
        return;
    len = strlen(c->value);
    remember_field_undo(a, c);
    memmove(c->value + start, c->value + end, len - (size_t)end + 1);
    a->field_cursor = start;
    field_clear_selection(a);
}

static void field_value_insert(App *a, FormControl *c, int ch)
{
    size_t len = strlen(c->value ? c->value : "");

    if (!c->value)
        c->value = xstrdup_local("");
    if (field_selection_bounds(a, NULL, NULL)) {
        field_delete_selection(a, c);
        len = strlen(c->value);
    }
    remember_field_undo(a, c);
    c->value = xrealloc(c->value, len + 2);
    memmove(c->value + a->field_cursor + 1, c->value + a->field_cursor,
            len - (size_t)a->field_cursor + 1);
    c->value[a->field_cursor++] = (char)ch;
    field_clear_selection(a);
}

static void field_value_insert_string(App *a, FormControl *c, const char *text)
{
    size_t old_len;
    size_t add_len;

    if (!text || !*text)
        return;
    if (!c->value)
        c->value = xstrdup_local("");
    if (field_selection_bounds(a, NULL, NULL))
        field_delete_selection(a, c);
    old_len = strlen(c->value);
    add_len = strlen(text);
    remember_field_undo(a, c);
    c->value = xrealloc(c->value, old_len + add_len + 1);
    memmove(c->value + a->field_cursor + add_len,
            c->value + a->field_cursor,
            old_len - (size_t)a->field_cursor + 1);
    memcpy(c->value + a->field_cursor, text, add_len);
    a->field_cursor += (int)add_len;
    field_clear_selection(a);
}

static void field_backspace(App *a, FormControl *c)
{
    size_t len = strlen(c->value ? c->value : "");

    if (field_selection_bounds(a, NULL, NULL)) {
        field_delete_selection(a, c);
        return;
    }
    if (a->field_cursor <= 0 || !c->value) return;
    remember_field_undo(a, c);
    memmove(c->value + a->field_cursor - 1, c->value + a->field_cursor,
            len - (size_t)a->field_cursor + 1);
    a->field_cursor--;
}

static void field_delete(App *a, FormControl *c)
{
    size_t len = strlen(c->value ? c->value : "");

    if (field_selection_bounds(a, NULL, NULL)) {
        field_delete_selection(a, c);
        return;
    }
    if (!c->value || a->field_cursor >= (int)len) return;
    remember_field_undo(a, c);
    memmove(c->value + a->field_cursor, c->value + a->field_cursor + 1,
            len - (size_t)a->field_cursor);
}

static void field_copy_selection(App *a, FormControl *c)
{
    int start;
    int end;
    char *copy;

    if (!field_selection_bounds(a, &start, &end)) {
        set_status(a, "No field selection");
        return;
    }
    copy = xstrndup_local(c->value ? c->value + start : "", (size_t)(end - start));
    free(a->field_clipboard);
    a->field_clipboard = xstrdup_local(copy);
    if (write_system_clipboard(a, copy))
        set_status(a, "Copied field selection");
    else
        set_status(a, "Copied field selection locally");
    free(copy);
}

static void field_cut_selection(App *a, FormControl *c)
{
    if (!field_selection_bounds(a, NULL, NULL)) {
        set_status(a, "No field selection");
        return;
    }
    field_copy_selection(a, c);
    field_delete_selection(a, c);
}

static char *field_normalize_paste(FormControl *c, char *text)
{
    char *out;
    size_t len;
    size_t i;

    if (!text)
        return NULL;
    if (c->type == CONTROL_TEXTAREA)
        return text;
    len = strlen(text);
    out = xmalloc(len + 1);
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        out[i] = (ch == '\r' || ch == '\n') ? ' ' : (char)ch;
    }
    out[len] = 0;
    free(text);
    return out;
}

static void field_paste_clipboard(App *a, FormControl *c)
{
    char *clip = read_system_clipboard(a);

    if (!clip && a->field_clipboard)
        clip = xstrdup_local(a->field_clipboard);
    clip = field_normalize_paste(c, clip);
    if (!clip || !*clip) {
        free(clip);
        set_status(a, "Clipboard is empty");
        return;
    }
    field_value_insert_string(a, c, clip);
    set_status(a, "Pasted");
    free(clip);
}

static void field_undo(App *a, FormControl *c)
{
    if (!a->field_undo_value) {
        set_status(a, "Nothing to undo");
        return;
    }
    free(a->field_redo_value);
    a->field_redo_value = xstrdup_local(c->value ? c->value : "");
    a->field_redo_cursor = a->field_cursor;
    free(c->value);
    c->value = a->field_undo_value;
    a->field_cursor = a->field_undo_cursor;
    a->field_undo_value = NULL;
    field_clear_selection(a);
    set_status(a, "Undo");
}

static void field_redo(App *a, FormControl *c)
{
    if (!a->field_redo_value) {
        set_status(a, "Nothing to redo");
        return;
    }
    free(a->field_undo_value);
    a->field_undo_value = xstrdup_local(c->value ? c->value : "");
    a->field_undo_cursor = a->field_cursor;
    free(c->value);
    c->value = a->field_redo_value;
    a->field_cursor = a->field_redo_cursor;
    a->field_redo_value = NULL;
    field_clear_selection(a);
    set_status(a, "Redo");
}

static void toggle_control(App *a, int control_index)
{
    FormControl *c;
    size_t i;

    if (control_index < 0 || control_index >= (int)a->page.control_count)
        return;
    c = &a->page.controls[control_index];
    if (!control_is_checkable(c)) {
        set_status(a, "Selected field is not a checkbox or radio");
        return;
    }
    if (c->type == CONTROL_RADIO) {
        for (i = 0; i < a->page.control_count; i++) {
            FormControl *other = &a->page.controls[i];

            if (other->type == CONTROL_RADIO &&
                other->form_index == c->form_index &&
                other->name && c->name &&
                !strcmp(other->name, c->name)) {
                other->checked = 0;
                if (a->page.text && other->marker_offset + 2 < strlen(a->page.text))
                    a->page.text[other->marker_offset + 1] = ' ';
            }
        }
        c->checked = 1;
    } else {
        c->checked = !c->checked;
    }
    if (a->page.text && c->marker_offset + 2 < strlen(a->page.text))
        a->page.text[c->marker_offset + 1] = c->checked ? 'x' : ' ';
    snprintf(a->status, sizeof(a->status), "%s %s",
             c->checked ? "Checked" : "Unchecked",
             c->label && *c->label ? c->label : control_type_name(c));
}

static void cycle_select_control(App *a, int control_index)
{
    FormControl *c;

    if (control_index < 0 || control_index >= (int)a->page.control_count)
        return;
    c = &a->page.controls[control_index];
    if (c->type != CONTROL_SELECT || !c->option_count) {
        set_status(a, "Selected field is not a select menu");
        return;
    }
    c->selected++;
    if (c->selected < 0 || (size_t)c->selected >= c->option_count)
        c->selected = 0;
    free(c->value);
    c->value = xstrdup_local(c->option_values[c->selected]);
    snprintf(a->status, sizeof(a->status), "%s: %s",
             c->label && *c->label ? c->label : "Select",
             c->options[c->selected]);
}

static void activate_selected_control(App *a)
{
    FormControl *c;

    if (a->selected_control < 0 ||
        a->selected_control >= (int)a->page.control_count) {
        set_status(a, "No selected field");
        return;
    }
    c = &a->page.controls[a->selected_control];
    if (control_is_textual(c)) {
        begin_field_edit(a, a->selected_control);
    } else if (control_is_checkable(c)) {
        toggle_control(a, a->selected_control);
    } else if (c->type == CONTROL_SELECT) {
        cycle_select_control(a, a->selected_control);
    } else {
        submit_control(a, a->selected_control);
    }
}

static void handle_field_key(App *a, int ch)
{
    FormControl *c;
    size_t len;

    if (a->editing_control < 0 ||
        a->editing_control >= (int)a->page.control_count) {
        finish_field_edit(a);
        return;
    }
    c = &a->page.controls[a->editing_control];
    len = strlen(c->value ? c->value : "");
    if (a->field_cursor < 0) a->field_cursor = 0;
    if (a->field_cursor > (int)len) a->field_cursor = (int)len;

    switch (ch) {
    case 27:
        timeout(25);
        ch = getch();
        timeout(-1);
        if (ch == 'w' || ch == 'W') {
            field_copy_selection(a, c);
        } else {
            if (ch != ERR)
                ungetch(ch);
            finish_field_edit(a);
        }
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        finish_field_edit(a);
        submit_control(a, a->selected_control);
        break;
    case KEY_LEFT:
        {
            int start;
            int end;

            if (field_selection_bounds(a, &start, &end))
                field_move_cursor(a, c, start, 0);
            else
                field_move_cursor(a, c, a->field_cursor - 1, 0);
        }
        break;
    case KEY_RIGHT:
        {
            int start;
            int end;

            if (field_selection_bounds(a, &start, &end))
                field_move_cursor(a, c, end, 0);
            else
                field_move_cursor(a, c, a->field_cursor + 1, 0);
        }
        break;
    case KEY_SLEFT:
        field_move_cursor(a, c, a->field_cursor - 1, 1);
        break;
    case KEY_SRIGHT:
        field_move_cursor(a, c, a->field_cursor + 1, 1);
        break;
    case BROWSE_KEY_WORD_LEFT:
        field_move_cursor(a, c, field_prev_word(c->value, a->field_cursor), 0);
        break;
    case BROWSE_KEY_WORD_RIGHT:
        field_move_cursor(a, c, field_next_word(c->value, a->field_cursor), 0);
        break;
    case BROWSE_KEY_SELECT_WORD_LEFT:
        field_move_cursor(a, c, field_prev_word(c->value, a->field_cursor), 1);
        break;
    case BROWSE_KEY_SELECT_WORD_RIGHT:
        field_move_cursor(a, c, field_next_word(c->value, a->field_cursor), 1);
        break;
    case KEY_HOME:
    case CTRL_KEY('a'):
        field_move_cursor(a, c, 0, 0);
        break;
    case KEY_END:
    case CTRL_KEY('e'):
        field_move_cursor(a, c, (int)len, 0);
        break;
    case KEY_BACKSPACE:
    case 127:
    case '\b':
        field_backspace(a, c);
        break;
    case KEY_DC:
        field_delete(a, c);
        break;
    case CTRL_KEY('w'):
        field_cut_selection(a, c);
        break;
    case CTRL_KEY('y'):
        field_paste_clipboard(a, c);
        break;
    case CTRL_KEY('x'):
        set_status(a, "Use Esc to leave field editing");
        break;
    case CTRL_KEY('z'):
        field_undo(a, c);
        break;
    case CTRL_KEY('r'):
        field_redo(a, c);
        break;
    case '\t':
        field_value_insert(a, c, '\t');
        break;
    default:
        if (ch >= 32 && ch < 127)
            field_value_insert(a, c, ch);
        break;
    }
}

static void url_insert(App *a, int ch)
{
    if (a->url_len >= URL_MAX - 1) return;
    memmove(a->url_bar + a->url_pos + 1, a->url_bar + a->url_pos,
            (size_t)(a->url_len - a->url_pos + 1));
    a->url_bar[a->url_pos++] = (char)ch;
    a->url_len++;
}

static void handle_url_key(App *a, int ch)
{
    switch (ch) {
    case '\n':
    case '\r':
    case KEY_ENTER:
        a->url_focus = 0;
        load_url(a, a->url_bar, NAV_NORMAL);
        break;
    case 27:
        a->url_focus = 0;
        a->mode = MODE_PAGE;
        break;
    case KEY_LEFT:
        if (a->url_pos > 0) a->url_pos--;
        break;
    case KEY_RIGHT:
        if (a->url_pos < a->url_len) a->url_pos++;
        break;
    case KEY_HOME:
        a->url_pos = 0;
        break;
    case KEY_END:
        a->url_pos = a->url_len;
        break;
    case KEY_BACKSPACE:
    case 127:
    case '\b':
        if (a->url_pos > 0) {
            memmove(a->url_bar + a->url_pos - 1, a->url_bar + a->url_pos,
                    (size_t)(a->url_len - a->url_pos + 1));
            a->url_pos--;
            a->url_len--;
        }
        break;
    case KEY_DC:
        if (a->url_pos < a->url_len) {
            memmove(a->url_bar + a->url_pos, a->url_bar + a->url_pos + 1,
                    (size_t)(a->url_len - a->url_pos));
            a->url_len--;
        }
        break;
    default:
        if (ch >= 32 && ch < 127) url_insert(a, ch);
        break;
    }
}

static void find_insert(App *a, int ch)
{
    if (a->find_len >= (int)sizeof(a->find_query) - 1) return;
    memmove(a->find_query + a->find_pos + 1, a->find_query + a->find_pos,
            (size_t)(a->find_len - a->find_pos + 1));
    a->find_query[a->find_pos++] = (char)ch;
    a->find_len++;
}

static void handle_find_key(App *a, int ch)
{
    switch (ch) {
    case '\n':
    case '\r':
    case KEY_ENTER:
        a->find_focus = 0;
        a->mode = MODE_PAGE;
        search_page(a, 1, top_text_offset(a));
        break;
    case 27:
        a->find_focus = 0;
        a->mode = MODE_PAGE;
        break;
    case KEY_LEFT:
        if (a->find_pos > 0) a->find_pos--;
        break;
    case KEY_RIGHT:
        if (a->find_pos < a->find_len) a->find_pos++;
        break;
    case KEY_HOME:
        a->find_pos = 0;
        break;
    case KEY_END:
        a->find_pos = a->find_len;
        break;
    case KEY_BACKSPACE:
    case 127:
    case '\b':
        if (a->find_pos > 0) {
            memmove(a->find_query + a->find_pos - 1, a->find_query + a->find_pos,
                    (size_t)(a->find_len - a->find_pos + 1));
            a->find_pos--;
            a->find_len--;
        }
        break;
    case KEY_DC:
        if (a->find_pos < a->find_len) {
            memmove(a->find_query + a->find_pos, a->find_query + a->find_pos + 1,
                    (size_t)(a->find_len - a->find_pos));
            a->find_len--;
        }
        break;
    default:
        if (ch >= 32 && ch < 127) find_insert(a, ch);
        break;
    }
}

static void handle_bookmark_key(App *a, int ch)
{
    int h, w;
    int body_h;

    getmaxyx(stdscr, h, w);
    (void)w;
    body_h = h - 3;
    if (body_h < 1) body_h = 1;

    switch (ch) {
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (a->bookmark_count &&
            a->selected_bookmark >= 0 &&
            a->selected_bookmark < (int)a->bookmark_count) {
            load_url(a, a->bookmarks[a->selected_bookmark].url, NAV_NORMAL);
        } else {
            set_status(a, "No bookmark selected");
        }
        break;
    case 'B':
    case 27:
    case KEY_BACKSPACE:
    case 127:
    case '\b':
        a->bookmark_mode = 0;
        a->mode = MODE_PAGE;
        set_status(a, "Closed bookmarks");
        break;
    case 'q':
        a->running = 0;
        break;
    case KEY_UP:
    case 'k':
        if (a->selected_bookmark > 0) a->selected_bookmark--;
        break;
    case KEY_DOWN:
    case 'j':
        if (a->selected_bookmark + 1 < (int)a->bookmark_count)
            a->selected_bookmark++;
        break;
    case KEY_PPAGE:
        a->selected_bookmark -= body_h > 1 ? body_h - 1 : 1;
        if (a->selected_bookmark < 0) a->selected_bookmark = 0;
        break;
    case KEY_NPAGE:
        a->selected_bookmark += body_h > 1 ? body_h - 1 : 1;
        if (a->selected_bookmark >= (int)a->bookmark_count)
            a->selected_bookmark = a->bookmark_count ? (int)a->bookmark_count - 1 : 0;
        break;
    case KEY_HOME:
    case 'g':
        a->selected_bookmark = 0;
        break;
    case KEY_END:
    case 'G':
        a->selected_bookmark = a->bookmark_count ? (int)a->bookmark_count - 1 : 0;
        break;
    default:
        break;
    }
}

static void handle_page_key(App *a, int ch)
{
    int h, w;
    int body_h;
    int body_w;

    getmaxyx(stdscr, h, w);
    body_h = h - 3;
    body_w = browse_read_width(w);

    if (isdigit((unsigned char)ch)) {
        append_number_entry(a, ch);
        return;
    }

    if (a->number_len && ch != '\n' && ch != '\r' && ch != KEY_ENTER)
        clear_number_entry(a);

    switch (ch) {
    case CTRL_KEY('l'):
        a->mode = MODE_URL;
        a->url_focus = 1;
        a->find_focus = 0;
        a->bookmark_mode = 0;
        a->url_pos = a->url_len;
        break;
    case '/':
        a->mode = MODE_FIND;
        a->find_focus = 1;
        a->url_focus = 0;
        a->bookmark_mode = 0;
        a->find_pos = a->find_len;
        set_status(a, "Find");
        break;
    case 'i':
        jump_to_first_text_field(a);
        break;
    case '\t':
    case KEY_BTAB:
        set_status(a, "Use PgUp/PgDown to move through links and fields; press i for first search field");
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (!a->number_len && a->selected_control >= 0)
            activate_selected_control(a);
        else
            open_numbered_link(a);
        break;
    case 'f':
        go_forward(a);
        break;
    case 'r':
        if (a->current_url[0]) load_url(a, a->current_url, NAV_REPLACE);
        else set_status(a, "No page to reload");
        break;
    case 'J':
        if (a->current_url[0]) load_url_js(a, a->current_url, NAV_REPLACE);
        else set_status(a, "No page to reload with WebKit");
        break;
    case 'n':
        search_page(a, 1, a->has_match ? a->match_offset + 1 : top_text_offset(a));
        break;
    case 'N':
        search_page(a, -1, a->has_match ? a->match_offset : top_text_offset(a));
        break;
    case 'm':
        bookmark_current_page(a);
        break;
    case 'B':
        open_bookmark_list(a);
        break;
    case 's':
        save_page_text(a);
        break;
    case 'C':
    {
        size_t removed = 0;

        page_cache_clear(a);
        if (clear_browser_cache(&removed))
            snprintf(a->status, sizeof(a->status), "Cleared cache: %zu files", removed);
        else
            set_status(a, "Could not clear cache");
        break;
    }
    case 'o':
        open_external(a, a->current_url);
        break;
    case 'O':
        if (a->selected_link >= 0 && a->selected_link < (int)a->page.link_count)
            open_external(a, a->page.links[a->selected_link].url);
        else
            set_status(a, "No selected link");
        break;
    case 'q':
        a->running = 0;
        break;
    case KEY_UP:
        scroll_page(a, -5, body_h, body_w);
        break;
    case 'k':
        scroll_page(a, -1, body_h, body_w);
        break;
    case KEY_DOWN:
        scroll_page(a, 5, body_h, body_w);
        break;
    case 'j':
        scroll_page(a, 1, body_h, body_w);
        break;
    case KEY_PPAGE:
        next_link_stop(a, -1, body_h, body_w);
        break;
    case 'b':
        scroll_page(a, -5, body_h, body_w);
        break;
    case KEY_BACKSPACE:
    case 127:
    case '\b':
        go_back(a);
        break;
    case KEY_NPAGE:
        next_link_stop(a, 1, body_h, body_w);
        break;
    case ' ':
        if (a->selected_control >= 0 &&
            a->selected_control < (int)a->page.control_count &&
            control_is_checkable(&a->page.controls[a->selected_control]))
            toggle_control(a, a->selected_control);
        else
            scroll_page(a, 5, body_h, body_w);
        break;
    case KEY_HOME:
        a->top = 0;
        a->selected_link = -1;
        a->selected_control = -1;
        clamp_top(a, body_h, body_w);
        break;
    case KEY_END:
        page_layout(&a->page, body_w);
        a->top = a->page.display_count > (size_t)body_h ?
                 (int)a->page.display_count - body_h : 0;
        a->selected_link = -1;
        a->selected_control = -1;
        clamp_top(a, body_h, body_w);
        break;
    case 'g':
        jump_to_article_heading(a);
        break;
    case 'G':
        jump_past_top_navigation(a);
        break;
    default:
        break;
    }
}

static void app_free(App *a)
{
    webkitd_stop();
    page_free(&a->page);
    page_cache_clear(a);
    stack_clear(a->back_stack, &a->back_count);
    stack_clear(a->forward_stack, &a->forward_count);
    stack_clear(a->visited_urls, &a->visited_count);
    free_bookmarks(a);
    free(a->field_undo_value);
    free(a->field_redo_value);
    free(a->field_clipboard);
}

static int fetch_page_for_dump(const char *input, Page *page, int force_js)
{
    char *url = normalize_input_url(input);
    char *fallback = NULL;
    char *html;
    char original_url[URL_MAX];
    FetchResult result;
    FetchResult retry;
    FetchResult js_result;
    char final_url[URL_MAX];
    int used_webkit = 0;
    int explicit_https_input = starts_https_url_trimmed(input);

    memset(page, 0, sizeof(*page));
    page->layout_width = -1;

    if (!*url) {
        fprintf(stderr, "simplebrowse: empty URL\n");
        free(url);
        return 1;
    }
    snprintf(original_url, sizeof(original_url), "%s", url);

    if (force_js || webkit_first_url(url)) {
        html = fetch_url_js(url, &result);
        used_webkit = html != NULL;
    } else {
        html = fetch_url(url, &result);
        if (!html && result.network_error) {
            fallback = explicit_https_input ? NULL : http_fallback_url(url);
            if (fallback) {
                html = fetch_url(fallback, &retry);
                if (html) {
                    free(url);
                    url = fallback;
                    fallback = NULL;
                    result = retry;
                } else if (failed_http_fallback_returned_to_https(&retry)) {
                    char original_error[sizeof(result.error)];

                    snprintf(original_error, sizeof(original_error), "%s",
                             result.error[0] ? result.error : "request failed");
                    snprintf(result.error, sizeof(result.error),
                             "%.240s; HTTP fallback redirected back to HTTPS and failed: %.180s",
                             original_error,
                             retry.error[0] ? retry.error : "request failed");
                } else {
                    char combined[512];

                    snprintf(combined, sizeof(combined),
                             "HTTPS failed: %.180s; HTTP failed: %.180s",
                             result.error, retry.error);
                    snprintf(result.error, sizeof(result.error), "%s", combined);
                    if (retry.effective[0])
                        snprintf(result.effective, sizeof(result.effective), "%s", retry.effective);
                    result.code = retry.code;
                    snprintf(result.reason, sizeof(result.reason), "%s", retry.reason);
                    result.network_error = retry.network_error;
                }
            }
        }
    }

    snprintf(final_url, sizeof(final_url), "%s", result.effective[0] ? result.effective : url);
    if (!force_js && !used_webkit && !html && result.network_error &&
        curl_error_should_retry_js(result.curl_code)) {
        html = fetch_url_js(webkit_retry_target(original_url, final_url),
                            &js_result);
        if (html) {
            result = js_result;
            snprintf(final_url, sizeof(final_url), "%s",
                     result.effective[0] ? result.effective :
                     webkit_retry_target(original_url, final_url));
            used_webkit = 1;
        }
    }

    if (!force_js && !used_webkit && http_status_should_retry_js(result.code)) {
        const char *retry_url = webkit_retry_target(original_url, final_url);
        char *old_html = html;

        html = fetch_url_js(retry_url, &js_result);
        if (html) {
            free(old_html);
            result = js_result;
            snprintf(final_url, sizeof(final_url), "%s",
                     result.effective[0] ? result.effective : retry_url);
            used_webkit = 1;
        } else {
            html = old_html;
        }
    }

    if (html && used_webkit &&
        webkit_snapshot_rejected(final_url, html, strlen(html), &result)) {
        free(html);
        html = NULL;
        result.network_error = 0;
        if (!result.code)
            result.code = 429;
        snprintf(result.reason, sizeof(result.reason), "%s",
                 http_reason_phrase(result.code));
        snprintf(result.error, sizeof(result.error),
                 "site rejected the WebKit browser session");
    }

    if (!html) {
        fprintf(stderr, "simplebrowse: %s: %s\n",
                result.effective[0] ? result.effective : url,
                result.error[0] ? result.error : "request failed");
        free(url);
        free(fallback);
        return 1;
    }

    *page = parse_html(html, strlen(html), final_url);
    if (!force_js && static_page_should_retry_js(final_url, html, strlen(html), page)) {
        char *js_html;

        page_free(page);
        memset(&js_result, 0, sizeof(js_result));
        js_html = fetch_url_js(webkit_retry_target(original_url, final_url),
                               &js_result);
        if (js_html) {
            if (webkit_snapshot_rejected(js_result.effective[0] ?
                                         js_result.effective : final_url,
                                         js_html, strlen(js_html),
                                         &js_result)) {
                fprintf(stderr, "simplebrowse: WebKit retry rejected: %s\n",
                        js_result.error[0] ? js_result.error :
                        "site rejected browser session");
                free(js_html);
                free(html);
                free(url);
                free(fallback);
                return 1;
            }
            free(html);
            html = js_html;
            if (js_result.effective[0])
                snprintf(final_url, sizeof(final_url), "%s", js_result.effective);
            *page = parse_html(html, strlen(html), final_url);
        } else {
            fprintf(stderr, "simplebrowse: WebKit retry failed: %s\n",
                    js_result.error[0] ? js_result.error : "backend failed");
            free(html);
            free(url);
            free(fallback);
            return 1;
        }
    }
    normalize_reddit_listing_page(page);
    free(html);
    free(url);
    free(fallback);
    return 0;
}

static int dump_url_text(const char *input, int force_js)
{
    Page page;
    int ok = 1;

    if (fetch_page_for_dump(input, &page, force_js) != 0)
        return 1;

    if (page.text && *page.text) {
        fputs(page.text, stdout);
        fputc('\n', stdout);
    } else {
        ok = 1;
    }

    page_free(&page);
    return ok ? 0 : 1;
}

static void page_offset_line_col(Page *p, size_t offset, int *line_out,
                                 int *col_out)
{
    int line = 0;

    if (display_line_for_offset(p, offset, &line)) {
        DisplayLine *display = &p->display[line];

        *line_out = line + 1;
        *col_out = (int)(offset - display->start) + 1;
        return;
    }
    *line_out = 1;
    *col_out = 1;
}

static char *debug_label_for_range(Page *p, size_t start, size_t end)
{
    const char *text = p->text ? p->text : "";
    Buffer b = {0};
    int pending_space = 0;

    while (start < end) {
        unsigned char c = (unsigned char)text[start++];

        if (isspace(c)) {
            pending_space = b.len > 0;
            continue;
        }
        if (pending_space) {
            buf_addc(&b, ' ');
            pending_space = 0;
        }
        if (c == '\t') c = ' ';
        buf_addc(&b, (char)c);
    }

    return b.data ? b.data : xstrdup_local("");
}

static int dump_url_links(const char *input, int force_js)
{
    Page page;
    size_t i;

    if (fetch_page_for_dump(input, &page, force_js) != 0)
        return 1;

    page_layout(&page, 100);
    printf("index\ttype\tstart\tend\ttarget\tlabel\n");
    for (i = 0; i < page.stop_count; i++) {
        LinkStop *stop = &page.stops[i];
        char *label = debug_label_for_range(&page, stop->start, stop->end);
        int start_line;
        int start_col;
        int end_line;
        int end_col;
        const char *type = "link";
        const char *target = "";

        page_offset_line_col(&page, stop->start, &start_line, &start_col);
        page_offset_line_col(&page, stop->end > stop->start ? stop->end - 1 : stop->start,
                             &end_line, &end_col);
        if (stop->kind == STOP_LINK && stop->first_link >= 0 &&
            stop->first_link < (int)page.link_count) {
            target = page.links[stop->first_link].url;
        } else if (stop->kind == STOP_CONTROL && stop->control_index >= 0 &&
                   stop->control_index < (int)page.control_count) {
            FormControl *c = &page.controls[stop->control_index];

            type = control_type_name(c);
            target = c->form_action && *c->form_action ? c->form_action : page.url;
        }
        printf("%zu\t%s\t%d:%d\t%d:%d\t%s\t%s\n",
               i + 1, type, start_line, start_col, end_line, end_col,
               target ? target : "", label);
        free(label);
    }

    page_free(&page);
    return 0;
}

static void print_help(void)
{
    printf("SimpleBrowse %s\n", SIMPLEBROWSE_VERSION);
    printf("Usage:\n");
    printf("  simplebrowse [URL]\n");
    printf("  simplebrowse --js URL\n");
    printf("  simplebrowse --dump URL...\n");
    printf("  simplebrowse --dump-js URL...\n");
    printf("  simplebrowse --dump-links URL...\n");
    printf("  simplebrowse --dump-links-js URL...\n");
    printf("  simplebrowse --clear-cache\n");
    printf("\nKeys:\n");
    printf("  PgDown/PgUp    next/previous visible link or field\n");
    printf("  Enter          open selected link, edit field, or submit form\n");
    printf("  Space          toggle selected checkbox/radio; otherwise page down\n");
    printf("  Backspace      back\n");
    printf("  J              reload current page with WebKit\n");
    printf("  r              reload using the fast static path\n");
    printf("  C              clear cached pages\n");
    printf("  Up/Down/j/k/b/Space scroll\n");
    printf("  Ctrl-L         URL bar\n");
    printf("  /              find\n");
    printf("  Field edit     Esc done, Tab insert tab, Ctrl-Left/Right words\n");
    printf("                 Shift-Left/Right select, Alt-w copy, Ctrl-w cut, Ctrl-y paste\n");
    printf("  q              quit\n");
}

int main(int argc, char **argv)
{
    App app;
    int i;
    int rc;
    int argi = 1;
    int start_js = 0;

    memset(&app, 0, sizeof(app));
    app.running = 1;
    app.url_focus = 1;
    app.mode = MODE_URL;
    app.selected_link = -1;
    app.selected_control = -1;
    app.editing_control = -1;
    app.page.text = xstrdup_local("Ctrl-L focuses the URL bar. Enter a URL and press Enter.");
    app.page.layout_width = -1;
    set_status(&app, "Enter a URL");

    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        print_help();
        curl_global_cleanup();
        app_free(&app);
        return 0;
    }

    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("SimpleBrowse %s\n", SIMPLEBROWSE_VERSION);
        curl_global_cleanup();
        app_free(&app);
        return 0;
    }

    if (argc > 1 && !strcmp(argv[1], "--clear-cache")) {
        size_t removed = 0;

        if (!clear_browser_cache(&removed)) {
            fprintf(stderr, "simplebrowse: could not clear cache\n");
            curl_global_cleanup();
            app_free(&app);
            return 1;
        }
        printf("Cleared SimpleBrowse cache: %zu files\n", removed);
        curl_global_cleanup();
        app_free(&app);
        return 0;
    }

    if (argc > 2 && (!strcmp(argv[1], "--dump") ||
                     !strcmp(argv[1], "--dump-js") ||
                     !strcmp(argv[1], "--dump-links") ||
                     !strcmp(argv[1], "--dump-links-js"))) {
        int dump_links = !strcmp(argv[1], "--dump-links") ||
                         !strcmp(argv[1], "--dump-links-js");
        int force_js = !strcmp(argv[1], "--dump-js") ||
                       !strcmp(argv[1], "--dump-links-js");

        rc = 0;
        for (i = 2; i < argc; i++) {
            if (i > 2) putchar('\n');
            if ((dump_links ? dump_url_links(argv[i], force_js) :
                              dump_url_text(argv[i], force_js)) != 0)
                rc = 1;
        }
        curl_global_cleanup();
        app_free(&app);
        return rc;
    }

    if (argc > 1 && !strcmp(argv[1], "--js")) {
        if (argc < 3) {
            fprintf(stderr, "simplebrowse: --js requires a URL\n");
            curl_global_cleanup();
            app_free(&app);
            return 2;
        }
        start_js = 1;
        argi = 2;
    } else if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "simplebrowse: unknown option: %s\n", argv[1]);
        fprintf(stderr, "Try: simplebrowse --help\n");
        curl_global_cleanup();
        app_free(&app);
        return 2;
    }

    init_bookmarks(&app);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    use_extended_names(TRUE);
    init_browser_colors();
    discover_browser_keys();

    if (argc > argi) {
        snprintf(app.url_bar, sizeof(app.url_bar), "%s", argv[argi]);
        app.url_len = (int)strlen(app.url_bar);
        app.url_pos = app.url_len;
        app.url_focus = 0;
        app.mode = MODE_PAGE;
        if (start_js)
            load_url_js(&app, app.url_bar, NAV_REPLACE);
        else
            load_url(&app, app.url_bar, NAV_REPLACE);
    }

    while (app.running) {
        int ch;

        draw_screen(&app);
        ch = read_browser_key();
        if (app.find_focus)
            handle_find_key(&app, ch);
        else if (app.bookmark_mode)
            handle_bookmark_key(&app, ch);
        else if (app.url_focus)
            handle_url_key(&app, ch);
        else if (app.mode == MODE_FIELD)
            handle_field_key(&app, ch);
        else
            handle_page_key(&app, ch);
    }

    endwin();
    curl_global_cleanup();
    app_free(&app);
    return 0;
}
