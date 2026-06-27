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
 * Build:
 *   cc -O2 -Wall -Wextra -std=c99 simplemail.c -lncursesw -o simplemail
 */

#define _XOPEN_SOURCE 700

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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

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
static int expanded_threads[MAX_MESSAGES];
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
static int pending_delete = 0;
static int pending_restore = 0;

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

static void toggle_current_selection(void) {
    if (message_count <= 0 || selected < 0 || selected >= message_count) return;

    selected_flags[selected] = !selected_flags[selected];

    if (selected_flags[selected] && select_anchor < 0)
        select_anchor = selected;

    if (selection_count() == 0)
        select_anchor = -1;

    if (selected < message_count - 1)
        selected++;
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
    memset(expanded_threads, 0, sizeof expanded_threads);
    selected_thread_header = 1;
}


static void strip_newsletter_footer(char *s) {
    if (!s) return;

    char *p;

    p = strcasestr(s, "\nunsubscribe\n");
    if (!p) p = strcasestr(s, "\nunsubscribe\r\n");
    if (!p) p = strcasestr(s, "\nmanage subscription");
    if (!p) p = strcasestr(s, "\nmanage preferences");
    if (!p) p = strcasestr(s, "\nemail preferences");
    if (!p) p = strcasestr(s, "\nupdate preferences");

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
    if (!dst || dstsz == 0 || !src) return;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", src);
    strip_optional_quotes(tmp);
    if (tmp[0]) snprintf(dst, dstsz, "%s", tmp);
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
    ssize_t n;
    char *slash;

    if (!out || outsz == 0) return 0;
    out[0] = '\0';

    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = '\0';

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
    snprintf(out, outsz, "%s/attachments", cache);
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
        snprintf(path, sizeof path, "%s/config", dir);
        write_default_simplemail_config(path);
        f = fopen(path, "r");
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
    snprintf(p, sizeof p, "%s/cur", base); ensure_dir(p);
    snprintf(p, sizeof p, "%s/new", base); ensure_dir(p);
    snprintf(p, sizeof p, "%s/tmp", base); ensure_dir(p);
}

static void write_sample_mail(void) {
    char inbox[PATH_MAX], sample[PATH_MAX];
    snprintf(inbox, sizeof inbox, "%s/%s", mail_root, simplemail_role_box("Inbox"));
    make_maildir(inbox);
    snprintf(sample, sizeof sample, "%s/new/sample-simplemail.eml", inbox);

    struct stat st;
    if (stat(sample, &st) == 0) return;

    FILE *f = fopen(sample, "w");
    if (!f) return;

    fprintf(f,
        "From: SimpleMail Draft <simplemail@localhost>\n"
        "To: Keelan\n"
        "Subject: SimpleMail first boot\n"
        "Date: Tue, 23 Jun 2026 19:00:00 -0400\n"
        "Content-Type: text/plain; charset=UTF-8\n"
        "\n"
        "SimpleMail is alive.\n"
        "\n"
        "This is only the local-reader draft: mailbox list, reading screen,\n"
        "mailbox overlay, compose handoff, and reply handoff.\n"
        "\n"
        "Next comes real account setup: IMAP, SMTP, and ProtonBridge done sanely.\n"
    );

    fclose(f);
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
        snprintf(p, sizeof p, "%s/%s", mail_root, simplemail_box_name_at(i));
        make_maildir(p);
    }

    return 1;
}

static void init_mailboxes(void) {
    mailbox_count = 0;

    for (size_t i = 0; i < 5 && mailbox_count < MAX_MAILBOXES; i++) {
        snprintf(mailboxes[mailbox_count].name, sizeof mailboxes[mailbox_count].name, "%s", simplemail_box_name_at(i));
        snprintf(mailboxes[mailbox_count].path, sizeof mailboxes[mailbox_count].path, "%s/%s", mail_root, simplemail_box_name_at(i));
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

static char *decode_transfer_text(const char *src, const char *cte) {
    if (cte && find_case(cte, "base64"))
        return decode_base64_text(src);
    if (cte && find_case(cte, "quoted-printable"))
        return decode_quoted_printable_text(src);
    return strdup(src ? src : "");
}

static void append_char(char **out, size_t *used, size_t *cap, char c);
static void append_text(char **out, size_t *used, size_t *cap, const char *text);

static char *decode_rfc2047_q(const char *src) {
    char *out = malloc(strlen(src) + 1);
    if (!out) return strdup(src ? src : "");

    size_t j = 0;
    for (size_t i = 0; src[i]; i++) {
        if (src[i] == '_') {
            out[j++] = ' ';
        } else if (src[i] == '=' && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
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
                append_text(&out, &used, &cap, decoded);
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

static int extract_param_value(const char *s, const char *name, char *out, size_t outsz) {
    const char *p = find_case(s, name);
    if (!p || outsz == 0) return 0;

    p += strlen(name);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    char quote = 0;
    if (*p == '"' || *p == '\'') {
        quote = *p;
        p++;
    }

    size_t n = 0;
    while (*p && n + 1 < outsz) {
        if (quote) {
            if (*p == quote) break;
        } else {
            if (*p == ';' || *p == '\r' || *p == '\n') break;
        }
        out[n++] = *p++;
    }

    out[n] = '\0';
    trim(out);
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

    if (!is_attach && !find_case(ctype, "name=")) {
        free(copy);
        return 0;
    }

    if (!extract_param_value(cdisp, "filename", out, outsz))
        extract_param_value(ctype, "name", out, outsz);

    if (!out[0])
        snprintf(out, outsz, "attachment.bin");

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
    snprintf(tmpl, sizeof tmpl, "%s/attachment-XXXXXX", dir);

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

    if (cp == 8216 || cp == 8217) { append_char(out, used, cap, '\''); return; }
    if (cp == 8220 || cp == 8221) { append_char(out, used, cap, '"'); return; }
    if (cp == 8211 || cp == 8212) { append_char(out, used, cap, '-'); return; }
    if (cp == 8230) { append_text(out, used, cap, "..."); return; }

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
    *advance = 0;

    if (!strncmp(s, "&amp;", 5))  { append_char(out, used, cap, '&'); *advance = 5; return 1; }
    if (!strncmp(s, "&lt;", 4))   { append_char(out, used, cap, '<'); *advance = 4; return 1; }
    if (!strncmp(s, "&gt;", 4))   { append_char(out, used, cap, '>'); *advance = 4; return 1; }
    if (!strncmp(s, "&quot;", 6)) { append_char(out, used, cap, '"'); *advance = 6; return 1; }
    if (!strncmp(s, "&apos;", 6)) { append_char(out, used, cap, '\''); *advance = 6; return 1; }
    if (!strncmp(s, "&nbsp;", 6)) { append_char(out, used, cap, ' '); *advance = 6; return 1; }

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


static int tag_starts(const char *tag, const char *name) {
    while (*tag && isspace((unsigned char)*tag)) tag++;
    if (*tag == '/') tag++;
    while (*name) {
        if (tolower((unsigned char)*tag) != tolower((unsigned char)*name))
            return 0;
        tag++;
        name++;
    }
    return *tag == '\0' || isspace((unsigned char)*tag) || *tag == '/' || *tag == '>';
}

static int extract_href(const char *tag, char *out, size_t outsz) {
    const char *p = find_case(tag, "href");
    if (!p || outsz == 0) return 0;
    p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    char quote = 0;
    if (*p == '"' || *p == '\'') quote = *p++;

    size_t n = 0;
    while (*p && n + 1 < outsz) {
        if (quote) {
            if (*p == quote) break;
        } else {
            if (isspace((unsigned char)*p) || *p == '>') break;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return out[0] != '\0';
}

static char *html_to_text(const char *html) {
    char *out = NULL;
    size_t used = 0, cap = 0;
    char href[1024] = "";
    int in_anchor = 0;
    int last_space = 0;
    int suppress_leading_li_ws = 0;
    int list_is_ordered[16] = {0};
    int list_counter[16] = {0};
    int list_depth = 0;

    for (size_t i = 0; html && html[i]; i++) {
        if (html[i] == '<') {
            const char *end = strchr(html + i, '>');
            if (!end) break;

            size_t tl = (size_t)(end - (html + i + 1));
            char tag[1024];
            if (tl >= sizeof tag) tl = sizeof tag - 1;
            memcpy(tag, html + i + 1, tl);
            tag[tl] = '\0';
            trim(tag);

            if (tag_starts(tag, "style") || tag_starts(tag, "script")) {
                const char *close = tag_starts(tag, "style") ? find_case(end + 1, "</style>") : find_case(end + 1, "</script>");
                if (close) {
                    i = (size_t)(close - html) + (tag_starts(tag, "style") ? 7 : 8);
                    continue;
                }
            }

            int closing_tag = tag[0] == '/';

            if (tag_starts(tag, "br")) {
                append_char(&out, &used, &cap, '\n');
                last_space = 1;
            } else if ((!closing_tag && tag_starts(tag, "p")) || tag_starts(tag, "/p") ||
                       (!closing_tag && tag_starts(tag, "div")) || tag_starts(tag, "/div") ||
                       (!closing_tag && tag_starts(tag, "h1")) ||
                       (!closing_tag && tag_starts(tag, "h2")) ||
                       (!closing_tag && tag_starts(tag, "h3")) ||
                       (!closing_tag && tag_starts(tag, "h4")) ||
                       (!closing_tag && tag_starts(tag, "h5")) ||
                       (!closing_tag && tag_starts(tag, "h6"))) {
                if (suppress_leading_li_ws && !closing_tag && tag_starts(tag, "p")) {
                    suppress_leading_li_ws = 1;
                    last_space = 0;
                } else {
                    append_char(&out, &used, &cap, '\n');
                    append_char(&out, &used, &cap, '\n');
                    last_space = 1;
                }
            } else if (!closing_tag && tag_starts(tag, "ol")) {
                if (list_depth < 15) {
                    list_depth++;
                    list_is_ordered[list_depth] = 1;
                    list_counter[list_depth] = 0;
                }
                append_char(&out, &used, &cap, '\n');
                last_space = 1;
            } else if (!closing_tag && tag_starts(tag, "ul")) {
                if (list_depth < 15) {
                    list_depth++;
                    list_is_ordered[list_depth] = 0;
                    list_counter[list_depth] = 0;
                }
                append_char(&out, &used, &cap, '\n');
                last_space = 1;
            } else if (tag_starts(tag, "/ol") || tag_starts(tag, "/ul")) {
                if (list_depth > 0) list_depth--;
                append_char(&out, &used, &cap, '\n');
                last_space = 1;
            } else if (!closing_tag && tag_starts(tag, "li")) {
                append_char(&out, &used, &cap, '\n');

                for (int d = 1; d < list_depth; d++)
                    append_text(&out, &used, &cap, "  ");

                if (list_depth > 0 && list_is_ordered[list_depth]) {
                    char num[32];
                    list_counter[list_depth]++;
                    snprintf(num, sizeof num, "%d. ", list_counter[list_depth]);
                    append_text(&out, &used, &cap, num);
                } else {
                    append_text(&out, &used, &cap, "• ");
                }

                last_space = 0;
                suppress_leading_li_ws = 1;
            }

            if (tag_starts(tag, "a")) {
                href[0] = '\0';
                if (extract_href(tag, href, sizeof href))
                    in_anchor = 1;
            } else if (tag_starts(tag, "/a")) {
                /* Keep anchor text, but do not print newsletter tracking URLs. */
                in_anchor = 0;
                href[0] = '\0';
            }

            i = (size_t)(end - html);
            continue;
        }

        if (suppress_leading_li_ws && isspace((unsigned char)html[i]))
            continue;

        suppress_leading_li_ws = 0;

        /* Shorten naked URLs that newsletters print directly into the body. */
        if ((i == 0 || isspace((unsigned char)html[i - 1]) || html[i - 1] == '(') &&
            (!strncmp(html + i, "http://", 7) || !strncmp(html + i, "https://", 8))) {
            size_t start = i;
            size_t len = 0;

            while (html[i] &&
                   !isspace((unsigned char)html[i]) &&
                   html[i] != ')' &&
                   html[i] != '"' &&
                   html[i] != '\'' &&
                   html[i] != '<') {
                i++;
                len++;
            }

            for (size_t k = 0; k < len && k < 30; k++)
                append_char(&out, &used, &cap, html[start + k]);

            if (len > 30)
                append_text(&out, &used, &cap, "...");

            if (html[i] == ')' || html[i] == '"' || html[i] == '\'')
                continue;

            if (html[i] == '\0')
                break;
        }

        if (html[i] == '&') {
            size_t adv = 0;
            if (consume_html_entity(html + i, &out, &used, &cap, &adv)) {
                if (adv > 0) i += adv - 1;
            } else {
                append_char(&out, &used, &cap, '&');
            }
            last_space = 0;
            continue;
        }

        if (isspace((unsigned char)html[i])) {
            if (html[i] == '\n' || html[i] == '\r') {
                if (used > 0 && out[used - 1] != '\n')
                    append_char(&out, &used, &cap, '\n');
                last_space = 1;
            } else if (!last_space) {
                append_char(&out, &used, &cap, ' ');
                last_space = 1;
            }
        } else {
            append_char(&out, &used, &cap, html[i]);
            last_space = 0;
        }
    }

    if (!out) return strdup("");
    trim(out);
    strip_tracking_gibberish(out);
    return out;
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



static void newsletter_plaintext_spacing_inplace(char *s) {
    if (!s) return;

    const char *markers[] = {
        "To dive right in,",
        "I've got an audio version",
        "Consider upgrading",
        "With the free subscription",
        "An invite to",
        "Full access",
        "A year free",
        "Many readers expense",
        "I guarantee",
        "Upgrade to paid",
        "P.S.",
        "Reply to this email",
        "Move the email",
        "Private podcast setup:",
        "To set up your podcast app,",
        "Unsubscribe"
    };

    char *out = calloc(strlen(s) * 2 + 512, 1);
    if (!out) return;

    size_t j = 0;
    int at_line_start = 1;

    for (size_t i = 0; s[i]; i++) {
        if (at_line_start) {
            for (size_t k = 0; k < sizeof(markers) / sizeof(markers[0]); k++) {
                size_t ml = strlen(markers[k]);
                if (!strncmp(s + i, markers[k], ml)) {
                    if (j >= 2 && !(out[j-1] == '\n' && out[j-2] == '\n'))
                        out[j++] = '\n';
                    break;
                }
            }
        }

        out[j++] = s[i];
        at_line_start = s[i] == '\n';
    }

    out[j] = '\0';
    snprintf(s, strlen(s) * 2 + 512, "%s", out);
    free(out);
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

    if (find_case(root_ctype, "x-simplemail-attachment") ||
        find_case(root_ctype, "Content-Disposition: attachment") ||
        (!find_case(root_ctype, "text/") && !extract_boundary(root_ctype, boundary, sizeof boundary))) {
        return strdup("");
    }

    if (extract_boundary(root_ctype, boundary, sizeof boundary)) {
        char marker[320];
        snprintf(marker, sizeof marker, "--%s", boundary);

        const char *p = raw_body;
        char *fallback_html = NULL;
        char attachment_lines[1024] = "";

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
                free(fallback_html);
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

            if (extract_boundary(ctype, boundary, sizeof boundary)) {
                char *nested = extract_mime_display_body(m, part, ctype, cte);
                free(part);

                if (nested && nested[0]) {
                    if (find_case(ctype, "multipart/alternative") ||
                        find_case(nested, "http://") ||
                        find_case(nested, "https://") ||
                        !find_case(nested, "<style")) {
                        free(fallback_html);
                        return nested;
                    }
                }

                free(nested);
                p = next;
                continue;
            }

            if (find_case(ctype, "text/plain")) {
                char *decoded = decode_transfer_text(part, cte);
                strip_html_comments_inplace(decoded);
                newsletter_plaintext_spacing_inplace(decoded);
                strip_newsletter_tracking_urls_inplace(decoded);
                strip_tracking_gibberish(decoded);
                collapse_unsubscribe_tracking(decoded);
                free(part);
                free(fallback_html);

                if (attachment_lines[0]) {
                    size_t need = strlen(decoded) + strlen(attachment_lines) + 4;
                    char *joined = malloc(need);
                    if (joined) {
                        snprintf(joined, need, "%s%s%s",
                                 decoded,
                                 decoded[0] ? "\n" : "",
                                 attachment_lines);
                        free(decoded);
                        return joined;
                    }
                }

                return decoded;
            }

            if (find_case(ctype, "text/html") && !fallback_html) {
                char *html = decode_transfer_text(part, cte);
                if (html) {
                    strip_html_comments_inplace(html);
                    fallback_html = html_to_text(html);
                    strip_newsletter_tracking_urls_inplace(fallback_html);
                    free(html);
                }
            }

            free(part);
            p = next;
        }

        if (fallback_html) {
            if (attachment_lines[0]) {
                size_t need = strlen(fallback_html) + strlen(attachment_lines) + 4;
                char *joined = malloc(need);
                if (joined) {
                    snprintf(joined, need, "%s%s%s",
                             fallback_html,
                             fallback_html[0] ? "\n" : "",
                             attachment_lines);
                    free(fallback_html);
                    return joined;
                }
            }
            return fallback_html;
        }

        if (attachment_lines[0])
            return strdup(attachment_lines);

        return strdup("");
    }

    if (find_case(root_ctype, "text/plain") || root_ctype[0] == '\0')
        return decode_transfer_text(raw_body, root_cte);

    if (find_case(root_ctype, "text/html")) {
        char *html = decode_transfer_text(raw_body, root_cte);
        char *text = html_to_text(html);
        free(html);
        return text;
    }

    return strdup("");
}


static void parse_message_file(Message *m) {
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

    m->from[0] = m->subject[0] = m->date[0] = '\0';

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

    char *headers = unfold_headers(raw_headers ? raw_headers : "");
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
            else if (starts_case(hline, "Content-Type:"))
                copy_field(root_ctype, sizeof root_ctype, hline + 13);
            else if (starts_case(hline, "Content-Transfer-Encoding:"))
                copy_field(root_cte, sizeof root_cte, hline + 26);

            hline = strtok_r(NULL, "\n", &saveptr);
        }

        free(headers);
    }

    free(raw_headers);

    decode_header_field_inplace(m->from, sizeof m->from);
    decode_header_field_inplace(m->subject, sizeof m->subject);

    if (!m->from[0]) snprintf(m->from, sizeof m->from, "(unknown)");
    if (!m->subject[0]) snprintf(m->subject, sizeof m->subject, "(no subject)");
    if (!m->date[0]) m->date[0] = '\0';

    if (!raw_body) raw_body = strdup("");

    m->body = extract_mime_display_body(m, raw_body, root_ctype, root_cte);
    if (m->body) {
        strip_newsletter_tracking_urls_inplace(m->body);
        strip_tracking_gibberish(m->body);
        collapse_unsubscribe_tracking(m->body);
    }
    scan_attachments_fallback(m, raw_body);

    if (m->has_attachment && m->attachment_name[0] &&
        m->body && !strstr(m->body, "[Attachment:")) {
        size_t need = strlen(m->body) + strlen(m->attachment_name) + 32;
        char *joined = malloc(need);
        if (joined) {
            snprintf(joined, need, "%s%s[Attachment: %s]\n",
                     m->body,
                     m->body[0] ? "\n" : "",
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
        parse_message_file(m);
    }

    closedir(d);
}

static int message_order_cmp_newest_first(const void *aa, const void *bb) {
    const Message *a = (const Message *)aa;
    const Message *b = (const Message *)bb;

    Message *base = messages;
    int ia = (int)(a - base);
    int ib = (int)(b - base);

    time_t ta = message_order_time(ia);
    time_t tb = message_order_time(ib);

    if (ta > tb) return -1;
    if (ta < tb) return 1;

    return strcmp(b->path, a->path);
}

static void sort_messages_newest_first(void) {
    if (message_count > 1)
        qsort(messages, message_count, sizeof messages[0], message_order_cmp_newest_first);
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


static void draw_ready_to_send_footer(void) {
    int h, w;
    getmaxyx(stdscr, h, w);

    noecho();
    curs_set(0);

    mvhline(h - 3, 0, ACS_HLINE, w);

    move(h - 2, 0);
    clrtoeol();
    mvaddnstr(h - 2, 1, "Ready to send.", w - 2);

    move(h - 1, 0);
    clrtoeol();
    mvaddnstr(h - 1, 1, "s Send    v Save Draft    e Edit    d Discard", w - 2);

    move(0, 0);
}

static int simplemail_has_sync_state(void) {
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/%s/.uidvalidity", mail_root, simplemail_role_box("Inbox"));
    if (access(path, F_OK) == 0) return 1;

    snprintf(path, sizeof path, "%s/%s/.mbsyncstate", mail_root, simplemail_role_box("Inbox"));
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

static int thread_first_index(int idx) {
    if (idx < 0 || idx >= message_count) return idx;
    for (int i = 0; i < message_count; i++)
        if (same_thread(i, idx))
            return i;
    return idx;
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

static int build_thread_view(int *view, int max) {
    int n = 0;

    for (int i = 0; i < message_count && n < max; i++) {
        int seen = 0;
        for (int j = 0; j < n; j++) {
            if (same_thread(view[j], i)) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            view[n++] = i;
    }

    for (int pass = 0; pass < n; pass++) {
        for (int i = 0; i + 1 < n; i++) {
            time_t ta = 0, tb = 0;

            for (int a = 0; a < message_count; a++)
                if (same_thread(a, view[i]) && message_order_time(a) > ta)
                    ta = message_order_time(a);

            for (int b = 0; b < message_count; b++)
                if (same_thread(b, view[i + 1]) && message_order_time(b) > tb)
                    tb = message_order_time(b);

            if (ta < tb) {
                int tmp = view[i];
                view[i] = view[i + 1];
                view[i + 1] = tmp;
            }
        }
    }

    return n;
}

static int selected_thread_row(int *view, int count) {
    for (int i = 0; i < count; i++)
        if (same_thread(view[i], selected))
            return i;
    return 0;
}

static void select_visible_thread_row(int row) {
    int view[MAX_MESSAGES];
    int count = build_thread_view(view, MAX_MESSAGES);
    if (count <= 0) {
        selected = 0;
        return;
    }
    if (row < 0) row = 0;
    if (row >= count) row = count - 1;
    selected = view[row];
}

static void toggle_current_thread_selection(void) {
    if (message_count <= 0 || selected < 0 || selected >= message_count) return;

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

    if (selection_count() == 0)
        select_anchor = -1;
    else if (select_anchor < 0)
        select_anchor = selected;

    int view[MAX_MESSAGES];
    int count = build_thread_view(view, MAX_MESSAGES);
    int row = selected_thread_row(view, count);
    if (row < count - 1)
        selected = view[row + 1];
}



static int thread_is_expanded(int idx) {
    idx = thread_first_index(idx);
    if (idx < 0 || idx >= MAX_MESSAGES) return 0;
    return expanded_threads[idx];
}

static void toggle_thread_expanded(int idx) {
    idx = thread_first_index(idx);
    if (idx < 0 || idx >= MAX_MESSAGES) return;
    expanded_threads[idx] = !expanded_threads[idx];
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

    if (strlen(tmp) > 30 &&
        (!strncmp(tmp, "http://", 7) || !strncmp(tmp, "https://", 8)))
        return 1;

    if (simplemail_machine_token_line(tmp))
        return 1;

    if (looks_like_gibberish_line(tmp))
        return 1;

    return 0;
}

static int wrap_next_chunk(const char *line, int len, int pos, int width) {
    if (width < 1) return 1;
    if (pos + width >= len) return len - pos;

    int break_at = -1;
    for (int i = pos + width; i > pos; i--) {
        if (isspace((unsigned char)line[i])) {
            break_at = i;
            break;
        }
    }

    if (break_at > pos)
        return break_at - pos;

    return width;
}

static int wrapped_visual_chunks(const char *line, int len, int width) {
    if (len <= 0) return 1;
    if (width < 1) width = 1;

    int chunks = 0;
    int pos = 0;

    while (pos < len) {
        while (pos < len && isspace((unsigned char)line[pos])) pos++;
        if (pos >= len) break;

        int take = wrap_next_chunk(line, len, pos, width);
        pos += take;
        chunks++;

        while (pos < len && isspace((unsigned char)line[pos])) pos++;
    }

    return chunks > 0 ? chunks : 1;
}

static int body_visual_line_count(const char *body, int w) {
    if (!body) return 0;

    int width = w - 4;
    if (width < 1) width = 1;

    int count = 0;
    const char *p = body;

    if (*p == '\0') return 1;

    while (*p) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);

        if (render_should_omit_line(p, len))
            count += 1;
        else
            count += wrapped_visual_chunks(p, len, width);

        if (!e) break;
        p = e + 1;
    }

    return count > 0 ? count : 1;
}


static void draw_body_from_visual_scroll(const char *body, int scroll, int w, int start_y, int max_y) {
    int width = w - 4;
    int visual = 0;
    int row = start_y;
    const char *p = body;

    if (width < 1) width = 1;

    while (p && *p && row < max_y) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);

        if (render_should_omit_line(p, len)) {
            if (visual >= scroll && row < max_y)
                mvaddnstr(row++, 2, "[tracking link omitted]", w - 4);
            visual++;
        } else {
            int pos = 0;

            if (len == 0) {
                if (visual >= scroll && row < max_y)
                    row++;
                visual++;
            }

            while (pos < len && row < max_y) {
                while (pos < len && isspace((unsigned char)p[pos])) pos++;
                if (pos >= len) break;

                int take = wrap_next_chunk(p, len, pos, width);

                if (visual >= scroll && row < max_y) {
                    char buf[4096];
                    int n = take;
                    if (n >= (int)sizeof buf) n = sizeof buf - 1;
                    memcpy(buf, p + pos, n);
                    buf[n] = '\0';
                    mvaddnstr(row++, 2, buf, w - 4);
                }

                pos += take;
                visual++;

                while (pos < len && isspace((unsigned char)p[pos])) pos++;
            }
        }

        if (!e) break;
        p = e + 1;
    }
}


static void draw_read(void) {
    erase();
    int h, w;
    getmaxyx(stdscr, h, w);

    if (message_count == 0 || selected < 0 || selected >= message_count) {
        mvaddstr(2, 2, "(No message selected.)");
        draw_footer("Backspace Back  q Quit");
        refresh();
        return;
    }

    Message *m = &messages[selected];

    mvhline(0, 0, ACS_HLINE, w);
    mvaddnstr(0, 2, " Message ", w - 4);

    mvprintw(2, 2, "From: %.200s", m->from);
    mvprintw(3, 2, "Subject: %.200s", m->subject);
    if (m->date[0]) mvprintw(4, 2, "Date: %.120s", m->date);

    mvhline(6, 0, ACS_HLINE, w);

    int y = 8;
    int max_y = h - 3;
    int visible_rows = max_y - y;
    if (visible_rows < 1) visible_rows = 1;

    int total_rows = body_visual_line_count(m->body, w);
    int max_scroll = total_rows - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (read_scroll > max_scroll) read_scroll = max_scroll;
    if (read_scroll < 0) read_scroll = 0;

    draw_body_from_visual_scroll(m->body, read_scroll, w, y, max_y);

    if (current_box_is("Trash") || current_box_is("Archive"))
        draw_footer(m->has_attachment ?
                    "↑↓ Scroll  Backspace Inbox  o Open Attachment  s Save Attachment  u Restore  dD Delete  q Quit" :
                    "↑↓ Scroll  Backspace Inbox  u Restore  dD Delete  q Quit");
    else
        draw_footer(m->has_attachment ?
                    "↑↓ Scroll  Backspace Inbox  r Reply  o Open Attachment  s Save Attachment  a Archive  dD Delete  q Quit" :
                    "↑↓ Scroll  Backspace Inbox  r Reply  a Archive  dD Delete  q Quit");
    refresh();
}

static void draw_mailbox_overlay(void) {
    erase();
    int h, w;
    getmaxyx(stdscr, h, w);

    mvhline(0, 0, ACS_HLINE, w);
    mvaddnstr(0, 2, " Mailboxes ", w - 4);

    int start_y = 3;
    for (int i = 0; i < mailbox_count; i++) {
        if (i == selected_mailbox) attron(A_REVERSE);
        mvaddnstr(start_y + i, 2, i == selected_mailbox ? ">" : " ", 1);
        char label[256];
        int attention = mailbox_attention_count(i);
        if (attention > 0)
            snprintf(label, sizeof label, "%s (%d)", mailboxes[i].name, attention);
        else
            snprintf(label, sizeof label, "%s", mailboxes[i].name);

        mvaddnstr(start_y + i, 4, label, w - 6);
        if (i == selected_mailbox) attroff(A_REVERSE);
    }

    draw_footer("↑↓ Move  Enter Select  m Return  q Quit");
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

        if (ch == '\n' || ch == KEY_ENTER)
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
    curs_set(0);
    clear();
    touchwin(stdscr);
    refresh();

    return status;
}


static void save_sent_copy_from_file(const char *src_path) {
    if (!src_path || !*src_path) return;

    char sentdir[PATH_MAX];
    snprintf(sentdir, sizeof sentdir, "%s/%s/cur", mail_root, simplemail_role_box("Sent"));

    time_t now = time(NULL);
    char outpath[PATH_MAX];

    for (int tries = 0; tries < 10000; tries++) {
        snprintf(outpath, sizeof outpath, "%s/%ld-%ld-%d.eml",
                 sentdir, (long)now, (long)getpid(), tries);

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


static int send_mail_msmtp_ex(const char *to, const char *subject, const char *body_path,
                              const char *in_reply_to, const char *references) {
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "/tmp/simplemail-send-XXXXXX");

    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;

    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(tmpl);
        return -1;
    }

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
    fprintf(out, "Content-Type: text/plain; charset=UTF-8\n");
    fprintf(out, "Content-Transfer-Encoding: 8bit\n");
    fprintf(out, "\n");

    FILE *in = fopen(body_path, "r");
    if (in) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
        fclose(in);
    }

    fclose(out);

    pid_t pid = fork();
    if (pid < 0) {
        unlink(tmpl);
        return -1;
    }

    if (pid == 0) {
        freopen(tmpl, "r", stdin);
        execlp("sh", "sh", "-c",
               simplemail_send_cmd[0] ? simplemail_send_cmd : "msmtp -t",
               (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        save_sent_copy_from_file(tmpl);
        unlink(tmpl);
        return 0;
    }

    unlink(tmpl);
    return -1;
}


static int send_mail_msmtp(const char *to, const char *subject, const char *body_path) {
    return send_mail_msmtp_ex(to, subject, body_path, NULL, NULL);
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
    if (!simplemail_state_dir(state, sizeof state))
        return 0;
    snprintf(pickfile, sizeof pickfile, "%s/attach-pick-%ld-%ld",
             state, (long)getpid(), (long)time(NULL));
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
        freopen(tmpl, "r", stdin);
        execlp("sh", "sh", "-c",
               simplemail_send_cmd[0] ? simplemail_send_cmd : "msmtp -t",
               (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        save_sent_copy_from_file(tmpl);
        unlink(tmpl);
        return 0;
    }

    unlink(tmpl);
    return -1;
}

static int send_mail_msmtp_attach(const char *to, const char *subject,
                                  const char *body_path, const char *attachment_path) {
    return send_mail_msmtp_attach_ex(to, subject, body_path, attachment_path, NULL, NULL);
}


static void save_draft_record_ex(const char *to, const char *subject, const char *body_path,
                                 const char *in_reply_to, const char *references) {
    char drafts[PATH_MAX];
    snprintf(drafts, sizeof drafts, "%s/%s/new", mail_root, simplemail_role_box("Drafts"));

    time_t now = time(NULL);
    char out[PATH_MAX];
    snprintf(out, sizeof out, "%s/draft-%ld.eml", drafts, (long)now);

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

static void compose_new(void) {
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
            if (send_mail_msmtp_attach(to, subject, tmpl, attachment_path) == 0)
                snprintf(status_msg, sizeof status_msg, "Mail sent.");
            else {
                save_draft_record(to, subject, tmpl);
                snprintf(status_msg, sizeof status_msg, "Send failed; saved draft.");
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
    unlink(tmpl);
    load_current_mailbox();
}


static void reply_current(void) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

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
            if (send_mail_msmtp_attach_ex(to, subject, tmpl, attachment_path, in_reply_to, references) == 0)
                snprintf(status_msg, sizeof status_msg, "Mail sent.");
            else {
                save_draft_record_ex(to, subject, tmpl, in_reply_to, references);
                snprintf(status_msg, sizeof status_msg, "Send failed; saved draft.");
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
    unlink(tmpl);
}



static void move_selected_or_current_to(const char *boxname);

static int move_file_to_mailbox(const char *src, const char *boxname) {
    char destdir[PATH_MAX];
    char dest[PATH_MAX];
    const char *role_box = simplemail_role_box(boxname);

    snprintf(destdir, sizeof destdir, "%s/%s/cur", mail_root, role_box);

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    for (int tries = 0; tries < 10000; tries++) {
        snprintf(dest, sizeof dest, "%s/%ld-%ld-%d-%s",
                 destdir, (long)time(NULL), (long)getpid(), tries, base);

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


static time_t message_sort_time(int idx) {
    if (idx < 0 || idx >= message_count) return 0;

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

        char *end = strptime(messages[idx].date, formats[i], &tmv);
        if (end)
            return mktime(&tmv);
    }

    struct stat st;
    if (stat(messages[idx].path, &st) == 0)
        return st.st_mtime;

    return 0;
}

static int compare_message_indices_by_time(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;

    time_t ta = message_sort_time(ia);
    time_t tb = message_sort_time(ib);

    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return ia - ib;
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

static time_t message_sort_time_loose(int idx) {
    if (idx < 0 || idx >= message_count) return 0;

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
        if (end)
            return mktime(&tmv);
    }

    struct stat st;
    if (stat(messages[idx].path, &st) == 0)
        return st.st_mtime;

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

    long long fn = leading_number_in_basename(messages[idx].path);
    if (fn > 1000000000LL)
        return (time_t)fn;

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
        if (end)
            return mktime(&tmv);
    }

    struct stat st;
    if (stat(messages[idx].path, &st) == 0)
        return st.st_mtime;

    return 0;
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
    } else if (ch == '\n' || ch == KEY_ENTER) {
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
        else if (ch == '\n' || ch == KEY_ENTER) {
            current_mailbox = selected_mailbox;
            selected_thread_header = 1;
            mailbox_overlay = 0;
            load_current_mailbox();
        } else if (ch == 'm' || ch == 'M') {
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
    else if ((ch == '\n' || ch == KEY_ENTER) && message_count > 0) {
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
    if (!m->has_attachment || !m->attachment_path[0]) {
        draw_footer("No attachment.");
        refresh();
        napms(2000);
        return;
    }

    char def[PATH_MAX];
    snprintf(def, sizeof def, "~/Downloads/%s",
             m->attachment_name[0] ? m->attachment_name : "attachment.bin");

    char typed[PATH_MAX];
    prompt_line_prefill("Save attachment as:", def, typed, sizeof typed);

    if (!typed[0]) {
        draw_footer("Save canceled.");
        refresh();
        napms(2000);
        return;
    }

    char expanded[PATH_MAX];
    expand_user_path(typed, expanded, sizeof expanded);

    if (copy_file_bytes(m->attachment_path, expanded) == 0)
        snprintf(status_msg, sizeof status_msg, "Saved attachment: %.180s", expanded);
    else
        snprintf(status_msg, sizeof status_msg, "Save failed: %.180s", expanded);

    draw_footer(status_msg);
    refresh();
    napms(2000);
    status_msg[0] = '\0';
}


static void open_current_attachment(void) {
    if (message_count == 0 || selected < 0 || selected >= message_count) return;

    Message *m = &messages[selected];
    if (!m->has_attachment || !m->attachment_path[0]) {
        snprintf(status_msg, sizeof status_msg, "No attachment.");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", m->attachment_path, (char *)NULL);
        _exit(127);
    }

    snprintf(status_msg, sizeof status_msg, "Opened attachment: %.180s", m->attachment_name);
}


static void handle_read_key(int ch) {
    if (handle_restore_sequence(ch)) return;
    if (handle_delete_sequence(ch)) return;

    int max_scroll = 0;
    if (message_count > 0 && selected >= 0 && selected < message_count) {
        int h, w;
        getmaxyx(stdscr, h, w);
        int visible_rows = (h - 3) - 8;
        if (visible_rows < 1) visible_rows = 1;

        int total_rows = body_visual_line_count(messages[selected].body, w);
        max_scroll = total_rows - visible_rows;
        if (max_scroll < 0) max_scroll = 0;
    }

    int page = LINES - 11;
    if (page < 1) page = 1;

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

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(100);
    curs_set(0);

    int running = 1;
    while (running) {
        if (mailbox_overlay) draw_mailbox_overlay();
        else if (view == VIEW_READ) draw_read();
        else if (view == VIEW_THREAD) draw_thread();
        else draw_list();

        int ch = getch();

        if (ch == ERR)
            continue;

        if (status_msg[0] && !pull_running)
            status_msg[0] = '\0';

        if (ch == 'q' || ch == 'Q') {
            if (confirm_quit()) {
                running = 0;
                break;
            }
            continue;
        }

        if (view == VIEW_READ && !mailbox_overlay) {
            handle_read_key(ch);
        } else if (view == VIEW_THREAD && !mailbox_overlay) {
            handle_thread_key(ch);
        } else {
            handle_list_key(ch);
        }
    }

    endwin();
    free_messages();
    return 0;
}
