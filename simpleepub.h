#ifndef SIMPLEEPUB_H
#define SIMPLEEPUB_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SIMPLEEPUB_CAPTURE_LIMIT ((size_t)512 * 1024 * 1024)
#define SIMPLEEPUB_ARG_LIMIT ((size_t)256 * 1024)
#define SIMPLEEPUB_MARKER_START '\x1e'
#define SIMPLEEPUB_MARKER_END '\x1f'

typedef struct {
    char **items;
    int count;
    int capacity;
} SimpleEpubList;

typedef struct {
    char *id;
    char *path;
} SimpleEpubManifestItem;

typedef struct {
    SimpleEpubManifestItem *items;
    int count;
    int capacity;
} SimpleEpubManifest;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int pending_space;
    int pending_breaks;
    int failed;
} SimpleEpubText;

static char *simpleepub_strdup(const char *text)
{
    char *copy = strdup(text ? text : "");
    return copy;
}

static void simpleepub_list_free(SimpleEpubList *list)
{
    if (!list)
        return;
    for (int i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof *list);
}

static int simpleepub_list_add(SimpleEpubList *list, const char *item)
{
    if (list->count == list->capacity) {
        int capacity = list->capacity ? list->capacity * 2 : 64;
        char **grown = realloc(list->items,
                               (size_t)capacity * sizeof(*grown));
        if (!grown)
            return -1;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count] = simpleepub_strdup(item);
    if (!list->items[list->count])
        return -1;
    list->count++;
    return 0;
}

static int simpleepub_list_index(const SimpleEpubList *list,
                                 const char *item)
{
    for (int i = 0; i < list->count; i++)
        if (!strcmp(list->items[i], item))
            return i;
    return -1;
}

static void simpleepub_manifest_free(SimpleEpubManifest *manifest)
{
    if (!manifest)
        return;
    for (int i = 0; i < manifest->count; i++) {
        free(manifest->items[i].id);
        free(manifest->items[i].path);
    }
    free(manifest->items);
    memset(manifest, 0, sizeof *manifest);
}

static int simpleepub_manifest_add(SimpleEpubManifest *manifest,
                                   const char *id, const char *path)
{
    if (manifest->count == manifest->capacity) {
        int capacity = manifest->capacity ? manifest->capacity * 2 : 64;
        SimpleEpubManifestItem *grown =
            realloc(manifest->items, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return -1;
        manifest->items = grown;
        manifest->capacity = capacity;
    }

    SimpleEpubManifestItem *item = &manifest->items[manifest->count];
    item->id = simpleepub_strdup(id);
    item->path = simpleepub_strdup(path);
    if (!item->id || !item->path) {
        free(item->id);
        free(item->path);
        item->id = NULL;
        item->path = NULL;
        return -1;
    }
    manifest->count++;
    return 0;
}

static const char *simpleepub_manifest_path(const SimpleEpubManifest *manifest,
                                            const char *id)
{
    for (int i = 0; i < manifest->count; i++)
        if (!strcmp(manifest->items[i].id, id))
            return manifest->items[i].path;
    return NULL;
}

static int simpleepub_wait(pid_t pid)
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

static int simpleepub_capture(char *const argv[], char **output,
                              size_t *output_size, size_t limit)
{
    int pipefd[2];
    pid_t pid;
    char *data = NULL;
    size_t used = 0;
    size_t capacity = 0;
    int failed = 0;

    *output = NULL;
    if (output_size)
        *output_size = 0;
    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    for (;;) {
        char chunk[65536];
        ssize_t bytes = read(pipefd[0], chunk, sizeof chunk);

        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0) {
            failed = 1;
            break;
        }
        if (bytes == 0)
            break;
        if ((size_t)bytes > limit - used) {
            failed = 1;
            break;
        }
        if (used + (size_t)bytes + 1 > capacity) {
            size_t grown_capacity = capacity ? capacity * 2 : 8192;
            while (grown_capacity < used + (size_t)bytes + 1) {
                if (grown_capacity > limit / 2) {
                    grown_capacity = limit + 1;
                    break;
                }
                grown_capacity *= 2;
            }
            if (grown_capacity > limit) {
                failed = 1;
                break;
            }
            char *grown = realloc(data, grown_capacity);
            if (!grown) {
                failed = 1;
                break;
            }
            data = grown;
            capacity = grown_capacity;
        }
        memcpy(data + used, chunk, (size_t)bytes);
        used += (size_t)bytes;
    }
    close(pipefd[0]);

    if (failed)
        kill(pid, SIGTERM);
    int status = simpleepub_wait(pid);
    if (failed || status != 0) {
        free(data);
        return status != 0 ? status : -1;
    }

    if (!data) {
        data = malloc(1);
        if (!data)
            return -1;
    }
    data[used] = 0;
    *output = data;
    if (output_size)
        *output_size = used;
    return 0;
}

static int simpleepub_has_html_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".html") ||
                   !strcasecmp(dot, ".htm") ||
                   !strcasecmp(dot, ".xhtml"));
}

static int simpleepub_has_extension(const char *path, const char *extension)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, extension);
}

static int simpleepub_archive_entries(const char *infile,
                                      SimpleEpubList *entries)
{
    char *argv[] = {"unzip", "-Z1", (char *)infile, NULL};
    char *listing = NULL;
    size_t listing_size = 0;

    if (simpleepub_capture(argv, &listing, &listing_size,
                           (size_t)32 * 1024 * 1024) != 0)
        return -1;

    char *cursor = listing;
    char *end = listing + listing_size;
    while (cursor < end) {
        char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        char *line_end = newline ? newline : end;
        while (line_end > cursor && line_end[-1] == '\r')
            line_end--;
        if (line_end > cursor) {
            char saved = *line_end;
            *line_end = 0;
            if (simpleepub_list_add(entries, cursor) != 0) {
                *line_end = saved;
                free(listing);
                return -1;
            }
            *line_end = saved;
        }
        if (!newline)
            break;
        cursor = newline + 1;
    }
    free(listing);
    return entries->count > 0 ? 0 : -1;
}

static int simpleepub_read_entry(const char *infile, const char *entry,
                                 char **output, size_t *output_size,
                                 size_t limit)
{
    char *argv[] = {"unzip", "-p", (char *)infile, (char *)entry, NULL};
    return simpleepub_capture(argv, output, output_size, limit);
}

static int simpleepub_name_equal(const char *left, size_t left_length,
                                 const char *right)
{
    size_t right_length = strlen(right);
    return left_length == right_length &&
           !strncasecmp(left, right, left_length);
}

static char *simpleepub_xml_attribute(const char *tag, size_t tag_length,
                                      const char *wanted)
{
    const char *p = tag;
    const char *end = tag + tag_length;

    while (p < end && *p != '<')
        p++;
    if (p < end)
        p++;
    while (p < end && !isspace((unsigned char)*p) && *p != '>')
        p++;

    while (p < end) {
        const char *name;
        const char *value;
        size_t name_length;
        size_t value_length;
        char quote = 0;

        while (p < end && (isspace((unsigned char)*p) || *p == '/'))
            p++;
        if (p >= end || *p == '>')
            break;
        name = p;
        while (p < end && !isspace((unsigned char)*p) &&
               *p != '=' && *p != '/' && *p != '>')
            p++;
        name_length = (size_t)(p - name);
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p >= end || *p != '=') {
            while (p < end && !isspace((unsigned char)*p) &&
                   *p != '/' && *p != '>')
                p++;
            continue;
        }
        p++;
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p < end && (*p == '"' || *p == '\''))
            quote = *p++;
        value = p;
        if (quote) {
            while (p < end && *p != quote)
                p++;
        } else {
            while (p < end && !isspace((unsigned char)*p) &&
                   *p != '/' && *p != '>')
                p++;
        }
        value_length = (size_t)(p - value);
        if (simpleepub_name_equal(name, name_length, wanted)) {
            char *copy = malloc(value_length + 1);
            if (!copy)
                return NULL;
            memcpy(copy, value, value_length);
            copy[value_length] = 0;
            return copy;
        }
        if (quote && p < end)
            p++;
    }
    return NULL;
}

static int simpleepub_hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void simpleepub_decode_path(char *path)
{
    char *source = path;
    char *dest = path;

    while (*source) {
        if (source[0] == '%' && source[1] && source[2] &&
            simpleepub_hex_value(source[1]) >= 0 &&
            simpleepub_hex_value(source[2]) >= 0) {
            int value = simpleepub_hex_value(source[1]) * 16 +
                        simpleepub_hex_value(source[2]);
            if (value != 0)
                *dest++ = (char)value;
            source += 3;
        } else if (!strncmp(source, "&amp;", 5)) {
            *dest++ = '&';
            source += 5;
        } else {
            *dest++ = *source++;
        }
    }
    *dest = 0;
}

static char *simpleepub_resolve_path(const char *package_path,
                                     const char *href)
{
    size_t base_length = 0;
    const char *slash = strrchr(package_path, '/');
    size_t href_length = strcspn(href, "?#");
    char *joined;
    char *segments[1024];
    int segment_count = 0;

    if (slash)
        base_length = (size_t)(slash - package_path + 1);
    joined = malloc(base_length + href_length + 1);
    if (!joined)
        return NULL;
    memcpy(joined, package_path, base_length);
    memcpy(joined + base_length, href, href_length);
    joined[base_length + href_length] = 0;
    simpleepub_decode_path(joined);

    if (joined[0] == '/' || strchr(joined, '\\')) {
        free(joined);
        return NULL;
    }

    char *save = NULL;
    for (char *part = strtok_r(joined, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !*part)
            continue;
        if (!strcmp(part, "..")) {
            if (segment_count == 0) {
                free(joined);
                return NULL;
            }
            segment_count--;
            continue;
        }
        if (segment_count >= (int)(sizeof segments / sizeof segments[0])) {
            free(joined);
            return NULL;
        }
        segments[segment_count++] = part;
    }

    size_t output_length = 0;
    for (int i = 0; i < segment_count; i++)
        output_length += strlen(segments[i]) + (i > 0);
    char *output = malloc(output_length + 1);
    if (!output) {
        free(joined);
        return NULL;
    }
    char *out = output;
    for (int i = 0; i < segment_count; i++) {
        if (i > 0)
            *out++ = '/';
        size_t length = strlen(segments[i]);
        memcpy(out, segments[i], length);
        out += length;
    }
    *out = 0;
    free(joined);
    return output;
}

static char *simpleepub_resolve_href_key(const char *document_path,
                                         const char *href)
{
    const char *fragment;
    const char *colon;
    const char *slash;
    char *path = NULL;
    char *key = NULL;

    if (!document_path || !*document_path || !href || !*href)
        return NULL;

    fragment = strchr(href, '#');
    colon = strchr(href, ':');
    slash = strpbrk(href, "/?#");
    if (colon && (!slash || colon < slash))
        return NULL;

    if (href[0] == '#')
        path = simpleepub_strdup(document_path);
    else
        path = simpleepub_resolve_path(document_path, href);
    if (!path || !*path)
        goto done;

    if (fragment && fragment[1]) {
        char *decoded = simpleepub_strdup(fragment + 1);
        if (!decoded)
            goto done;
        simpleepub_decode_path(decoded);
        size_t needed = strlen(path) + strlen(decoded) + 2;
        key = malloc(needed);
        if (key)
            snprintf(key, needed, "%s#%s", path, decoded);
        free(decoded);
    } else {
        key = simpleepub_strdup(path);
    }

done:
    free(path);
    return key;
}

static char *simpleepub_id_key(const char *document_path, const char *id)
{
    char *decoded;
    char *key;
    size_t needed;

    if (!document_path || !*document_path || !id || !*id)
        return NULL;
    decoded = simpleepub_strdup(id);
    if (!decoded)
        return NULL;
    simpleepub_decode_path(decoded);
    needed = strlen(document_path) + strlen(decoded) + 2;
    key = malloc(needed);
    if (key)
        snprintf(key, needed, "%s#%s", document_path, decoded);
    free(decoded);
    return key;
}

static const char *simpleepub_tag_end(const char *tag)
{
    char quote = 0;

    for (const char *p = tag; *p; p++) {
        if (quote) {
            if (*p == quote)
                quote = 0;
            continue;
        }
        if (*p == '"' || *p == '\'')
            quote = *p;
        else if (*p == '>')
            return p;
    }
    return NULL;
}

static int simpleepub_spine_from_opf(const char *opf,
                                     const char *package_path,
                                     SimpleEpubList *spine)
{
    SimpleEpubManifest manifest = {0};
    const char *manifest_start = strstr(opf, "<manifest");
    const char *manifest_end = manifest_start
                                   ? strstr(manifest_start, "</manifest>")
                                   : NULL;
    const char *spine_start = strstr(opf, "<spine");
    const char *spine_end = spine_start ? strstr(spine_start, "</spine>")
                                         : NULL;
    int result = -1;

    if (!manifest_start || !manifest_end || !spine_start || !spine_end)
        return -1;

    const char *p = manifest_start;
    while ((p = strstr(p, "<item")) != NULL && p < manifest_end) {
        if (p[5] && !isspace((unsigned char)p[5]) && p[5] != '>') {
            p += 5;
            continue;
        }
        const char *end = simpleepub_tag_end(p);
        if (!end || end > manifest_end)
            break;
        char *id = simpleepub_xml_attribute(p, (size_t)(end - p + 1), "id");
        char *href = simpleepub_xml_attribute(p, (size_t)(end - p + 1),
                                              "href");
        char *media = simpleepub_xml_attribute(p, (size_t)(end - p + 1),
                                               "media-type");
        char *path = href ? simpleepub_resolve_path(package_path, href) : NULL;
        if (id && path && (simpleepub_has_html_extension(path) ||
                           (media && strstr(media, "xhtml")))) {
            if (simpleepub_manifest_add(&manifest, id, path) != 0) {
                free(id);
                free(href);
                free(media);
                free(path);
                goto done;
            }
        }
        free(id);
        free(href);
        free(media);
        free(path);
        p = end + 1;
    }

    p = spine_start;
    while ((p = strstr(p, "<itemref")) != NULL && p < spine_end) {
        const char *end = simpleepub_tag_end(p);
        if (!end || end > spine_end)
            break;
        char *idref = simpleepub_xml_attribute(p, (size_t)(end - p + 1),
                                               "idref");
        const char *path = idref ? simpleepub_manifest_path(&manifest, idref)
                                 : NULL;
        if (path && simpleepub_list_add(spine, path) != 0) {
            free(idref);
            goto done;
        }
        free(idref);
        p = end + 1;
    }

    result = spine->count > 0 ? 0 : -1;

done:
    simpleepub_manifest_free(&manifest);
    return result;
}

static int simpleepub_content_order(const char *infile,
                                    const SimpleEpubList *entries,
                                    SimpleEpubList *spine)
{
    const char *package_path = NULL;
    char *container = NULL;
    char *opf = NULL;
    size_t size = 0;
    int result = -1;

    if (simpleepub_read_entry(infile, "META-INF/container.xml", &container,
                              &size, (size_t)2 * 1024 * 1024) == 0) {
        const char *root = strstr(container, "<rootfile");
        const char *end = root ? simpleepub_tag_end(root) : NULL;
        if (root && end) {
            char *path = simpleepub_xml_attribute(
                root, (size_t)(end - root + 1), "full-path");
            if (path) {
                simpleepub_decode_path(path);
                int index = simpleepub_list_index(entries, path);
                if (index >= 0 && simpleepub_has_extension(path, ".opf"))
                    package_path = entries->items[index];
                free(path);
            }
        }
    }

    if (!package_path) {
        for (int i = 0; i < entries->count; i++)
            if (simpleepub_has_extension(entries->items[i], ".opf")) {
                package_path = entries->items[i];
                break;
            }
    }
    if (package_path &&
        simpleepub_read_entry(infile, package_path, &opf, &size,
                              (size_t)32 * 1024 * 1024) == 0)
        result = simpleepub_spine_from_opf(opf, package_path, spine);

    free(container);
    free(opf);

    if (result == 0)
        return 0;
    simpleepub_list_free(spine);
    for (int i = 0; i < entries->count; i++)
        if (simpleepub_has_html_extension(entries->items[i]) &&
            simpleepub_list_add(spine, entries->items[i]) != 0)
            return -1;
    return spine->count > 0 ? 0 : -1;
}

static int simpleepub_append_bytes(char **output, size_t *used,
                                   size_t *capacity, const char *data,
                                   size_t length)
{
    if (length > SIMPLEEPUB_CAPTURE_LIMIT - *used)
        return -1;
    if (*used + length + 1 > *capacity) {
        size_t grown_capacity = *capacity ? *capacity * 2 : 8192;
        while (grown_capacity < *used + length + 1) {
            if (grown_capacity > SIMPLEEPUB_CAPTURE_LIMIT / 2)
                return -1;
            grown_capacity *= 2;
        }
        char *grown = realloc(*output, grown_capacity);
        if (!grown)
            return -1;
        *output = grown;
        *capacity = grown_capacity;
    }
    memcpy(*output + *used, data, length);
    *used += length;
    (*output)[*used] = 0;
    return 0;
}

static int simpleepub_safe_unzip_name(const char *path)
{
    return path && *path && path[0] != '-' &&
           strpbrk(path, "*?[") == NULL;
}

static int simpleepub_read_content(const char *infile,
                                   const SimpleEpubList *entries,
                                   const SimpleEpubList *spine,
                                   char **html, size_t *html_size)
{
    int ordered = 1;
    int previous = -1;
    size_t argument_bytes = strlen(infile) + 32;

    for (int i = 0; i < spine->count; i++) {
        int index = simpleepub_list_index(entries, spine->items[i]);
        if (index < 0 || index <= previous ||
            !simpleepub_safe_unzip_name(spine->items[i]))
            ordered = 0;
        previous = index;
        argument_bytes += strlen(spine->items[i]) + 1;
    }

    if (ordered && argument_bytes <= SIMPLEEPUB_ARG_LIMIT) {
        char **argv = calloc((size_t)spine->count + 4, sizeof(*argv));
        if (!argv)
            return -1;
        argv[0] = "unzip";
        argv[1] = "-p";
        argv[2] = (char *)infile;
        for (int i = 0; i < spine->count; i++)
            argv[i + 3] = spine->items[i];
        int result = simpleepub_capture(argv, html, html_size,
                                        SIMPLEEPUB_CAPTURE_LIMIT);
        free(argv);
        return result;
    }

    char *combined = NULL;
    size_t used = 0;
    size_t capacity = 0;
    for (int i = 0; i < spine->count; i++) {
        char *part = NULL;
        size_t part_size = 0;
        if (!simpleepub_safe_unzip_name(spine->items[i]) ||
            simpleepub_read_entry(infile, spine->items[i], &part, &part_size,
                                  SIMPLEEPUB_CAPTURE_LIMIT) != 0 ||
            simpleepub_append_bytes(&combined, &used, &capacity, part,
                                    part_size) != 0 ||
            simpleepub_append_bytes(&combined, &used, &capacity, "\n\n", 2) !=
                0) {
            free(part);
            free(combined);
            return -1;
        }
        free(part);
    }
    if (!combined) {
        combined = simpleepub_strdup("");
        if (!combined)
            return -1;
    }
    *html = combined;
    *html_size = used;
    return 0;
}

static int simpleepub_text_reserve(SimpleEpubText *text, size_t extra)
{
    if (text->failed)
        return -1;
    if (extra > SIZE_MAX - text->length - 1) {
        text->failed = 1;
        return -1;
    }
    size_t needed = text->length + extra + 1;
    if (needed <= text->capacity)
        return 0;
    size_t capacity = text->capacity ? text->capacity * 2 : 8192;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            text->failed = 1;
            return -1;
        }
        capacity *= 2;
    }
    char *grown = realloc(text->data, capacity);
    if (!grown) {
        text->failed = 1;
        return -1;
    }
    text->data = grown;
    text->capacity = capacity;
    return 0;
}

static int simpleepub_text_raw(SimpleEpubText *text, const char *data,
                               size_t length)
{
    if (simpleepub_text_reserve(text, length) != 0)
        return -1;
    memcpy(text->data + text->length, data, length);
    text->length += length;
    text->data[text->length] = 0;
    return 0;
}

static void simpleepub_text_break(SimpleEpubText *text, int count)
{
    text->pending_space = 0;
    if (count > text->pending_breaks)
        text->pending_breaks = count;
}

static void simpleepub_text_space(SimpleEpubText *text)
{
    if (text->length > 0 && text->pending_breaks == 0)
        text->pending_space = 1;
}

static int simpleepub_text_flush(SimpleEpubText *text)
{
    if (text->pending_breaks > 0 && text->length > 0) {
        while (text->length > 0 &&
               (text->data[text->length - 1] == ' ' ||
                text->data[text->length - 1] == '\t'))
            text->length--;
        int existing = 0;
        while ((size_t)existing < text->length &&
               text->data[text->length - (size_t)existing - 1] == '\n')
            existing++;
        while (existing < text->pending_breaks) {
            if (simpleepub_text_raw(text, "\n", 1) != 0)
                return -1;
            existing++;
        }
    } else if (text->pending_space && text->length > 0 &&
               !isspace((unsigned char)text->data[text->length - 1])) {
        if (simpleepub_text_raw(text, " ", 1) != 0)
            return -1;
    }
    text->pending_space = 0;
    text->pending_breaks = 0;
    return 0;
}

static int simpleepub_text_codepoint(SimpleEpubText *text,
                                     unsigned long codepoint)
{
    char bytes[4];
    size_t count;

    if (codepoint == 0 || codepoint == 0xad || codepoint == 0x200b)
        return 0;
    if (codepoint == 0xa0 || codepoint == 0x2002 || codepoint == 0x2003 ||
        codepoint == 0x2009) {
        simpleepub_text_space(text);
        return 0;
    }
    if (simpleepub_text_flush(text) != 0)
        return -1;

    if (codepoint <= 0x7f) {
        bytes[0] = (char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3f));
        count = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (char)(0xe0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (codepoint & 0x3f));
        count = 3;
    } else if (codepoint <= 0x10ffff) {
        bytes[0] = (char)(0xf0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (codepoint & 0x3f));
        count = 4;
    } else {
        return 0;
    }
    return simpleepub_text_raw(text, bytes, count);
}

static int simpleepub_text_byte(SimpleEpubText *text, unsigned char byte)
{
    char value = (char)byte;
    if (simpleepub_text_flush(text) != 0)
        return -1;
    return simpleepub_text_raw(text, &value, 1);
}

static int simpleepub_text_literal(SimpleEpubText *text, const char *value)
{
    if (simpleepub_text_flush(text) != 0)
        return -1;
    return simpleepub_text_raw(text, value, strlen(value));
}

static int simpleepub_text_marker(SimpleEpubText *text, char kind,
                                  const char *value)
{
    static const char hex[] = "0123456789abcdef";
    char prefix[2] = {SIMPLEEPUB_MARKER_START, kind};

    if (simpleepub_text_flush(text) != 0 ||
        simpleepub_text_raw(text, prefix, sizeof prefix) != 0)
        return -1;
    if (value)
        for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
            char encoded[2] = {hex[*p >> 4], hex[*p & 15]};
            if (simpleepub_text_raw(text, encoded, sizeof encoded) != 0)
                return -1;
        }
    char end = SIMPLEEPUB_MARKER_END;
    return simpleepub_text_raw(text, &end, 1);
}

static int simpleepub_entity(const char *source, size_t remaining,
                             unsigned long *codepoint, size_t *consumed)
{
    static const struct {
        const char *name;
        unsigned long codepoint;
    } named[] = {
        {"amp", '&'},       {"lt", '<'},        {"gt", '>'},
        {"quot", '"'},     {"apos", '\''},     {"nbsp", 0xa0},
        {"ensp", 0x2002},  {"emsp", 0x2003},   {"thinsp", 0x2009},
        {"shy", 0xad},     {"ndash", 0x2013},  {"mdash", 0x2014},
        {"hellip", 0x2026},{"lsquo", 0x2018},  {"rsquo", 0x2019},
        {"ldquo", 0x201c}, {"rdquo", 0x201d},  {"bull", 0x2022},
        {"middot", 0xb7},  {"copy", 0xa9},     {"reg", 0xae},
        {"trade", 0x2122}, {"euro", 0x20ac},   {"pound", 0xa3},
        {"yen", 0xa5},     {"cent", 0xa2},     {"laquo", 0xab},
        {"raquo", 0xbb},   {"times", 0xd7},    {"divide", 0xf7},
        {"plusmn", 0xb1},  {"deg", 0xb0}
    };

    if (remaining < 3 || source[0] != '&')
        return 0;
    const char *semi = memchr(source + 1, ';', remaining - 1);
    if (!semi || semi - source > 32)
        return 0;
    size_t length = (size_t)(semi - source - 1);

    if (source[1] == '#') {
        const char *digits = source + 2;
        int base = 10;
        unsigned long value = 0;
        if (digits < semi && (*digits == 'x' || *digits == 'X')) {
            base = 16;
            digits++;
        }
        if (digits == semi)
            return 0;
        for (const char *p = digits; p < semi; p++) {
            int digit = base == 16 ? simpleepub_hex_value(*p)
                                   : (isdigit((unsigned char)*p) ? *p - '0'
                                                               : -1);
            if (digit < 0 || digit >= base || value > 0x10ffffUL / base)
                return 0;
            value = value * (unsigned long)base + (unsigned long)digit;
        }
        *codepoint = value;
        *consumed = (size_t)(semi - source + 1);
        return 1;
    }

    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        if (strlen(named[i].name) == length &&
            !strncmp(source + 1, named[i].name, length)) {
            *codepoint = named[i].codepoint;
            *consumed = (size_t)(semi - source + 1);
            return 1;
        }
    }
    return 0;
}

static int simpleepub_tag_name(const char *tag, size_t length, char *name,
                               size_t name_size, int *closing)
{
    const char *p = tag;
    const char *end = tag + length;
    size_t used = 0;

    *closing = 0;
    while (p < end && isspace((unsigned char)*p))
        p++;
    if (p < end && *p == '/') {
        *closing = 1;
        p++;
    }
    while (p < end && isspace((unsigned char)*p))
        p++;
    while (p < end && (isalnum((unsigned char)*p) || *p == '-' ||
                       *p == '_' || *p == ':')) {
        if (*p == ':')
            used = 0;
        else if (used + 1 < name_size)
            name[used++] = (char)tolower((unsigned char)*p);
        p++;
    }
    name[used] = 0;
    return used > 0;
}

static int simpleepub_is_heading(const char *name)
{
    return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' &&
           name[2] == 0;
}

static int simpleepub_is_block(const char *name)
{
    static const char *blocks[] = {
        "p", "div", "section", "article", "main", "header", "footer",
        "address", "figure", "figcaption", "blockquote", "pre", "table",
        "tr", "dl", "dt", "dd"
    };
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; i++)
        if (!strcmp(name, blocks[i]))
            return 1;
    return 0;
}

static const char *simpleepub_find_closing(const char *start,
                                           const char *document_end,
                                           const char *name)
{
    size_t name_length = strlen(name);
    for (const char *p = start; p < document_end; p++) {
        if (*p != '<' || p + 2 + name_length > document_end || p[1] != '/')
            continue;
        if (!strncasecmp(p + 2, name, name_length) &&
            (p + 2 + name_length == document_end ||
             isspace((unsigned char)p[2 + name_length]) ||
             p[2 + name_length] == '>'))
            return p;
    }
    return NULL;
}

static char *simpleepub_html_to_text_with_spine(
    const char *html, size_t html_length, const SimpleEpubList *spine)
{
    SimpleEpubText text = {0};
    const char *end = html + html_length;
    int in_pre = 0;
    int table_cells = 0;
    int document_index = -1;
    int internal_link_open = 0;
    const char *document_path = NULL;

    for (const char *p = html; p < end && !text.failed; ) {
        if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char *close = strstr(p + 4, "-->");
            p = close && close < end ? close + 3 : end;
            continue;
        }
        if (*p == '<') {
            const char *tag_end = simpleepub_tag_end(p);
            if (!tag_end || tag_end >= end)
                break;
            char name[32];
            int closing = 0;
            if (!simpleepub_tag_name(p + 1, (size_t)(tag_end - p - 1),
                                     name, sizeof name, &closing)) {
                p = tag_end + 1;
                continue;
            }

            if (!closing && (!strcmp(name, "head") ||
                             !strcmp(name, "style") ||
                             !strcmp(name, "script") ||
                             !strcmp(name, "svg") ||
                             !strcmp(name, "iframe") ||
                             !strcmp(name, "object") ||
                             !strcmp(name, "noscript"))) {
                const char *close = simpleepub_find_closing(tag_end + 1, end,
                                                            name);
                if (close) {
                    const char *close_end = simpleepub_tag_end(close);
                    p = close_end && close_end < end ? close_end + 1 : end;
                    continue;
                }
            }

            if (!closing && !strcmp(name, "html")) {
                document_index++;
                document_path = spine && document_index < spine->count
                                    ? spine->items[document_index] : NULL;
                if (document_path)
                    simpleepub_text_marker(&text, 'T', document_path);
            }

            if (simpleepub_is_heading(name)) {
                simpleepub_text_break(&text, closing ? 2 : 3);
            } else if (!strcmp(name, "br")) {
                simpleepub_text_break(&text, 1);
            } else if (!strcmp(name, "hr")) {
                simpleepub_text_break(&text, 2);
                simpleepub_text_literal(&text, "---");
                simpleepub_text_break(&text, 2);
            } else if (!strcmp(name, "li")) {
                simpleepub_text_break(&text, 1);
                if (!closing)
                    simpleepub_text_literal(&text, "* ");
            } else if (!strcmp(name, "td") || !strcmp(name, "th")) {
                if (!closing && table_cells++ > 0)
                    simpleepub_text_literal(&text, " | ");
            } else if (!strcmp(name, "tr")) {
                simpleepub_text_break(&text, 1);
                table_cells = 0;
            } else if (!strcmp(name, "pre")) {
                simpleepub_text_break(&text, 2);
                in_pre = !closing;
            } else if (simpleepub_is_block(name)) {
                simpleepub_text_break(&text, 2);
            } else if (!strcmp(name, "body") || !strcmp(name, "html")) {
                if (closing)
                    simpleepub_text_break(&text, 3);
            }


            if (!closing && document_path) {
                char *id = simpleepub_xml_attribute(
                    p, (size_t)(tag_end - p + 1), "id");
                if (!id && !strcmp(name, "a"))
                    id = simpleepub_xml_attribute(
                        p, (size_t)(tag_end - p + 1), "name");
                if (id) {
                    char *key = simpleepub_id_key(document_path, id);
                    if (key)
                        simpleepub_text_marker(&text, 'T', key);
                    free(key);
                    free(id);
                }
            }

            if (!closing && !strcmp(name, "a") && document_path) {
                char *href = simpleepub_xml_attribute(
                    p, (size_t)(tag_end - p + 1), "href");
                char *key = href
                                ? simpleepub_resolve_href_key(document_path,
                                                              href)
                                : NULL;
                if (key && simpleepub_text_marker(&text, 'L', key) == 0)
                    internal_link_open++;
                free(key);
                free(href);
            } else if (closing && !strcmp(name, "a") &&
                       internal_link_open > 0) {
                simpleepub_text_marker(&text, 'E', NULL);
                internal_link_open--;
            }
            p = tag_end + 1;
            continue;
        }

        if (*p == '&') {
            unsigned long codepoint = 0;
            size_t consumed = 0;
            if (simpleepub_entity(p, (size_t)(end - p), &codepoint,
                                  &consumed)) {
                simpleepub_text_codepoint(&text, codepoint);
                p += consumed;
                continue;
            }
        }

        unsigned char c = (unsigned char)*p++;
        if (isspace(c)) {
            if (in_pre && (c == '\n' || c == '\r'))
                simpleepub_text_break(&text, 1);
            else
                simpleepub_text_space(&text);
        } else {
            simpleepub_text_byte(&text, c);
        }
    }

    while (text.length > 0 && isspace((unsigned char)text.data[text.length - 1]))
        text.length--;
    if (!text.failed && text.length > 0) {
        text.pending_space = 0;
        text.pending_breaks = 0;
        simpleepub_text_raw(&text, "\n", 1);
    }
    if (text.failed) {
        free(text.data);
        return NULL;
    }
    if (!text.data)
        return simpleepub_strdup("");
    text.data[text.length] = 0;
    return text.data;
}

static int simpleepub_extract(const char *infile, const char *output_path)
{
    SimpleEpubList entries = {0};
    SimpleEpubList spine = {0};
    char *html = NULL;
    size_t html_size = 0;
    char *plain = NULL;
    int result = -1;

    if (simpleepub_archive_entries(infile, &entries) != 0 ||
        simpleepub_content_order(infile, &entries, &spine) != 0 ||
        simpleepub_read_content(infile, &entries, &spine, &html,
                                &html_size) != 0)
        goto done;

    plain = simpleepub_html_to_text_with_spine(html, html_size, &spine);
    if (!plain || !*plain)
        goto done;

    FILE *output = fopen(output_path, "wb");
    if (!output)
        goto done;
    size_t length = strlen(plain);
    int write_ok = fwrite(plain, 1, length, output) == length;
    int close_ok = fclose(output) == 0;
    if (write_ok && close_ok)
        result = 0;
    else
        result = -1;

done:
    free(html);
    free(plain);
    simpleepub_list_free(&spine);
    simpleepub_list_free(&entries);
    return result;
}

#endif
