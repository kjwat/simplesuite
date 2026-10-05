#ifndef SIMPLENOTE_STORE_H
#define SIMPLENOTE_STORE_H

/* Plain UTF-8 records with byte lengths: note text can contain any delimiter.
 * Files are replaced atomically; the directory lock covers a whole session. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SN_BATCH_SIZE 100
#define SN_TEXT_LIMIT (16u * 1024u * 1024u)
#define SN_FILE_HEADER "SimpleNote 1\n"
#define SN_DRAFT_HEADER "SimpleNote draft 1\n"

typedef struct SnBatch SnBatch;
typedef struct {
    char id[33];
    char created[25];
    char updated[25];
    char *text;
    size_t len;
    int deleted;
    SnBatch *batch;
} SnNote;

struct SnBatch {
    char path[PATH_MAX];
    int year;
    unsigned sequence;
    SnNote *notes[SN_BATCH_SIZE];
    size_t count;
};

typedef struct {
    char dir[PATH_MAX];
    char error[512];
    SnBatch **batches;
    size_t count, capacity;
    int lock_fd;
    int year;
} SnStore;

static inline int sn_error(SnStore *s, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(s->error, sizeof s->error, format, args);
    va_end(args);
    return 0;
}

static inline int sn_path(SnStore *s, char *out, const char *dir, const char *name)
{
    int n = snprintf(out, PATH_MAX, "%s/%s", dir, name);
    return (n >= 0 && n < PATH_MAX) || sn_error(s, "Notes path is too long");
}

static inline int sn_mkdirs(SnStore *s, const char *path)
{
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof copy) return sn_error(s, "Notes path is too long");
    strcpy(copy, path);
    for (char *p = copy + 1; ; p++) {
        if (*p == '/' || !*p) {
            char saved = *p;
            *p = 0;
            if (mkdir(copy, 0700) != 0 && errno != EEXIST)
                return sn_error(s, "Cannot create %s: %s", copy, strerror(errno));
            struct stat st;
            if (stat(copy, &st) != 0 || !S_ISDIR(st.st_mode))
                return sn_error(s, "Not a directory: %s", copy);
            *p = saved;
            if (!saved) break;
        }
    }
    return 1;
}

static inline int sn_sync_dir(SnStore *s, const char *path)
{
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof parent) return sn_error(s, "Notes path is too long");
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    int fd = open(slash ? (parent[0] ? parent : "/") : ".", O_RDONLY);
    if (fd < 0) return sn_error(s, "Cannot open notes directory: %s", strerror(errno));
    int rc = fsync(fd), saved = errno;
    close(fd);
    /* Some filesystems do not implement directory fsync. */
    if (rc && saved != EINVAL && saved != ENOTSUP)
        return sn_error(s, "Cannot sync notes directory: %s", strerror(saved));
    return 1;
}

static inline int sn_timestamp_valid(const char *v)
{
    int y, m, d, h, mi, sec, zh, zm;
    char sign, tail;
    return strlen(v) == 24 &&
        sscanf(v, "%4d-%2d-%2dT%2d:%2d:%2d%c%2d%2d%c",
               &y, &m, &d, &h, &mi, &sec, &sign, &zh, &zm, &tail) == 9 &&
        y >= 1900 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= 31 &&
        h >= 0 && h < 24 && mi >= 0 && mi < 60 && sec >= 0 && sec <= 60 &&
        (sign == '+' || sign == '-') && zh >= 0 && zh <= 23 && zm >= 0 && zm < 60;
}

static inline void sn_timestamp(char out[25])
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(out, 25, "%Y-%m-%dT%H:%M:%S%z", &local);
}

static inline int sn_id_valid(const char *id)
{
    return strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32;
}

static inline void sn_new_id(char out[33])
{
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t got = fd < 0 ? -1 : read(fd, bytes, sizeof bytes);
    if (fd >= 0) close(fd);
    if (got != (ssize_t)sizeof bytes) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        unsigned long long a = (unsigned long long)t.tv_sec;
        unsigned long long b = (unsigned long long)t.tv_nsec ^ ((unsigned long long)getpid() << 32);
        memcpy(bytes, &a, 8);
        memcpy(bytes + 8, &b, 8);
    }
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
}

static inline void sn_free_note(SnNote *n)
{
    if (n) { free(n->text); free(n); }
}

static inline void sn_store_close(SnStore *s)
{
    for (size_t i = 0; i < s->count; i++) {
        for (size_t j = 0; j < s->batches[i]->count; j++)
            sn_free_note(s->batches[i]->notes[j]);
        free(s->batches[i]);
    }
    free(s->batches);
    if (s->lock_fd >= 0) close(s->lock_fd);
    s->batches = NULL;
    s->count = s->capacity = 0;
    s->lock_fd = -1;
}

static inline SnNote *sn_find(SnStore *s, const char *id)
{
    for (size_t i = 0; i < s->count; i++)
        for (size_t j = 0; j < s->batches[i]->count; j++)
            if (!strcmp(s->batches[i]->notes[j]->id, id)) return s->batches[i]->notes[j];
    return NULL;
}

static inline int sn_write_records(SnStore *s, const char *path, const char *header,
                                   SnNote **notes, size_t count)
{
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path);
    if (n < 0 || n >= (int)sizeof tmp) return sn_error(s, "Notes path is too long");
    int fd = mkstemp(tmp);
    if (fd < 0) return sn_error(s, "Cannot save notes: %s", strerror(errno));
    FILE *file = fdopen(fd, "w");
    if (!file) { close(fd); unlink(tmp); return sn_error(s, "Cannot open notes output"); }
    int ok = fputs(header, file) != EOF;
    for (size_t i = 0; i < count && ok; i++) {
        SnNote *note = notes[i];
        ok = fprintf(file, "\n--- Note %s ---\nCreated: %s\nUpdated: %s\nDeleted: %d\nBytes: %zu\n\n",
                     note->id, note->created, note->updated, note->deleted, note->len) >= 0 &&
             fwrite(note->text, 1, note->len, file) == note->len &&
             fputs("\n--- End ---\n", file) != EOF;
    }
    if (fflush(file) || fsync(fd)) ok = 0;
    int saved = errno;
    if (fclose(file)) { saved = errno; ok = 0; }
    if (!ok || rename(tmp, path)) {
        if (ok) saved = errno;
        unlink(tmp);
        return sn_error(s, "Cannot save %s: %s", path, strerror(saved));
    }
    return sn_sync_dir(s, path);
}

static inline int sn_write_file(SnStore *s, const char *path, SnNote **notes, size_t count)
{
    return sn_write_records(s, path, SN_FILE_HEADER, notes, count);
}

static inline int sn_read_field(FILE *file, const char *prefix, char *out, size_t size)
{
    char line[512];
    if (!fgets(line, sizeof line, file) || !strchr(line, '\n') ||
        strncmp(line, prefix, strlen(prefix))) return 0;
    char *value = line + strlen(prefix);
    value[strcspn(value, "\n")] = 0;
    if (strlen(value) >= size) return 0;
    strcpy(out, value);
    return 1;
}

/* 1 = record, 0 = clean EOF, -1 = malformed/incomplete data. */
static inline int sn_read_note(FILE *file, SnNote **out)
{
    char line[512], id[64], tail, value[64];
    if (!fgets(line, sizeof line, file)) return ferror(file) ? -1 : 0;
    if (strcmp(line, "\n") || !fgets(line, sizeof line, file) ||
        sscanf(line, "--- Note %32[0123456789abcdef] ---%c", id, &tail) != 2 ||
        tail != '\n' || !sn_id_valid(id)) return -1;
    SnNote *note = calloc(1, sizeof *note);
    if (!note) return -1;
    strcpy(note->id, id);
    if (!sn_read_field(file, "Created: ", note->created, sizeof note->created) ||
        !sn_read_field(file, "Updated: ", note->updated, sizeof note->updated) ||
        !sn_timestamp_valid(note->created) || !sn_timestamp_valid(note->updated) ||
        !sn_read_field(file, "Deleted: ", value, sizeof value) ||
        (strcmp(value, "0") && strcmp(value, "1"))) goto invalid;
    note->deleted = value[0] == '1';
    if (!sn_read_field(file, "Bytes: ", value, sizeof value) || !value[0] ||
        strspn(value, "0123456789") != strlen(value)) goto invalid;
    errno = 0;
    unsigned long long length = strtoull(value, NULL, 10);
    if (errno || length > SN_TEXT_LIMIT) goto invalid;
    note->len = (size_t)length;
    if (!fgets(line, sizeof line, file) || strcmp(line, "\n")) goto invalid;
    note->text = malloc(note->len + 1);
    if (!note->text || fread(note->text, 1, note->len, file) != note->len ||
        memchr(note->text, 0, note->len)) goto invalid;
    note->text[note->len] = 0;
    if (!fgets(line, sizeof line, file) || strcmp(line, "\n") ||
        !fgets(line, sizeof line, file) || strcmp(line, "--- End ---\n")) goto invalid;
    *out = note;
    return 1;
invalid:
    sn_free_note(note);
    return -1;
}

static inline int sn_batch_name(const char *name, int *year, unsigned *sequence)
{
    int y, m, d, consumed = 0;
    unsigned seq;
    if (sscanf(name, "%4d-%2d-%2d-%u.txt%n", &y, &m, &d, &seq, &consumed) != 4 ||
        !consumed || name[consumed] || y < 1900 || y > 9999 ||
        m < 1 || m > 12 || d < 1 || d > 31 || seq < 1) return 0;
    /* Do not sweep arbitrary filenames that happen to parse as dates. */
    char expected[64];
    snprintf(expected, sizeof expected, "%04d-%02d-%02d-%03u.txt", y, m, d, seq);
    if (strcmp(name, expected)) return 0;
    *year = y; *sequence = seq;
    return 1;
}

static inline int sn_add_batch(SnStore *s, SnBatch *batch)
{
    if (s->count == s->capacity) {
        size_t cap = s->capacity ? s->capacity * 2 : 16;
        SnBatch **p = realloc(s->batches, cap * sizeof *p);
        if (!p) return sn_error(s, "Out of memory");
        s->batches = p; s->capacity = cap;
    }
    s->batches[s->count++] = batch;
    return 1;
}

static inline int sn_load_batch(SnStore *s, const char *path, int year, unsigned sequence)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode))
        return sn_error(s, "Not a regular notes file: %s", path);
    for (size_t i = 0; i < s->count; i++) {
        struct stat existing;
        if (!stat(s->batches[i]->path, &existing) && st.st_dev == existing.st_dev &&
            st.st_ino == existing.st_ino) return 1; /* interrupted year sweep */
        if (s->batches[i]->year == year && s->batches[i]->sequence == sequence)
            return sn_error(s, "Duplicate notes batch number: %s", path);
    }
    FILE *file = fopen(path, "r");
    if (!file) return sn_error(s, "Cannot read %s: %s", path, strerror(errno));
    char line[64];
    SnBatch *batch = calloc(1, sizeof *batch);
    if (!batch) { fclose(file); return sn_error(s, "Out of memory"); }
    strcpy(batch->path, path); batch->year = year; batch->sequence = sequence;
    int ok = fgets(line, sizeof line, file) && !strcmp(line, SN_FILE_HEADER);
    while (ok) {
        SnNote *note = NULL;
        int rc = sn_read_note(file, &note);
        if (!rc) break;
        if (rc < 0) { ok = 0; break; }
        if (batch->count == SN_BATCH_SIZE || atoi(note->created) != year || sn_find(s, note->id)) {
            sn_free_note(note); ok = 0; break;
        }
        for (size_t j = 0; j < batch->count; j++)
            if (!strcmp(batch->notes[j]->id, note->id)) ok = 0;
        if (!ok) { sn_free_note(note); break; }
        note->batch = batch; batch->notes[batch->count++] = note;
    }
    if (fclose(file)) ok = 0;
    if (ok && sn_add_batch(s, batch)) return 1;
    for (size_t i = 0; i < batch->count; i++) sn_free_note(batch->notes[i]);
    free(batch);
    return sn_error(s, "Cannot load notes file (incomplete or invalid): %s", path);
}

static inline int sn_load_dir(SnStore *s, const char *path, int archive_year)
{
    DIR *dir = opendir(path);
    if (!dir) return sn_error(s, "Cannot read %s: %s", path, strerror(errno));
    struct dirent *entry;
    int ok = 1;
    while ((entry = readdir(dir)) && ok) {
        int year; unsigned seq;
        if (!sn_batch_name(entry->d_name, &year, &seq)) continue;
        char file[PATH_MAX];
        if (!sn_path(s, file, path, entry->d_name)) { ok = 0; break; }
        if (archive_year && year != archive_year) {
            ok = sn_error(s, "Notes file is in the wrong year folder: %s", file); break;
        }
        ok = sn_load_batch(s, file, year, seq);
    }
    closedir(dir);
    return ok;
}

static inline int sn_rollover(SnStore *s, int year)
{
    for (size_t i = 0; i < s->count; i++) {
        SnBatch *b = s->batches[i];
        if (b->year >= year || strrchr(b->path, '/') != b->path + strlen(s->dir)) continue;
        char folder[PATH_MAX], target[PATH_MAX], name[16];
        snprintf(name, sizeof name, "%04d", b->year);
        if (!sn_path(s, folder, s->dir, name) || !sn_mkdirs(s, folder) ||
            !sn_path(s, target, folder, strrchr(b->path, '/') + 1)) return 0;
        if (link(b->path, target)) {
            struct stat a, c;
            if (errno != EEXIST || stat(b->path, &a) || stat(target, &c) ||
                a.st_dev != c.st_dev || a.st_ino != c.st_ino)
                return sn_error(s, "Cannot archive %s: %s", b->path, strerror(errno));
        }
        if (!sn_sync_dir(s, target)) return 0;
        if (unlink(b->path)) return sn_error(s, "Cannot finish year archive: %s", strerror(errno));
        char old[PATH_MAX]; strcpy(old, b->path); strcpy(b->path, target);
        if (!sn_sync_dir(s, old)) return 0;
    }
    s->year = year;
    return 1;
}

static inline int sn_store_open(SnStore *s, const char *dir, int year)
{
    memset(s, 0, sizeof *s); s->lock_fd = -1; s->year = year;
    if (!dir[0] || strlen(dir) >= sizeof s->dir) return sn_error(s, "Invalid notes directory");
    strcpy(s->dir, dir);
    size_t len = strlen(s->dir);
    while (len > 1 && s->dir[len - 1] == '/') s->dir[--len] = 0;
    if (!sn_mkdirs(s, s->dir)) return 0;
    char lock[PATH_MAX];
    if (!sn_path(s, lock, s->dir, ".lock")) return 0;
    s->lock_fd = open(lock, O_CREAT | O_RDWR, 0600);
    if (s->lock_fd < 0) return sn_error(s, "Cannot open notes lock: %s", strerror(errno));
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB))
        return sn_error(s, "These notes are already open in another simplenote session");
    fcntl(s->lock_fd, F_SETFD, FD_CLOEXEC);
    if (!sn_load_dir(s, s->dir, 0)) return 0;
    DIR *root = opendir(s->dir);
    if (!root) return sn_error(s, "Cannot read notes directory");
    struct dirent *entry;
    int ok = 1;
    while ((entry = readdir(root)) && ok) {
        if (strlen(entry->d_name) != 4 || strspn(entry->d_name, "0123456789") != 4) continue;
        int y = atoi(entry->d_name);
        if (y < 1900) continue;
        char path[PATH_MAX];
        if (!sn_path(s, path, s->dir, entry->d_name)) { ok = 0; break; }
        struct stat st;
        if (lstat(path, &st) || !S_ISDIR(st.st_mode)) continue;
        ok = sn_load_dir(s, path, y);
    }
    closedir(root);
    return ok && sn_rollover(s, year);
}

static inline SnBatch *sn_destination(SnStore *s, const char *created)
{
    int year = atoi(created);
    SnBatch *last = NULL;
    for (size_t i = 0; i < s->count; i++)
        if (s->batches[i]->year == year && (!last || s->batches[i]->sequence > last->sequence))
            last = s->batches[i];
    if (last && last->count < SN_BATCH_SIZE) return last;
    SnBatch *batch = calloc(1, sizeof *batch);
    if (!batch) { sn_error(s, "Out of memory"); return NULL; }
    batch->year = year; batch->sequence = last ? last->sequence + 1 : 1;
    if (!batch->sequence) { free(batch); sn_error(s, "Too many notes batches"); return NULL; }
    char folder[PATH_MAX], name[64];
    strcpy(folder, s->dir);
    if (year < s->year) {
        char y[16]; snprintf(y, sizeof y, "%04d", year);
        if (!sn_path(s, folder, s->dir, y) || !sn_mkdirs(s, folder)) { free(batch); return NULL; }
    }
    snprintf(name, sizeof name, "%.10s-%03u.txt", created, batch->sequence);
    if (!sn_path(s, batch->path, folder, name) || !sn_add_batch(s, batch)) { free(batch); return NULL; }
    return batch;
}

static inline int sn_store_put(SnStore *s, const char *id, const char *created,
                               const char *updated, const char *text, int deleted, SnNote **out)
{
    if (!sn_id_valid(id) || !sn_timestamp_valid(created) || !sn_timestamp_valid(updated) ||
        strlen(text) > SN_TEXT_LIMIT || (deleted != 0 && deleted != 1))
        return sn_error(s, "Invalid note or note exceeds 16 MiB");
    SnNote *note = sn_find(s, id);
    SnBatch *batch = note ? note->batch : sn_destination(s, created);
    if (!batch) return 0;
    char *copy = strdup(text);
    if (!copy) return sn_error(s, "Out of memory");
    int is_new = !note;
    SnNote before;
    if (is_new) {
        note = calloc(1, sizeof *note);
        if (!note) { free(copy); return sn_error(s, "Out of memory"); }
        strcpy(note->id, id); strcpy(note->created, created);
        note->batch = batch; batch->notes[batch->count++] = note;
    }
    before = *note;
    note->text = copy; note->len = strlen(copy); note->deleted = deleted;
    strcpy(note->updated, updated);
    if (!sn_write_file(s, batch->path, batch->notes, batch->count)) {
        *note = before; free(copy);
        if (is_new) { batch->count--; free(note); }
        return 0;
    }
    free(before.text);
    if (out) *out = note;
    s->error[0] = 0;
    return 1;
}

/* Discarding a new page removes its autosave outright, without making trash. */
static inline int sn_store_remove(SnStore *s, const char *id)
{
    SnNote *note = sn_find(s, id);
    if (!note) { s->error[0] = 0; return 1; }
    SnBatch *batch = note->batch;
    SnNote *remaining[SN_BATCH_SIZE];
    size_t count = 0;
    for (size_t i = 0; i < batch->count; i++)
        if (batch->notes[i] != note) remaining[count++] = batch->notes[i];
    if (count) {
        if (!sn_write_file(s, batch->path, remaining, count)) return 0;
    } else {
        if (unlink(batch->path) && errno != ENOENT)
            return sn_error(s, "Cannot discard note: %s", strerror(errno));
        if (!sn_sync_dir(s, batch->path)) return 0;
    }
    memcpy(batch->notes, remaining, count * sizeof *remaining);
    batch->count = count;
    sn_free_note(note);
    if (!count) {
        for (size_t i = 0; i < s->count; i++) if (s->batches[i] == batch) {
            memmove(s->batches + i, s->batches + i + 1,
                    (s->count - i - 1) * sizeof *s->batches);
            s->count--;
            break;
        }
        free(batch);
    }
    s->error[0] = 0;
    return 1;
}

static inline int sn_draft_path(SnStore *s, char path[PATH_MAX])
{
    return sn_path(s, path, s->dir, ".draft");
}

static inline int sn_save_edit_draft(SnStore *s, SnNote *note, SnNote *original)
{
    char path[PATH_MAX];
    SnNote *notes[] = {note, original};
    return sn_draft_path(s, path) &&
        sn_write_records(s, path, SN_DRAFT_HEADER, notes, original ? 2 : 1);
}

static inline int sn_save_draft(SnStore *s, SnNote *note)
{
    return sn_save_edit_draft(s, note, NULL);
}

/* The second record is the saved version from before an editing session. A
 * single record with the draft header identifies a new, discardable page.
 * Older recovery files have the normal file header and no baseline metadata. */
static inline int sn_load_edit_draft(SnStore *s, SnNote **out, SnNote **original,
                                    int *baseline_known)
{
    char path[PATH_MAX], header[64];
    *out = *original = NULL;
    *baseline_known = 0;
    if (!sn_draft_path(s, path)) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return errno == ENOENT || sn_error(s, "Cannot read note recovery file");
    int ok = fgets(header, sizeof header, file) &&
        (!strcmp(header, SN_FILE_HEADER) || !strcmp(header, SN_DRAFT_HEADER));
    if (ok) {
        *baseline_known = !strcmp(header, SN_DRAFT_HEADER);
        ok = sn_read_note(file, out) == 1;
        if (ok && *baseline_known) {
            int rc = sn_read_note(file, original);
            ok = rc >= 0;
            if (ok && *original)
                ok = !strcmp((*out)->id, (*original)->id) &&
                    !strcmp((*out)->created, (*original)->created);
        }
        if (ok) ok = fgetc(file) == EOF && !ferror(file);
    }
    fclose(file);
    if (!ok) {
        sn_free_note(*out); sn_free_note(*original); *out = *original = NULL;
        return sn_error(s, "Incomplete note recovery file: %s", path);
    }
    return 1;
}

static inline int sn_load_draft(SnStore *s, SnNote **out)
{
    SnNote *original = NULL;
    int baseline_known;
    int ok = sn_load_edit_draft(s, out, &original, &baseline_known);
    sn_free_note(original);
    return ok;
}

static inline int sn_clear_draft(SnStore *s)
{
    char path[PATH_MAX];
    if (!sn_draft_path(s, path)) return 0;
    if (unlink(path) && errno != ENOENT) return sn_error(s, "Cannot clear note recovery file");
    return sn_sync_dir(s, path);
}
#endif
