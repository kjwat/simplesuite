#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE_EXTENDED 1

#include <ncurses.h>
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define URL_MAX 4096
#define RESPONSE_LIMIT (16u * 1024u * 1024u)
#define MAX_HISTORY 128
#define CTRL_KEY(ch) ((ch) & 0x1f)
#define SIMPLEBROWSE_UA "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"

enum {
    NAV_REPLACE,
    NAV_NORMAL,
    NAV_BACK,
    NAV_FORWARD
};

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
    size_t start;
    size_t len;
    int link;
} DisplayLine;

typedef struct {
    char *text;
    char *url;
    Link *links;
    size_t link_count;
    size_t link_cap;
    DisplayLine *display;
    size_t display_count;
    size_t display_cap;
    int layout_width;
} Page;

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
    char number_buf[16];
    int number_len;
    int top;
    int selected_link;
    int url_focus;
    int running;
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
    int network_error;
    char reason[64];
    char effective[URL_MAX];
    char error[512];
} FetchResult;

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

static int starts_http_url(const char *s)
{
    return !strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8);
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
    for (i = 0; i < p->link_count; i++) {
        free(p->links[i].label);
        free(p->links[i].url);
    }
    free(p->links);
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
        "head", "script", "style", "nav", "footer", "svg", "noscript", NULL
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

static void finish_anchor(Page *page, TextBuilder *tb, AnchorBuilder *ab,
                          char **href, const char *base_url)
{
    char *label;
    char *resolved;
    char marker[64];
    size_t marker_offset;

    if (!*href) {
        buf_clear(&ab->text);
        ab->pending_space = 0;
        return;
    }

    resolved = absolute_url(base_url, *href);
    if (resolved) {
        while (ab->text.len && ab->text.data[ab->text.len - 1] == ' ')
            ab->text.data[--ab->text.len] = 0;
        label = trim_copy(ab->text.data && *ab->text.data ? ab->text.data : resolved);
        snprintf(marker, sizeof(marker), "[%zu] ", page->link_count + 1);
        if (tb->pending_space) {
            buf_addc(&tb->out, ' ');
            tb->pending_space = 0;
        }
        marker_offset = tb->out.len;
        buf_addn(&tb->out, marker, strlen(marker));
        buf_addn(&tb->out, label, strlen(label));
        page_add_link(page, label, resolved, marker_offset);
        tb_space(tb);
        free(label);
    }

    free(resolved);
    free(*href);
    *href = NULL;
    buf_clear(&ab->text);
    ab->pending_space = 0;
}

static void skip_element_content(const char **cursor, const char *end, const char *name)
{
    char closing[64];
    const char *p;
    const char *gt;

    snprintf(closing, sizeof(closing), "</%s", name);
    p = ci_find(*cursor, end, closing);
    if (!p) {
        *cursor = end;
        return;
    }
    gt = find_tag_end(p + 2, end);
    *cursor = gt ? gt + 1 : end;
}

static Page parse_html(const char *html, size_t len, const char *base_url)
{
    Page page = {0};
    TextBuilder tb = {0};
    AnchorBuilder anchor = {0};
    char *anchor_href = NULL;
    const char *p = html;
    const char *end = html + len;
    int in_anchor = 0;

    page.layout_width = -1;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        const char *gt;
        const char *name;
        size_t name_len;
        int closing;

        if (!lt) {
            if (in_anchor) ab_text_bytes(&anchor, p, (size_t)(end - p));
            else tb_text_bytes(&tb, p, (size_t)(end - p));
            break;
        }

        if (lt > p) {
            if (in_anchor) ab_text_bytes(&anchor, p, (size_t)(lt - p));
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

        if (!closing && tag_is_stripped_block(name, name_len)) {
            char tag[32];
            size_t copy_len = name_len < sizeof(tag) - 1 ? name_len : sizeof(tag) - 1;

            memcpy(tag, name, copy_len);
            tag[copy_len] = 0;
            p = gt + 1;
            skip_element_content(&p, end, tag);
            continue;
        }

        if (tag_is(name, name_len, "br")) {
            if (in_anchor) ab_space(&anchor);
            else tb_newline(&tb);
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

    tb_trim_space(&tb);
    while (tb.out.len && tb.out.data[tb.out.len - 1] == '\n')
        tb.out.data[--tb.out.len] = 0;
    if (!tb.out.data)
        tb_text_bytes(&tb, "(empty page)", 12);

    page.text = tb.out.data ? tb.out.data : xstrdup_local("(empty page)");
    page.url = xstrdup_local(base_url);
    buf_clear(&anchor.text);
    free(anchor_href);
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

static char *http_fallback_url(const char *url)
{
    Buffer b = {0};

    if (strncasecmp(url, "https://", 8)) return NULL;
    buf_addn(&b, "http://", 7);
    buf_addn(&b, url + 8, strlen(url + 8));
    return b.data;
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
}

static int link_line(Page *p, int link_index)
{
    size_t i;

    for (i = 0; i < p->display_count; i++) {
        if (p->display[i].link == link_index) return (int)i;
    }
    return 0;
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

static void ensure_selected_visible(App *a, int body_h, int body_w)
{
    int line;

    if (a->selected_link < 0 || body_h < 1) return;
    page_layout(&a->page, body_w);
    line = link_line(&a->page, a->selected_link);
    if (line < a->top) a->top = line;
    if (line >= a->top + body_h) a->top = line - body_h + 1;
}

static void draw_slice(int y, int x, const char *s, size_t n)
{
    char *tmp = xstrndup_local(s, n);
    mvaddnstr(y, x, tmp, (int)n);
    free(tmp);
}

static void draw_screen(App *a)
{
    int h, w;
    int body_top = 2;
    int status_row;
    int body_h;
    int body_w;
    size_t i;
    char prompt[16] = "URL: ";

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
    body_w = w - 1;
    page_layout(&a->page, body_w);
    if (a->selected_link >= 0)
        ensure_selected_visible(a, body_h, body_w);

    erase();

    attron(A_REVERSE);
    mvhline(0, 0, ' ', w);
    mvaddnstr(0, 1, "SimpleBrowse", w - 2);
    attroff(A_REVERSE);

    mvhline(1, 0, ' ', w);
    mvaddnstr(1, 0, prompt, w);
    if (w > (int)strlen(prompt))
        mvaddnstr(1, (int)strlen(prompt), a->url_bar, w - (int)strlen(prompt) - 1);

    for (i = 0; i < (size_t)body_h; i++) {
        size_t idx = (size_t)a->top + i;
        DisplayLine *line;

        if (idx >= a->page.display_count) break;
        line = &a->page.display[idx];
        if (line->link >= 0 && line->link == a->selected_link) attron(A_REVERSE);
        draw_slice(body_top + (int)i, 0, a->page.text + line->start, line->len);
        if (line->link >= 0 && line->link == a->selected_link) attroff(A_REVERSE);
    }

    attron(A_REVERSE);
    mvhline(status_row, 0, ' ', w);
    mvaddnstr(status_row, 1, a->status[0] ? a->status :
              "Ctrl-L URL  digits+Enter link  Tab links  b/f back/forward  r reload  q quit", w - 2);
    attroff(A_REVERSE);

    if (a->url_focus) {
        int cursor_x = (int)strlen(prompt) + a->url_pos;
        if (cursor_x >= w) cursor_x = w - 1;
        move(1, cursor_x);
        curs_set(1);
    } else {
        curs_set(0);
    }

    refresh();
}

static void make_error_page(App *a, const char *url, const char *err)
{
    Page p = {0};
    Buffer b = {0};

    buf_addn(&b, "Could not load page.\n\n", 22);
    buf_addn(&b, url, strlen(url));
    buf_addn(&b, "\n\n", 2);
    buf_addn(&b, err, strlen(err));

    page_free(&a->page);
    p.text = b.data;
    p.url = xstrdup_local(url);
    p.layout_width = -1;
    a->page = p;
}

static void finish_navigation(App *a, int mode, const char *old_url,
                              const char *new_url, int success)
{
    int same = old_url && *old_url && new_url && !strcmp(old_url, new_url);
    char *discard;

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

static int load_url(App *a, const char *input, int nav_mode)
{
    char *url = normalize_input_url(input);
    char *old_url = xstrdup_local(a->current_url);
    char *fallback = NULL;
    char *html;
    const char *final_url;
    char status_text[96];
    FetchResult result;
    FetchResult retry;
    Page p;

    if (!*url) {
        free(url);
        free(old_url);
        set_status(a, "Enter a URL");
        return 0;
    }

    clear_number_entry(a);
    snprintf(a->url_bar, sizeof(a->url_bar), "%s", url);
    a->url_len = (int)strlen(a->url_bar);
    a->url_pos = a->url_len;
    snprintf(a->status, sizeof(a->status), "Loading %s", url);
    draw_screen(a);

    html = fetch_url(url, &result);
    if (!html && result.network_error) {
        fallback = http_fallback_url(url);
        if (fallback) {
            snprintf(a->status, sizeof(a->status), "HTTPS failed; trying %s", fallback);
            draw_screen(a);

            html = fetch_url(fallback, &retry);
            if (html) {
                free(url);
                url = fallback;
                fallback = NULL;
                result = retry;
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

    final_url = result.effective[0] ? result.effective : url;

    if (!html) {
        finish_navigation(a, nav_mode, old_url, final_url, 1);
        make_error_page(a, final_url, result.error[0] ? result.error : "request failed");
        snprintf(a->current_url, sizeof(a->current_url), "%s", final_url);
        snprintf(a->url_bar, sizeof(a->url_bar), "%s", final_url);
        a->url_len = (int)strlen(a->url_bar);
        a->url_pos = a->url_len;
        snprintf(a->status, sizeof(a->status), "Load failed: %s",
                 result.error[0] ? result.error : "request failed");
        free(url);
        free(fallback);
        free(old_url);
        a->top = 0;
        a->selected_link = -1;
        return 0;
    }

    finish_navigation(a, nav_mode, old_url, final_url, 1);

    p = parse_html(html, strlen(html), final_url);
    free(html);
    page_free(&a->page);
    a->page = p;
    snprintf(a->current_url, sizeof(a->current_url), "%s", final_url);
    snprintf(a->url_bar, sizeof(a->url_bar), "%s", final_url);
    a->url_len = (int)strlen(a->url_bar);
    a->url_pos = a->url_len;
    a->top = 0;
    a->selected_link = a->page.link_count ? 0 : -1;
    format_http_status(result.code, status_text, sizeof(status_text));
    snprintf(a->status, sizeof(a->status), "%s | %zu links | %s",
             status_text, a->page.link_count, final_url);
    free(url);
    free(fallback);
    free(old_url);
    return 1;
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
    int count = (int)a->page.link_count;
    int h, w;

    if (!count) {
        set_status(a, "No links");
        return;
    }

    if (a->selected_link < 0) {
        a->selected_link = dir > 0 ? 0 : count - 1;
    } else {
        a->selected_link = (a->selected_link + dir + count) % count;
    }

    getmaxyx(stdscr, h, w);
    ensure_selected_visible(a, h - 3, w - 1);
    snprintf(a->status, sizeof(a->status), "[%d] %s", a->selected_link + 1,
             a->page.links[a->selected_link].url);
}

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

    if (!a->number_len) {
        open_selected_link(a);
        return;
    }

    n = strtol(a->number_buf, &end, 10);
    if (!end || *end || n < 1 || n > (long)a->page.link_count) {
        snprintf(a->status, sizeof(a->status), "No link %s", a->number_buf);
        clear_number_entry(a);
        return;
    }

    a->selected_link = (int)n - 1;
    clear_number_entry(a);
    open_selected_link(a);
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

static void handle_page_key(App *a, int ch)
{
    int h, w;
    int body_h;

    getmaxyx(stdscr, h, w);
    body_h = h - 3;

    if (isdigit((unsigned char)ch)) {
        append_number_entry(a, ch);
        return;
    }

    if (a->number_len && ch != '\n' && ch != '\r' && ch != KEY_ENTER)
        clear_number_entry(a);

    switch (ch) {
    case CTRL_KEY('l'):
        a->url_focus = 1;
        a->url_pos = a->url_len;
        break;
    case '\t':
        next_link(a, 1);
        break;
    case KEY_BTAB:
        next_link(a, -1);
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        open_numbered_link(a);
        break;
    case 'b':
        go_back(a);
        break;
    case 'f':
        go_forward(a);
        break;
    case 'r':
        if (a->current_url[0]) load_url(a, a->current_url, NAV_REPLACE);
        else set_status(a, "No page to reload");
        break;
    case 'q':
        a->running = 0;
        break;
    case KEY_UP:
    case 'k':
        if (a->top > 0) a->top--;
        break;
    case KEY_DOWN:
    case 'j':
        page_layout(&a->page, w - 1);
        if ((size_t)(a->top + 1) < a->page.display_count) a->top++;
        break;
    case KEY_PPAGE:
        a->top -= body_h > 1 ? body_h - 1 : 1;
        if (a->top < 0) a->top = 0;
        break;
    case KEY_NPAGE:
        page_layout(&a->page, w - 1);
        a->top += body_h > 1 ? body_h - 1 : 1;
        if ((size_t)a->top >= a->page.display_count)
            a->top = a->page.display_count ? (int)a->page.display_count - 1 : 0;
        break;
    case KEY_HOME:
    case 'g':
        a->top = 0;
        break;
    case KEY_END:
    case 'G':
        page_layout(&a->page, w - 1);
        a->top = a->page.display_count > (size_t)body_h ?
                 (int)a->page.display_count - body_h : 0;
        break;
    default:
        break;
    }
}

static void app_free(App *a)
{
    page_free(&a->page);
    stack_clear(a->back_stack, &a->back_count);
    stack_clear(a->forward_stack, &a->forward_count);
}

int main(int argc, char **argv)
{
    App app;

    memset(&app, 0, sizeof(app));
    app.running = 1;
    app.url_focus = 1;
    app.selected_link = -1;
    app.page.text = xstrdup_local("Ctrl-L focuses the URL bar. Enter a URL and press Enter.");
    app.page.layout_width = -1;
    set_status(&app, "Enter a URL");

    setlocale(LC_ALL, "");
    curl_global_init(CURL_GLOBAL_DEFAULT);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);

    if (argc > 1) {
        snprintf(app.url_bar, sizeof(app.url_bar), "%s", argv[1]);
        app.url_len = (int)strlen(app.url_bar);
        app.url_pos = app.url_len;
        app.url_focus = 0;
        load_url(&app, app.url_bar, NAV_REPLACE);
    }

    while (app.running) {
        int ch;

        draw_screen(&app);
        ch = getch();
        if (app.url_focus)
            handle_url_key(&app, ch);
        else
            handle_page_key(&app, ch);
    }

    endwin();
    curl_global_cleanup();
    app_free(&app);
    return 0;
}
