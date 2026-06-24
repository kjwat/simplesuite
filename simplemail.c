/*
 * simplemail.c - first SimpleSuite-style mail client draft
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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_MESSAGES 2048
#define MAX_MAILBOXES 32
#define MAX_LINE 4096
#define MAX_BODY 262144


typedef enum {
    VIEW_LIST,
    VIEW_READ
} View;

typedef struct {
    char path[PATH_MAX];
    char from[256];
    char subject[512];
    char date[128];
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

static Mailbox mailboxes[MAX_MAILBOXES];
static int mailbox_count = 0;
static int current_mailbox = 0;
static int mailbox_overlay = 0;
static int selected_mailbox = 0;

static View view = VIEW_LIST;
static int read_scroll = 0;
static int pending_delete = 0;
static int pending_restore = 0;

static char mail_root[PATH_MAX];
static char status_msg[256];

static int current_box_is(const char *name);
static void restore_current_message(void);
static void move_current_message_to(const char *boxname);
static void move_selected_or_current_to(const char *boxname);

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
    snprintf(inbox, sizeof inbox, "%s/Inbox", mail_root);
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

static void init_paths(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";

    snprintf(mail_root, sizeof mail_root, "%s/.local/share/simplemail/mail", home);

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.local", home); ensure_dir(p);
    snprintf(p, sizeof p, "%s/.local/share", home); ensure_dir(p);
    snprintf(p, sizeof p, "%s/.local/share/simplemail", home); ensure_dir(p);
    ensure_dir(mail_root);

    const char *default_boxes[] = {"Inbox", "Sent", "Drafts", "Archive", "Trash"};
    for (size_t i = 0; i < sizeof(default_boxes) / sizeof(default_boxes[0]); i++) {
        snprintf(p, sizeof p, "%s/%s", mail_root, default_boxes[i]);
        make_maildir(p);
    }

    write_sample_mail();
}

static void init_mailboxes(void) {
    mailbox_count = 0;
    const char *default_boxes[] = {"Inbox", "Sent", "Drafts", "Archive", "Trash"};

    for (size_t i = 0; i < sizeof(default_boxes) / sizeof(default_boxes[0]) && mailbox_count < MAX_MAILBOXES; i++) {
        snprintf(mailboxes[mailbox_count].name, sizeof mailboxes[mailbox_count].name, "%s", default_boxes[i]);
        snprintf(mailboxes[mailbox_count].path, sizeof mailboxes[mailbox_count].path, "%s/%s", mail_root, default_boxes[i]);
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
    snprintf(dir, sizeof dir, "/tmp/simplemail-attachments");
    ensure_dir(dir);

    snprintf(m->attachment_name, sizeof m->attachment_name, "%s", filename);
    safe_filename(m->attachment_name);

    snprintf(m->attachment_path, sizeof m->attachment_path, "%s/%ld-%s",
             dir, (long)time(NULL), m->attachment_name);

    FILE *f = fopen(m->attachment_path, "wb");
    if (!f) {
        m->attachment_path[0] = '\0';
        m->attachment_name[0] = '\0';
        return;
    }

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

        /* Skip naked URLs that newsletters print directly into the body. */
        if ((i == 0 || isspace((unsigned char)html[i - 1]) || html[i - 1] == '(') &&
            (!strncmp(html + i, "http://", 7) || !strncmp(html + i, "https://", 8))) {

            while (html[i] &&
                   !isspace((unsigned char)html[i]) &&
                   html[i] != ')' &&
                   html[i] != '"' &&
                   html[i] != '\'' &&
                   html[i] != '<') {
                i++;
            }

            while (used > 0 &&
                   (out[used - 1] == ' ' || out[used - 1] == '(')) {
                used--;
            }

            if (out)
                out[used] = '\0';

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
    return out;
}





static void strip_newsletter_tracking_urls_inplace(char *s) {
    if (!s) return;

    char *r = s;
    char *w = s;

    while (*r) {
        if (*r == '(' &&
            (!strncmp(r + 1, "http://", 7) ||
             !strncmp(r + 1, "https://", 8))) {

            while (*r && *r != ')')
                r++;

            if (*r == ')')
                r++;

            while (*r == ' ' || *r == '\t')
                r++;

            continue;
        }

        *w++ = *r++;
    }

    *w = '\0';
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

static void load_current_mailbox(void) {
    free_messages();

    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/new", mailboxes[current_mailbox].path);
    load_dir_messages(p, 1);

    snprintf(p, sizeof p, "%s/cur", mailboxes[current_mailbox].path);
    load_dir_messages(p, 0);

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

    if (strcmp(mailboxes[idx].name, "Inbox") == 0) {
        snprintf(path, sizeof path, "%s/new", mailboxes[idx].path);
        return count_regular_files_in_dir(path);
    }

    if (strcmp(mailboxes[idx].name, "Drafts") == 0) {
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
    } else if (pending_delete) {
        char msg[128];
        int n = selection_count();
        if (n > 1)
            snprintf(msg, sizeof msg, "dD Delete %d selected", n);
        else
            snprintf(msg, sizeof msg, "dD Delete");
        mvaddnstr(h - 1, 1, msg, w - 2);
    }
    else
        mvaddnstr(h - 1, 1, text, w - 2);

    move(0, 0);
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

static void pull_mail(void) {
    def_prog_mode();
    endwin();

    int rc = system("mbsync inbox");

    reset_prog_mode();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    clear();
    touchwin(stdscr);

    load_current_mailbox();

    if (rc == 0)
        snprintf(status_msg, sizeof status_msg, "Mail pulled.");
    else
        snprintf(status_msg, sizeof status_msg, "Pull failed: run mbsync inbox");
}

static void draw_list(void) {
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

    int rows = h - 4;
    if (selected < list_top) list_top = selected;
    if (selected >= list_top + rows) list_top = selected - rows + 1;
    if (list_top < 0) list_top = 0;

    if (message_count == 0) {
        mvaddnstr(3, 2, "(No messages here.)", w - 4);
    } else {
        for (int y = 0; y < rows && list_top + y < message_count; y++) {
            int idx = list_top + y;
            Message *m = &messages[idx];

            char line[1024];
            char from[64];
            display_from(from, sizeof from, m->from);

            snprintf(line, sizeof line, "%c %-24.24s  %s",
                     selected_flags[idx] ? '*' : (m->unread ? 'N' : ' '),
                     from,
                     m->subject);

            if (idx == selected) attron(A_REVERSE);
            mvaddnstr(y + 2, 1, idx == selected ? ">" : " ", 1);
            mvaddnstr(y + 2, 3, line, w - 4);
            if (idx == selected) attroff(A_REVERSE);
        }
    }

    {
        int n = selection_count();
        char footer[256];

        if (n > 0) {
            if (current_box_is("Trash") || current_box_is("Archive"))
                snprintf(footer, sizeof footer, "%d selected  ↑↓ Move  u Restore  dD Delete  Esc Clear  q Quit", n);
            else
                snprintf(footer, sizeof footer, "%d selected  ↑↓ Move  a Archive  dD Delete  p Pull  Esc Clear  q Quit", n);
            draw_footer(footer);
        } else if (current_box_is("Trash") || current_box_is("Archive")) {
            draw_footer("↑↓ Move  Enter Open  u Restore  m Mailboxes  c Compose  / Search  q Quit");
        } else {
            if (status_msg[0]) {
                draw_footer(status_msg);
                status_msg[0] = '\0';
            } else {
                draw_footer("↑↓ Move  Enter Open  m Mailboxes  c Compose  p Pull  / Search  q Quit");
            }
        }
    }
    refresh();
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
        count += wrapped_visual_chunks(p, len, width);
        if (!e) break;
        p = e + 1;
    }

    return count > 0 ? count : 1;
}

static void draw_body_from_visual_scroll(const char *body, int scroll, int w, int start_y, int max_y) {
    int width = w - 4;
    if (width < 1) width = 1;

    int y = start_y;
    int visual = 0;
    const char *p = body ? body : "";

    if (*p == '\0') {
        if (scroll == 0 && y < max_y) mvaddch(y, 1, ' ');
        return;
    }

    while (*p && y < max_y) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        int chunks = len <= 0 ? 1 : (len + width - 1) / width;

        int pos = 0;
        for (int c = 0; c < chunks && y < max_y; c++) {
            while (pos < len && isspace((unsigned char)p[pos])) pos++;

            if (visual >= scroll) {
                if (len <= 0 || pos >= len) {
                    mvaddch(y++, 1, ' ');
                } else {
                    int take = wrap_next_chunk(p, len, pos, width);
                    mvaddnstr(y++, 2, p + pos, take);
                    pos += take;
                }
            } else if (pos < len) {
                pos += wrap_next_chunk(p, len, pos, width);
            }

            while (pos < len && isspace((unsigned char)p[pos])) pos++;
            visual++;
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
                    "↑↓ Scroll  Backspace Inbox  o Open Attachment  u Restore  dD Delete  q Quit" :
                    "↑↓ Scroll  Backspace Inbox  u Restore  dD Delete  q Quit");
    else
        draw_footer(m->has_attachment ?
                    "↑↓ Scroll  Backspace Inbox  r Reply  o Open Attachment  a Archive  dD Delete  q Quit" :
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

static void prompt_line(const char *label, char *out, size_t outsz) {
    echo();
    curs_set(1);

    int h, w;
    getmaxyx(stdscr, h, w);
    mvhline(h - 3, 0, ' ', w);
    mvprintw(h - 3, 1, "%s", label);
    clrtoeol();

    move(h - 3, (int)strlen(label) + 2);
    char buf[1024];
    memset(buf, 0, sizeof buf);
    getnstr(buf, (int)sizeof(buf) - 1);

    snprintf(out, outsz, "%s", buf);
    trim(out);

    noecho();
    curs_set(0);
}

static int run_editor_on_file(const char *path) {
    const char *editor = getenv("SIMPLEMAIL_EDITOR");
    if (!editor || !*editor) editor = "simplewords";

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
            setenv("SIMPLEWORDS_AUTOSAVE_ON_EXIT", "1", 1);

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
    curs_set(0);
    clear();
    touchwin(stdscr);
    refresh();

    return status;
}


static int send_mail_msmtp(const char *to, const char *subject, const char *body_path) {
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

    const char *from = getenv("SIMPLEMAIL_FROM");
    if (!from || !*from) from = "poetnamedkeelan@gmail.com";

    fprintf(out, "From: %s\n", from);
    fprintf(out, "To: %s\n", to && *to ? to : "");
    fprintf(out, "Subject: %s\n", subject && *subject ? subject : "(no subject)");
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
        execlp("msmtp", "msmtp", "-t", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmpl);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;

    return -1;
}


static void save_draft_record(const char *to, const char *subject, const char *body_path) {
    char drafts[PATH_MAX];
    snprintf(drafts, sizeof drafts, "%s/Drafts/new", mail_root);

    time_t now = time(NULL);
    char out[PATH_MAX];
    snprintf(out, sizeof out, "%s/draft-%ld.eml", drafts, (long)now);

    FILE *in = fopen(body_path, "r");
    FILE *f = fopen(out, "w");
    if (!f) {
        if (in) fclose(in);
        return;
    }

    fprintf(f, "From: SimpleMail User\n");
    fprintf(f, "To: %s\n", to && *to ? to : "(unset)");
    fprintf(f, "Subject: %s\n", subject && *subject ? subject : "(no subject)");
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

static void compose_new(void) {
    char to[512] = {0};
    char subject[512] = {0};

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

    erase();
    draw_ready_to_send_footer();
    refresh();

    int ch;
    while ((ch = getch())) {
        if (ch == 's' || ch == 'S') {
            if (send_mail_msmtp(to, subject, tmpl) == 0)
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
            erase();
            draw_ready_to_send_footer();
            refresh();
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

    erase();
    mvprintw(2, 2, "To: %.200s", to);
    mvprintw(3, 2, "Subject: %.200s", subject);
    draw_ready_to_send_footer();
    refresh();

    int ch;
    while ((ch = getch())) {
        if (ch == 's' || ch == 'S') {
            if (send_mail_msmtp(to, subject, tmpl) == 0)
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
            erase();
            mvprintw(2, 2, "To: %.200s", to);
            mvprintw(3, 2, "Subject: %.200s", subject);
            draw_ready_to_send_footer();
            refresh();
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

    snprintf(destdir, sizeof destdir, "%s/%s/cur", mail_root, boxname);

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    snprintf(dest, sizeof dest, "%s/%ld-%s", destdir, (long)time(NULL), base);

    if (rename(src, dest) == 0) return 0;
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

    for (int i = message_count - 1; i >= 0; i--) {
        if (selected_flags[i])
            move_file_to_mailbox(messages[i].path, boxname);
    }

    load_current_mailbox();

    if (message_count <= 0)
        selected = 0;
    else if (old_selected >= message_count)
        selected = message_count - 1;
    else
        selected = old_selected;

    clear_selection();

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

static void delete_current_message(void) {
    move_selected_or_current_to("Trash");
}

static void archive_current_message(void) {
    move_selected_or_current_to("Archive");
}

static int current_box_is(const char *name) {
    return current_mailbox >= 0 &&
           current_mailbox < mailbox_count &&
           strcmp(mailboxes[current_mailbox].name, name) == 0;
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
    if (!pending_delete) {
        if (ch == 'd') {
            pending_delete = 1;
            return 1;
        }
        return 0;
    }

    if (ch == 'D') {
        pending_delete = 0;

        int h, w;
        getmaxyx(stdscr, h, w);
        mvhline(h - 2, 0, ACS_HLINE, w);
        move(h - 1, 0);
        clrtoeol();
        {
            char msg[128];
            int n = selection_count();
            if (n > 1)
                snprintf(msg, sizeof msg, "Move %d selected messages to Trash? y/N", n);
            else
                snprintf(msg, sizeof msg, "Move message to Trash? y/N");
            mvaddnstr(h - 1, 1, msg, w - 2);
        }
        refresh();

        int ans = getch();
        if (ans == 'y' || ans == 'Y')
            delete_current_message();

        return 1;
    }

    pending_delete = 0;
    return 0;
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


static void handle_list_key(int ch) {
    if (!mailbox_overlay && handle_restore_sequence(ch)) return;
    if (!mailbox_overlay && handle_delete_sequence(ch)) return;

    if (mailbox_overlay) {
        if (ch == KEY_UP && selected_mailbox > 0) selected_mailbox--;
        else if (ch == KEY_DOWN && selected_mailbox < mailbox_count - 1) selected_mailbox++;
        else if (ch == '\n' || ch == KEY_ENTER) {
            current_mailbox = selected_mailbox;
            mailbox_overlay = 0;
            load_current_mailbox();
        } else if (ch == 'm' || ch == 'M') {
            mailbox_overlay = 0;
        }
        return;
    }

    if (ch == ' ') {
        toggle_current_selection();
    } else if (ch == 27) {
        clear_selection();
    } else if (ch == KEY_UP && selected > 0) {
        selected--;
    } else if (ch == KEY_DOWN && selected < message_count - 1) {
        selected++;
    }
    else if ((ch == '\n' || ch == KEY_ENTER) && message_count > 0) {
        mark_current_message_read();
        view = VIEW_READ;
        read_scroll = 0;
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

    if (ch == KEY_UP && read_scroll > 0) read_scroll--;
    else if (ch == KEY_DOWN && read_scroll < max_scroll) read_scroll++;
    else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        view = VIEW_LIST;
    } else if (ch == 'o' || ch == 'O') {
        open_current_attachment();
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

    init_paths();
    init_mailboxes();
    load_current_mailbox();

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    int running = 1;
    while (running) {
        if (mailbox_overlay) draw_mailbox_overlay();
        else if (view == VIEW_READ) draw_read();
        else draw_list();

        int ch = getch();

        if (ch == 'q' || ch == 'Q') {
            running = 0;
            break;
        }

        if (view == VIEW_READ && !mailbox_overlay) {
            handle_read_key(ch);
        } else {
            handle_list_key(ch);
        }
    }

    endwin();
    free_messages();
    return 0;
}
