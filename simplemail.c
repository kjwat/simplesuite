/*
 * simplemail.c - first SimpleSuite-style mail client draft
 * Clean-build smoke test.
 *
 * v0.1 scope:
 *   - Local Maildir-style mailbox browser
 *   - Launches directly into Inbox
 *   - m toggles mailbox chooser overlay
 *   - Arrow keys move
 *   - Enter opens full-screen reading view
 *   - Backspace goes back
 *   - q quits
 *   - c compose: prompts To + Subject, then opens SimpleWords for body
 *   - r reply: from reading view, opens SimpleWords for reply body
 *
 * Not yet implemented:
 *   - IMAP sync
 *   - SMTP sending
 *   - ProtonBridge setup
 *   - Attachments extraction/opening
 *   - MIME decoding beyond basic text/plain-ish display
 *
 * Build from the SimpleSuite directory with ./build.sh.
 */

#define _XOPEN_SOURCE 700
#define _XOPEN_SOURCE_EXTENDED 1

#include <ncurses.h>
#include <locale.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <pthread.h>
#include <stdatomic.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdint.h>
#endif

#include "simplerender.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_MESSAGES 2048
#define MAX_MAILBOXES 32
#define MAX_LINE 4096
#define MAX_BODY 262144


typedef enum {
    VIEW_LIST,
    VIEW_THREAD,
    VIEW_READ
} View;

typedef struct {
    char path[PATH_MAX];
    char from[256];
    char subject[512];
    char date[128];
    char message_id[256];
    char in_reply_to[256];
    char references[1024];
    int unread;
    int has_attachment;
    char attachment_name[256];
    char attachment_path[PATH_MAX];
    char *body;
    int body_loaded;
    time_t order_time;
    int order_time_loaded;
} Message;

typedef struct {
    char name[128];
    char path[PATH_MAX];
} Mailbox;


static Message messages[MAX_MESSAGES];
static int message_count = 0;
static int selected = 0;
static int list_top = 0;
static int selected_flags[MAX_MESSAGES];
static int select_anchor = -1;
static int selected_thread_header = 1;
static int thread_cursor = 0;
static int thread_anchor = -1;
static int thread_all_boxes_loaded = 0;

static Mailbox mailboxes[MAX_MAILBOXES];
static int mailbox_count = 0;
static int current_mailbox = 0;
static int mailbox_overlay = 0;
static int selected_mailbox = 0;

static View view = VIEW_LIST;
static View read_return_view = VIEW_LIST;
static int read_scroll = 0;
static SsrRenderer read_renderer;
static int read_renderer_ready = 0;

/*
 * SimpleWords-style read surface state. The message body renderer owns the
 * body rectangle; this tracks when the surrounding page actually changed.
 */
static int read_surface_valid = 0;
static int read_surface_h = 0;
static int read_surface_w = 0;
static int read_surface_left = 0;
static int read_surface_width = 0;
static int read_surface_top = 0;
static int read_surface_height = 0;
static char read_surface_key[PATH_MAX];
static int pending_delete = 0;
static int pending_restore = 0;

static void simplemail_ensure_read_renderer(void)
{
    if (!read_renderer_ready) {
        ssr_init(&read_renderer);
        read_renderer_ready = 1;
    }

    /*
     * SimpleWords-style reader presentation:
     * isolate prose in the renderer body window. Keep physical terminal
     * scrolling disabled; that was the cursed path.
     */
    read_renderer.windowed_redraw_enabled = 1;
    read_renderer.scroll_window_enabled = 0;
}


static char mail_root[PATH_MAX];
static char status_msg[256];
static char simplemail_maildir[PATH_MAX] = "~/Mail";
static char simplemail_inbox_name[128] = "Inbox";
static char simplemail_sent_name[128] = "Sent";
static char simplemail_drafts_name[128] = "Drafts";
static char simplemail_archive_name[128] = "Archive";
static char simplemail_trash_name[128] = "Trash";
static int simplemail_maildir_explicit = 0;

static pid_t pull_pid = 0;
static int pull_running = 0;
static int pull_rc = 0;
static int pull_first = 0;
static pid_t send_pid = 0;
static int send_running = 0;

static char simplemail_sync_cmd[512] = "mbsync inbox";
static char simplemail_send_cmd[512] = "msmtp -t";
static char simplemail_editor_cmd[512] = "simplewords";
static char simplemail_from[256] = "";

static int current_box_is(const char *name);
static void ensure_dir(const char *path);
static void restore_current_message(void);
static void move_current_message_to(const char *boxname);
static void move_selected_or_current_to(const char *boxname);
static int confirm_quit(void);
static void draw_list(void);
static void finish_pull_if_done(void);
static void load_current_mailbox(void);
static void clear_selection(void);
static void load_simplemail_config(void);
static time_t message_order_time(int idx);
static void sort_messages_newest_first(void);
static void expand_user_path(const char *in, char *out, size_t outsz);

static void trim(char *s);

static int selection_count(void) {
    int n = 0;
    for (int i = 0; i < message_count; i++)
        if (selected_flags[i]) n++;
    return n;
}

static void clear_selection(void) {
    memset(selected_flags, 0, sizeof selected_flags);
    select_anchor = -1;
}


static void select_all_messages(void) {
    if (message_count <= 0) return;

    if (selection_count() == message_count) {
        clear_selection();
        return;
    }

    for (int i = 0; i < message_count; i++)
        selected_flags[i] = 1;

    select_anchor = selected;
}

static void invert_message_selection(void) {
    if (message_count <= 0) return;

    for (int i = 0; i < message_count; i++)
        selected_flags[i] = !selected_flags[i];

    if (selection_count() == 0)
        select_anchor = -1;
    else if (select_anchor < 0)
        select_anchor = selected;
}

static void free_messages(void) {
    for (int i = 0; i < message_count; i++) {
        free(messages[i].body);
        messages[i].body = NULL;
    }
    message_count = 0;
    selected = 0;
    list_top = 0;
    clear_selection();
    selected_thread_header = 1;
}

static char *simplemail_strcasestr_local(char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return haystack;

    for (char *h = haystack; *h; h++) {
        const char *a = h;
        const char *b = needle;

        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }

        if (!*b)
            return h;
    }

    return NULL;
}

static void strip_newsletter_footer(char *s) {
    if (!s) return;

    char *p;

    p = simplemail_strcasestr_local(s, "\nunsubscribe\n");
    if (!p) p = simplemail_strcasestr_local(s, "\nunsubscribe\r\n");
    if (!p) p = simplemail_strcasestr_local(s, "\nmanage subscription");
    if (!p) p = simplemail_strcasestr_local(s, "\nmanage preferences");
    if (!p) p = simplemail_strcasestr_local(s, "\nemail preferences");
    if (!p) p = simplemail_strcasestr_local(s, "\nupdate preferences");

    if (p)
        *p = '\0';
}


static int looks_like_tracking_gibberish(const char *line)
{
    int len = 0;
    int good = 0;
    int bad = 0;

    for (const unsigned char *p = (const unsigned char *)line; *p; p++) {
        len++;

        if ((*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == '-' ||
            *p == '/' || *p == '+' ||
            *p == '=')
            good++;
        else if (!isspace(*p))
            bad++;
    }

    if (len < 80)
        return 0;

    if (strchr(line, ' '))
        return 0;

    return good > (len * 9 / 10) && bad < 3;
}


static int looks_like_gibberish_line(const char *line)
{
    int len = 0;
    int spaces = 0;
    int good = 0;

    while (*line) {
        unsigned char c = (unsigned char)*line++;

        len++;

        if (isspace(c))
            spaces++;

        if (isalnum(c) || c == '_' || c == '-' ||
            c == '/' || c == '+' || c == '=')
            good++;
    }

    if (len < 40)
        return 0;

    if (spaces > 1)
        return 0;

    return good > (len * 9 / 10);
}

static void collapse_unsubscribe_tracking(char *text)
{
    char *src = text;
    char *dst = text;

    while (*src) {
        char *end = strchr(src, '\n');
        size_t len = end ? (size_t)(end - src) : strlen(src);

        char line[4096];
        if (len >= sizeof(line))
            len = sizeof(line) - 1;

        memcpy(line, src, len);
        line[len] = '\0';

        char trimmed[4096];
        snprintf(trimmed, sizeof trimmed, "%s", line);
        trim(trimmed);

        if (!strcasecmp(trimmed, "Unsubscribe")) {

            memcpy(dst, src, len);
            dst += len;
            *dst++ = '\n';

            const char *msg = "[tracking link omitted]\n";
            strcpy(dst, msg);
            dst += strlen(msg);

            if (end)
                src = end + 1;
            else
                break;

            while (*src) {
                char *e2 = strchr(src, '\n');
                size_t l2 = e2 ? (size_t)(e2 - src) : strlen(src);

                char t2[4096];
                if (l2 >= sizeof(t2))
                    l2 = sizeof(t2) - 1;

                memcpy(t2, src, l2);
                t2[l2] = '\0';
                trim(t2);

                if (!*t2 || looks_like_gibberish_line(t2)) {
                    if (!e2) {
                        src += strlen(src);
                        break;
                    }
                    src = e2 + 1;
                    continue;
                }

                break;
            }

            continue;
        }

        memcpy(dst, src, len);
        dst += len;

        if (end)
            *dst++ = '\n';
        else
            break;

        src = end + 1;
    }

    *dst = '\0';
}


static void strip_tracking_gibberish(char *text)
{
    if (!text) return;

    char *src = text;
    char *dst = text;

    while (*src) {
        char *end = strchr(src, '\n');
        size_t len = end ? (size_t)(end - src) : strlen(src);

        char line[4096];
        if (len >= sizeof(line))
            len = sizeof(line) - 1;

        memcpy(line, src, len);
        line[len] = '\0';

        char testline[4096];
        snprintf(testline, sizeof testline, "%s", line);
        trim(testline);

        if ((strlen(testline) > 30 &&
             (!strncmp(testline, "http://", 7) || !strncmp(testline, "https://", 8))) ||
            looks_like_tracking_gibberish(testline)) {
            const char *msg = "[long tracking URL omitted]\n";
            strcpy(dst, msg);
            dst += strlen(msg);
        } else {
            memcpy(dst, src, len);
            dst += len;
            if (end)
                *dst++ = '\n';
        }

        if (!end)
            break;

        src = end + 1;
    }

    *dst = '\0';
}

static void trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1])))
        s[--n] = '\0';

    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}


static void strip_optional_quotes(char *s) {
    if (!s) return;
    trim(s);
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
        trim(s);
    }
}

static void config_copy(char *dst, size_t dstsz, const char *src) {
    size_t length;

    if (!dst || dstsz == 0 || !src) return;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", src);
    strip_optional_quotes(tmp);
    if (!tmp[0]) return;
    length = strlen(tmp);
    if (length >= dstsz) length = dstsz - 1;
    memcpy(dst, tmp, length);
    dst[length] = '\0';
}

static void config_copy_path(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0 || !src) return;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", src);
    strip_optional_quotes(tmp);
    if (tmp[0]) snprintf(dst, dstsz, "%s", tmp);
}

static const char *simplemail_role_box(const char *role) {
    if (!strcmp(role, "Inbox")) return simplemail_inbox_name;
    if (!strcmp(role, "Sent")) return simplemail_sent_name;
    if (!strcmp(role, "Drafts")) return simplemail_drafts_name;
    if (!strcmp(role, "Archive")) return simplemail_archive_name;
    if (!strcmp(role, "Trash")) return simplemail_trash_name;
    return role;
}

static int simplemail_box_name_is_role(const char *name, const char *role) {
    return name && role && strcmp(name, simplemail_role_box(role)) == 0;
}

static const char *simplemail_box_name_at(size_t i) {
    switch (i) {
    case 0: return simplemail_inbox_name;
    case 1: return simplemail_sent_name;
    case 2: return simplemail_drafts_name;
    case 3: return simplemail_archive_name;
    case 4: return simplemail_trash_name;
    default: return "";
    }
}

static int ensure_dir_checked(const char *path) {
    struct stat st;

    if (!path || !*path) return 0;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    if (mkdir(path, 0700) == 0) return 1;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_p_checked(const char *path) {
    char tmp[PATH_MAX];
    size_t len;

    if (!path || !*path) return 0;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len >= sizeof tmp) return 0;
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!ensure_dir_checked(tmp)) return 0;
            *p = '/';
        }
    }

    return ensure_dir_checked(tmp);
}

static int dir_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int simplemail_snprintf_ok(int n, size_t outsz) {
    return n >= 0 && (size_t)n < outsz;
}

static int simplemail_is_absolute_path(const char *path) {
    return path && path[0] == '/';
}

static void simplemail_strip_trailing_slashes(char *path) {
    size_t len;

    if (!path) return;
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
}

static int simplemail_home_path(const char *suffix, char *out, size_t outsz) {
    const char *home = getenv("HOME");

    if (!out || outsz == 0) return 0;
    out[0] = '\0';
    if (!home || !*home) return 0;

    if (!suffix || !*suffix)
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s", home), outsz);

    if (suffix[0] == '/')
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s%s", home, suffix), outsz);

    return simplemail_snprintf_ok(snprintf(out, outsz, "%s/%s", home, suffix), outsz);
}

static int simplemail_expand_home_vars(const char *in, char *out, size_t outsz) {
    const char *home = getenv("HOME");

    if (!out || outsz == 0) return 0;
    out[0] = '\0';
    if (!in || !*in) return 0;

    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        if (!home || !*home) return 0;
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s%s", home, in + 1), outsz);
    }

    if (!strncmp(in, "$HOME", 5) && (in[5] == '/' || in[5] == '\0')) {
        if (!home || !*home) return 0;
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s%s", home, in + 5), outsz);
    }

    return simplemail_snprintf_ok(snprintf(out, outsz, "%s", in), outsz);
}

static int simplemail_join_path(const char *base, const char *leaf, char *out, size_t outsz) {
    char b[PATH_MAX];

    if (!base || !*base || !leaf || !*leaf || !out || outsz == 0) return 0;
    if (!simplemail_snprintf_ok(snprintf(b, sizeof b, "%s", base), sizeof b)) return 0;
    simplemail_strip_trailing_slashes(b);

    if (!strcmp(b, "/"))
        return simplemail_snprintf_ok(snprintf(out, outsz, "/%s", leaf), outsz);

    return simplemail_snprintf_ok(snprintf(out, outsz, "%s/%s", b, leaf), outsz);
}

static int simplemail_normalize_configured_path(const char *path,
                                                const char *relative_base,
                                                char *out,
                                                size_t outsz) {
    char expanded[PATH_MAX];
    char base[PATH_MAX];

    if (!simplemail_expand_home_vars(path, expanded, sizeof expanded))
        return 0;

    if (simplemail_is_absolute_path(expanded))
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s", expanded), outsz);

    if (!relative_base || !*relative_base)
        return 0;

    if (!simplemail_expand_home_vars(relative_base, base, sizeof base))
        return 0;

    if (!simplemail_is_absolute_path(base))
        return 0;

    return simplemail_join_path(base, expanded, out, outsz);
}

static int simplemail_xdg_or_home_base(const char *env_name,
                                       const char *home_suffix,
                                       char *out,
                                       size_t outsz) {
    const char *xdg = env_name ? getenv(env_name) : NULL;
    char expanded[PATH_MAX];

    if (!out || outsz == 0) return 0;
    out[0] = '\0';

    if (xdg && *xdg &&
        simplemail_expand_home_vars(xdg, expanded, sizeof expanded) &&
        simplemail_is_absolute_path(expanded))
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s", expanded), outsz);

    return simplemail_home_path(home_suffix, out, outsz);
}

static int simplemail_path_is_within(const char *base, const char *path) {
    char b[PATH_MAX];
    char p[PATH_MAX];
    size_t blen;

    if (!base || !*base || !path || !*path) return 0;
    if (!simplemail_snprintf_ok(snprintf(b, sizeof b, "%s", base), sizeof b)) return 0;
    if (!simplemail_snprintf_ok(snprintf(p, sizeof p, "%s", path), sizeof p)) return 0;
    simplemail_strip_trailing_slashes(b);
    simplemail_strip_trailing_slashes(p);

    if (!strcmp(b, "/"))
        return p[0] == '/';

    blen = strlen(b);
    return !strcmp(b, p) || (!strncmp(b, p, blen) && p[blen] == '/');
}

static int simplemail_regular_file(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int simplemail_source_marker_dir(const char *dir) {
    char path[PATH_MAX];

    if (!dir || !*dir) return 0;
    if (!simplemail_snprintf_ok(snprintf(path, sizeof path, "%s/simplemail.c", dir), sizeof path))
        return 0;
    return simplemail_regular_file(path);
}

static int simplemail_executable_dir(char *out, size_t outsz) {
    char exe[PATH_MAX];
    char *slash;

    if (!out || outsz == 0) return 0;
    out[0] = '\0';

#ifdef __APPLE__
    uint32_t exe_size = (uint32_t)sizeof exe;
    if (_NSGetExecutablePath(exe, &exe_size) != 0 || exe[0] != '/')
        return 0;
#else
    ssize_t n;
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = '\0';
#endif

    slash = strrchr(exe, '/');
    if (!slash) return 0;
    if (slash == exe)
        slash[1] = '\0';
    else
        *slash = '\0';

    return simplemail_snprintf_ok(snprintf(out, outsz, "%s", exe), outsz);
}

static int simplemail_maildir_is_unsafe(const char *path, char *reason, size_t reasonsz) {
    char source_root[PATH_MAX];
    char cwd[PATH_MAX];
    char exe_dir[PATH_MAX];

    if (!path || !*path) return 1;

    if (simplemail_home_path("simplesuite", source_root, sizeof source_root) &&
        simplemail_path_is_within(source_root, path)) {
        snprintf(reason, reasonsz, "it is inside ~/simplesuite");
        return 1;
    }

    if (getcwd(cwd, sizeof cwd) && simplemail_source_marker_dir(cwd) &&
        simplemail_path_is_within(cwd, path)) {
        snprintf(reason, reasonsz, "it is inside the SimpleSuite source tree");
        return 1;
    }

    if (simplemail_executable_dir(exe_dir, sizeof exe_dir) &&
        strcmp(exe_dir, "/tmp") &&
        strcmp(exe_dir, "/var/tmp") &&
        simplemail_path_is_within(exe_dir, path)) {
        snprintf(reason, reasonsz, "it is inside the executable directory");
        return 1;
    }

    return 0;
}

static int simplemail_parent_dir(const char *path, char *out, size_t outsz) {
    char *slash;

    if (!path || !*path || !out || outsz == 0) return 0;
    if (!simplemail_snprintf_ok(snprintf(out, outsz, "%s", path), outsz)) return 0;

    slash = strrchr(out, '/');
    if (!slash) return 0;
    if (slash == out)
        slash[1] = '\0';
    else
        *slash = '\0';

    return 1;
}

static int simplemail_copy_file_for_migration(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return 0;

    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) {
        close(in);
        return 0;
    }

    char buf[8192];
    int ok = 1;

    while (1) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) {
            ok = 0;
            break;
        }
        if (n == 0)
            break;

        char *p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w <= 0) {
                ok = 0;
                break;
            }
            p += w;
            n -= w;
        }

        if (!ok) break;
    }

    if (close(out) != 0) ok = 0;
    close(in);

    if (!ok) {
        unlink(dst);
        return 0;
    }

    return 1;
}

static int simplemail_move_file_for_migration(const char *src, const char *dst) {
    char parent[PATH_MAX];

    if (!simplemail_regular_file(src)) return 0;
    if (simplemail_regular_file(dst)) return 0;

    if (!simplemail_parent_dir(dst, parent, sizeof parent) || !mkdir_p_checked(parent))
        return 0;

    if (rename(src, dst) == 0)
        return 1;

    if (simplemail_copy_file_for_migration(src, dst)) {
        unlink(src);
        return 1;
    }

    return 0;
}

static int simplemail_append_log_for_migration(const char *src, const char *dst) {
    char parent[PATH_MAX];
    struct stat dst_st;
    FILE *in;
    FILE *out;
    char buf[8192];
    size_t n;
    int ok = 1;

    if (!simplemail_regular_file(src)) return 0;
    if (!simplemail_parent_dir(dst, parent, sizeof parent) || !mkdir_p_checked(parent))
        return 0;

    in = fopen(src, "rb");
    if (!in) return 0;

    out = fopen(dst, "ab");
    if (!out) {
        fclose(in);
        return 0;
    }

    if (stat(dst, &dst_st) == 0 && S_ISREG(dst_st.st_mode) && dst_st.st_size > 0)
        fputc('\n', out);

    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }

    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;

    if (ok)
        unlink(src);

    return ok;
}

static void simplemail_default_maildir(char *out, size_t outsz) {
    if (!simplemail_home_path("Mail", out, outsz) && out && outsz)
        out[0] = '\0';
}

static void simplemail_legacy_maildir(char *out, size_t outsz) {
    if (!simplemail_home_path(".local/share/simplemail/mail", out, outsz) && out && outsz)
        out[0] = '\0';
}

static int simplemail_path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int maildir_has_mailboxes(const char *root) {
    char p[PATH_MAX];
    const char *boxes[] = { "Inbox", "INBOX", "Maildir", NULL };

    for (int i = 0; boxes[i]; i++) {
        snprintf(p, sizeof p, "%s/%s/new", root, boxes[i]);
        if (simplemail_path_is_dir(p)) return 1;
        snprintf(p, sizeof p, "%s/%s/cur", root, boxes[i]);
        if (simplemail_path_is_dir(p)) return 1;
    }

    snprintf(p, sizeof p, "%s/new", root);
    if (simplemail_path_is_dir(p)) return 1;
    snprintf(p, sizeof p, "%s/cur", root);
    if (simplemail_path_is_dir(p)) return 1;

    return 0;
}

static int resolve_simplemail_maildir(char *out, size_t outsz) {
    char reason[256] = "";

    if (simplemail_maildir_explicit && simplemail_maildir[0]) {
        char base[PATH_MAX];
        char resolved[PATH_MAX];

        if (!simplemail_home_path("", base, sizeof base) &&
            !simplemail_is_absolute_path(simplemail_maildir)) {
            fprintf(stderr, "simplemail: relative maildir requires HOME: %s\n", simplemail_maildir);
            return 0;
        }

        if (!simplemail_home_path("", base, sizeof base))
            base[0] = '\0';

        if (!simplemail_normalize_configured_path(simplemail_maildir, base, resolved, sizeof resolved)) {
            fprintf(stderr, "simplemail: could not resolve configured maildir: %s\n", simplemail_maildir);
            return 0;
        }

        simplemail_strip_trailing_slashes(resolved);

        if (simplemail_maildir_is_unsafe(resolved, reason, sizeof reason)) {
            fprintf(stderr, "simplemail: refusing maildir %s because %s\n", resolved, reason);
            return 0;
        }

        return simplemail_snprintf_ok(snprintf(out, outsz, "%s", resolved), outsz);
    }

    char local[PATH_MAX], home_mail[PATH_MAX];
    simplemail_legacy_maildir(local, sizeof local);
    simplemail_default_maildir(home_mail, sizeof home_mail);

    if (!local[0] || !home_mail[0]) {
        fprintf(stderr, "simplemail: HOME is required for the default maildir\n");
        return 0;
    }

    if (maildir_has_mailboxes(local)) {
        if (simplemail_maildir_is_unsafe(local, reason, sizeof reason)) {
            fprintf(stderr, "simplemail: refusing maildir %s because %s\n", local, reason);
            return 0;
        }
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s", local), outsz);
    }

    if (maildir_has_mailboxes(home_mail)) {
        if (simplemail_maildir_is_unsafe(home_mail, reason, sizeof reason)) {
            fprintf(stderr, "simplemail: refusing maildir %s because %s\n", home_mail, reason);
            return 0;
        }
        return simplemail_snprintf_ok(snprintf(out, outsz, "%s", home_mail), outsz);
    }

    if (simplemail_maildir_is_unsafe(local, reason, sizeof reason)) {
        fprintf(stderr, "simplemail: refusing maildir %s because %s\n", local, reason);
        return 0;
    }

    return simplemail_snprintf_ok(snprintf(out, outsz, "%s", local), outsz);
}

static void simplemail_migrate_legacy_cwd_cache(const char *cache_dir) {
    static int done = 0;
    char old_attachments[PATH_MAX];
    char new_attachments[PATH_MAX];
    DIR *d;

    if (done) return;
    done = 1;

    snprintf(old_attachments, sizeof old_attachments, ".simplemail-cache/simplemail/attachments");
    if (!dir_exists(old_attachments)) return;

    if (!simplemail_snprintf_ok(snprintf(new_attachments, sizeof new_attachments,
                                         "%s/attachments", cache_dir), sizeof new_attachments))
        return;
    if (!mkdir_p_checked(new_attachments)) return;

    d = opendir(old_attachments);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d))) {
        char src[PATH_MAX];
        char dst[PATH_MAX];

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (!simplemail_snprintf_ok(snprintf(src, sizeof src, "%s/%s",
                                             old_attachments, de->d_name), sizeof src))
            continue;
        if (!simplemail_regular_file(src))
            continue;
        if (!simplemail_snprintf_ok(snprintf(dst, sizeof dst, "%s/%s",
                                             new_attachments, de->d_name), sizeof dst))
            continue;

        simplemail_move_file_for_migration(src, dst);
    }

    closedir(d);
}

static int simplemail_cache_dir(char *out, size_t outsz) {
    char base[PATH_MAX];

    if (!simplemail_xdg_or_home_base("XDG_CACHE_HOME", ".cache", base, sizeof base))
        return 0;

    if (!mkdir_p_checked(base)) return 0;
    if (!simplemail_snprintf_ok(snprintf(out, outsz, "%s/simplemail", base), outsz))
        return 0;
    if (!mkdir_p_checked(out)) return 0;

    simplemail_migrate_legacy_cwd_cache(out);
    return 1;
}

static int simplemail_attachment_dir(char *out, size_t outsz) {
    char cache[PATH_MAX];

    if (!simplemail_cache_dir(cache, sizeof cache)) return 0;
    if (!simplemail_join_path(cache, "attachments", out, outsz)) return 0;
    return ensure_dir_checked(out);
}

static int simplemail_state_dir(char *out, size_t outsz) {
    char base[PATH_MAX];

    if (!simplemail_home_path(".local/state", base, sizeof base))
        return 0;

    if (!mkdir_p_checked(base)) return 0;
    if (!simplemail_snprintf_ok(snprintf(out, outsz, "%s/simplemail", base), outsz))
        return 0;
    return mkdir_p_checked(out);
}

static void shell_quote_append(char *dst, size_t dstsz, const char *src) {
    strncat(dst, "'", dstsz - strlen(dst) - 1);
    for (const char *s = src ? src : ""; *s; s++) {
        if (*s == '\'')
            strncat(dst, "'\\''", dstsz - strlen(dst) - 1);
        else {
            char one[2] = {*s, '\0'};
            strncat(dst, one, dstsz - strlen(dst) - 1);
        }
    }
    strncat(dst, "'", dstsz - strlen(dst) - 1);
}

static void simplemail_migrate_legacy_pull_logs(const char *new_log) {
    static int done = 0;
    char old[PATH_MAX];
    char cache_base[PATH_MAX];

    if (done) return;
    done = 1;

    if (simplemail_xdg_or_home_base("XDG_CACHE_HOME", ".cache", cache_base, sizeof cache_base)) {
        if (simplemail_snprintf_ok(snprintf(old, sizeof old, "%s/simplemail/pull.log", cache_base), sizeof old))
            simplemail_append_log_for_migration(old, new_log);
    }

    if (simplemail_home_path(".cache/simplemail/pull.log", old, sizeof old))
        simplemail_append_log_for_migration(old, new_log);

    simplemail_append_log_for_migration(".simplemail-cache/simplemail/pull.log", new_log);
    simplemail_append_log_for_migration("simplemail-pull.log", new_log);
}

static int simplemail_pull_log_path(char *out, size_t outsz) {
    char state[PATH_MAX];

    if (!simplemail_state_dir(state, sizeof state)) {
        if (out && outsz) out[0] = '\0';
        return 0;
    }

    if (!simplemail_snprintf_ok(snprintf(out, outsz, "%s/pull.log", state), outsz))
        return 0;

    simplemail_migrate_legacy_pull_logs(out);
    return 1;
}

static int simplemail_config_dir(char *out, size_t outsz) {
    char base[PATH_MAX];

    if (!simplemail_xdg_or_home_base("XDG_CONFIG_HOME", ".config", base, sizeof base))
        return 0;

    return simplemail_snprintf_ok(snprintf(out, outsz, "%s/simplemail", base), outsz);
}

static void write_default_simplemail_config(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f,
        "# SimpleMail is a front end to your existing mail tools.\n"
        "# Configure accounts in ~/.mbsyncrc and ~/.msmtprc.\n"
        "#\n"
        "# Maildir precedence: uncommented maildir, then SIMPLEMAIL_MAILDIR,\n"
        "# then existing ~/.local/share/simplemail/mail, then existing ~/Mail,\n"
        "# then new ~/.local/share/simplemail/mail.\n"
        "# Relative maildir values are resolved from $HOME.\n"
        "# maildir=~/Mail\n"
        "inbox=Inbox\n"
        "sent=Sent\n"
        "drafts=Drafts\n"
        "archive=Archive\n"
        "trash=Trash\n"
        "\n"
        "sync_cmd=mbsync inbox\n"
        "send_cmd=msmtp -t\n"
        "editor=simplewords\n"
        "# from=Your Name <you@example.com>\n"
        "#\n"
        "# Proton Bridge example, if your account is named proton:\n"
        "# sync_cmd=mbsync proton\n"
        "# send_cmd=msmtp -a proton -t\n"
        "\n");

    fclose(f);
}

static void load_simplemail_config(void) {
    char dir[PATH_MAX];
    char path[PATH_MAX];

    FILE *f = NULL;
    if (simplemail_config_dir(dir, sizeof dir) && mkdir_p_checked(dir)) {
        if (simplemail_join_path(dir, "config", path, sizeof path)) {
            write_default_simplemail_config(path);
            f = fopen(path, "r");
        }
    }

    if (f) {
        char line[MAX_LINE];
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            trim(s);
            if (!s[0] || s[0] == '#') continue;

            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';

            char *key = s;
            char *val = eq + 1;
            trim(key);
            trim(val);

            if (!strcmp(key, "maildir")) {
                config_copy_path(simplemail_maildir, sizeof simplemail_maildir, val);
                if (simplemail_maildir[0])
                    simplemail_maildir_explicit = 1;
            }
            else if (!strcmp(key, "inbox"))
                config_copy(simplemail_inbox_name, sizeof simplemail_inbox_name, val);
            else if (!strcmp(key, "sent"))
                config_copy(simplemail_sent_name, sizeof simplemail_sent_name, val);
            else if (!strcmp(key, "drafts"))
                config_copy(simplemail_drafts_name, sizeof simplemail_drafts_name, val);
            else if (!strcmp(key, "archive"))
                config_copy(simplemail_archive_name, sizeof simplemail_archive_name, val);
            else if (!strcmp(key, "trash"))
                config_copy(simplemail_trash_name, sizeof simplemail_trash_name, val);
            else if (!strcmp(key, "sync_cmd"))
                config_copy(simplemail_sync_cmd, sizeof simplemail_sync_cmd, val);
            else if (!strcmp(key, "send_cmd"))
                config_copy(simplemail_send_cmd, sizeof simplemail_send_cmd, val);
            else if (!strcmp(key, "send")) {
                char send[512] = "";
                config_copy(send, sizeof send, val);
                if (!strcmp(send, "msmtp"))
                    snprintf(simplemail_send_cmd, sizeof simplemail_send_cmd, "msmtp -t");
                else if (send[0])
                    snprintf(simplemail_send_cmd, sizeof simplemail_send_cmd, "%s", send);
            }
            else if (!strcmp(key, "editor"))
                config_copy(simplemail_editor_cmd, sizeof simplemail_editor_cmd, val);
            else if (!strcmp(key, "from"))
                config_copy(simplemail_from, sizeof simplemail_from, val);
        }
        fclose(f);
    }

    const char *env;

    env = getenv("SIMPLEMAIL_SYNC_CMD");
    if (env && *env) config_copy(simplemail_sync_cmd, sizeof simplemail_sync_cmd, env);

    env = getenv("SIMPLEMAIL_SEND_CMD");
    if (env && *env) config_copy(simplemail_send_cmd, sizeof simplemail_send_cmd, env);

    env = getenv("SIMPLEMAIL_EDITOR");
    if (env && *env) config_copy(simplemail_editor_cmd, sizeof simplemail_editor_cmd, env);

    env = getenv("SIMPLEMAIL_FROM");
    if (env && *env) config_copy(simplemail_from, sizeof simplemail_from, env);

    env = getenv("SIMPLEMAIL_MAILDIR");
    if (env && *env && !simplemail_maildir_explicit) {
        config_copy_path(simplemail_maildir, sizeof simplemail_maildir, env);
        if (simplemail_maildir[0])
            simplemail_maildir_explicit = 1;
    }
}

static int starts_case(const char *s, const char *prefix) {
    while (*prefix && *s) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

static void copy_field(char *dst, size_t dstsz, const char *src) {
    if (!src || !dstsz) return;
    snprintf(dst, dstsz, "%s", src);
    trim(dst);
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    mkdir(path, 0700);
}

static void make_maildir(const char *base) {
    ensure_dir(base);

    char p[PATH_MAX];
    if (simplemail_join_path(base, "cur", p, sizeof p)) ensure_dir(p);
    if (simplemail_join_path(base, "new", p, sizeof p)) ensure_dir(p);
    if (simplemail_join_path(base, "tmp", p, sizeof p)) ensure_dir(p);
}

static int init_paths(void) {
    if (!resolve_simplemail_maildir(mail_root, sizeof mail_root))
        return 0;

    char p[PATH_MAX];
    if (!mkdir_p_checked(mail_root)) {
        fprintf(stderr, "simplemail: could not create maildir %s\n", mail_root);
        return 0;
    }

    for (size_t i = 0; i < 5; i++) {
        if (!simplemail_join_path(mail_root, simplemail_box_name_at(i), p, sizeof p)) {
            fprintf(stderr, "simplemail: mailbox path is too long\n");
            return 0;
        }
        make_maildir(p);
    }

    return 1;
}

static void init_mailboxes(void) {
    mailbox_count = 0;

    for (size_t i = 0; i < 5 && mailbox_count < MAX_MAILBOXES; i++) {
        snprintf(mailboxes[mailbox_count].name, sizeof mailboxes[mailbox_count].name, "%s", simplemail_box_name_at(i));
        if (!simplemail_join_path(mail_root, simplemail_box_name_at(i),
                                  mailboxes[mailbox_count].path,
                                  sizeof mailboxes[mailbox_count].path))
            continue;
        mailbox_count++;
    }
}

static int path_is_regular(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void append_body(char **body, size_t *used, size_t *cap, const char *line) {
    size_t len = strlen(line);
    if (*used + len + 2 >= *cap) {
        size_t newcap = *cap ? *cap * 2 : 8192;
        while (*used + len + 2 >= newcap) newcap *= 2;
        if (newcap > MAX_BODY) newcap = MAX_BODY;
        if (newcap <= *cap) return;
        char *nb = realloc(*body, newcap);
        if (!nb) return;
        *body = nb;
        *cap = newcap;
    }
    memcpy(*body + *used, line, len);
    *used += len;
    (*body)[*used] = '\0';
}

static int ci_char_eq(char a, char b) {
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static const char *find_case(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return haystack;
    for (const char *h = haystack; *h; h++) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b && ci_char_eq(*a, *b)) {
            a++;
            b++;
        }
        if (!*b) return h;
    }
    return NULL;
}

static int span_case_equal(const char *s, size_t len, const char *word)
{
    size_t word_len = strlen(word ? word : "");

    if (!s || len != word_len)
        return 0;

    for (size_t i = 0; i < len; i++)
        if (!ci_char_eq(s[i], word[i]))
            return 0;

    return 1;
}

/*
 * Read an exact MIME parameter name.  A substring search is not sufficient:
 * "name" must not accidentally match the tail of "filename", and extended
 * RFC 2231 parameters such as filename*= need to remain distinguishable.
 */
static int extract_mime_parameter(const char *s, const char *name,
                                  char *out, size_t outsz)
{
    const char *p = s ? s : "";

    if (!name || !*name || !out || outsz == 0)
        return 0;
    out[0] = '\0';

    while ((p = strchr(p, ';')) != NULL) {
        const char *key;
        const char *value;
        size_t key_len;
        size_t used = 0;
        char quote = 0;

        p++;
        while (*p && isspace((unsigned char)*p))
            p++;
        key = p;
        while (*p && *p != '=' && *p != ';' &&
               !isspace((unsigned char)*p))
            p++;
        key_len = (size_t)(p - key);
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p != '=')
            continue;
        p++;
        while (*p && isspace((unsigned char)*p))
            p++;

        value = p;
        if (*value == '"' || *value == '\'') {
            quote = *value++;
            p = value;
        }

        while (*p && used + 1 < outsz) {
            if (quote) {
                if (*p == quote)
                    break;
                if (*p == '\\' && p[1])
                    p++;
            } else if (*p == ';' || *p == '\r' || *p == '\n') {
                break;
            }
            out[used++] = *p++;
        }
        out[used] = '\0';

        while (used > 0 && isspace((unsigned char)out[used - 1]))
            out[--used] = '\0';

        if (span_case_equal(key, key_len, name))
            return out[0] != '\0';

        out[0] = '\0';
        if (quote) {
            while (*p && *p != quote)
                p++;
            if (*p == quote)
                p++;
        }
    }

    return 0;
}

static int b64_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *decode_base64_text(const char *src) {
    if (!src) return strdup("");

    size_t len = strlen(src);
    char *out = malloc(len + 4);
    if (!out) return strdup("");

    size_t used = 0;
    int vals[4];
    int n = 0;

    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (isspace(*p)) continue;

        if (*p == '=') {
            vals[n++] = -2;
        } else {
            int v = b64_value(*p);
            if (v < 0) continue;
            vals[n++] = v;
        }

        if (n == 4) {
            if (vals[0] >= 0 && vals[1] >= 0)
                out[used++] = (char)((vals[0] << 2) | (vals[1] >> 4));

            if (vals[2] >= 0)
                out[used++] = (char)(((vals[1] & 15) << 4) | (vals[2] >> 2));

            if (vals[3] >= 0)
                out[used++] = (char)(((vals[2] & 3) << 6) | vals[3]);

            n = 0;
        }
    }

    out[used] = '\0';
    return out;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static char *decode_quoted_printable_text(const char *src) {
    if (!src) return strdup("");

    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (!out) return strdup("");

    size_t used = 0;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '=' && i + 1 < len) {
            if (src[i + 1] == '\n') {
                i += 1;
                continue;
            }
            if (src[i + 1] == '\r' && i + 2 < len && src[i + 2] == '\n') {
                i += 2;
                continue;
            }
            if (i + 2 < len) {
                int a = hexval((unsigned char)src[i + 1]);
                int b = hexval((unsigned char)src[i + 2]);
                if (a >= 0 && b >= 0) {
                    out[used++] = (char)((a << 4) | b);
                    i += 2;
                    continue;
                }
            }
        }

        out[used++] = src[i];
    }

    out[used] = '\0';
    return out;
}

static void append_char(char **out, size_t *used, size_t *cap, char c);
static void append_text(char **out, size_t *used, size_t *cap, const char *text);

static int utf8_bytes_are_valid(const unsigned char *s)
{
    while (s && *s) {
        unsigned int cp;
        int need;

        if (*s < 0x80) {
            s++;
            continue;
        }
        if ((*s & 0xE0) == 0xC0) {
            cp = *s & 0x1F;
            need = 1;
            if (cp < 2)
                return 0;
        } else if ((*s & 0xF0) == 0xE0) {
            cp = *s & 0x0F;
            need = 2;
        } else if ((*s & 0xF8) == 0xF0) {
            cp = *s & 0x07;
            need = 3;
        } else {
            return 0;
        }

        s++;
        for (int i = 0; i < need; i++) {
            if ((s[i] & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        if ((need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            return 0;
        s += need;
    }
    return 1;
}

static int charset_is_utf8_or_ascii(const char *charset)
{
    return !charset || !*charset ||
           !strcasecmp(charset, "utf-8") ||
           !strcasecmp(charset, "utf8") ||
           !strcasecmp(charset, "us-ascii") ||
           !strcasecmp(charset, "ascii");
}

/* Convert declared MIME text to the UTF-8 used by the terminal renderer. */
static char *convert_charset_to_utf8(const char *src, const char *charset)
{
    iconv_t cd;
    const char *from = charset;
    char *out;
    char *write;
    char *input;
    size_t inleft;
    size_t outleft;
    size_t cap;

    if (!src)
        return strdup("");

    if (charset_is_utf8_or_ascii(charset) &&
        utf8_bytes_are_valid((const unsigned char *)src))
        return strdup(src);

    /* Undeclared legacy mail is overwhelmingly Windows-1252 in practice. */
    if (!from || !*from || !strcasecmp(from, "us-ascii") ||
        !strcasecmp(from, "ascii"))
        from = "WINDOWS-1252";

    cd = iconv_open("UTF-8", from);
    if (cd == (iconv_t)-1 && strcasecmp(from, "WINDOWS-1252"))
        cd = iconv_open("UTF-8", "WINDOWS-1252");
    if (cd == (iconv_t)-1)
        return strdup(src);

    inleft = strlen(src);
    cap = inleft > (MAX_BODY - 32) / 4 ? MAX_BODY + 1 : inleft * 4 + 32;
    if (cap < 64)
        cap = 64;
    out = malloc(cap);
    if (!out) {
        iconv_close(cd);
        return strdup(src);
    }

    input = (char *)src;
    write = out;
    outleft = cap - 1;

    while (inleft > 0) {
        size_t rc = iconv(cd, &input, &inleft, &write, &outleft);

        if (rc != (size_t)-1)
            break;
        if (errno == E2BIG) {
            size_t used = (size_t)(write - out);
            size_t grown = cap > (MAX_BODY + 1) / 2 ? MAX_BODY + 1 : cap * 2;
            char *next;

            if (grown <= cap)
                break;
            next = realloc(out, grown);
            if (!next)
                break;
            out = next;
            cap = grown;
            write = out + used;
            outleft = cap - used - 1;
            continue;
        }

        /* Keep the rest of a damaged message readable and visibly mark loss. */
        if ((errno == EILSEQ || errno == EINVAL) && outleft >= 3) {
            *write++ = (char)0xEF;
            *write++ = (char)0xBF;
            *write++ = (char)0xBD;
            outleft -= 3;
            input++;
            inleft--;
            iconv(cd, NULL, NULL, NULL, NULL);
            continue;
        }
        break;
    }

    *write = '\0';
    iconv_close(cd);
    return out;
}

static char *decode_transfer_text(const char *src, const char *cte) {
    if (cte && find_case(cte, "base64"))
        return decode_base64_text(src);
    if (cte && find_case(cte, "quoted-printable"))
        return decode_quoted_printable_text(src);
    return strdup(src ? src : "");
}

static void flowed_flush_line(char **out, size_t *used, size_t *cap,
                              char **logical, size_t *logical_used,
                              int quote_depth, int add_newline)
{
    int shown = quote_depth > 3 ? 3 : quote_depth;

    if (!logical || !*logical)
        return;
    for (int i = 0; i < shown; i++)
        append_char(out, used, cap, '>');
    if (shown > 0)
        append_char(out, used, cap, ' ');
    append_text(out, used, cap, *logical);
    if (add_newline)
        append_char(out, used, cap, '\n');
    (*logical)[0] = '\0';
    *logical_used = 0;
}

/* RFC 3676 format=flowed, including quote-depth-aware soft wrapping. */
static char *decode_format_flowed(const char *src, int delsp)
{
    char *out = NULL;
    char *logical = NULL;
    size_t used = 0, cap = 0;
    size_t logical_used = 0, logical_cap = 0;
    int logical_depth = 0;
    int logical_flowed = 0;

    for (const char *p = src ? src : ""; ; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *body = p;
        int depth = 0;
        int flowed = 0;
        char *piece;

        if (len > 0 && p[len - 1] == '\r')
            len--;
        while ((size_t)(body - p) < len && *body == '>') {
            depth++;
            body++;
        }
        if ((size_t)(body - p) < len && *body == ' ')
            body++; /* RFC space stuffing */

        len -= (size_t)(body - p);
        piece = malloc(len + 1);
        if (!piece)
            piece = strdup("");
        else {
            memcpy(piece, body, len);
            piece[len] = '\0';
        }

        if (len > 0 && piece[len - 1] == ' ' && strcmp(piece, "-- ")) {
            flowed = 1;
            if (delsp)
                piece[--len] = '\0';
        }

        if (logical_used > 0 && (!logical_flowed || depth != logical_depth))
            flowed_flush_line(&out, &used, &cap, &logical, &logical_used,
                              logical_depth, 1);

        if (len == 0 && !flowed) {
            append_char(&out, &used, &cap, '\n');
            logical_flowed = 0;
            free(piece);
            if (!e)
                break;
            p = e + 1;
            continue;
        }

        if (logical_used == 0)
            logical_depth = depth;
        else if (delsp && logical[logical_used - 1] != ' ' && piece[0] &&
                 ispunct((unsigned char)logical[logical_used - 1]))
            append_char(&logical, &logical_used, &logical_cap, ' ');

        append_text(&logical, &logical_used, &logical_cap, piece);
        logical_flowed = flowed;
        free(piece);

        if (!e)
            break;
        p = e + 1;
    }

    flowed_flush_line(&out, &used, &cap, &logical, &logical_used,
                      logical_depth, 0);
    free(logical);
    return out ? out : strdup("");
}

static char *decode_text_part(const char *src, const char *cte,
                              const char *ctype)
{
    char charset[128] = "";
    char format[64] = "";
    char delsp[32] = "";
    char *decoded = decode_transfer_text(src, cte);
    char *utf8;

    extract_mime_parameter(ctype, "charset", charset, sizeof charset);
    utf8 = convert_charset_to_utf8(decoded, charset);
    free(decoded);

    if (extract_mime_parameter(ctype, "format", format, sizeof format) &&
        !strcasecmp(format, "flowed")) {
        char *flowed = decode_format_flowed(
            utf8,
            extract_mime_parameter(ctype, "delsp", delsp, sizeof delsp) &&
            !strcasecmp(delsp, "yes"));
        free(utf8);
        return flowed;
    }

    return utf8;
}

static char *decode_rfc2047_q(const char *src) {
    char *out = malloc(strlen(src) + 1);
    if (!out) return strdup(src ? src : "");

    size_t j = 0;
    for (size_t i = 0; src[i]; i++) {
        if (src[i] == '_') {
            out[j++] = ' ';
        } else if (src[i] == '=' && src[i + 1] && src[i + 2] &&
                   isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            int a = hexval((unsigned char)src[i+1]);
            int b = hexval((unsigned char)src[i+2]);
            out[j++] = (char)((a << 4) | b);
            i += 2;
        } else {
            out[j++] = src[i];
        }
    }

    out[j] = '\0';
    return out;
}

static char *decode_rfc2047_header(const char *src) {
    if (!src) return strdup("");

    char *out = NULL;
    size_t used = 0, cap = 0;

    for (size_t i = 0; src[i]; ) {
        if (src[i] == '=' && src[i+1] == '?') {
            const char *cs = src + i + 2;
            const char *q1 = strstr(cs, "?");
            if (!q1) goto literal;

            const char *enc = q1 + 1;
            const char *q2 = strstr(enc, "?");
            if (!q2) goto literal;

            const char *data = q2 + 1;
            const char *end = strstr(data, "?=");
            if (!end) goto literal;

            size_t dl = (size_t)(end - data);
            char *chunk = malloc(dl + 1);
            if (!chunk) goto literal;
            memcpy(chunk, data, dl);
            chunk[dl] = '\0';

            char *decoded = NULL;
            if (*enc == 'q' || *enc == 'Q')
                decoded = decode_rfc2047_q(chunk);
            else if (*enc == 'b' || *enc == 'B')
                decoded = decode_base64_text(chunk);

            free(chunk);

            if (decoded) {
                char charset[128];
                size_t charset_len = (size_t)(q1 - cs);
                char *utf8;

                if (charset_len >= sizeof charset)
                    charset_len = sizeof charset - 1;
                memcpy(charset, cs, charset_len);
                charset[charset_len] = '\0';
                utf8 = convert_charset_to_utf8(decoded, charset);
                append_text(&out, &used, &cap, utf8 ? utf8 : decoded);
                free(utf8);
                free(decoded);
                i = (size_t)((end + 2) - src);
                while (src[i] == ' ' && src[i+1] == '=' && src[i+2] == '?')
                    i++;
                continue;
            }
        }

literal:
        append_char(&out, &used, &cap, src[i]);
        i++;
    }

    if (!out) return strdup("");

    char *r = out;
    char *w = out;
    int nl = 0;

    while (*r) {
        if (*r == '\r') {
            r++;
            continue;
        }

        if (*r == '\n') {
            if (nl < 2)
                *w++ = *r;
            nl++;
        } else {
            *w++ = *r;
            nl = 0;
        }

        r++;
    }

    *w = '\0';
    trim(out);
    strip_newsletter_footer(out);
    return out;
}

static void decode_header_field_inplace(char *s, size_t sz) {
    char *d = decode_rfc2047_header(s);
    snprintf(s, sz, "%s", d ? d : "");
    free(d);
}

static char *unfold_headers(const char *src) {
    if (!src) return strdup("");

    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (!out) return strdup("");

    size_t used = 0;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\r')
            continue;

        if (src[i] == '\n' && i + 1 < len &&
            (src[i + 1] == ' ' || src[i + 1] == '\t')) {
            out[used++] = ' ';
            i++;
            while (i + 1 < len && (src[i + 1] == ' ' || src[i + 1] == '\t'))
                i++;
            continue;
        }

        out[used++] = src[i];
    }

    out[used] = '\0';
    return out;
}

static int extract_boundary(const char *ctype, char *out, size_t outsz) {
    const char *b = find_case(ctype, "boundary=");
    if (!b || outsz == 0) return 0;

    b += 9;
    while (*b && isspace((unsigned char)*b)) b++;

    char quote = 0;
    if (*b == '"' || *b == '\'') {
        quote = *b;
        b++;
    }

    size_t n = 0;
    while (*b && n + 1 < outsz) {
        if (quote) {
            if (*b == quote) break;
        } else {
            if (*b == ';' || *b == '\r' || *b == '\n' || isspace((unsigned char)*b)) break;
        }
        out[n++] = *b++;
    }

    out[n] = '\0';
    return n > 0;
}

static void parse_part_headers(const char *hdrs, char *ctype, size_t ctypesz, char *cte, size_t ctesz) {
    ctype[0] = '\0';
    cte[0] = '\0';

    char *copy = unfold_headers(hdrs ? hdrs : "");
    if (!copy) return;

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);

    while (line) {
        trim(line);
        if (starts_case(line, "Content-Type:"))
            copy_field(ctype, ctypesz, line + 13);
        else if (starts_case(line, "Content-Transfer-Encoding:"))
            copy_field(cte, ctesz, line + 26);
        else if (starts_case(line, "Content-Disposition:")) {
            if (find_case(line, "attachment")) {
                if (ctype[0])
                    strncat(ctype, "; x-simplemail-attachment", ctypesz - strlen(ctype) - 1);
                else
                    copy_field(ctype, ctypesz, "application/octet-stream; x-simplemail-attachment");
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(copy);
}


static void safe_filename(char *s) {
    if (!s || !*s) return;
    for (char *p = s; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 32)
            *p = '_';
    }
}

static char *percent_decode_mime_value(const char *s)
{
    char *out = malloc(strlen(s ? s : "") + 1);
    size_t used = 0;

    if (!out)
        return strdup(s ? s : "");

    for (size_t i = 0; s && s[i]; i++) {
        if (s[i] == '%' && s[i + 1] && s[i + 2]) {
            int a = hexval((unsigned char)s[i + 1]);
            int b = hexval((unsigned char)s[i + 2]);
            if (a >= 0 && b >= 0) {
                out[used++] = (char)((a << 4) | b);
                i += 2;
                continue;
            }
        }
        out[used++] = s[i];
    }
    out[used] = '\0';
    return out;
}

static char *decode_rfc2231_value(const char *value)
{
    char charset[128] = "utf-8";
    const char *encoded = value ? value : "";
    const char *first = strchr(encoded, '\'');
    char *bytes;
    char *utf8;

    if (first) {
        const char *second = strchr(first + 1, '\'');
        if (second) {
            size_t len = (size_t)(first - encoded);
            if (len >= sizeof charset)
                len = sizeof charset - 1;
            if (len > 0) {
                memcpy(charset, encoded, len);
                charset[len] = '\0';
            }
            encoded = second + 1;
        }
    }

    bytes = percent_decode_mime_value(encoded);
    utf8 = convert_charset_to_utf8(bytes, charset);
    free(bytes);
    return utf8;
}

static int extract_extended_parameter(const char *s, const char *base,
                                      char *out, size_t outsz)
{
    char name[96];
    char raw[2048] = "";

    snprintf(name, sizeof name, "%s*", base);
    if (!extract_mime_parameter(s, name, raw, sizeof raw)) {
        size_t used = 0;
        int found = 0;

        raw[0] = '\0';
        for (int i = 0; i < 20; i++) {
            char piece[512] = "";

            snprintf(name, sizeof name, "%s*%d*", base, i);
            if (!extract_mime_parameter(s, name, piece, sizeof piece)) {
                snprintf(name, sizeof name, "%s*%d", base, i);
                if (!extract_mime_parameter(s, name, piece, sizeof piece))
                    break;
            }
            found = 1;
            size_t n = strlen(piece);
            if (n > sizeof raw - used - 1)
                n = sizeof raw - used - 1;
            memcpy(raw + used, piece, n);
            used += n;
            raw[used] = '\0';
        }
        if (!found)
            return 0;
    }

    char *decoded = decode_rfc2231_value(raw);
    const char *value = decoded ? decoded : "";
    size_t value_len = strlen(value);
    if (value_len >= outsz)
        value_len = outsz - 1;
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    free(decoded);
    safe_filename(out);
    return out[0] != '\0';
}

static int extract_filename_parameter(const char *s, const char *base,
                                      char *out, size_t outsz)
{
    char raw[1024];
    char *decoded;

    if (extract_extended_parameter(s, base, out, outsz))
        return 1;
    if (!extract_mime_parameter(s, base, raw, sizeof raw))
        return 0;

    decoded = decode_rfc2047_header(raw);
    const char *value = decoded ? decoded : raw;
    size_t value_len = strlen(value);
    if (value_len >= outsz)
        value_len = outsz - 1;
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    free(decoded);
    safe_filename(out);
    return out[0] != '\0';
}

static int headers_attachment_filename(const char *hdrs, char *out, size_t outsz) {
    out[0] = '\0';

    char *copy = unfold_headers(hdrs ? hdrs : "");
    if (!copy) return 0;

    int is_attach = 0;
    char ctype[512] = "";
    char cdisp[512] = "";

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);

    while (line) {
        trim(line);
        if (starts_case(line, "Content-Type:"))
            copy_field(ctype, sizeof ctype, line + 13);
        else if (starts_case(line, "Content-Disposition:")) {
            copy_field(cdisp, sizeof cdisp, line + 20);
            if (find_case(cdisp, "attachment")) is_attach = 1;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (!is_attach &&
        !extract_mime_parameter(ctype, "name", out, outsz) &&
        !extract_mime_parameter(ctype, "name*", out, outsz)) {
        out[0] = '\0';
        free(copy);
        return 0;
    }
    out[0] = '\0';

    if (!extract_filename_parameter(cdisp, "filename", out, outsz))
        extract_filename_parameter(ctype, "name", out, outsz);

    if (!out[0]) {
        if (find_case(ctype, "message/rfc822"))
            snprintf(out, outsz, "forwarded-message.eml");
        else if (find_case(ctype, "text/calendar"))
            snprintf(out, outsz, "invitation.ics");
        else
            snprintf(out, outsz, "attachment.bin");
    }

    free(copy);
    return 1;
}

static unsigned char *decode_base64_bytes(const char *src, size_t *outlen) {
    if (outlen) *outlen = 0;
    if (!src) return NULL;

    size_t len = strlen(src);
    unsigned char *out = malloc(len + 4);
    if (!out) return NULL;

    size_t used = 0;
    int vals[4];
    int n = 0;

    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (isspace(*p)) continue;

        if (*p == '=') vals[n++] = -2;
        else {
            int v = b64_value(*p);
            if (v < 0) continue;
            vals[n++] = v;
        }

        if (n == 4) {
            if (vals[0] >= 0 && vals[1] >= 0)
                out[used++] = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
            if (vals[2] >= 0)
                out[used++] = (unsigned char)(((vals[1] & 15) << 4) | (vals[2] >> 2));
            if (vals[3] >= 0)
                out[used++] = (unsigned char)(((vals[2] & 3) << 6) | vals[3]);
            n = 0;
        }
    }

    if (outlen) *outlen = used;
    return out;
}

static void save_attachment_part(Message *m, const char *filename, const char *payload, const char *cte) {
    if (!m || !filename || !*filename || m->has_attachment) return;

    char dir[PATH_MAX];
    if (!simplemail_attachment_dir(dir, sizeof dir)) return;

    snprintf(m->attachment_name, sizeof m->attachment_name, "%s", filename);
    safe_filename(m->attachment_name);

    char tmpl[PATH_MAX];
    if (!simplemail_join_path(dir, "attachment-XXXXXX", tmpl, sizeof tmpl)) {
        m->attachment_name[0] = '\0';
        return;
    }

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        m->attachment_path[0] = '\0';
        m->attachment_name[0] = '\0';
        return;
    }

    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmpl);
        m->attachment_path[0] = '\0';
        m->attachment_name[0] = '\0';
        return;
    }

    snprintf(m->attachment_path, sizeof m->attachment_path, "%s", tmpl);

    if (cte && find_case(cte, "base64")) {
        size_t n = 0;
        unsigned char *bytes = decode_base64_bytes(payload, &n);
        if (bytes && n) fwrite(bytes, 1, n, f);
        free(bytes);
    } else {
        fwrite(payload, 1, strlen(payload), f);
    }

    fclose(f);
    m->has_attachment = 1;
}



static void strip_html_comments_inplace(char *s) {
    if (!s) return;

    char *read = s;
    char *write = s;

    while (*read) {
        if (read[0] == '<' && read[1] == '!' && read[2] == '-' && read[3] == '-') {
            char *end = strstr(read + 4, "-->");
            if (end) {
                read = end + 3;
                while (*read && isspace((unsigned char)*read))
                    read++;
                continue;
            }
        }

        *write++ = *read++;
    }

    *write = '\0';
    trim(s);
}



static void append_char(char **out, size_t *used, size_t *cap, char c) {
    if (*used + 2 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 4096;
        if (nc < *cap || nc > MAX_BODY + 1)
            nc = MAX_BODY + 1;
        if (*used + 2 >= nc)
            return;
        char *p = realloc(*out, nc);
        if (!p) return;
        *out = p;
        *cap = nc;
    }
    (*out)[(*used)++] = c;
    (*out)[*used] = '\0';
}

static void append_text(char **out, size_t *used, size_t *cap, const char *text) {
    if (!text) return;
    while (*text) append_char(out, used, cap, *text++);
}

static void append_utf8_codepoint(char **out, size_t *used, size_t *cap, unsigned int cp) {
    if (cp == 0 || cp == 173 || cp == 847)
        return;

    if (cp == 160 || cp == 8199) {
        append_char(out, used, cap, ' ');
        return;
    }

    if (cp < 128) {
        append_char(out, used, cap, (char)cp);
        return;
    }

    char b[5] = {0};
    if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
    }
    append_text(out, used, cap, b);
}

static int consume_html_entity(const char *s, char **out, size_t *used, size_t *cap, size_t *advance) {
    static const struct {
        const char *name;
        unsigned int codepoint;
    } named[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'},
        {"apos", '\''}, {"nbsp", 0x00A0}, {"ensp", 0x2002},
        {"emsp", 0x2003}, {"thinsp", 0x2009}, {"shy", 0x00AD},
        {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
        {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0x00B7},
        {"copy", 0x00A9}, {"reg", 0x00AE}, {"trade", 0x2122},
        {"euro", 0x20AC}, {"pound", 0x00A3}, {"yen", 0x00A5},
        {"cent", 0x00A2}, {"laquo", 0x00AB}, {"raquo", 0x00BB},
        {"times", 0x00D7}, {"divide", 0x00F7}, {"plusmn", 0x00B1},
        {"deg", 0x00B0}, {"zwnj", 0x200C}, {"zwj", 0x200D},
        {"lrm", 0x200E}, {"rlm", 0x200F},
        {"ZeroWidthSpace", 0x200B}
    };

    *advance = 0;

    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        size_t len = strlen(named[i].name);
        if (s[0] == '&' && !strncmp(s + 1, named[i].name, len) &&
            s[len + 1] == ';') {
            if (named[i].codepoint == 0x00A0 ||
                named[i].codepoint == 0x2002 ||
                named[i].codepoint == 0x2003 ||
                named[i].codepoint == 0x2009)
                append_char(out, used, cap, ' ');
            else
                append_utf8_codepoint(out, used, cap, named[i].codepoint);
            *advance = len + 2;
            return 1;
        }
    }

    if (s[0] == '&' && s[1] == '#') {
        const char *p = s + 2;
        int base = 10;
        unsigned int cp = 0;

        if (*p == 'x' || *p == 'X') {
            base = 16;
            p++;
        }

        while (*p && *p != ';') {
            int v = -1;
            if (*p >= '0' && *p <= '9') v = *p - '0';
            else if (base == 16 && *p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
            else if (base == 16 && *p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
            else return 0;

            if (v >= base) return 0;
            cp = cp * (unsigned int)base + (unsigned int)v;
            p++;
        }

        if (*p == ';') {
            append_utf8_codepoint(out, used, cap, cp);
            *advance = (size_t)(p - s + 1);
            return 1;
        }
    }

    return 0;
}


static int extract_html_attribute(const char *tag, const char *name,
                                  char *out, size_t outsz)
{
    const char *p = tag ? tag : "";

    if (!name || !*name || !out || outsz == 0)
        return 0;
    out[0] = '\0';

    while (*p && !isspace((unsigned char)*p))
        p++;

    while (*p) {
        const char *key;
        size_t key_len;
        char quote = 0;
        size_t used = 0;

        while (*p && (isspace((unsigned char)*p) || *p == '/'))
            p++;
        if (!*p)
            break;
        key = p;
        while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '/')
            p++;
        key_len = (size_t)(p - key);
        while (*p && isspace((unsigned char)*p))
            p++;

        if (*p != '=') {
            if (span_case_equal(key, key_len, name)) {
                snprintf(out, outsz, "true");
                return 1;
            }
            continue;
        }
        p++;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '"' || *p == '\'')
            quote = *p++;

        while (*p && used + 1 < outsz) {
            if (quote ? *p == quote : isspace((unsigned char)*p))
                break;
            out[used++] = *p++;
        }
        out[used] = '\0';

        if (span_case_equal(key, key_len, name))
            return 1;

        if (quote && *p == quote)
            p++;
    }
    return 0;
}

static int html_tag_is_hidden(const char *tag)
{
    char value[1024];
    char compact[1024];
    size_t used = 0;

    if (extract_html_attribute(tag, "hidden", value, sizeof value) ||
        (extract_html_attribute(tag, "aria-hidden", value, sizeof value) &&
         !strcasecmp(value, "true")))
        return 1;

    if (extract_html_attribute(tag, "class", value, sizeof value)) {
        if (find_case(value, "preheader") ||
            find_case(value, "preview-text") ||
            find_case(value, "preview_text") ||
            find_case(value, "email-preview") ||
            find_case(value, "mcnpreviewtext"))
            return 1;
    }
    if (extract_html_attribute(tag, "id", value, sizeof value) &&
        (find_case(value, "preheader") ||
         find_case(value, "preview-text") ||
         find_case(value, "preview_text") ||
         find_case(value, "email-preview") ||
         find_case(value, "mcnpreviewtext")))
        return 1;

    if (!extract_html_attribute(tag, "style", value, sizeof value))
        return 0;

    for (const unsigned char *p = (const unsigned char *)value;
         *p && used + 1 < sizeof compact; p++) {
        if (!isspace(*p))
            compact[used++] = (char)tolower(*p);
    }
    compact[used] = '\0';

    return strstr(compact, "display:none") ||
           strstr(compact, "visibility:hidden") ||
           (strstr(compact, "max-height:0") && strstr(compact, "overflow:hidden"));
}

static int html_tag_base_name(const char *tag, char *out, size_t outsz)
{
    const char *p = tag ? tag : "";
    size_t used = 0;

    if (!out || outsz == 0)
        return 0;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '/')
        p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == ':') &&
           used + 1 < outsz)
        out[used++] = (char)tolower((unsigned char)*p++);
    out[used] = '\0';
    return used > 0;
}

static int html_tag_name_is(const char *tag, const char *name, int want_closing)
{
    int closing = 0;

    while (*tag && isspace((unsigned char)*tag))
        tag++;

    if (*tag == '/') {
        closing = 1;
        tag++;
        while (*tag && isspace((unsigned char)*tag))
            tag++;
    }

    if (want_closing >= 0 && closing != want_closing)
        return 0;

    while (*name) {
        if (tolower((unsigned char)*tag) != tolower((unsigned char)*name))
            return 0;
        tag++;
        name++;
    }

    return *tag == '\0' || isspace((unsigned char)*tag) ||
           *tag == '/' || *tag == '>';
}

static const char *html_find_tag_end(const char *start)
{
    char quote = 0;

    for (const char *p = start ? start : ""; *p; p++) {
        if (quote) {
            if (*p == quote)
                quote = 0;
            else if (*p == '\\' && p[1])
                p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            quote = *p;
            continue;
        }
        if (*p == '>')
            return p;
    }
    return NULL;
}

static const char *html_find_close_tag(const char *from, const char *name)
{
    const char *p = from ? from : "";
    int depth = 1;

    while ((p = strchr(p, '<')) != NULL) {
        const char *end;
        char tag[1024];
        size_t len;

        if (!strncmp(p, "<!--", 4)) {
            const char *comment_end = strstr(p + 4, "-->");
            if (!comment_end)
                return NULL;
            p = comment_end + 3;
            continue;
        }

        end = html_find_tag_end(p + 1);
        if (!end)
            return NULL;
        len = (size_t)(end - (p + 1));
        if (len >= sizeof tag)
            len = sizeof tag - 1;
        memcpy(tag, p + 1, len);
        tag[len] = '\0';
        trim(tag);

        if (html_tag_name_is(tag, name, 1)) {
            if (--depth == 0)
                return end;
        } else if (html_tag_name_is(tag, name, 0)) {
            size_t tag_len = strlen(tag);
            if (tag_len == 0 || tag[tag_len - 1] != '/')
                depth++;
        }
        p = end + 1;
    }

    return NULL;
}

static void html_trim_output(char *s)
{
    if (!s)
        return;
    trim(s);
}

static char *html_decode_entities_string(const char *s)
{
    char *out = NULL;
    size_t used = 0, cap = 0;
    size_t adv = 0;

    for (size_t i = 0; s && s[i]; i++) {
        if (s[i] == '&' &&
            consume_html_entity(s + i, &out, &used, &cap, &adv)) {
            if (adv > 0)
                i += adv - 1;
            continue;
        }
        append_char(&out, &used, &cap, s[i]);
    }

    if (!out)
        return strdup("");
    html_trim_output(out);
    return out;
}

static int html_url_is_tracking_or_too_long(const char *url)
{
    size_t len;

    if (!url)
        return 1;

    while (*url && isspace((unsigned char)*url))
        url++;

    if (!*url)
        return 1;

    len = strlen(url);
    if (len > 160)
        return 1;

    if (find_case(url, "utm_") || find_case(url, "utm=") ||
        find_case(url, "mkt_tok") || find_case(url, "mc_cid") ||
        find_case(url, "mc_eid") || find_case(url, "ga_source") ||
        find_case(url, "campaign_id")) {
        if (len > 70)
            return 1;
    }

    if (find_case(url, "list-manage.com") ||
        find_case(url, "sendgrid.net") ||
        find_case(url, "mandrillapp.com") ||
        find_case(url, "hubspotemail.net") ||
        find_case(url, "/track") ||
        find_case(url, "tracking") ||
        find_case(url, "click.") ||
        find_case(url, "click/") ||
        find_case(url, "pixel") ||
        find_case(url, "beacon") ||
        find_case(url, "open.gif") ||
        find_case(url, "unsubscribe"))
        return len > 35;

    return 0;
}

static int html_label_is_meaningless(const char *label)
{
    char tmp[256];
    int alnum = 0;

    snprintf(tmp, sizeof tmp, "%s", label ? label : "");
    trim(tmp);

    if (!tmp[0] || strlen(tmp) <= 1)
        return 1;

    if (!strncasecmp(tmp, "http://", 7) ||
        !strncasecmp(tmp, "https://", 8) ||
        !strncasecmp(tmp, "www.", 4))
        return 1;

    for (const unsigned char *p = (const unsigned char *)tmp; *p; p++)
        if (isalnum(*p) || *p >= 0x80)
            alnum++;

    if (alnum < 2)
        return 1;

    if (!strcasecmp(tmp, "link") ||
        !strcasecmp(tmp, "here") ||
        !strcasecmp(tmp, "click here") ||
        !strcasecmp(tmp, "read more") ||
        !strcasecmp(tmp, "learn more") ||
        !strcasecmp(tmp, "view") ||
        !strcasecmp(tmp, "view online") ||
        !strcasecmp(tmp, "view in browser") ||
        !strcasecmp(tmp, "open") ||
        !strcasecmp(tmp, "button") ||
        !strcasecmp(tmp, "image") ||
        !strcasecmp(tmp, "unsubscribe") ||
        !strcasecmp(tmp, "manage preferences") ||
        !strcasecmp(tmp, "privacy policy"))
        return 1;

    return 0;
}

static void html_rstrip_output(char **out, size_t *used)
{
    while (*out && *used > 0 &&
           ((*out)[*used - 1] == ' ' || (*out)[*used - 1] == '\t')) {
        (*out)[--(*used)] = '\0';
    }
}

static void html_append_space(char **out, size_t *used, size_t *cap)
{
    if (*used == 0)
        return;
    if ((*out)[*used - 1] != ' ' && (*out)[*used - 1] != '\n')
        append_char(out, used, cap, ' ');
}

static void html_append_break(char **out, size_t *used, size_t *cap,
                              int blank, int quote_depth)
{
    int have = 0;
    int want = blank ? 2 : 1;

    html_rstrip_output(out, used);

    while (*out && *used > 0 && (*out)[*used - 1 - have] == '\n') {
        have++;
        if ((size_t)have >= *used)
            break;
    }

    while (have < want) {
        append_char(out, used, cap, '\n');
        have++;
    }

    if (quote_depth > 0 && *used > 0 &&
        (*out)[*used - 1] == '\n') {
        int depth = quote_depth > 3 ? 3 : quote_depth;
        for (int i = 0; i < depth; i++)
            append_char(out, used, cap, '>');
        append_char(out, used, cap, ' ');
    }
}

static int html_phone_labels_match(const char *a, const char *b)
{
    char da[64], db[64];
    size_t na = 0, nb = 0;

    for (const unsigned char *p = (const unsigned char *)(a ? a : "");
         *p && na + 1 < sizeof da; p++)
        if (isdigit(*p))
            da[na++] = (char)*p;
    for (const unsigned char *p = (const unsigned char *)(b ? b : "");
         *p && nb + 1 < sizeof db; p++)
        if (isdigit(*p))
            db[nb++] = (char)*p;
    da[na] = '\0';
    db[nb] = '\0';
    return na >= 7 && na == nb && !strcmp(da, db);
}

static void html_append_anchor(char **out, size_t *used, size_t *cap,
                               const char *label, const char *href)
{
    /* Anchor text has already passed through the entity decoder while the
     * HTML stream was consumed; href attributes have not. */
    char *clean_label = strdup(label ? label : "");
    char *clean_href = html_decode_entities_string(href ? href : "");
    const char *display_href = clean_href;
    int safe_href = 0;
    int telephone_href = 0;
    int useful_label = clean_label && !html_label_is_meaningless(clean_label);

    if (clean_label)
        html_trim_output(clean_label);

    while (display_href && *display_href && isspace((unsigned char)*display_href))
        display_href++;
    safe_href = display_href &&
                (!strncasecmp(display_href, "http://", 7) ||
                 !strncasecmp(display_href, "https://", 8) ||
                 !strncasecmp(display_href, "mailto:", 7) ||
                 !strncasecmp(display_href, "tel:", 4)) &&
                !html_url_is_tracking_or_too_long(display_href);

    if (safe_href && !strncasecmp(display_href, "mailto:", 7))
        display_href += 7;
    else if (safe_href && !strncasecmp(display_href, "tel:", 4)) {
        telephone_href = 1;
        display_href += 4;
    }

    if (useful_label) {
        html_append_space(out, used, cap);
        append_text(out, used, cap, clean_label);
        if (safe_href && *display_href &&
            !find_case(clean_label, display_href) &&
            !(telephone_href &&
              html_phone_labels_match(clean_label, display_href))) {
            append_text(out, used, cap, " (");
            append_text(out, used, cap, display_href);
            append_char(out, used, cap, ')');
        }
    } else if (safe_href && *display_href) {
        html_append_space(out, used, cap);
        append_text(out, used, cap, display_href);
    }

    free(clean_label);
    free(clean_href);
}

static void html_append_image(char **out, size_t *used, size_t *cap,
                              const char *tag)
{
    char alt[1024] = "";
    char width[32] = "";
    char height[32] = "";
    char *clean;
    int w = 0, h = 0;

    if (!extract_html_attribute(tag, "alt", alt, sizeof alt))
        return;
    extract_html_attribute(tag, "width", width, sizeof width);
    extract_html_attribute(tag, "height", height, sizeof height);
    if (width[0])
        w = atoi(width);
    if (height[0])
        h = atoi(height);
    if ((w > 0 && w <= 2) || (h > 0 && h <= 2))
        return;

    clean = html_decode_entities_string(alt);
    if (clean && clean[0] && !html_label_is_meaningless(clean)) {
        html_append_space(out, used, cap);
        append_text(out, used, cap, "[Image: ");
        append_text(out, used, cap, clean);
        append_char(out, used, cap, ']');
    }
    free(clean);
}

static char *html_to_readable_text(const char *html) {
    char *out = NULL;
    size_t used = 0, cap = 0;
    char href[1024] = "";
    char *anchor = NULL;
    size_t anchor_used = 0, anchor_cap = 0;
    int in_anchor = 0;
    int in_pre = 0;
    int pre_line_start = 0;
    int quote_depth = 0;
    int last_space = 0;
    int suppress_leading_li_ws = 0;
    int list_is_ordered[16] = {0};
    int list_counter[16] = {0};
    int list_depth = 0;
    int table_cell_count = 0;

    for (size_t i = 0; html && html[i]; i++) {
        char **target = in_anchor ? &anchor : &out;
        size_t *target_used = in_anchor ? &anchor_used : &used;
        size_t *target_cap = in_anchor ? &anchor_cap : &cap;

        if (!strncmp(html + i, "<!--", 4)) {
            const char *end = strstr(html + i + 4, "-->");
            if (end) {
                /* A downlevel-hidden Outlook conditional is one complete
                 * comment, so this first terminator already closes it.  The
                 * !mso form closes immediately and exposes its normal HTML. */
                i = (size_t)(end - html) + 2;
                continue;
            }
        }

        if (html[i] == '<') {
            unsigned char next = (unsigned char)html[i + 1];
            if (!(isalpha(next) || next == '/' || next == '!' || next == '?')) {
                append_char(target, target_used, target_cap, '<');
                last_space = 0;
                continue;
            }
            const char *end = html_find_tag_end(html + i + 1);
            if (!end) break;

            size_t tl = (size_t)(end - (html + i + 1));
            char tag[1024];
            if (tl >= sizeof tag) tl = sizeof tag - 1;
            memcpy(tag, html + i + 1, tl);
            tag[tl] = '\0';
            trim(tag);

            if (tag[0] != '/' && html_tag_is_hidden(tag)) {
                char hidden_name[64];
                const char *close_end = NULL;

                if (html_tag_base_name(tag, hidden_name, sizeof hidden_name))
                    close_end = html_find_close_tag(end + 1, hidden_name);
                i = close_end ? (size_t)(close_end - html)
                              : (size_t)(end - html);
                continue;
            }

            if (html_tag_name_is(tag, "style", 0) ||
                html_tag_name_is(tag, "script", 0) ||
                html_tag_name_is(tag, "head", 0) ||
                html_tag_name_is(tag, "svg", 0) ||
                html_tag_name_is(tag, "iframe", 0) ||
                html_tag_name_is(tag, "object", 0) ||
                html_tag_name_is(tag, "noscript", 0)) {
                const char *close_end = NULL;
                if (html_tag_name_is(tag, "style", 0)) close_end = html_find_close_tag(end + 1, "style");
                else if (html_tag_name_is(tag, "script", 0)) close_end = html_find_close_tag(end + 1, "script");
                else if (html_tag_name_is(tag, "head", 0)) close_end = html_find_close_tag(end + 1, "head");
                else if (html_tag_name_is(tag, "svg", 0)) close_end = html_find_close_tag(end + 1, "svg");
                else if (html_tag_name_is(tag, "iframe", 0)) close_end = html_find_close_tag(end + 1, "iframe");
                else if (html_tag_name_is(tag, "object", 0)) close_end = html_find_close_tag(end + 1, "object");
                else if (html_tag_name_is(tag, "noscript", 0)) close_end = html_find_close_tag(end + 1, "noscript");
                if (close_end) {
                    i = (size_t)(close_end - html);
                    continue;
                }
            }

            int closing_tag = tag[0] == '/';

            if (html_tag_name_is(tag, "img", 0)) {
                html_append_image(target, target_used, target_cap, tag);
                i = (size_t)(end - html);
                continue;
            }

            if (html_tag_name_is(tag, "meta", 0) ||
                html_tag_name_is(tag, "source", 0) ||
                html_tag_name_is(tag, "picture", 0)) {
                i = (size_t)(end - html);
                continue;
            }

            if (html_tag_name_is(tag, "pre", 0)) {
                in_pre = 1;
                pre_line_start = 1;
                html_append_break(&out, &used, &cap, 1, quote_depth);
            } else if (html_tag_name_is(tag, "pre", 1)) {
                in_pre = 0;
                pre_line_start = 0;
                html_append_break(&out, &used, &cap, 1, quote_depth);
            } else if (html_tag_name_is(tag, "blockquote", 0)) {
                quote_depth++;
                html_append_break(&out, &used, &cap, 1, quote_depth);
            } else if (html_tag_name_is(tag, "blockquote", 1)) {
                if (quote_depth > 0) quote_depth--;
                html_append_break(&out, &used, &cap, 1, quote_depth);
            } else if (html_tag_name_is(tag, "br", 0)) {
                if (in_anchor)
                    html_append_space(&anchor, &anchor_used, &anchor_cap);
                else {
                    html_append_break(&out, &used, &cap, 0, quote_depth);
                    if (in_pre)
                        pre_line_start = 1;
                }
                last_space = 1;
            } else if ((!closing_tag && html_tag_name_is(tag, "p", 0)) || html_tag_name_is(tag, "p", 1) ||
                       (!closing_tag && html_tag_name_is(tag, "div", 0)) || html_tag_name_is(tag, "div", 1) ||
                       (!closing_tag && html_tag_name_is(tag, "section", 0)) || html_tag_name_is(tag, "section", 1) ||
                       (!closing_tag && html_tag_name_is(tag, "article", 0)) || html_tag_name_is(tag, "article", 1) ||
                       html_tag_name_is(tag, "main", -1) ||
                       html_tag_name_is(tag, "header", -1) ||
                       html_tag_name_is(tag, "footer", -1) ||
                       html_tag_name_is(tag, "address", -1) ||
                       html_tag_name_is(tag, "figure", -1) ||
                       html_tag_name_is(tag, "figcaption", -1) ||
                       html_tag_name_is(tag, "caption", -1) ||
                       html_tag_name_is(tag, "h1", -1) ||
                       html_tag_name_is(tag, "h2", -1) ||
                       html_tag_name_is(tag, "h3", -1) ||
                       html_tag_name_is(tag, "h4", -1) ||
                       html_tag_name_is(tag, "h5", -1) ||
                       html_tag_name_is(tag, "h6", -1)) {
                if (in_anchor) {
                    html_append_space(&anchor, &anchor_used, &anchor_cap);
                } else if (suppress_leading_li_ws && !closing_tag && html_tag_name_is(tag, "p", 0)) {
                    suppress_leading_li_ws = 1;
                    last_space = 0;
                } else {
                    html_append_break(&out, &used, &cap, 1, quote_depth);
                    last_space = 1;
                }
            } else if (html_tag_name_is(tag, "hr", 0)) {
                html_append_break(&out, &used, &cap, 1, quote_depth);
                append_text(&out, &used, &cap, "---");
                html_append_break(&out, &used, &cap, 1, quote_depth);
                last_space = 1;
            } else if (!closing_tag && html_tag_name_is(tag, "ol", 0)) {
                if (list_depth < 15) {
                    char start[32] = "";
                    list_depth++;
                    list_is_ordered[list_depth] = 1;
                    list_counter[list_depth] = 0;
                    if (extract_html_attribute(tag, "start", start, sizeof start))
                        list_counter[list_depth] = atoi(start) - 1;
                }
                html_append_break(&out, &used, &cap, 0, quote_depth);
                last_space = 1;
            } else if (!closing_tag && html_tag_name_is(tag, "ul", 0)) {
                if (list_depth < 15) {
                    list_depth++;
                    list_is_ordered[list_depth] = 0;
                    list_counter[list_depth] = 0;
                }
                html_append_break(&out, &used, &cap, 0, quote_depth);
                last_space = 1;
            } else if (html_tag_name_is(tag, "ol", 1) || html_tag_name_is(tag, "ul", 1)) {
                if (list_depth > 0) list_depth--;
                html_append_break(&out, &used, &cap, 0, quote_depth);
                last_space = 1;
            } else if (!closing_tag && html_tag_name_is(tag, "li", 0)) {
                char value[32] = "";
                html_append_break(&out, &used, &cap, 0, quote_depth);

                for (int d = 1; d < list_depth; d++)
                    append_text(&out, &used, &cap, "  ");

                if (list_depth > 0 && list_is_ordered[list_depth]) {
                    char num[32];
                    if (extract_html_attribute(tag, "value", value, sizeof value))
                        list_counter[list_depth] = atoi(value);
                    else
                        list_counter[list_depth]++;
                    snprintf(num, sizeof num, "%d. ", list_counter[list_depth]);
                    append_text(&out, &used, &cap, num);
                } else {
                    append_text(&out, &used, &cap, "* ");
                }

                last_space = 0;
                suppress_leading_li_ws = 1;
            } else if (html_tag_name_is(tag, "dl", -1)) {
                html_append_break(&out, &used, &cap, 1, quote_depth);
                last_space = 1;
            } else if (html_tag_name_is(tag, "dt", 0)) {
                html_append_break(&out, &used, &cap, 0, quote_depth);
                last_space = 1;
            } else if (html_tag_name_is(tag, "dd", 0)) {
                html_append_break(&out, &used, &cap, 0, quote_depth);
                append_text(&out, &used, &cap, "  ");
                last_space = 0;
            } else if (html_tag_name_is(tag, "tr", 0)) {
                html_append_break(&out, &used, &cap, 0, quote_depth);
                table_cell_count = 0;
                last_space = 1;
            } else if (html_tag_name_is(tag, "tr", 1)) {
                html_append_break(&out, &used, &cap, 0, quote_depth);
                table_cell_count = 0;
                last_space = 1;
            } else if (html_tag_name_is(tag, "td", 0) ||
                       html_tag_name_is(tag, "th", 0)) {
                if (table_cell_count++ > 0)
                    append_text(&out, &used, &cap, " | ");
                last_space = 0;
            }

            if (html_tag_name_is(tag, "a", 0)) {
                href[0] = '\0';
                if (extract_html_attribute(tag, "href", href, sizeof href)) {
                    in_anchor = 1;
                    free(anchor);
                    anchor = NULL;
                    anchor_used = anchor_cap = 0;
                }
            } else if (html_tag_name_is(tag, "a", 1)) {
                html_append_anchor(&out, &used, &cap, anchor ? anchor : "", href);
                free(anchor);
                anchor = NULL;
                anchor_used = anchor_cap = 0;
                in_anchor = 0;
                href[0] = '\0';
            }

            i = (size_t)(end - html);
            continue;
        }

        if (suppress_leading_li_ws && isspace((unsigned char)html[i]))
            continue;

        suppress_leading_li_ws = 0;

        if (in_pre && !in_anchor && pre_line_start &&
            html[i] != '\r' && html[i] != '\n') {
            append_text(&out, &used, &cap, "    ");
            pre_line_start = 0;
        }

        /* Drop naked newsletter/tracking URLs; keep short useful URLs. */
        if ((i == 0 || isspace((unsigned char)html[i - 1]) || html[i - 1] == '(') &&
            (!strncmp(html + i, "http://", 7) || !strncmp(html + i, "https://", 8))) {
            size_t start = i;
            size_t len = 0;
            char url[1024];

            while (html[i] &&
                   !isspace((unsigned char)html[i]) &&
                   html[i] != ')' &&
                   html[i] != '"' &&
                   html[i] != '\'' &&
                   html[i] != '<') {
                i++;
                len++;
            }

            if (len >= sizeof url)
                len = sizeof url - 1;
            memcpy(url, html + start, len);
            url[len] = '\0';

            if (in_anchor) {
                append_text(target, target_used, target_cap, url);
            } else if (!html_url_is_tracking_or_too_long(url)) {
                html_append_space(&out, &used, &cap);
                append_text(&out, &used, &cap, url);
            }

            if (html[i] == ')' || html[i] == '"' || html[i] == '\'')
                continue;

            if (html[i] == '\0')
                break;

            if (html[i] == '<') {
                i--;
                continue;
            }
        }

        if (html[i] == '&') {
            size_t adv = 0;
            if (consume_html_entity(html + i, target, target_used, target_cap, &adv)) {
                if (adv > 0) i += adv - 1;
            } else {
                append_char(target, target_used, target_cap, '&');
            }
            last_space = 0;
            continue;
        }

        if (isspace((unsigned char)html[i])) {
            if (in_pre && !in_anchor) {
                if (html[i] == '\r') {
                    if (html[i + 1] == '\n') {
                        pre_line_start = 1;
                        continue;
                    }
                    append_char(&out, &used, &cap, '\n');
                } else {
                    append_char(&out, &used, &cap, html[i]);
                }
                if (html[i] == '\r' || html[i] == '\n')
                    pre_line_start = 1;
                last_space = html[i] != '\n';
            } else if (html[i] == '\n' || html[i] == '\r') {
                html_append_space(target, target_used, target_cap);
                last_space = 1;
            } else if (!last_space) {
                append_char(target, target_used, target_cap, ' ');
                last_space = 1;
            }
        } else {
            append_char(target, target_used, target_cap, html[i]);
            last_space = 0;
        }
    }

    if (in_anchor) {
        html_append_anchor(&out, &used, &cap, anchor ? anchor : "", href);
        free(anchor);
    }

    if (!out) return strdup("");
    trim(out);
    strip_tracking_gibberish(out);
    return out;
}

static char *html_to_text(const char *html) {
    return html_to_readable_text(html);
}





static int simplemail_machine_token_line(const char *line) {
    char tmp[4096];
    int len = 0;
    int ok = 0;
    int bad = 0;

    snprintf(tmp, sizeof tmp, "%s", line ? line : "");
    trim(tmp);

    if (!tmp[0])
        return 0;

    for (const unsigned char *p = (const unsigned char *)tmp; *p; p++) {
        len++;
        if (isalnum(*p) || *p == '_' || *p == '-' || *p == '/' ||
            *p == '+' || *p == '=' || *p == '.' || *p == ':')
            ok++;
        else
            bad++;
    }

    if (len < 35)
        return 0;

    return ok > (len * 9 / 10) && bad < 3;
}

static void strip_newsletter_tracking_urls_inplace(char *s) {
    if (!s) return;

    char *src = s;
    char *dst = s;

    while (*src) {
        char *end = strchr(src, '\n');
        size_t len = end ? (size_t)(end - src) : strlen(src);

        char line[4096];
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, src, len);
        line[len] = '\0';

        char trimmed[4096];
        snprintf(trimmed, sizeof trimmed, "%s", line);
        trim(trimmed);

        int is_long_url =
            (!strncmp(trimmed, "http://", 7) || !strncmp(trimmed, "https://", 8)) &&
            strlen(trimmed) > 30;

        if (is_long_url) {
            const char *msg = "[long tracking URL omitted]\n";
            strcpy(dst, msg);
            dst += strlen(msg);

            if (end)
                src = end + 1;
            else
                break;

            while (*src) {
                char *e2 = strchr(src, '\n');
                size_t l2 = e2 ? (size_t)(e2 - src) : strlen(src);

                char nextline[4096];
                if (l2 >= sizeof nextline)
                    l2 = sizeof nextline - 1;
                memcpy(nextline, src, l2);
                nextline[l2] = '\0';

                if (!simplemail_machine_token_line(nextline))
                    break;

                if (e2)
                    src = e2 + 1;
                else {
                    src += strlen(src);
                    break;
                }
            }

            continue;
        }

        memcpy(dst, src, len);
        dst += len;
        if (end) {
            *dst++ = '\n';
            src = end + 1;
        } else {
            break;
        }
    }

    *dst = '\0';
}



static char *mail_dup_slice(const char *s, size_t len)
{
    char *out;

    if (!s)
        return strdup("");
    if (len > MAX_BODY)
        len = MAX_BODY;

    out = malloc(len + 1);
    if (!out)
        return strdup("");

    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void mail_rstrip(char *s)
{
    size_t n;

    if (!s)
        return;

    n = strlen(s);
    while (n > 0 &&
           (s[n - 1] == '\r' || s[n - 1] == '\n' ||
            s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static const char *mail_skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static char *mail_sanitize_text_bytes(const char *s)
{
    char *out = NULL;
    size_t used = 0, cap = 0;

    for (size_t i = 0; s && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\r') {
            append_char(&out, &used, &cap, '\n');
            if (s[i + 1] == '\n')
                i++;
            continue;
        }

        if (c == '\n' || c == '\t') {
            append_char(&out, &used, &cap, (char)c);
            continue;
        }

        if (c < 32 || c == 127) {
            append_char(&out, &used, &cap, ' ');
            continue;
        }

        if (c == 0xC2 && (unsigned char)s[i + 1] == 0xA0) {
            append_char(&out, &used, &cap, ' ');
            i++;
            continue;
        }

        if (c == 0xC2 && (unsigned char)s[i + 1] == 0xAD) {
            i++;
            continue;
        }

        /* Drop zero-width space, but retain joiners and LRM/RLM: they carry
         * real meaning in emoji, Arabic-family scripts, and bidi mail. */
        if (c == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
            (unsigned char)s[i + 2] == 0x8B) {
            i += 2;
            continue;
        }

        if (c == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
            append_char(&out, &used, &cap, '\n');
            i += 2;
            continue;
        }

        if (c == 0xE2 && (unsigned char)s[i + 1] == 0x81 &&
            (unsigned char)s[i + 2] == 0xA0) {
            i += 2;
            continue;
        }

        if (c == 0xEF && (unsigned char)s[i + 1] == 0xBB &&
            (unsigned char)s[i + 2] == 0xBF) {
            i += 2;
            continue;
        }

        append_char(&out, &used, &cap, (char)c);
    }

    if (!out)
        return strdup("");
    return out;
}

static int mail_line_quote_depth(const char *line)
{
    int depth = 0;
    const char *p = mail_skip_space(line ? line : "");

    while (*p == '>') {
        depth++;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }

    return depth;
}

static const char *mail_after_quote_markers(const char *line, int *depth_out)
{
    int depth = 0;
    const char *p = mail_skip_space(line ? line : "");

    while (*p == '>') {
        depth++;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
    }

    if (depth_out)
        *depth_out = depth;
    return p;
}

static int mail_line_is_listish(const char *line)
{
    const char *p = mail_skip_space(line ? line : "");

    if ((p[0] == '-' || p[0] == '*' || p[0] == '+') &&
        (p[1] == ' ' || p[1] == '\t'))
        return 1;

    if ((unsigned char)p[0] == 0xE2 &&
        (unsigned char)p[1] == 0x80 &&
        (unsigned char)p[2] == 0xA2)
        return 1;

    if (isdigit((unsigned char)p[0])) {
        while (isdigit((unsigned char)*p))
            p++;
        if ((*p == '.' || *p == ')') &&
            (p[1] == ' ' || p[1] == '\t'))
            return 1;
    }

    return 0;
}

static int mail_line_is_attachment_note(const char *line)
{
    const char *p = mail_skip_space(line ? line : "");
    return !strncmp(p, "[Attachment:", 12);
}

static int mail_line_is_signature_marker(const char *line)
{
    const char *p = mail_skip_space(line ? line : "");
    return !strcmp(p, "--") || !strcmp(p, "-- ");
}

static int mail_line_looks_codeish(const char *line)
{
    int leading = 0;
    const char *p = line ? line : "";

    while (*p == ' ') {
        leading++;
        p++;
    }

    if (leading >= 4 || *p == '\t')
        return 1;

    if ((strchr(p, '{') || strchr(p, '}')) &&
        (strchr(p, ';') || strchr(p, '=')))
        return 1;

    return 0;
}

static int mail_line_is_hash_blob(const char *line)
{
    int len = 0;
    int hex = 0;
    int ok = 0;
    int spaces = 0;
    const unsigned char *p = (const unsigned char *)mail_skip_space(line ? line : "");

    for (; *p; p++) {
        len++;
        if (isspace(*p))
            spaces++;
        if (isxdigit(*p))
            hex++;
        if (isxdigit(*p) || *p == '-' || *p == '_' || *p == ':')
            ok++;
    }

    if (len < 32 || spaces > 1)
        return 0;

    return ok > len * 9 / 10 && hex > len * 3 / 5;
}

static int mail_line_has_markdown_link_soup(const char *line)
{
    int links = 0;
    int words = 0;
    const char *p = line ? line : "";

    while ((p = strstr(p, "](")) != NULL) {
        links++;
        p += 2;
    }

    for (p = line ? line : ""; *p; p++) {
        if (isalnum((unsigned char)*p) &&
            (p == line || !isalnum((unsigned char)p[-1])))
            words++;
    }

    return links >= 2 || (links == 1 && strlen(line ? line : "") > 140 && words < 12);
}

static int mail_line_is_footer_sludge(const char *line)
{
    char tmp[512];

    snprintf(tmp, sizeof tmp, "%s", line ? line : "");
    trim(tmp);

    if (!tmp[0])
        return 0;

    if (!strcasecmp(tmp, "unsubscribe") ||
        !strcasecmp(tmp, "manage preferences") ||
        !strcasecmp(tmp, "email preferences") ||
        !strcasecmp(tmp, "privacy policy") ||
        !strcasecmp(tmp, "view in browser") ||
        !strcasecmp(tmp, "view online"))
        return 1;

    if (strlen(tmp) < 140 &&
        (find_case(tmp, "unsubscribe") ||
         find_case(tmp, "manage your preferences") ||
         find_case(tmp, "update your preferences") ||
         find_case(tmp, "email preferences") ||
         find_case(tmp, "why did i get this") ||
         find_case(tmp, "you are receiving this") ||
         find_case(tmp, "you received this email") ||
         find_case(tmp, "you received this because") ||
         find_case(tmp, "this email was sent to") ||
         find_case(tmp, "mailing address") ||
         find_case(tmp, "powered by mailchimp")))
        return 1;

    return 0;
}

static int mail_line_is_urlish(const char *line)
{
    const char *p = mail_skip_space(line ? line : "");
    return !strncmp(p, "http://", 7) || !strncmp(p, "https://", 8);
}

static int mail_line_is_machine_noise(const char *line)
{
    char tmp[4096];

    snprintf(tmp, sizeof tmp, "%s", line ? line : "");
    trim(tmp);

    if (!tmp[0] || mail_line_is_attachment_note(tmp))
        return 0;

    if (mail_line_is_urlish(tmp) && (strlen(tmp) > 80 || html_url_is_tracking_or_too_long(tmp)))
        return 1;

    if (simplemail_machine_token_line(tmp) ||
        looks_like_gibberish_line(tmp) ||
        looks_like_tracking_gibberish(tmp) ||
        mail_line_is_hash_blob(tmp) ||
        mail_line_has_markdown_link_soup(tmp))
        return 1;

    return 0;
}

static char *mail_collapse_spaces_dup(const char *line)
{
    char *out = NULL;
    size_t used = 0, cap = 0;
    int in_space = 0;
    const char *p = line ? line : "";

    while (*p && isspace((unsigned char)*p))
        p++;

    for (; *p; p++) {
        if (*p == '\n' || *p == '\r')
            break;
        if (*p == '\t' || *p == ' ') {
            in_space = 1;
            continue;
        }
        if (in_space && used > 0)
            append_char(&out, &used, &cap, ' ');
        append_char(&out, &used, &cap, *p);
        in_space = 0;
    }

    if (!out)
        return strdup("");
    trim(out);
    return out;
}

static int mail_sentence_ended(const char *s)
{
    size_t n;

    if (!s)
        return 0;

    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;

    while (n > 0 && (s[n - 1] == '"' || s[n - 1] == '\'' || s[n - 1] == ')'))
        n--;

    if (n == 0)
        return 0;

    return s[n - 1] == '.' || s[n - 1] == '!' || s[n - 1] == '?' ||
           s[n - 1] == ':' || s[n - 1] == ';';
}

static int mail_should_join_lines(const char *prev, int prev_len, int line_len)
{
    if (!prev || prev_len <= 0)
        return 0;

    if (prev_len < 34 && line_len < 34)
        return 0;

    if (prev_len < 48 && line_len < 48 && mail_sentence_ended(prev))
        return 0;

    return 1;
}

static void mail_append_blank(char **out, size_t *used, size_t *cap)
{
    int have = 0;

    while (*out && *used > 0 && (*out)[*used - 1 - have] == '\n') {
        have++;
        if ((size_t)have >= *used)
            break;
    }

    while (have < 2) {
        append_char(out, used, cap, '\n');
        have++;
    }
}

static void mail_flush_paragraph(char **out, size_t *used, size_t *cap,
                                 char **para, size_t *para_used)
{
    if (!para || !*para || *para_used == 0)
        return;

    trim(*para);
    if ((*para)[0]) {
        append_text(out, used, cap, *para);
        mail_append_blank(out, used, cap);
    }

    (*para)[0] = '\0';
    *para_used = 0;
}

static void mail_append_line(char **out, size_t *used, size_t *cap, const char *line)
{
    append_text(out, used, cap, line ? line : "");
    append_char(out, used, cap, '\n');
}

static void mail_append_quote_line(char **out, size_t *used, size_t *cap,
                                   const char *line)
{
    int depth = 0;
    const char *body = mail_after_quote_markers(line, &depth);
    char *clean = mail_collapse_spaces_dup(body);
    int shown = depth > 3 ? 3 : depth;

    if (!clean || !clean[0]) {
        free(clean);
        return;
    }

    for (int i = 0; i < shown; i++)
        append_char(out, used, cap, '>');
    append_char(out, used, cap, ' ');
    append_text(out, used, cap, clean);
    append_char(out, used, cap, '\n');
    free(clean);
}

static void mail_collapse_blank_lines_inplace(char *s)
{
    char *r = s;
    char *w = s;
    int nls = 0;

    if (!s)
        return;

    while (*r) {
        if (*r == '\n') {
            if (nls < 2)
                *w++ = *r;
            nls++;
        } else {
            *w++ = *r;
            nls = 0;
        }
        r++;
    }

    *w = '\0';
}

static char *normalize_mail_text_mode(const char *text, int preserve_hard_breaks)
{
    char *clean = mail_sanitize_text_bytes(text ? text : "");
    char *out = NULL;
    char *para = NULL;
    size_t used = 0, cap = 0;
    size_t para_used = 0, para_cap = 0;
    int prev_para_len = 0;
    int omitted_deep_quote_noise = 0;
    int in_signature = 0;

    if (!clean)
        return strdup("");

    strip_html_comments_inplace(clean);
    strip_newsletter_tracking_urls_inplace(clean);
    strip_tracking_gibberish(clean);
    collapse_unsubscribe_tracking(clean);

    for (const char *p = clean; ; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = mail_dup_slice(p, len);
        char *trimmed;

        mail_rstrip(line);
        trimmed = mail_collapse_spaces_dup(line);

        if (!trimmed || !trimmed[0]) {
            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
            mail_append_blank(&out, &used, &cap);
            omitted_deep_quote_noise = 0;
        } else if (mail_line_is_machine_noise(trimmed)) {
            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
        } else if (mail_line_is_footer_sludge(trimmed)) {
            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
        } else if (mail_line_quote_depth(line) > 0) {
            int depth = mail_line_quote_depth(line);

            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
            if (depth > 3 && (strlen(trimmed) > 180 || mail_line_is_machine_noise(trimmed))) {
                if (!omitted_deep_quote_noise) {
                    mail_append_line(&out, &used, &cap, "> [quoted noise omitted]");
                    omitted_deep_quote_noise = 1;
                }
            } else {
                mail_append_quote_line(&out, &used, &cap, line);
                omitted_deep_quote_noise = 0;
            }
        } else if (mail_line_is_signature_marker(trimmed)) {
            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
            mail_append_line(&out, &used, &cap, trimmed);
            omitted_deep_quote_noise = 0;
            in_signature = 1;
        } else if (in_signature) {
            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
            mail_append_line(&out, &used, &cap, line);
            omitted_deep_quote_noise = 0;
        } else if (mail_line_is_listish(trimmed) ||
                   mail_line_is_attachment_note(trimmed) ||
                   mail_line_looks_codeish(line)) {
            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
            mail_append_line(&out, &used, &cap,
                             (mail_line_looks_codeish(line) ||
                              mail_line_is_listish(trimmed)) ? line : trimmed);
            omitted_deep_quote_noise = 0;
        } else if (preserve_hard_breaks) {
            mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
            mail_append_line(&out, &used, &cap, line);
            omitted_deep_quote_noise = 0;
        } else {
            int line_len = (int)strlen(trimmed);

            if (para_used > 0 && mail_should_join_lines(para, prev_para_len, line_len)) {
                append_char(&para, &para_used, &para_cap, ' ');
                append_text(&para, &para_used, &para_cap, trimmed);
            } else {
                mail_flush_paragraph(&out, &used, &cap, &para, &para_used);
                append_text(&para, &para_used, &para_cap, trimmed);
            }

            prev_para_len = line_len;
            omitted_deep_quote_noise = 0;
        }

        free(trimmed);
        free(line);

        if (!e)
            break;
        p = e + 1;
    }

    mail_flush_paragraph(&out, &used, &cap, &para, &para_used);

    free(para);
    free(clean);

    if (!out)
        return strdup("");

    mail_collapse_blank_lines_inplace(out);
    trim(out);
    strip_newsletter_footer(out);
    mail_collapse_blank_lines_inplace(out);
    trim(out);
    return out;
}

static char *normalize_mail_text(const char *text)
{
    return normalize_mail_text_mode(text, 0);
}

static char *normalize_html_text(const char *text)
{
    return normalize_mail_text_mode(text, 1);
}

static int score_text_readability(const char *text)
{
    int score = 0;
    int words = 0;
    int letters = 0;
    int nonblank = 0;
    int good_lines = 0;
    int url_lines = 0;
    int noise_lines = 0;
    int tag_count = 0;
    int footer_lines = 0;
    int in_word = 0;

    if (!text || !*text)
        return -1000;

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        int wordish = isalnum(*p) || (*p >= 0xC0);

        if (isalpha(*p) || *p >= 0xC0)
            letters++;
        if (wordish && !in_word)
            words++;
        if (*p < 0x80)
            in_word = wordish;
        if (*p == '<' && isalpha((unsigned char)p[1]))
            tag_count++;
    }

    for (const char *p = text; ; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = mail_dup_slice(p, len);
        char *clean_line = mail_collapse_spaces_dup(line);

        if (clean_line && clean_line[0]) {
            nonblank++;
            if (mail_line_is_urlish(clean_line))
                url_lines++;
            if (mail_line_is_machine_noise(clean_line))
                noise_lines++;
            if (mail_line_is_footer_sludge(clean_line))
                footer_lines++;
            if (!mail_line_is_urlish(clean_line) &&
                !mail_line_is_machine_noise(clean_line) &&
                !mail_line_is_footer_sludge(clean_line) &&
                letters > 0)
                good_lines++;
        }

        free(clean_line);
        free(line);

        if (!e)
            break;
        p = e + 1;
    }

    score += words * 3;
    score += good_lines * 12;
    score += letters / 24;
    score -= url_lines * 30;
    score -= noise_lines * 40;
    score -= footer_lines * 20;
    score -= tag_count * 25;

    if (words < 4)
        score -= 60;
    if (nonblank > 0 && (url_lines + noise_lines) * 2 >= nonblank)
        score -= 120;
    if (nonblank == 0)
        score -= 200;

    return score;
}

typedef struct {
    char *text;
    int score;
} MailBodyCandidate;

static void mail_candidate_init(MailBodyCandidate *c)
{
    c->text = NULL;
    c->score = -10000;
}

static void mail_candidate_clear(MailBodyCandidate *c)
{
    if (!c)
        return;
    free(c->text);
    c->text = NULL;
    c->score = -10000;
}

static void mail_candidate_consider_mode(MailBodyCandidate *c, char *text,
                                         int mode)
{
    char *clean;
    int score;

    if (!c || !text)
        return;

    if (mode < 0)
        clean = text;
    else {
        clean = mode ? normalize_html_text(text) : normalize_mail_text(text);
        free(text);
    }
    if (!clean)
        return;

    score = score_text_readability(clean);
    if (!c->text || score > c->score) {
        free(c->text);
        c->text = clean;
        c->score = score;
    } else {
        free(clean);
    }
}

static void mail_candidate_consider(MailBodyCandidate *c, char *text)
{
    mail_candidate_consider_mode(c, text, 0);
}

static void mail_candidate_consider_html(MailBodyCandidate *c, char *text)
{
    mail_candidate_consider_mode(c, text, 1);
}

static void mail_candidate_consider_normalized(MailBodyCandidate *c, char *text)
{
    mail_candidate_consider_mode(c, text, -1);
}

static char *mail_candidate_take(MailBodyCandidate *c)
{
    char *text;

    if (!c)
        return NULL;

    text = c->text;
    c->text = NULL;
    c->score = -10000;
    return text;
}

static char *choose_best_body_part(MailBodyCandidate *plain,
                                   MailBodyCandidate *html,
                                   int multipart_alternative)
{
    int plain_ok = plain && plain->text && plain->score >= 25;
    int html_ok = html && html->text && html->score >= 25;

    if (multipart_alternative) {
        if (html_ok && (!plain_ok ||
                        html->score + 10 >= plain->score ||
                        strlen(html->text) > strlen(plain->text) + 80))
            return mail_candidate_take(html);
        if (plain_ok)
            return mail_candidate_take(plain);
        if (html_ok)
            return mail_candidate_take(html);
    }

    if (plain && plain->text && html && html->text) {
        if (html->score > plain->score + (multipart_alternative ? 0 : 8))
            return mail_candidate_take(html);
        return mail_candidate_take(plain);
    }

    if (plain && plain->text)
        return mail_candidate_take(plain);
    if (html && html->text)
        return mail_candidate_take(html);

    return NULL;
}

static char *mail_append_attachment_lines(char *body, const char *attachment_lines)
{
    size_t body_len;
    size_t attach_len;
    size_t need;
    char *joined;

    if (!attachment_lines || !attachment_lines[0])
        return body ? body : strdup("");

    if (!body)
        return strdup(attachment_lines);

    body_len = strlen(body);
    attach_len = strlen(attachment_lines);
    if (body_len > MAX_BODY)
        body_len = MAX_BODY;
    if (attach_len > MAX_BODY - body_len)
        attach_len = MAX_BODY - body_len;

    need = body_len + attach_len + 4;
    joined = malloc(need);
    if (!joined)
        return body;

    snprintf(joined, need, "%.*s%s%.*s",
             (int)body_len, body,
             body[0] ? "\n\n" : "",
             (int)attach_len, attachment_lines);
    free(body);
    return joined;
}



static void scan_attachments_fallback(Message *m, const char *raw_body) {
    if (!m || !raw_body || m->has_attachment) return;

    const char *p = raw_body;

    while (!m->has_attachment && (p = find_case(p, "Content-Disposition:"))) {
        const char *hdr_start = raw_body;
        const char *q = raw_body;
        const char *last_boundary = NULL;

        while ((q = strstr(q, "\n--")) && q < p) {
            last_boundary = q;
            q++;
        }

        if (last_boundary) {
            const char *nl = strchr(last_boundary + 1, '\n');
            if (nl) hdr_start = nl + 1;
        }

        const char *header_end = strstr(p, "\r\n\r\n");
        int sep_len = 4;
        if (!header_end) {
            header_end = strstr(p, "\n\n");
            sep_len = 2;
        }
        if (!header_end) break;

        size_t hdr_len = (size_t)(header_end - hdr_start);
        char *hdrs = malloc(hdr_len + 1);
        if (!hdrs) break;
        memcpy(hdrs, hdr_start, hdr_len);
        hdrs[hdr_len] = '\0';

        char attach_name[256];
        char ctype[512], cte[128];

        int is_attach = headers_attachment_filename(hdrs, attach_name, sizeof attach_name);
        parse_part_headers(hdrs, ctype, sizeof ctype, cte, sizeof cte);
        free(hdrs);

        if (!is_attach || !attach_name[0]) {
            p = header_end + sep_len;
            continue;
        }

        const char *body_start = header_end + sep_len;
        const char *next = strstr(body_start, "\n--");
        if (!next) next = raw_body + strlen(raw_body);

        size_t part_len = (size_t)(next - body_start);
        while (part_len && (body_start[part_len - 1] == '\n' || body_start[part_len - 1] == '\r'))
            part_len--;

        char *part = malloc(part_len + 1);
        if (!part) break;
        memcpy(part, body_start, part_len);
        part[part_len] = '\0';

        save_attachment_part(m, attach_name, part, cte);
        free(part);

        p = body_start;
    }
}


static char *extract_mime_display_body(Message *m, const char *raw_body, const char *root_ctype, const char *root_cte) {
    char boundary[256];

    if (root_ctype && root_ctype[0] &&
        (find_case(root_ctype, "x-simplemail-attachment") ||
         find_case(root_ctype, "Content-Disposition: attachment") ||
         (!find_case(root_ctype, "text/") && !extract_boundary(root_ctype, boundary, sizeof boundary)))) {
        return strdup("");
    }

    if (extract_boundary(root_ctype, boundary, sizeof boundary)) {
        char marker[320];
        snprintf(marker, sizeof marker, "--%s", boundary);

        const char *p = raw_body;
        int multipart_alternative = find_case(root_ctype, "multipart/alternative") != NULL;
        MailBodyCandidate best_plain;
        MailBodyCandidate best_html;
        MailBodyCandidate best_other;
        char attachment_lines[1024] = "";

        mail_candidate_init(&best_plain);
        mail_candidate_init(&best_html);
        mail_candidate_init(&best_other);

        while ((p = strstr(p, marker))) {
            p += strlen(marker);

            if (p[0] == '-' && p[1] == '-') break;
            if (p[0] == '\r' && p[1] == '\n') p += 2;
            else if (p[0] == '\n') p++;

            const char *header_end = strstr(p, "\r\n\r\n");
            int sep_len = 4;
            if (!header_end) {
                header_end = strstr(p, "\n\n");
                sep_len = 2;
            }
            if (!header_end) break;

            size_t hdr_len = (size_t)(header_end - p);
            char *hdrs = malloc(hdr_len + 1);
            if (!hdrs) break;
            memcpy(hdrs, p, hdr_len);
            hdrs[hdr_len] = '\0';

            char ctype[512], cte[128];
            char attach_name[256];
            int is_attach = headers_attachment_filename(hdrs, attach_name, sizeof attach_name);
            parse_part_headers(hdrs, ctype, sizeof ctype, cte, sizeof cte);
            free(hdrs);

            const char *body_start = header_end + sep_len;
            const char *next = strstr(body_start, marker);
            if (!next) next = raw_body + strlen(raw_body);

            size_t part_len = (size_t)(next - body_start);
            while (part_len && (body_start[part_len - 1] == '\n' || body_start[part_len - 1] == '\r'))
                part_len--;

            char *part = malloc(part_len + 1);
            if (!part) {
                mail_candidate_clear(&best_plain);
                mail_candidate_clear(&best_html);
                mail_candidate_clear(&best_other);
                return strdup("(Could not decode message body.)\n");
            }
            memcpy(part, body_start, part_len);
            part[part_len] = '\0';

            if (is_attach ||
                find_case(ctype, "x-simplemail-attachment") ||
                find_case(ctype, "Content-Disposition: attachment") ||
                find_case(ctype, "image/") ||
                find_case(ctype, "application/")) {
                if (attach_name[0]) {
                    save_attachment_part(m, attach_name, part, cte);
                    if (strlen(attachment_lines) + strlen(attach_name) + 20 < sizeof attachment_lines) {
                        strcat(attachment_lines, "[Attachment: ");
                        strcat(attachment_lines, attach_name);
                        strcat(attachment_lines, "]\n");
                    }
                }
                free(part);
                p = next;
                continue;
            }

            {
                char child_boundary[256];
                if (extract_boundary(ctype, child_boundary, sizeof child_boundary)) {
                char *nested = extract_mime_display_body(m, part, ctype, cte);
                free(part);

                    if (nested && nested[0]) {
                        if (find_case(ctype, "multipart/related"))
                            mail_candidate_consider_normalized(&best_html, nested);
                        else
                            mail_candidate_consider_normalized(&best_other, nested);
                    } else {
                        free(nested);
                    }

                p = next;
                continue;
                }
            }

            if (find_case(ctype, "text/plain") || ctype[0] == '\0') {
                char *decoded = decode_text_part(part, cte, ctype);
                free(part);
                mail_candidate_consider(&best_plain, decoded);
                p = next;
                continue;
            }

            if (find_case(ctype, "text/html")) {
                char *html = decode_text_part(part, cte, ctype);
                if (html) {
                    char *readable = html_to_text(html);
                    mail_candidate_consider_html(&best_html, readable);
                    free(html);
                }
                free(part);
                p = next;
                continue;
            }

            free(part);
            p = next;
        }

        {
            char *chosen = NULL;

            if (multipart_alternative) {
                chosen = choose_best_body_part(&best_plain, &best_html, 1);
                if (!chosen)
                    chosen = mail_candidate_take(&best_other);
            } else {
                MailBodyCandidate *best = NULL;

                if (best_plain.text)
                    best = &best_plain;
                if (best_html.text && (!best || best_html.score > best->score))
                    best = &best_html;
                if (best_other.text && (!best || best_other.score > best->score))
                    best = &best_other;

                if (best)
                    chosen = mail_candidate_take(best);
                else
                    chosen = choose_best_body_part(&best_plain, &best_html, 0);
            }

            mail_candidate_clear(&best_plain);
            mail_candidate_clear(&best_html);
            mail_candidate_clear(&best_other);

            if (!chosen && attachment_lines[0])
                return strdup(attachment_lines);
            return mail_append_attachment_lines(chosen ? chosen : strdup(""), attachment_lines);
        }
    }

    if (find_case(root_ctype, "text/plain") || root_ctype[0] == '\0') {
        char *decoded = decode_text_part(raw_body, root_cte, root_ctype);
        char *clean = normalize_mail_text(decoded);
        free(decoded);
        return clean;
    }

    if (find_case(root_ctype, "text/html")) {
        char *html = decode_text_part(raw_body, root_cte, root_ctype);
        char *text = html_to_text(html);
        char *clean = normalize_html_text(text);
        free(html);
        free(text);
        return clean;
    }

    return strdup("");
}


static void parse_message_header_block(Message *m, const char *raw_headers,
                                       char *root_ctype, size_t root_ctype_size,
                                       char *root_cte, size_t root_cte_size) {
    char *headers;

    m->from[0] = '\0';
    m->subject[0] = '\0';
    m->date[0] = '\0';
    m->message_id[0] = '\0';
    m->in_reply_to[0] = '\0';
    m->references[0] = '\0';
    if (root_ctype && root_ctype_size) root_ctype[0] = '\0';
    if (root_cte && root_cte_size) root_cte[0] = '\0';

    headers = unfold_headers(raw_headers ? raw_headers : "");
    if (headers) {
        char *saveptr = NULL;
        char *hline = strtok_r(headers, "\n", &saveptr);

        while (hline) {
            trim(hline);

            if (starts_case(hline, "From:"))
                copy_field(m->from, sizeof m->from, hline + 5);
            else if (starts_case(hline, "Subject:"))
                copy_field(m->subject, sizeof m->subject, hline + 8);
            else if (starts_case(hline, "Date:"))
                copy_field(m->date, sizeof m->date, hline + 5);
            else if (starts_case(hline, "Message-ID:"))
                copy_field(m->message_id, sizeof m->message_id, hline + 11);
            else if (starts_case(hline, "In-Reply-To:"))
                copy_field(m->in_reply_to, sizeof m->in_reply_to, hline + 12);
            else if (starts_case(hline, "References:"))
                copy_field(m->references, sizeof m->references, hline + 11);
            else if (root_ctype && starts_case(hline, "Content-Type:"))
                copy_field(root_ctype, root_ctype_size, hline + 13);
            else if (root_cte &&
                     starts_case(hline, "Content-Transfer-Encoding:"))
                copy_field(root_cte, root_cte_size, hline + 26);

            hline = strtok_r(NULL, "\n", &saveptr);
        }
        free(headers);
    }

    decode_header_field_inplace(m->from, sizeof m->from);
    decode_header_field_inplace(m->subject, sizeof m->subject);

    if (!m->from[0]) snprintf(m->from, sizeof m->from, "(unknown)");
    if (!m->subject[0])
        snprintf(m->subject, sizeof m->subject, "(no subject)");
}

static void parse_message_headers(Message *m) {
    FILE *f = fopen(m->path, "r");
    char line[MAX_LINE];
    size_t used = 0;
    size_t capacity = 0;
    char *raw_headers = NULL;

    if (!f) {
        parse_message_header_block(m, "", NULL, 0, NULL, 0);
        return;
    }

    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\n' || line[0] == '\r')
            break;
        append_body(&raw_headers, &used, &capacity, line);
    }
    fclose(f);

    parse_message_header_block(m, raw_headers, NULL, 0, NULL, 0);
    free(raw_headers);
}

static void parse_message_file(Message *m) {
    if (m->body_loaded)
        return;
    if (m->body) {
        m->body_loaded = 1;
        return;
    }
    m->body_loaded = 1;

    FILE *f = fopen(m->path, "r");
    if (!f) {
        m->body = strdup("(Could not open message.)\n");
        return;
    }

    char line[MAX_LINE];
    int in_headers = 1;

    size_t h_used = 0, h_cap = 0;
    size_t b_used = 0, b_cap = 0;
    char *raw_headers = NULL;
    char *raw_body = NULL;

    char root_ctype[512] = "";
    char root_cte[128] = "";

    while (fgets(line, sizeof line, f)) {
        if (in_headers) {
            if (line[0] == '\n' || line[0] == '\r') {
                in_headers = 0;
                continue;
            }
            append_body(&raw_headers, &h_used, &h_cap, line);
        } else {
            append_body(&raw_body, &b_used, &b_cap, line);
        }
    }

    fclose(f);

    parse_message_header_block(m, raw_headers,
                               root_ctype, sizeof root_ctype,
                               root_cte, sizeof root_cte);
    free(raw_headers);

    if (!raw_body) raw_body = strdup("");

    m->body = extract_mime_display_body(m, raw_body, root_ctype, root_cte);
    scan_attachments_fallback(m, raw_body);

    if (m->has_attachment && m->attachment_name[0] &&
        m->body && !strstr(m->body, "[Attachment:")) {
        size_t need = strlen(m->body) + strlen(m->attachment_name) + 32;
        char *joined = malloc(need);
        if (joined) {
            snprintf(joined, need, "%s%s[Attachment: %s]\n",
                     m->body,
                     m->body[0] ? "\n\n" : "",
                     m->attachment_name);
            free(m->body);
            m->body = joined;
        }
    }

    free(raw_body);

    if (!m->body || !m->body[0]) {
        free(m->body);
        m->body = strdup("(No displayable message body.)\n");
    }
}


static void load_dir_messages(const char *dir, int unread) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) && message_count < MAX_MESSAGES) {
        if (ent->d_name[0] == '.') continue;

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        if (!path_is_regular(path)) continue;

        Message *m = &messages[message_count++];
        memset(m, 0, sizeof *m);
        snprintf(m->path, sizeof m->path, "%s", path);
        m->unread = unread;
        parse_message_headers(m);
    }

    closedir(d);
}

static int message_order_cmp_newest_first(const void *aa, const void *bb) {
    const Message *a = (const Message *)aa;
    const Message *b = (const Message *)bb;

    time_t ta = a->order_time;
    time_t tb = b->order_time;

    if (ta > tb) return -1;
    if (ta < tb) return 1;

    return strcmp(b->path, a->path);
}

static void sort_messages_newest_first(void) {
    if (message_count > 1) {
        for (int i = 0; i < message_count; i++)
            (void)message_order_time(i);
        qsort(messages, message_count, sizeof messages[0], message_order_cmp_newest_first);
    }
}

static void load_current_mailbox(void) {
    free_messages();

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/new", mailboxes[current_mailbox].path);
    load_dir_messages(p, 1);

    snprintf(p, sizeof p, "%s/cur", mailboxes[current_mailbox].path);
    load_dir_messages(p, 0);

    sort_messages_newest_first();

    if (selected >= message_count) selected = message_count - 1;
    if (selected < 0) selected = 0;
}

static void load_all_mailboxes_for_thread(void) {
    free_messages();

    char p[PATH_MAX];

    for (int i = 0; i < mailbox_count; i++) {
        snprintf(p, sizeof p, "%s/new", mailboxes[i].path);
        load_dir_messages(p, 1);

        snprintf(p, sizeof p, "%s/cur", mailboxes[i].path);
        load_dir_messages(p, 0);
    }

    sort_messages_newest_first();

    if (selected >= message_count) selected = message_count - 1;
    if (selected < 0) selected = 0;
}


static int count_regular_files_in_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);

        if (path_is_regular(path)) count++;
    }

    closedir(d);
    return count;
}

static int mailbox_attention_count(int idx) {
    if (idx < 0 || idx >= mailbox_count) return 0;

    char path[PATH_MAX];

    if (simplemail_box_name_is_role(mailboxes[idx].name, "Inbox")) {
        snprintf(path, sizeof path, "%s/new", mailboxes[idx].path);
        return count_regular_files_in_dir(path);
    }

    if (simplemail_box_name_is_role(mailboxes[idx].name, "Drafts")) {
        snprintf(path, sizeof path, "%s/new", mailboxes[idx].path);
        return count_regular_files_in_dir(path);
    }

    return 0;
}

static int unread_count(void) {
    int n = 0;
    for (int i = 0; i < message_count; i++)
        if (messages[i].unread) n++;
    return n;
}

static void display_from(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    dst[0] = '\0';

    if (!src || !*src) {
        snprintf(dst, dstsz, "(unknown)");
        return;
    }

    const char *lt = strchr(src, '<');

    if (lt && lt > src) {
        size_t n = (size_t)(lt - src);
        if (n >= dstsz) n = dstsz - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
        trim(dst);

        if (dst[0] == '"' && strlen(dst) > 1) {
            memmove(dst, dst + 1, strlen(dst));
            size_t len = strlen(dst);
            if (len && dst[len - 1] == '"')
                dst[len - 1] = '\0';
        }

        trim(dst);
        if (dst[0])
            return;
    }

    snprintf(dst, dstsz, "%s", src);
    trim(dst);
}

static void draw_footer(const char *text) {
    int h, w;
    getmaxyx(stdscr, h, w);
    mvhline(h - 2, 0, ACS_HLINE, w);

    move(h - 1, 0);
    clrtoeol();

    if (pending_restore) {
        char msg[128];
        int n = selection_count();
        if (n > 1)
            snprintf(msg, sizeof msg, "Restore %d selected messages to Inbox? y/N", n);
        else
            snprintf(msg, sizeof msg, "Restore message to Inbox? y/N");
        mvaddnstr(h - 1, 1, msg, w - 2);
    } else if (pending_delete == 1) {
        char msg[128];
        int n = selection_count();
        if (n > 1)
            snprintf(msg, sizeof msg, "dD Delete %d selected", n);
        else
            snprintf(msg, sizeof msg, "dD Delete");
        mvaddnstr(h - 1, 1, msg, w - 2);
    } else if (pending_delete == 2) {
        char msg[128];
        int n = selection_count();
        if (n > 1)
            snprintf(msg, sizeof msg,
                     current_box_is("Trash")
                     ? "Permanently delete %d selected messages? y/N"
                     : "Move %d selected messages to Trash? y/N", n);
        else
            snprintf(msg, sizeof msg,
                     current_box_is("Trash")
                     ? "Permanently delete message? y/N"
                     : "Move message to Trash? y/N");
        mvaddnstr(h - 1, 1, msg, w - 2);
    }
    else if (pull_running) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 pull_first ? "First mail download running... you can keep moving." : "Checking mail in background...");
        mvaddnstr(h - 1, 1, msg, w - 2);
    } else
        mvaddnstr(h - 1, 1, text, w - 2);

    move(0, 0);
}

static int confirm_quit(void) {
    int h, w;
    getmaxyx(stdscr, h, w);

    timeout(-1);

    mvhline(h - 2, 0, ACS_HLINE, w);
    move(h - 1, 0);
    clrtoeol();
    mvaddnstr(h - 1, 1, "Quit SimpleMail? y/N", w - 2);
    refresh();

    int ans = getch();

    timeout(100);

    return ans == 'y' || ans == 'Y';
}


static int simplemail_has_sync_state(void) {
    char inbox[PATH_MAX];
    char path[PATH_MAX];

    if (!simplemail_join_path(mail_root, simplemail_role_box("Inbox"),
                              inbox, sizeof inbox))
        return 0;
    if (!simplemail_join_path(inbox, ".uidvalidity", path, sizeof path))
        return 0;
    if (access(path, F_OK) == 0) return 1;

    if (!simplemail_join_path(inbox, ".mbsyncstate", path, sizeof path))
        return 0;
    if (access(path, F_OK) == 0) return 1;

    return 0;
}

static int simplemail_pull_log_has_uid_error(void) {
    char path[PATH_MAX];
    if (!simplemail_pull_log_path(path, sizeof path))
        return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[1024];
    int found = 0;

    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "Maildir error: UID") &&
            strstr(line, "is beyond highest assigned UID")) {
            found = 1;
            break;
        }
    }

    fclose(f);
    return found;
}

static void simplemail_reset_mbsync_state(void) {
    char path[PATH_MAX];

    for (int i = 0; i < mailbox_count; i++) {
        snprintf(path, sizeof path, "%s/.uidvalidity", mailboxes[i].path);
        unlink(path);

        snprintf(path, sizeof path, "%s/.mbsyncstate", mailboxes[i].path);
        unlink(path);
    }
}

static int simplemail_run_sync_once(void) {
    char log_path[PATH_MAX];
    char cmd[PATH_MAX * 4 + 1024];

    if (!simplemail_pull_log_path(log_path, sizeof log_path))
        return -1;

    snprintf(cmd, sizeof cmd, "%s >",
             simplemail_sync_cmd[0] ? simplemail_sync_cmd : "mbsync inbox");
    shell_quote_append(cmd, sizeof cmd, log_path);
    strncat(cmd, " 2>&1 </dev/null", sizeof cmd - strlen(cmd) - 1);
    return system(cmd);
}

static void finish_pull_if_done(void) {
    if (!pull_running || pull_pid <= 0) return;

    int st = 0;
    pid_t got = waitpid(pull_pid, &st, WNOHANG);

    if (got == 0) return;
    if (got < 0) {
        pull_running = 0;
        pull_pid = 0;
        snprintf(status_msg, sizeof status_msg, "Mail pull ended oddly.");
        return;
    }

    pull_rc = st;
    pull_running = 0;
    pull_pid = 0;

    load_current_mailbox();

    if (pull_rc == 0)
        snprintf(status_msg, sizeof status_msg,
                 pull_first ? "First mail download complete." : "Mail checked.");
    else
        snprintf(status_msg, sizeof status_msg,
                 "Mail checked.");
}

static void pull_mail(void) {
    if (pull_running) {
        snprintf(status_msg, sizeof status_msg, "Mail pull already running...");
        return;
    }

    pull_first = !simplemail_has_sync_state();

    snprintf(status_msg, sizeof status_msg,
             pull_first ? "First mail download running... keep reading." : "Checking mail in background...");

    pull_pid = fork();

    if (pull_pid < 0) {
        pull_running = 0;
        pull_pid = 0;
        snprintf(status_msg, sizeof status_msg, "Could not start mail pull.");
        return;
    }

    if (pull_pid == 0) {
        int rc = simplemail_run_sync_once();

        if (rc != 0 && simplemail_pull_log_has_uid_error()) {
            simplemail_reset_mbsync_state();
            rc = simplemail_run_sync_once();
        }

        if (WIFEXITED(rc))
            _exit(WEXITSTATUS(rc));

        _exit(1);
    }

    pull_running = 1;
}


static void thread_key_for_subject(const char *subject, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    snprintf(out, outsz, "%s", subject ? subject : "");
    trim(out);

    int changed = 1;
    while (changed) {
        changed = 0;
        if (starts_case(out, "Re:")) {
            memmove(out, out + 3, strlen(out + 3) + 1);
            trim(out);
            changed = 1;
        } else if (starts_case(out, "Fw:")) {
            memmove(out, out + 3, strlen(out + 3) + 1);
            trim(out);
            changed = 1;
        } else if (starts_case(out, "Fwd:")) {
            memmove(out, out + 4, strlen(out + 4) + 1);
            trim(out);
            changed = 1;
        }
    }

    if (!out[0]) snprintf(out, outsz, "(no subject)");

    for (char *c = out; *c; c++)
        *c = (char)tolower((unsigned char)*c);
}

static int id_mentions(const char *haystack, const char *id) {
    return haystack && id && *haystack && *id && strstr(haystack, id) != NULL;
}

static int parent_of_message(int idx) {
    if (idx < 0 || idx >= message_count) return -1;

    Message *m = &messages[idx];

    if (m->in_reply_to[0]) {
        for (int i = 0; i < message_count; i++) {
            if (i == idx) continue;
            if (messages[i].message_id[0] &&
                id_mentions(m->in_reply_to, messages[i].message_id))
                return i;
        }
    }

    if (m->references[0]) {
        for (int i = message_count - 1; i >= 0; i--) {
            if (i == idx) continue;
            if (messages[i].message_id[0] &&
                id_mentions(m->references, messages[i].message_id))
                return i;
        }
    }

    return -1;
}

static int thread_root(int idx) {
    int seen[MAX_MESSAGES] = {0};

    while (idx >= 0 && idx < message_count && !seen[idx]) {
        seen[idx] = 1;

        int p = parent_of_message(idx);
        if (p < 0) break;

        idx = p;
    }

    return idx;
}

static int same_orphan_parent(int a, int b) {
    if (!messages[a].in_reply_to[0] || !messages[b].in_reply_to[0])
        return 0;

    return strcmp(messages[a].in_reply_to, messages[b].in_reply_to) == 0;
}

static int same_thread(int a, int b) {
    if (a < 0 || b < 0 || a >= message_count || b >= message_count) return 0;
    if (a == b) return 1;

    if (thread_root(a) == thread_root(b))
        return 1;

    if (same_orphan_parent(a, b))
        return 1;

    return 0;
}

static int thread_member_count(int idx) {
    int n = 0;
    for (int i = 0; i < message_count; i++)
        if (same_thread(i, idx))
            n++;
    return n;
}

static int thread_has_unread(int idx) {
    for (int i = 0; i < message_count; i++)
        if (same_thread(i, idx) && messages[i].unread)
            return 1;
    return 0;
}

static int thread_has_selection(int idx) {
    for (int i = 0; i < message_count; i++)
        if (same_thread(i, idx) && selected_flags[i])
            return 1;
    return 0;
}

static int build_thread_rows(int *rows, int *is_header, int max) {
    int n = 0;

    for (int i = 0; i < message_count && n < max; i++) {
        int seen = 0;
        for (int r = 0; r < n; r++) {
            if (is_header[r] && same_thread(rows[r], i)) {
                seen = 1;
                break;
            }
        }
        if (seen) continue;

        rows[n] = i;
        is_header[n] = 1;
        n++;

        /* Gmail-style model: list shows conversations only.
           Enter opens VIEW_THREAD instead of expanding inline. */
    }

    return n;
}

static int selected_visible_row(int *rows, int *is_header, int count) {
    for (int i = 0; i < count; i++) {
        if (rows[i] == selected && is_header[i] == selected_thread_header)
            return i;
    }

    for (int i = 0; i < count; i++) {
        if (same_thread(rows[i], selected) && is_header[i]) {
            selected = rows[i];
            selected_thread_header = 1;
            return i;
        }
    }

    return 0;
}

static void select_visible_row_expanded(int row) {
    int rows[MAX_MESSAGES * 2];
    int is_header[MAX_MESSAGES * 2];
    int count = build_thread_rows(rows, is_header, MAX_MESSAGES * 2);

    if (count <= 0) {
        selected = 0;
        selected_thread_header = 1;
        return;
    }

    if (row < 0) row = 0;
    if (row >= count) row = count - 1;

    selected = rows[row];
    selected_thread_header = is_header[row];
}

static void toggle_visible_selection(void) {
    int rows[MAX_MESSAGES * 2];
    int is_header[MAX_MESSAGES * 2];
    int count = build_thread_rows(rows, is_header, MAX_MESSAGES * 2);
    int row = selected_visible_row(rows, is_header, count);

    if (selected_thread_header) {
        int any_unselected = 0;

        for (int i = 0; i < message_count; i++) {
            if (same_thread(i, selected) && !selected_flags[i]) {
                any_unselected = 1;
                break;
            }
        }

        for (int i = 0; i < message_count; i++)
            if (same_thread(i, selected))
                selected_flags[i] = any_unselected;
    } else {
        selected_flags[selected] = !selected_flags[selected];
    }

    if (selection_count() == 0)
        select_anchor = -1;
    else if (select_anchor < 0)
        select_anchor = selected;

    if (row < count - 1)
        select_visible_row_expanded(row + 1);
}


static void draw_list(void) {
    finish_pull_if_done();
    erase();
    int h, w;
    getmaxyx(stdscr, h, w);

    int unread = unread_count();
    char title[512];
    snprintf(title, sizeof title, " SimpleMail - %s%s%d unread%s ",
             mailboxes[current_mailbox].name,
             unread ? " (" : "",
             unread,
             unread ? ")" : "");
    if (!unread) snprintf(title, sizeof title, " SimpleMail - %s ", mailboxes[current_mailbox].name);

    mvhline(0, 0, ACS_HLINE, w);
    mvaddnstr(0, 2, title, w - 4);

    int rows_map[MAX_MESSAGES * 2];
    int is_header[MAX_MESSAGES * 2];
    int view_count = build_thread_rows(rows_map, is_header, MAX_MESSAGES * 2);
    int selected_row = selected_visible_row(rows_map, is_header, view_count);

    int rows = h - 4;
    if (selected_row < list_top) list_top = selected_row;
    if (selected_row >= list_top + rows) list_top = selected_row - rows + 1;
    if (list_top < 0) list_top = 0;

    if (message_count == 0 || view_count == 0) {
        mvaddnstr(3, 2, "(No messages here.)", w - 4);
    } else {
        for (int y = 0; y < rows && list_top + y < view_count; y++) {
            int row = list_top + y;
            int idx = rows_map[row];
            Message *m = &messages[idx];

            char line[1024];
            char from[64];
            char subj[640];
            int tc = thread_member_count(idx);

            display_from(from, sizeof from, m->from);

            if (is_header[row]) {
                if (tc > 1)
                    snprintf(subj, sizeof subj, "%s (%d)", m->subject, tc);
                else
                    snprintf(subj, sizeof subj, "%s", m->subject);

                snprintf(line, sizeof line, "%c %-24.24s  %s",
                         thread_has_selection(idx) ? '*' : (thread_has_unread(idx) ? 'N' : ' '),
                         from,
                         subj);
            } else {
                snprintf(line, sizeof line, "%c   %-22.22s  %.620s",
                         selected_flags[idx] ? '*' : (m->unread ? 'N' : ' '),
                         from,
                         m->subject);
            }

            if (row == selected_row) attron(A_REVERSE);
            mvaddnstr(y + 2, 1, row == selected_row ? ">" : " ", 1);
            mvaddnstr(y + 2, 3, line, w - 4);
            if (row == selected_row) attroff(A_REVERSE);
        }
    }

    {
        int n = selection_count();
        char footer[256];

        if (n > 0) {
            if (current_box_is("Trash") || current_box_is("Archive"))
                snprintf(footer, sizeof footer, "%d selected  Enter Open/Expand  Space Select  u Restore  dD Delete  v All  V Invert  Esc Clear  q Quit", n);
            else
                snprintf(footer, sizeof footer, "%d selected  Enter Open/Expand  Space Select  a Archive  dD Delete  p Inbox  P Full  v All  V Invert  Esc Clear  q Quit", n);
            draw_footer(footer);
        } else if (current_box_is("Trash") || current_box_is("Archive")) {
            draw_footer("↑↓ Move  Enter Open/Expand  Space Select  u Restore  m Mailboxes  c Compose  v All  V Invert  / Search  q Quit");
        } else {
            if (status_msg[0]) {
                draw_footer(status_msg);
            } else {
                draw_footer("↑↓ Move  Enter Open/Expand  Space Select  m Mailboxes  c Compose  p Inbox  P Full  v All  V Invert  / Search  q Quit");
            }
        }
    }
    refresh();
}



static int render_is_quote_only_line(const char *line)
{
    const unsigned char *p = (const unsigned char *)(line ? line : "");

    while (*p && isspace(*p))
        p++;

    if (*p == '>')
        p++;

    while (*p && isspace(*p))
        p++;

    return *p == '\0';
}











static int simplemail_amazon_is_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

static int simplemail_amazon_is_alpha(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

static int simplemail_amazon_prefix_has_word(const char *s, int upto)
{
    int run = 0;

    if (!s || upto <= 0)
        return 0;

    for (int i = 0; s[i] && i < upto; i++) {
        unsigned char c = (unsigned char)s[i];

        if (simplemail_amazon_is_alpha(c)) {
            run++;
            if (run >= 3)
                return 1;
        } else {
            run = 0;
        }
    }

    return 0;
}

static int simplemail_amazon_find_short_token_soup(const char *s)
{
    const unsigned char *base = (const unsigned char *)(s ? s : "");
    const unsigned char *p = base;

    while (*p) {
        const unsigned char *candidate;
        const unsigned char *q;
        int tokens = 0;
        int short_tokens = 0;
        int one_letter_alpha = 0;
        int letters = 0;
        int big_gap = 0;
        int span = 0;

        while (*p && !simplemail_amazon_is_alnum(*p))
            p++;

        if (!*p)
            break;

        candidate = p;
        q = candidate;

        while (*q) {
            const unsigned char *tok;
            int len = 0;
            int gap = 0;

            while (*q && !simplemail_amazon_is_alnum(*q)) {
                if (isspace(*q))
                    gap++;
                q++;
            }

            if (!*q)
                break;

            tok = q;

            while (*q && simplemail_amazon_is_alnum(*q)) {
                len++;
                q++;
            }

            if (len <= 0)
                break;

            tokens++;

            if (len <= 2) {
                short_tokens++;
                letters += len;

                if (len == 1 && simplemail_amazon_is_alpha(*tok))
                    one_letter_alpha++;
            } else {
                break;
            }

            if (gap >= 2)
                big_gap = 1;

            span = (int)(q - candidate);

            /*
             * Amazon HTML sometimes collapses into spaced one/two-letter
             * fragments after URL/link cleanup. Plain prose like Moby Dick
             * will not look like this.
             */
            if (tokens >= 5 &&
                short_tokens >= 5 &&
                one_letter_alpha >= 3 &&
                letters >= 6 &&
                big_gap &&
                span >= tokens * 2)
                return (int)(candidate - base);
        }

        p = candidate + 1;
    }

    return -1;
}

static void simplemail_amazon_rstrip(char *s)
{
    size_t n;

    if (!s)
        return;

    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static void simplemail_amazon_scrub_line(char *s)
{
    int pos;

    if (!s || !*s)
        return;

    pos = simplemail_amazon_find_short_token_soup(s);
    if (pos < 0)
        return;

    if (!simplemail_amazon_prefix_has_word(s, pos)) {
        s[0] = '\0';
        return;
    }

    while (pos > 0 && isspace((unsigned char)s[pos - 1]))
        pos--;

    s[pos] = '\0';
    simplemail_amazon_rstrip(s);
}


static int render_should_omit_line(const char *line, int len) {
    char tmp[4096];
    int n = len;

    if (!line || len <= 0)
        return 0;

    if (n >= (int)sizeof tmp)
        n = (int)sizeof tmp - 1;

    memcpy(tmp, line, n);
    tmp[n] = '\0';
    trim(tmp);

    if (!tmp[0])
        return 0;


    if (render_is_quote_only_line(tmp))
        return 1;


    if (strlen(tmp) > 30 &&
        (!strncmp(tmp, "http://", 7) || !strncmp(tmp, "https://", 8)))
        return 1;

    if (simplemail_machine_token_line(tmp))
        return 1;

    if (looks_like_gibberish_line(tmp))
        return 1;

    return 0;
}

static int render_append(char **out, size_t *used, size_t *cap,
                         const char *text, size_t len)
{
    if (len > MAX_BODY)
        len = MAX_BODY;
    if (*used >= MAX_BODY)
        return 1;
    if (len > MAX_BODY - *used)
        len = MAX_BODY - *used;

    if (*used + len + 1 > *cap) {
        size_t n = *cap ? *cap : 4096;
        while (n < *used + len + 1) {
            if (n > ((size_t)-1) / 2)
                return 0;
            n *= 2;
            if (n > MAX_BODY + 1) {
                n = MAX_BODY + 1;
                break;
            }
        }
        if (n < *used + len + 1)
            return 0;
        char *grown = realloc(*out, n);
        if (!grown)
            return 0;
        *out = grown;
        *cap = n;
    }

    memcpy(*out + *used, text, len);
    *used += len;
    (*out)[*used] = '\0';
    return 1;
}


static int render_append_char(char **out, size_t *used, size_t *cap, char ch)
{
    return render_append(out, used, cap, &ch, 1);
}

static int render_line_starts_urlish(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;

    return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8);
}

static int render_line_has_markdown_link(const char *s)
{
    const char *p = s;

    while ((p = strchr(p, '[')) != NULL) {
        const char *close = strchr(p + 1, ']');

        if (close && close[1] == '(')
            return 1;

        p++;
    }

    return 0;
}

static char *render_clean_mail_line(const char *line, int len)
{
    char *tmp;
    char *p;
    char *out = NULL;
    size_t used = 0;
    size_t cap = 0;

    if (!line || len <= 0)
        return strdup("");

    tmp = malloc((size_t)len + 1);
    if (!tmp)
        return strdup("");

    memcpy(tmp, line, (size_t)len);
    tmp[len] = '\0';
    p = tmp;

    /*
     * HTML mail often arrives as quoted markdown-ish link soup:
     *
     *   > [Human label](huge-tracking-url)
     *
     * The reading view should keep the human label and drop the machinery.
     */
    {
        char *q = p;

        while (*q && isspace((unsigned char)*q))
            q++;

        if (*q == '>') {
            char *after = q + 1;

            if (*after == ' ')
                after++;

            if (render_line_has_markdown_link(after) ||
                render_line_starts_urlish(after) ||
                simplemail_machine_token_line(after) ||
                looks_like_gibberish_line(after)) {
                p = after;
            }
        }
    }

    while (*p) {
        if (*p == '[') {
            char *close = strchr(p + 1, ']');

            if (close && close[1] == '(') {
                char *end = strchr(close + 2, ')');

                if (end) {
                    int label_len = (int)(close - (p + 1));

                    if (label_len > 0) {
                        if (!render_append(&out, &used, &cap,
                                           p + 1, (size_t)label_len))
                            goto fail;
                    } else {
                        if (!render_append(&out, &used, &cap, "link", 4))
                            goto fail;
                    }

                    p = end + 1;
                    continue;
                }
            }
        }

        if (!render_append_char(&out, &used, &cap, *p))
            goto fail;

        p++;
    }

    free(tmp);

    if (!out)
        return strdup("");


    return out;

fail:
    free(tmp);
    free(out);
    return strdup("");
}

static char *render_body_text(const char *body)
{
    char *out = NULL;
    /* MIME extraction has already normalized the selected representation.
     * Reflowing it again here erased hard HTML breaks and indentation. */
    char *normalized = strdup(body ? body : "");
    size_t used = 0;
    size_t cap = 0;
    const char *p = normalized ? normalized : "";

    if (!*p) {
        free(normalized);
        return strdup("");
    }

    while (*p) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);

        char *clean = render_clean_mail_line(p, len);
        if (clean)
            simplemail_amazon_scrub_line(clean);
        int clean_len = clean ? (int)strlen(clean) : 0;

        if (clean && clean_len > 0 && !render_should_omit_line(clean, clean_len)) {
            if (!render_append(&out, &used, &cap, clean, (size_t)clean_len)) {
                free(clean);
                goto fail;
            }
        }

        free(clean);

        if (!e)
            break;
        if (!render_append(&out, &used, &cap, "\n", 1))
            goto fail;
        p = e + 1;
    }

    free(normalized);

    if (!out)
        return strdup("");
    return out;

fail:
    free(normalized);
    free(out);
    return strdup(body ? body : "");
}


static int simplemail_read_width(int screen_w)
{
    int width = screen_w - 4;

    if (width > 80)
        width = 80;
    if (width < 20)
        width = screen_w > 0 ? screen_w : 1;
    if (width < 1)
        width = 1;
    return width;
}

static int simplemail_read_left(int screen_w, int width)
{
    int left = (screen_w - width) / 2;

    if (left < 0)
        left = 0;
    return left;
}

static void simplemail_paint_cell(int y, int x, wchar_t wc, attr_t attrs, short pair)
{
    cchar_t cell;
    wchar_t out[2];

    out[0] = wc;
    out[1] = L'\0';
    setcchar(&cell, out, attrs, pair, NULL);
    mvadd_wch(y, x, &cell);
}

static void simplemail_fill_cells(int y, int left, int width, attr_t attrs)
{
    short pair = 0;
    int h, w;

    getmaxyx(stdscr, h, w);
    if (y < 0 || y >= h || width <= 0)
        return;
    if (left < 0) {
        width += left;
        left = 0;
    }
    if (left >= w || width <= 0)
        return;
    if (left + width > w)
        width = w - left;

    for (int col = 0; col < width; col++)
        simplemail_paint_cell(y, left + col, L' ', attrs, pair);
}

static void simplemail_clear_line(int y)
{
    int h, w;

    getmaxyx(stdscr, h, w);
    if (y < 0 || y >= h)
        return;
    simplemail_fill_cells(y, 0, w, A_NORMAL);
}

static void simplemail_put_cells(int y, int left, int width, const char *text, attr_t attrs)
{
    const char *p = text ? text : "";
    int col = 0;
    short pair = 0;
    int h, screen_w;

    getmaxyx(stdscr, h, screen_w);
    if (y < 0 || y >= h || width <= 0)
        return;
    if (left < 0) {
        width += left;
        left = 0;
    }
    if (left >= screen_w || width <= 0)
        return;
    if (left + width > screen_w)
        width = screen_w - left;

    /*
     * Own the whole row segment, including the quiet blank paper after text.
     * This is one of the SimpleWords tricks: no leftover terminal attributes
     * sitting in the unused cells around the prose.
     */
    simplemail_fill_cells(y, left, width, attrs);

    while (*p && col < width) {
        wchar_t wc;
        mbstate_t st;
        size_t n;
        int used;
        int glyph_w;

        if (*p == '\t') {
            int spaces = 4 - (col % 4);
            while (spaces-- > 0 && col < width)
                simplemail_paint_cell(y, left + col++, L' ', attrs, pair);
            p++;
            continue;
        }

        memset(&st, 0, sizeof st);
        n = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
            wc = L'\xfffd';
            used = 1;
        } else {
            used = (int)n;
        }

        glyph_w = wcwidth(wc);
        if (glyph_w < 1)
            glyph_w = 1;
        if (col + glyph_w > width)
            break;

        simplemail_paint_cell(y, left + col, wc, attrs, pair);
        col += glyph_w;
        p += used;
    }
}

static void simplemail_put_clipped(int y, int left, int width, const char *text)
{
    simplemail_put_cells(y, left, width, text, A_NORMAL);
}

static void draw_read_footer_at(int left, int width, const char *text)
{
    int h, w;

    getmaxyx(stdscr, h, w);
    (void)w;
    if (h < 1)
        return;

    simplemail_clear_line(h - 1);

    if (pending_restore) {
        char msg[128];
        int n = selection_count();

        if (n > 1)
            snprintf(msg, sizeof msg, "Restore %d selected messages to Inbox? y/N", n);
        else
            snprintf(msg, sizeof msg, "Restore message to Inbox? y/N");
        simplemail_put_clipped(h - 1, left, width, msg);
    } else if (pending_delete == 1) {
        char msg[128];
        int n = selection_count();

        if (n > 1)
            snprintf(msg, sizeof msg, "dD Delete %d selected", n);
        else
            snprintf(msg, sizeof msg, "dD Delete");
        simplemail_put_clipped(h - 1, left, width, msg);
    } else if (pending_delete == 2) {
        char msg[128];
        int n = selection_count();

        if (n > 1)
            snprintf(msg, sizeof msg, "Delete %d selected messages? y/N", n);
        else
            snprintf(msg, sizeof msg, "Delete message? y/N");
        simplemail_put_clipped(h - 1, left, width, msg);
    } else {
        simplemail_put_clipped(h - 1, left, width, text ? text : "");
    }
}

static int read_surface_matches(Message *m, int h, int w, int left, int width,
                                int body_top, int body_height)
{
    const char *key = m && m->path[0] ? m->path : "";

    return read_surface_valid &&
           read_surface_h == h &&
           read_surface_w == w &&
           read_surface_left == left &&
           read_surface_width == width &&
           read_surface_top == body_top &&
           read_surface_height == body_height &&
           !strcmp(read_surface_key, key);
}

static void remember_read_surface(Message *m, int h, int w, int left, int width,
                                  int body_top, int body_height)
{
    const char *key = m && m->path[0] ? m->path : "";

    read_surface_valid = 1;
    read_surface_h = h;
    read_surface_w = w;
    read_surface_left = left;
    read_surface_width = width;
    read_surface_top = body_top;
    read_surface_height = body_height;
    snprintf(read_surface_key, sizeof read_surface_key, "%s", key);
}

static void draw_read(void) {
    int h, w;
    int left;
    int body_width;
    int body_top;
    int max_y;
    int visible_rows;
    int old_cursor = curs_set(0);

    simplemail_ensure_read_renderer();

    getmaxyx(stdscr, h, w);
    body_width = simplemail_read_width(w);
    left = simplemail_read_left(w, body_width);

    if (message_count == 0 || selected < 0 || selected >= message_count) {
        erase();
        read_surface_valid = 0;
        ssr_deactivate(&read_renderer);
        simplemail_put_clipped(3, left, body_width, "(No message selected.)");
        draw_read_footer_at(left, body_width, "Backspace Back  q Quit");
        refresh();
        if (old_cursor >= 0)
            curs_set(old_cursor);
        return;
    }

    Message *m = &messages[selected];
    parse_message_file(m);
    body_top = m->date[0] ? 7 : 6;
    max_y = h - 2;
    visible_rows = max_y - body_top;
    if (visible_rows < 1)
        visible_rows = 1;

    /*
     * Desired-screen discipline for the mail reader: only rebuild the whole
     * read surface when the message or geometry changes. Plain scroll keys
     * leave the chrome alone and let SimpleRender reconcile the body rows.
     */
    if (!read_surface_matches(m, h, w, left, body_width, body_top, visible_rows)) {
        erase();
        ssr_invalidate(&read_renderer);
        remember_read_surface(m, h, w, left, body_width, body_top, visible_rows);
    }

    /* Quiet SimpleWords-like page chrome: no full-width ACS rails. */
    simplemail_put_clipped(1, left, body_width, m->subject[0] ? m->subject : "(no subject)");

    {
        char line[1024];
        snprintf(line, sizeof line, "From: %.900s", m->from);
        simplemail_put_clipped(3, left, body_width, line);

        if (m->date[0]) {
            snprintf(line, sizeof line, "Date: %.900s", m->date);
            simplemail_put_clipped(4, left, body_width, line);
        } else {
            simplemail_clear_line(4);
        }
    }

    simplemail_clear_line(body_top - 1);

    char *display_body = render_body_text(m->body);
    int total_rows = ssr_visual_rows(display_body ? display_body : "", body_width);
    int max_scroll = total_rows - visible_rows;
    if (max_scroll < 0)
        max_scroll = 0;
    if (read_scroll > max_scroll)
        read_scroll = max_scroll;
    if (read_scroll < 0)
        read_scroll = 0;

    if (current_box_is("Trash") || current_box_is("Archive"))
        draw_read_footer_at(left, body_width, m->has_attachment ?
                            "↑↓ Scroll  Backspace Inbox  o Open Attachment  s Save Attachment  u Restore  dD Delete  q Quit" :
                            "↑↓ Scroll  Backspace Inbox  u Restore  dD Delete  q Quit");
    else
        draw_read_footer_at(left, body_width, m->has_attachment ?
                            "↑↓ Scroll  Backspace Inbox  r Reply  o Open Attachment  s Save Attachment  a Archive  dD Delete  q Quit" :
                            "↑↓ Scroll  Backspace Inbox  r Reply  a Archive  dD Delete  q Quit");

    if (!ssr_render_text(&read_renderer, display_body ? display_body : "",
                         read_scroll, body_top, left, visible_rows, body_width, A_NORMAL))
        refresh();
    free(display_body);

    if (old_cursor >= 0)
        curs_set(old_cursor);
}


static void draw_read_body_only(void)
{
    int h, w;
    int left;
    int body_width;
    int body_top;
    int max_y;
    int visible_rows;

    if (message_count == 0 || selected < 0 || selected >= message_count)
        return;

    Message *m = &messages[selected];
    parse_message_file(m);

    getmaxyx(stdscr, h, w);
    body_width = simplemail_read_width(w);
    left = simplemail_read_left(w, body_width);
    body_top = m->date[0] ? 7 : 6;
    max_y = h - 2;
    visible_rows = max_y - body_top;
    if (visible_rows < 1)
        visible_rows = 1;

    if (!read_surface_matches(m, h, w, left, body_width, body_top, visible_rows)) {
        draw_read();
        return;
    }

#ifdef __GNUC__
    /*
     * If the body-window helper exists from the recent patch, use it.
     * If it does not, this still compiles only after that helper has landed.
     */
#endif
    simplemail_ensure_read_renderer();

    char *display_body = render_body_text(m->body);
    int total_rows = ssr_visual_rows(display_body ? display_body : "", body_width);
    int max_scroll = total_rows - visible_rows;

    if (max_scroll < 0)
        max_scroll = 0;
    if (read_scroll > max_scroll)
        read_scroll = max_scroll;
    if (read_scroll < 0)
        read_scroll = 0;

    ssr_render_text(&read_renderer, display_body ? display_body : "",
                    read_scroll, body_top, left, visible_rows,
                    body_width, A_NORMAL);

    free(display_body);
}


static void draw_mailbox_overlay(void) {
    read_surface_valid = 0;
    erase();
    int w = getmaxx(stdscr);

    mvhline(0, 0, ACS_HLINE, w);
    mvaddnstr(0, 2, " Mailboxes ", w - 4);

    int start_y = 3;
    for (int i = 0; i < mailbox_count; i++) {
        if (i == selected_mailbox) attron(A_REVERSE);
        mvaddnstr(start_y + i, 2, i == selected_mailbox ? ">" : " ", 1);
        char label[256];
        int attention = mailbox_attention_count(i);
        if (attention > 0)
            snprintf(label, sizeof label, "%.127s (%d)", mailboxes[i].name, attention);
        else
            snprintf(label, sizeof label, "%.127s", mailboxes[i].name);

        mvaddnstr(start_y + i, 4, label, w - 6);
        if (i == selected_mailbox) attroff(A_REVERSE);
    }

    draw_footer("↑↓ Move  Enter Select  Esc/m Return  q Quit");
    refresh();
}


static void prompt_line_prefill(const char *label, const char *prefill, char *out, size_t outsz) {
    if (!out || outsz == 0) return;

    snprintf(out, outsz, "%s", prefill ? prefill : "");

    echo();
    curs_set(1);

    int h, w;
    getmaxyx(stdscr, h, w);

    size_t len = strlen(out);
    int pos = (int)len;

    while (1) {
        mvhline(h - 3, 0, ' ', w);
        mvprintw(h - 3, 1, "%s", label);
        mvaddnstr(h - 3, (int)strlen(label) + 2, out, w - (int)strlen(label) - 3);
        move(h - 3, (int)strlen(label) + 2 + pos);
        refresh();

        finish_pull_if_done();
        int ch = getch();
        finish_pull_if_done();
        if (ch == ERR) continue;

        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
            break;

        if (ch == 27) {
            out[0] = '\0';
            break;
        }

        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && pos > 0) {
            memmove(out + pos - 1, out + pos, strlen(out + pos) + 1);
            pos--;
            continue;
        }

        if (ch == KEY_LEFT && pos > 0) {
            pos--;
            continue;
        }

        if (ch == KEY_RIGHT && pos < (int)strlen(out)) {
            pos++;
            continue;
        }

        if (isprint(ch) && strlen(out) + 1 < outsz) {
            memmove(out + pos + 1, out + pos, strlen(out + pos) + 1);
            out[pos++] = (char)ch;
        }
    }

    noecho();
    curs_set(0);
    trim(out);
}

static void prompt_line(const char *label, char *out, size_t outsz) {
    prompt_line_prefill(label, "", out, outsz);
}

static void make_message_id(char *out, size_t outsz) {
    char host[128] = "simplemail.local";
    gethostname(host, sizeof host);
    host[sizeof(host) - 1] = '\0';

    snprintf(out, outsz, "<simplemail.%ld.%ld@%s>",
             (long)time(NULL), (long)getpid(), host);
}

static void write_header_line(FILE *out, const char *name,
                              const char *value, const char *fallback) {
    const char *s = value && *value ? value : fallback;

    fprintf(out, "%s: ", name);
    for (; s && *s; s++) {
        if (*s == '\r' || *s == '\n')
            fputc(' ', out);
        else
            fputc(*s, out);
    }
    fputc('\n', out);
}

static void build_reply_references(const Message *m, char *out, size_t outsz) {
    out[0] = '\0';
    if (!m || outsz == 0) return;

    if (m->references[0])
        snprintf(out, outsz, "%s", m->references);

    if (m->message_id[0] && !strstr(out, m->message_id)) {
        size_t used = strlen(out);
        snprintf(out + used, outsz - used, "%s%s",
                 used ? " " : "", m->message_id);
    }

    trim(out);
}


static int run_editor_on_file(const char *path) {
    const char *editor = simplemail_editor_cmd[0] ? simplemail_editor_cmd : "simplewords";

    def_prog_mode();
    endwin();

    pid_t pid = fork();
    if (pid < 0) {
        reset_prog_mode();
        refresh();
        return -1;
    }

    if (pid == 0) {
        if (strstr(editor, "simplewords"))
            unsetenv("SIMPLEWORDS_AUTOSAVE_ON_EXIT");

        execlp(editor, editor, path, (char *)NULL);
        execlp("nano", "nano", path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    reset_prog_mode();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(100);
    intrflush(stdscr, FALSE);
    leaveok(stdscr, FALSE);
    scrollok(stdscr, FALSE);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    attrset(A_NORMAL);
    wbkgdset(stdscr, (chtype)' ' | A_NORMAL);
    curs_set(0);
    clear();
    touchwin(stdscr);
    refresh();

    return status;
}


static void save_sent_copy_from_file(const char *src_path) {
    if (!src_path || !*src_path) return;

    char sentbox[PATH_MAX];
    char sentdir[PATH_MAX];
    if (!simplemail_join_path(mail_root, simplemail_role_box("Sent"),
                              sentbox, sizeof sentbox) ||
        !simplemail_join_path(sentbox, "cur", sentdir, sizeof sentdir))
        return;

    time_t now = time(NULL);
    char outpath[PATH_MAX];

    for (int tries = 0; tries < 10000; tries++) {
        char filename[96];
        int written = snprintf(filename, sizeof filename, "%ld-%ld-%d.eml",
                               (long)now, (long)getpid(), tries);

        if (!simplemail_snprintf_ok(written, sizeof filename) ||
            !simplemail_join_path(sentdir, filename, outpath, sizeof outpath))
            return;

        if (path_is_regular(outpath))
            continue;

        FILE *in = fopen(src_path, "rb");
        FILE *out = fopen(outpath, "wb");

        if (!in || !out) {
            if (in) fclose(in);
            if (out) fclose(out);
            return;
        }

        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);

        fclose(in);
        fclose(out);
        return;
    }
}


static const char *simplemail_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

static void draw_compose_review(const char *to, const char *subject, const char *attachment_path) {
    int h, w;
    getmaxyx(stdscr, h, w);

    erase();
    mvaddnstr(1, 2, "Compose", w - 4);
    mvhline(2, 0, ACS_HLINE, w);

    mvprintw(4, 2, "To: %.200s", to && *to ? to : "(unset)");
    mvprintw(5, 2, "Subject: %.200s", subject && *subject ? subject : "(no subject)");

    if (attachment_path && *attachment_path)
        mvprintw(7, 2, "Attachment: %.200s", simplemail_basename(attachment_path));
    else
        mvaddnstr(7, 2, "Attachment: none", w - 4);

    mvhline(h - 3, 0, ACS_HLINE, w);
    move(h - 2, 0);
    clrtoeol();
    mvaddnstr(h - 2, 1, "Ready to send.", w - 2);

    move(h - 1, 0);
    clrtoeol();
    mvaddnstr(h - 1, 1, "y Send    e Edit body    a Attach/change    v Save Draft    d Discard", w - 2);

    move(0, 0);
    refresh();
}

static int prompt_yes_no_footer(const char *msg) {
    int h, w;
    getmaxyx(stdscr, h, w);

    timeout(-1);
    noecho();
    curs_set(0);

    move(h - 1, 0);
    clrtoeol();
    mvaddnstr(h - 1, 1, msg, w - 2);
    refresh();

    int ch = getch();

    timeout(100);

    return ch == 'y' || ch == 'Y';
}

static int pick_attachment(char *out, size_t outsz) {
    if (!out || outsz == 0) return 0;
    out[0] = '\0';

    char pickfile[PATH_MAX];
    char state[PATH_MAX];
    char filename[96];
    if (!simplemail_state_dir(state, sizeof state))
        return 0;
    int written = snprintf(filename, sizeof filename, "attach-pick-%ld-%ld",
                           (long)getpid(), (long)time(NULL));
    if (!simplemail_snprintf_ok(written, sizeof filename) ||
        !simplemail_join_path(state, filename, pickfile, sizeof pickfile))
        return 0;
    unlink(pickfile);

    def_prog_mode();
    endwin();

    pid_t pid = fork();
    if (pid == 0) {
        execlp("simplefiles", "simplefiles", "--pick", pickfile, (char *)NULL);
        _exit(127);
    }

    int st = 0;
    if (pid > 0)
        waitpid(pid, &st, 0);

    reset_prog_mode();
    refresh();
    curs_set(0);
    noecho();

    FILE *f = fopen(pickfile, "r");
    if (!f) {
        unlink(pickfile);
        return 0;
    }

    if (!fgets(out, outsz, f))
        out[0] = '\0';

    fclose(f);
    unlink(pickfile);

    trim(out);

    if (!out[0] || access(out, R_OK) != 0) {
        out[0] = '\0';
        return 0;
    }

    return 1;
}

static void write_base64_file(FILE *out, const char *path) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    FILE *in = fopen(path, "rb");
    if (!in) return;

    unsigned char buf[57];

    while (1) {
        size_t n = fread(buf, 1, sizeof buf, in);
        if (n == 0) break;

        for (size_t i = 0; i < n; i += 3) {
            unsigned int a = buf[i];
            unsigned int b = (i + 1 < n) ? buf[i + 1] : 0;
            unsigned int c = (i + 2 < n) ? buf[i + 2] : 0;

            fputc(tbl[a >> 2], out);
            fputc(tbl[((a & 3) << 4) | (b >> 4)], out);
            fputc((i + 1 < n) ? tbl[((b & 15) << 2) | (c >> 6)] : '=', out);
            fputc((i + 2 < n) ? tbl[c & 63] : '=', out);
        }

        fputc('\n', out);
    }

    fclose(in);
}

static int write_outbound_message_file(const char *out_path,
                                       const char *to,
                                       const char *subject,
                                       const char *body_path,
                                       const char *attachment_path,
                                       const char *in_reply_to,
                                       const char *references) {
    FILE *out = fopen(out_path, "w");
    if (!out) return -1;

    const char *from = simplemail_from[0] ? simplemail_from : "simplemail@localhost";

    char mid[256];
    make_message_id(mid, sizeof mid);

    write_header_line(out, "From", from, "simplemail@localhost");
    write_header_line(out, "To", to, "");
    write_header_line(out, "Subject", subject, "(no subject)");
    write_header_line(out, "Message-ID", mid, "");
    if (in_reply_to && *in_reply_to)
        write_header_line(out, "In-Reply-To", in_reply_to, "");
    if (references && *references)
        write_header_line(out, "References", references, "");
    fprintf(out, "MIME-Version: 1.0\n");

    char boundary[128] = "";
    if (attachment_path && *attachment_path) {
        snprintf(boundary, sizeof boundary, "simplemail-boundary-%ld-%ld", (long)getpid(), (long)time(NULL));
        fprintf(out, "Content-Type: multipart/mixed; boundary=\"%s\"\n", boundary);
        fprintf(out, "\n");
        fprintf(out, "--%s\n", boundary);
        fprintf(out, "Content-Type: text/plain; charset=UTF-8\n");
        fprintf(out, "Content-Transfer-Encoding: 8bit\n\n");
    } else {
        fprintf(out, "Content-Type: text/plain; charset=UTF-8\n");
        fprintf(out, "Content-Transfer-Encoding: 8bit\n\n");
    }

    FILE *in = fopen(body_path, "r");
    if (in) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
        fclose(in);
    }

    if (attachment_path && *attachment_path) {
        const char *name = simplemail_basename(attachment_path);
        fprintf(out, "\n--%s\n", boundary);
        fprintf(out, "Content-Type: application/octet-stream; name=\"%s\"\n", name);
        fprintf(out, "Content-Transfer-Encoding: base64\n");
        fprintf(out, "Content-Disposition: attachment; filename=\"%s\"\n\n", name);
        write_base64_file(out, attachment_path);
        fprintf(out, "--%s--\n", boundary);
    }

    fclose(out);
    return 0;
}

static int send_mail_msmtp_attach_ex(const char *to, const char *subject, const char *body_path,
                                     const char *attachment_path,
                                     const char *in_reply_to, const char *references) {
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "/tmp/simplemail-send-XXXXXX");

    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    close(fd);

    if (write_outbound_message_file(tmpl, to, subject, body_path, attachment_path,
                                    in_reply_to, references) != 0) {
        unlink(tmpl);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        unlink(tmpl);
        return -1;
    }

    if (pid == 0) {
        if (!freopen(tmpl, "r", stdin)) _exit(127);
        execlp("sh", "sh", "-c",
               simplemail_send_cmd[0] ? simplemail_send_cmd : "msmtp -t",
               (char *)NULL);
        _exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        save_sent_copy_from_file(tmpl);
        unlink(tmpl);
        return 0;
    }

    unlink(tmpl);
    return -1;
}

static void save_draft_record_ex(const char *to, const char *subject, const char *body_path,
                                 const char *in_reply_to, const char *references) {
    char drafts_box[PATH_MAX];
    char drafts[PATH_MAX];
    if (!simplemail_join_path(mail_root, simplemail_role_box("Drafts"),
                              drafts_box, sizeof drafts_box) ||
        !simplemail_join_path(drafts_box, "new", drafts, sizeof drafts))
        return;

    time_t now = time(NULL);
    char filename[64];
    char out[PATH_MAX];
    int written = snprintf(filename, sizeof filename, "draft-%ld.eml", (long)now);
    if (!simplemail_snprintf_ok(written, sizeof filename) ||
        !simplemail_join_path(drafts, filename, out, sizeof out))
        return;

    FILE *in = fopen(body_path, "r");
    FILE *f = fopen(out, "w");
    if (!f) {
        if (in) fclose(in);
        return;
    }

    char mid[256];
    make_message_id(mid, sizeof mid);

    write_header_line(f, "From", "SimpleMail User", "");
    write_header_line(f, "To", to, "(unset)");
    write_header_line(f, "Subject", subject, "(no subject)");
    write_header_line(f, "Message-ID", mid, "");
    if (in_reply_to && *in_reply_to)
        write_header_line(f, "In-Reply-To", in_reply_to, "");
    if (references && *references)
        write_header_line(f, "References", references, "");
    fprintf(f, "Date: draft\n");
    fprintf(f, "\n");

    if (in) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, f);
        fclose(in);
    }

    fclose(f);
}

static void save_draft_record(const char *to, const char *subject, const char *body_path) {
    save_draft_record_ex(to, subject, body_path, NULL, NULL);
}

static int start_background_send(const char *to, const char *subject,
                                 const char *body_path,
                                 const char *attachment_path,
                                 const char *in_reply_to,
                                 const char *references) {
    if (send_running) {
        snprintf(status_msg, sizeof status_msg, "A message is already sending.");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        int result;

        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        result = send_mail_msmtp_attach_ex(
            to, subject, body_path, attachment_path,
            in_reply_to, references);
        if (result != 0)
            save_draft_record_ex(to, subject, body_path,
                                 in_reply_to, references);
        unlink(body_path);
        _exit(result == 0 ? 0 : 1);
    }
    if (pid < 0) {
        snprintf(status_msg, sizeof status_msg, "Could not start mail send.");
        return 0;
    }

    send_pid = pid;
    send_running = 1;
    snprintf(status_msg, sizeof status_msg, "Sending mail in background...");
    return 1;
}

static void finish_send_if_done(void) {
    int status = 0;
    pid_t result;

    if (!send_running || send_pid <= 0)
        return;
    do {
        result = waitpid(send_pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0)
        return;

    send_pid = 0;
    send_running = 0;
    if (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        snprintf(status_msg, sizeof status_msg, "Mail sent.");
    else
        snprintf(status_msg, sizeof status_msg,
                 "Send failed; saved draft.");
}

static void compose_new(void) {
    int send_started = 0;

    if (send_running) {
        snprintf(status_msg, sizeof status_msg,
                 "Wait for the current message to finish sending.");
        return;
    }
    ssr_deactivate(&read_renderer);

    char to[512] = {0};
    char subject[512] = {0};
    char attachment_path[PATH_MAX] = {0};

    prompt_line("To:", to, sizeof to);
    if (!to[0]) return;

    prompt_line("Subject:", subject, sizeof subject);

    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "/tmp/simplemail-compose-XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) return;

    FILE *f = fdopen(fd, "w");
    if (f) {
        fprintf(f, "\n");
        fclose(f);
    } else {
        close(fd);
    }

    run_editor_on_file(tmpl);

    draw_compose_review(to, subject, attachment_path);
    if (prompt_yes_no_footer("Attach a file? y/N")) {
        if (pick_attachment(attachment_path, sizeof attachment_path))
            snprintf(status_msg, sizeof status_msg, "Attachment added: %.120s", simplemail_basename(attachment_path));
        else
            snprintf(status_msg, sizeof status_msg, "No attachment selected.");
    }

    draw_compose_review(to, subject, attachment_path);

    int ch;
    while ((ch = getch())) {
        if (ch == 'y' || ch == 'Y') {
            if (start_background_send(to, subject, tmpl, attachment_path,
                                      NULL, NULL)) {
                send_started = 1;
            } else {
                save_draft_record(to, subject, tmpl);
                snprintf(status_msg, sizeof status_msg,
                         "Could not start send; saved draft.");
            }
            break;
        } else if (ch == 'v' || ch == 'V') {
            save_draft_record(to, subject, tmpl);
            break;
        } else if (ch == 'e' || ch == 'E') {
            run_editor_on_file(tmpl);
            draw_compose_review(to, subject, attachment_path);
        } else if (ch == 'a' || ch == 'A') {
            if (pick_attachment(attachment_path, sizeof attachment_path))
                snprintf(status_msg, sizeof status_msg, "Attachment added: %.120s", simplemail_basename(attachment_path));
            else
                snprintf(status_msg, sizeof status_msg, "No attachment selected.");
            draw_compose_review(to, subject, attachment_path);
        } else if (ch == 'd' || ch == 'D' || ch == 'q') {
            break;
        }
    }

    curs_set(0);
    noecho();
    if (!send_started)
        unlink(tmpl);
    load_current_mailbox();
}


static void reply_current(void) {
    int send_started = 0;

    if (send_running) {
        snprintf(status_msg, sizeof status_msg,
                 "Wait for the current message to finish sending.");
        return;
    }
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    ssr_deactivate(&read_renderer);

    Message *m = &messages[selected];

    char to[512];
    snprintf(to, sizeof to, "%s", m->from);

    char subject[512];
    if (starts_case(m->subject, "Re:")) snprintf(subject, sizeof subject, "%s", m->subject);
    else snprintf(subject, sizeof subject, "Re: %s", m->subject);

    char attachment_path[PATH_MAX] = {0};

    char in_reply_to[256] = "";
    char references[1024] = "";

    if (m->message_id[0])
        snprintf(in_reply_to, sizeof in_reply_to, "%s", m->message_id);
    build_reply_references(m, references, sizeof references);

    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "/tmp/simplemail-reply-XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) return;

    FILE *f = fdopen(fd, "w");
    if (f) {
        fprintf(f, "\n");
        fclose(f);
    } else {
        close(fd);
    }

    run_editor_on_file(tmpl);

    draw_compose_review(to, subject, attachment_path);
    if (prompt_yes_no_footer("Attach a file? y/N")) {
        if (pick_attachment(attachment_path, sizeof attachment_path))
            snprintf(status_msg, sizeof status_msg, "Attachment added: %.120s", simplemail_basename(attachment_path));
        else
            snprintf(status_msg, sizeof status_msg, "No attachment selected.");
    }

    draw_compose_review(to, subject, attachment_path);

    int ch;
    while ((ch = getch())) {
        if (ch == 'y' || ch == 'Y') {
            if (start_background_send(to, subject, tmpl, attachment_path,
                                      in_reply_to, references)) {
                send_started = 1;
            } else {
                save_draft_record_ex(to, subject, tmpl, in_reply_to, references);
                snprintf(status_msg, sizeof status_msg,
                         "Could not start send; saved draft.");
            }
            break;
        } else if (ch == 'v' || ch == 'V') {
            save_draft_record_ex(to, subject, tmpl, in_reply_to, references);
            break;
        } else if (ch == 'e' || ch == 'E') {
            run_editor_on_file(tmpl);
            draw_compose_review(to, subject, attachment_path);
        } else if (ch == 'a' || ch == 'A') {
            if (pick_attachment(attachment_path, sizeof attachment_path))
                snprintf(status_msg, sizeof status_msg, "Attachment added: %.120s", simplemail_basename(attachment_path));
            else
                snprintf(status_msg, sizeof status_msg, "No attachment selected.");
            draw_compose_review(to, subject, attachment_path);
        } else if (ch == 'd' || ch == 'D' || ch == 'q') {
            break;
        }
    }

    curs_set(0);
    noecho();
    if (!send_started)
        unlink(tmpl);
}



static void move_selected_or_current_to(const char *boxname);

static int move_file_to_mailbox(const char *src, const char *boxname) {
    char destbox[PATH_MAX];
    char destdir[PATH_MAX];
    char dest[PATH_MAX];
    const char *role_box = simplemail_role_box(boxname);

    if (!simplemail_join_path(mail_root, role_box, destbox, sizeof destbox) ||
        !simplemail_join_path(destbox, "cur", destdir, sizeof destdir))
        return -1;

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    for (int tries = 0; tries < 10000; tries++) {
        char filename[PATH_MAX];
        int written = snprintf(filename, sizeof filename, "%ld-%ld-%d-",
                               (long)time(NULL), (long)getpid(), tries);
        size_t base_len = strlen(base);

        if (!simplemail_snprintf_ok(written, sizeof filename) ||
            base_len >= sizeof filename - (size_t)written)
            return -1;
        memcpy(filename + written, base, base_len + 1);
        if (!simplemail_join_path(destdir, filename, dest, sizeof dest))
            return -1;

        if (rename(src, dest) == 0)
            return 0;

        if (errno != EEXIST)
            return -1;
    }

    return -1;
}

static void move_selected_or_current_to(const char *boxname) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    int n = selection_count();

    if (n <= 0) {
        move_current_message_to(boxname);
        return;
    }

    int old_selected = selected;
    int moved = 0;
    int failed = 0;

    for (int i = message_count - 1; i >= 0; i--) {
        if (selected_flags[i]) {
            if (move_file_to_mailbox(messages[i].path, boxname) == 0)
                moved++;
            else
                failed++;
        }
    }

    load_current_mailbox();

    if (message_count <= 0)
        selected = 0;
    else if (old_selected >= message_count)
        selected = message_count - 1;
    else
        selected = old_selected;

    clear_selection();

    if (failed)
        snprintf(status_msg, sizeof status_msg, "Moved %d, failed %d.", moved, failed);
    else if (moved == 1)
        snprintf(status_msg, sizeof status_msg, "Moved 1 message.");
    else
        snprintf(status_msg, sizeof status_msg, "Moved %d messages.", moved);

    if (view == VIEW_READ) view = VIEW_LIST;
}


static void move_current_message_to(const char *boxname) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    int old_selected = selected;

    move_file_to_mailbox(messages[selected].path, boxname);
    load_current_mailbox();

    if (message_count <= 0) {
        selected = 0;
    } else if (old_selected >= message_count) {
        selected = message_count - 1;
    } else {
        selected = old_selected;
    }

    if (view == VIEW_READ) view = VIEW_LIST;
}

static void permanently_delete_selected_or_current(void) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    int n = selection_count();
    int old_selected = selected;
    int deleted = 0;
    int failed = 0;

    if (n <= 0) {
        if (unlink(messages[selected].path) == 0)
            deleted++;
        else
            failed++;
    } else {
        for (int i = message_count - 1; i >= 0; i--) {
            if (selected_flags[i]) {
                if (unlink(messages[i].path) == 0)
                    deleted++;
                else
                    failed++;
            }
        }
    }

    load_current_mailbox();

    if (message_count <= 0)
        selected = 0;
    else if (old_selected >= message_count)
        selected = message_count - 1;
    else
        selected = old_selected;

    clear_selection();

    if (failed)
        snprintf(status_msg, sizeof status_msg, "Deleted %d, failed %d.", deleted, failed);
    else if (deleted == 1)
        snprintf(status_msg, sizeof status_msg, "Deleted 1 message permanently.");
    else
        snprintf(status_msg, sizeof status_msg, "Deleted %d messages permanently.", deleted);

    if (view == VIEW_READ) view = VIEW_LIST;
}

static void delete_current_message(void) {
    int made_thread_selection = 0;

    if ((view == VIEW_LIST || view == VIEW_THREAD) &&
        selection_count() == 0 &&
        message_count > 0 &&
        selected >= 0 &&
        selected < message_count) {

        for (int i = 0; i < message_count; i++) {
            if (same_thread(i, selected)) {
                selected_flags[i] = 1;
                made_thread_selection = 1;
            }
        }
    }

    if (current_box_is("Trash"))
        permanently_delete_selected_or_current();
    else
        move_selected_or_current_to("Trash");

    if (made_thread_selection)
        clear_selection();
}

static void archive_current_message(void) {
    move_selected_or_current_to("Archive");
}

static int current_box_is(const char *name) {
    return current_mailbox >= 0 &&
           current_mailbox < mailbox_count &&
           simplemail_box_name_is_role(mailboxes[current_mailbox].name, name);
}

static void restore_current_message(void) {
    if (current_box_is("Trash") || current_box_is("Archive"))
        move_selected_or_current_to("Inbox");
}

static int handle_restore_sequence(int ch) {
    if (!pending_restore) {
        if ((ch == 'u' || ch == 'U') &&
            (current_box_is("Trash") || current_box_is("Archive"))) {
            pending_restore = 1;
            pending_delete = 0;
            return 1;
        }
        return 0;
    }

    pending_restore = 0;

    if (ch == 'y' || ch == 'Y')
        restore_current_message();

    return 1;
}


static int handle_delete_sequence(int ch) {
    if (pending_delete == 0) {
        if (ch == 'd') {
            pending_delete = 1;
            pending_restore = 0;
            return 1;
        }
        return 0;
    }

    if (pending_delete == 1) {
        if (ch == 'D') {
            pending_delete = 2;
            return 1;
        }
        pending_delete = 0;
        return 1;
    }

    if (pending_delete == 2) {
        pending_delete = 0;
        if (ch == 'y' || ch == 'Y')
            delete_current_message();
        return 1;
    }

    pending_delete = 0;
    return 1;
}

static void mark_current_message_read(void) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;
    if (!messages[selected].unread) return;

    char *hit = strstr(messages[selected].path, "/new/");
    if (!hit) {
        messages[selected].unread = 0;
        return;
    }

    char dest[PATH_MAX];
    size_t prefix_len = (size_t)(hit - messages[selected].path);

    if (prefix_len + 5 + strlen(hit + 5) >= sizeof dest) {
        messages[selected].unread = 0;
        return;
    }

    memcpy(dest, messages[selected].path, prefix_len);
    dest[prefix_len] = '\0';
    strcat(dest, "/cur/");
    strcat(dest, hit + 5);

    if (rename(messages[selected].path, dest) == 0) {
        snprintf(messages[selected].path, sizeof messages[selected].path, "%s", dest);
    }

    messages[selected].unread = 0;
}


static int find_thread_anchor_after_reload(const char *mid, const char *subj) {
    if (mid && *mid) {
        for (int i = 0; i < message_count; i++) {
            if (messages[i].message_id[0] && strcmp(messages[i].message_id, mid) == 0)
                return i;
        }
    }

    if (subj && *subj) {
        char want[512], got[512];
        thread_key_for_subject(subj, want, sizeof want);

        for (int i = 0; i < message_count; i++) {
            thread_key_for_subject(messages[i].subject, got, sizeof got);
            if (strcmp(want, got) == 0)
                return i;
        }
    }

    return message_count > 0 ? 0 : -1;
}


static int message_is_ancestor(int maybe_parent, int child) {
    int seen[MAX_MESSAGES] = {0};

    while (child >= 0 && child < message_count && !seen[child]) {
        seen[child] = 1;
        int p = parent_of_message(child);
        if (p < 0) return 0;
        if (p == maybe_parent) return 1;
        child = p;
    }

    return 0;
}

static long long leading_number_in_basename(const char *path) {
    if (!path) return 0;

    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;

    long long n = 0;
    int any = 0;

    while (*b && isdigit((unsigned char)*b)) {
        any = 1;
        n = n * 10 + (*b - '0');
        b++;
    }

    return any ? n : 0;
}

static time_t message_order_time(int idx) {
    if (idx < 0 || idx >= message_count) return 0;
    if (messages[idx].order_time_loaded)
        return messages[idx].order_time;

    time_t result = 0;

    long long fn = leading_number_in_basename(messages[idx].path);
    if (fn > 1000000000LL) {
        result = (time_t)fn;
        goto done;
    }

    char d[256];
    snprintf(d, sizeof d, "%s", messages[idx].date);
    trim(d);

    char *paren = strchr(d, '(');
    if (paren) {
        while (paren > d && isspace((unsigned char)paren[-1])) paren--;
        *paren = '\0';
    }

    struct tm tmv;
    const char *formats[] = {
        "%a, %d %b %Y %H:%M:%S %z",
        "%d %b %Y %H:%M:%S %z",
        "%a, %d %b %Y %H:%M:%S",
        "%d %b %Y %H:%M:%S"
    };

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        memset(&tmv, 0, sizeof tmv);
        tmv.tm_isdst = -1;
        char *end = strptime(d, formats[i], &tmv);
        if (end) {
            result = mktime(&tmv);
            goto done;
        }
    }

    struct stat st;
    if (stat(messages[idx].path, &st) == 0)
        result = st.st_mtime;

done:
    messages[idx].order_time = result;
    messages[idx].order_time_loaded = 1;
    return result;
}


static int conversation_order_before(int a, int b) {
    if (message_is_ancestor(a, b)) return 1;
    if (message_is_ancestor(b, a)) return 0;

    time_t ta = message_order_time(a);
    time_t tb = message_order_time(b);

    if (ta < tb) return 1;
    if (ta > tb) return 0;

    return a < b;
}

static void sort_thread_members_gmailish(int *out, int n) {
    for (int pass = 0; pass < n; pass++) {
        for (int i = 0; i + 1 < n; i++) {
            if (!conversation_order_before(out[i], out[i + 1])) {
                int tmp = out[i];
                out[i] = out[i + 1];
                out[i + 1] = tmp;
            }
        }
    }
}


static int collect_thread_members(int root, int *out, int max) {
    int n = 0;

    if (thread_anchor >= 0 && thread_anchor < message_count)
        root = thread_anchor;

    for (int i = 0; i < message_count && n < max; i++) {
        if (same_thread(i, root))
            out[n++] = i;
    }

    sort_thread_members_gmailish(out, n);

    return n;
}

static void draw_thread(void) {
    erase();

    int h, w;
    getmaxyx(stdscr, h, w);

    int members[MAX_MESSAGES];
    int count = collect_thread_members(selected, members, MAX_MESSAGES);

    if (count <= 0) {
        view = VIEW_LIST;
        return;
    }

    if (thread_cursor < 0) thread_cursor = 0;
    if (thread_cursor >= count) thread_cursor = count - 1;

    mvhline(0, 0, ACS_HLINE, w);

    char title[700];
    snprintf(title, sizeof title, " Conversation - %.560s (%d) ",
             messages[members[0]].subject, count);
    mvaddnstr(0, 2, title, w - 4);

    int y = 2;

    for (int i = 0; i < count && y < h - 4; i++) {
        Message *m = &messages[members[i]];
        parse_message_file(m);

        char from[80];
        display_from(from, sizeof from, m->from);

        char line[1024];
        snprintf(line, sizeof line, "%c %-24.24s  %.700s",
                 m->unread ? 'N' : ' ',
                 from,
                 m->subject);

        if (i == thread_cursor) attron(A_REVERSE);
        mvaddnstr(y, 1, i == thread_cursor ? ">" : " ", 1);
        mvaddnstr(y, 3, line, w - 4);
        if (i == thread_cursor) attroff(A_REVERSE);

        y++;

        if (m->body && m->body[0] && y < h - 4) {
            char preview[900];
            snprintf(preview, sizeof preview, "%.850s", m->body);
            char *nl = strchr(preview, '\n');
            if (nl) *nl = '\0';

            mvaddnstr(y, 6, preview, w - 8);
            y++;
        } else if (y < h - 4) {
            mvaddnstr(y, 6, "(No body.)", w - 8);
            y++;
        }

        y++;
    }

    draw_footer("↑↓ Move  Enter Open Message  r Reply  a Archive  dD Delete  Backspace Inbox  q Quit");
    refresh();
}

static void handle_thread_key(int ch) {
    if (handle_delete_sequence(ch)) return;

    int members[MAX_MESSAGES];
    int count = collect_thread_members(selected, members, MAX_MESSAGES);

    if (count <= 0) {
        view = VIEW_LIST;
        return;
    }

    if (thread_cursor < 0) thread_cursor = 0;
    if (thread_cursor >= count) thread_cursor = count - 1;

    int page = LINES - 6;
    if (page < 1) page = 1;

    if (ch == KEY_UP && thread_cursor > 0) {
        thread_cursor--;
    } else if (ch == KEY_DOWN && thread_cursor < count - 1) {
        thread_cursor++;
    } else if (ch == KEY_PPAGE) {
        thread_cursor -= page;
        if (thread_cursor < 0) thread_cursor = 0;
    } else if (ch == KEY_NPAGE) {
        thread_cursor += page;
        if (thread_cursor >= count) thread_cursor = count - 1;
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        selected = members[thread_cursor];
        mark_current_message_read();
        read_return_view = VIEW_THREAD;
        view = VIEW_READ;
        read_scroll = 0;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (thread_all_boxes_loaded) {
            thread_all_boxes_loaded = 0;
            thread_anchor = -1;
            load_current_mailbox();
        }
        view = VIEW_LIST;
    } else if (ch == 'r' || ch == 'R') {
        selected = members[thread_cursor];
        reply_current();
    } else if (ch == 'a' || ch == 'A') {
        selected = members[thread_cursor];
        archive_current_message();
    }
}


static void handle_list_key(int ch) {
    if (!mailbox_overlay && handle_restore_sequence(ch)) return;
    if (!mailbox_overlay && handle_delete_sequence(ch)) return;

    if (mailbox_overlay) {
        if (ch == KEY_UP && selected_mailbox > 0) selected_mailbox--;
        else if (ch == KEY_DOWN && selected_mailbox < mailbox_count - 1) selected_mailbox++;
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            current_mailbox = selected_mailbox;
            selected_thread_header = 1;
            mailbox_overlay = 0;
            load_current_mailbox();
        } else if (ch == 27 || ch == 'm' || ch == 'M') {
            mailbox_overlay = 0;
        }
        return;
    }

    if (ch == ' ') {
        toggle_visible_selection();
    } else if (ch == 'v') {
        select_all_messages();
    } else if (ch == 'V') {
        invert_message_selection();
    } else if (ch == 27) {
        clear_selection();
    } else if (ch == KEY_UP || ch == KEY_DOWN ||
               ch == KEY_PPAGE || ch == KEY_NPAGE) {
        int rows[MAX_MESSAGES * 2];
        int is_header[MAX_MESSAGES * 2];
        int count = build_thread_rows(rows, is_header, MAX_MESSAGES * 2);
        int row = selected_visible_row(rows, is_header, count);
        int page = LINES - 4;
        if (page < 1) page = 1;

        if (ch == KEY_UP) row--;
        else if (ch == KEY_DOWN) row++;
        else if (ch == KEY_PPAGE) row -= page;
        else if (ch == KEY_NPAGE) row += page;

        select_visible_row_expanded(row);
    }
    else if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && message_count > 0) {
        if (thread_member_count(selected) > 1) {
            char anchor_mid[256];
            char anchor_subj[512];

            snprintf(anchor_mid, sizeof anchor_mid, "%s", messages[selected].message_id);
            snprintf(anchor_subj, sizeof anchor_subj, "%s", messages[selected].subject);

            selected_thread_header = 0;
            thread_cursor = 0;

            load_all_mailboxes_for_thread();
            thread_all_boxes_loaded = 1;

            thread_anchor = find_thread_anchor_after_reload(anchor_mid, anchor_subj);
            if (thread_anchor >= 0)
                selected = thread_anchor;

            view = VIEW_THREAD;
        } else {
            selected_thread_header = 0;
            mark_current_message_read();
            read_return_view = VIEW_LIST;
            view = VIEW_READ;
            read_scroll = 0;
        }
    } else if (ch == 'm' || ch == 'M') {
        selected_mailbox = current_mailbox;
        mailbox_overlay = 1;
    } else if (ch == 'a' || ch == 'A') {
        archive_current_message();
    } else if (ch == 'p' || ch == 'P') {
        pull_mail();
    } else if (ch == 'c' || ch == 'C') {
        compose_new();
    } else if ((ch == 'u' || ch == 'U') &&
               (current_box_is("Trash") || current_box_is("Archive"))) {
        pending_restore = 1;
        pending_delete = 0;
    }
}



static void expand_user_path(const char *in, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = '\0';

    if (!in || !*in) return;

    if (!simplemail_expand_home_vars(in, out, outsz))
        snprintf(out, outsz, "%s", in);
}

static int copy_file_bytes(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    char buf[8192];
    size_t n;
    int ok = 0;

    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = -1;
            break;
        }
    }

    if (ferror(in)) ok = -1;
    fclose(in);
    if (fclose(out) != 0) ok = -1;
    return ok;
}

static void save_current_attachment(void) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    Message *m = &messages[selected];
    parse_message_file(m);
    if (!m->has_attachment || !m->attachment_path[0]) {
        snprintf(status_msg, sizeof status_msg, "No attachment.");
        return;
    }

    ssr_deactivate(&read_renderer);

    char def[PATH_MAX];
    snprintf(def, sizeof def, "~/Downloads/%s",
             m->attachment_name[0] ? m->attachment_name : "attachment.bin");

    char typed[PATH_MAX];
    prompt_line_prefill("Save attachment as:", def, typed, sizeof typed);

    if (!typed[0]) {
        snprintf(status_msg, sizeof status_msg, "Save canceled.");
        return;
    }

    char expanded[PATH_MAX];
    expand_user_path(typed, expanded, sizeof expanded);

    if (copy_file_bytes(m->attachment_path, expanded) == 0)
        snprintf(status_msg, sizeof status_msg, "Saved attachment: %.180s", expanded);
    else
        snprintf(status_msg, sizeof status_msg, "Save failed: %.180s", expanded);

}


static void open_current_attachment(void) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    Message *m = &messages[selected];
    parse_message_file(m);
    if (!m->has_attachment || !m->attachment_path[0]) {
        snprintf(status_msg, sizeof status_msg, "No attachment.");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        pid_t opener = fork();

        if (opener < 0)
            _exit(127);
        if (opener > 0)
            _exit(0);
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execlp("xdg-open", "xdg-open", m->attachment_path, (char *)NULL);
        execlp("gio", "gio", "open", m->attachment_path, (char *)NULL);
        execlp("open", "open", m->attachment_path, (char *)NULL);
        _exit(127);
    }

    if (pid < 0) {
        snprintf(status_msg, sizeof status_msg, "Could not start attachment opener.");
        return;
    }
    int opener_status = 0;
    while (waitpid(pid, &opener_status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(opener_status) || WEXITSTATUS(opener_status) != 0) {
        snprintf(status_msg, sizeof status_msg, "Could not start attachment opener.");
        return;
    }

    snprintf(status_msg, sizeof status_msg, "Opening attachment: %.180s", m->attachment_name);
}


static void handle_read_key(int ch) {
    if (handle_restore_sequence(ch)) return;
    if (handle_delete_sequence(ch)) return;

    int max_scroll = 0;
    int page = 1;

    if (message_count > 0 && selected >= 0 && selected < message_count) {
        int h, w;
        int body_width;
        int body_top;
        int visible_rows;

        getmaxyx(stdscr, h, w);
        body_width = simplemail_read_width(w);
        body_top = messages[selected].date[0] ? 7 : 6;
        visible_rows = (h - 2) - body_top;
        if (visible_rows < 1)
            visible_rows = 1;

        parse_message_file(&messages[selected]);
        char *display_body = render_body_text(messages[selected].body);
        int total_rows = ssr_visual_rows(display_body ? display_body : "", body_width);
        free(display_body);
        max_scroll = total_rows - visible_rows;
        if (max_scroll < 0)
            max_scroll = 0;
        page = visible_rows;
    }

    if (page < 1)
        page = 1;

    if (ch == KEY_UP && read_scroll > 0) read_scroll--;
    else if (ch == KEY_DOWN && read_scroll < max_scroll) read_scroll++;
    else if (ch == KEY_PPAGE) {
        read_scroll -= page;
        if (read_scroll < 0) read_scroll = 0;
    } else if (ch == KEY_NPAGE) {
        read_scroll += page;
        if (read_scroll > max_scroll) read_scroll = max_scroll;
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        view = read_return_view;
        read_surface_valid = 0;
        if (view == VIEW_LIST && thread_all_boxes_loaded) {
            thread_all_boxes_loaded = 0;
            thread_anchor = -1;
            load_current_mailbox();
        }
    } else if (ch == 'o' || ch == 'O') {
        open_current_attachment();
    } else if (ch == 's' || ch == 'S') {
        save_current_attachment();
    } else if (ch == 'r' || ch == 'p' || ch == 'P') {
        reply_current();
    } else if (ch == 'a' || ch == 'A') {
        archive_current_message();
    } else if ((ch == 'u' || ch == 'U') &&
               (current_box_is("Trash") || current_box_is("Archive"))) {
        pending_restore = 1;
        pending_delete = 0;
    }
}

int main(void) {
    setlocale(LC_ALL, "");

    load_simplemail_config();
    if (!init_paths())
        return 1;
    init_mailboxes();
    load_current_mailbox();

    use_extended_names(TRUE);
    initscr();
    set_escdelay(25);
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(100);
    intrflush(stdscr, FALSE);
    leaveok(stdscr, FALSE);
    scrollok(stdscr, FALSE);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    attrset(A_NORMAL);
    wbkgdset(stdscr, (chtype)' ' | A_NORMAL);
    curs_set(0);
    simplemail_ensure_read_renderer();

    int running = 1;
    int dirty = 1;

    while (running) {
        if (dirty) {
            if (mailbox_overlay) {
                read_surface_valid = 0;
                ssr_deactivate(&read_renderer);
                draw_mailbox_overlay();
            } else if (view == VIEW_READ) {
                draw_read();
            } else if (view == VIEW_THREAD) {
                read_surface_valid = 0;
                ssr_deactivate(&read_renderer);
                draw_thread();
            } else {
                read_surface_valid = 0;
                ssr_deactivate(&read_renderer);
                draw_list();
            }
            dirty = 0;
        }

        int ch = getch();

        if (ch == ERR) {
            /*
             * Keep background mail checking alive without repainting the
             * whole screen on every idle timeout.
             */
            if (pull_running) {
                int was_running = pull_running;
                finish_pull_if_done();
                if (was_running && !pull_running)
                    dirty = 1;
            }
            if (send_running) {
                int was_running = send_running;
                finish_send_if_done();
                if (was_running && !send_running)
                    dirty = 1;
            }
            continue;
        }

        dirty = 1;

        if (status_msg[0] && !pull_running && !send_running)
            status_msg[0] = '\0';

        if (ch == 'q' || ch == 'Q') {
            if (confirm_quit()) {
                running = 0;
                break;
            }
            continue;
        }

        if (view == VIEW_READ && !mailbox_overlay) {
            int old_scroll = read_scroll;
            int old_pending_delete = pending_delete;
            int old_pending_restore = pending_restore;
            View old_view = view;
            int scroll_key = ch == KEY_UP || ch == KEY_DOWN ||
                             ch == KEY_PPAGE || ch == KEY_NPAGE;

            handle_read_key(ch);

            if (scroll_key &&
                view == old_view &&
                pending_delete == old_pending_delete &&
                pending_restore == old_pending_restore &&
                read_scroll != old_scroll) {
                draw_read_body_only();
                dirty = 0;
            }
        } else if (view == VIEW_THREAD && !mailbox_overlay) {
            handle_thread_key(ch);
        } else {
            handle_list_key(ch);
        }
    }

    if (read_renderer_ready)
        ssr_destroy(&read_renderer);
    endwin();
    free_messages();
    return 0;
}
