/* simpleradio.c
   Native C port of simpleradio.py.

   Build from the SimpleSuite directory with ./build.sh.

   Runtime dependency:
     mpv
*/

#define _GNU_SOURCE

#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "simpleui.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_VOLUME 130
#define STALL_TIMEOUT_SECONDS 15
#define WATCHDOG_POLL_SECONDS 1
#define RECONNECT_BACKOFF_MAX_SECONDS 30
#define MPV_STARTUP_TIMEOUT_SECONDS 10
#define MPV_IPC_CONNECT_TIMEOUT_MS 10
#define MPV_IPC_RESPONSE_TIMEOUT_MS 10

typedef enum {
    ENTRY_UP,
    ENTRY_DIR,
    ENTRY_PLAYLIST,
    ENTRY_STATION,
    ENTRY_EMPTY
} EntryKind;

typedef struct {
    char *title;
    char *url;
    char *source;
} Station;

typedef struct {
    EntryKind kind;
    char *path;
    Station *station;
    char *label;
} Entry;

typedef struct {
    Entry *items;
    size_t len;
    size_t cap;
} EntryList;

typedef struct {
    Station *items;
    size_t len;
    size_t cap;
} StationList;

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StringList;

typedef struct {
    dev_t device;
    ino_t inode;
    off_t size;
    time_t modified;
    long modified_nsec;
    bool valid;
} ListingStamp;

typedef enum {
    PLAYBACK_EVENT_NONE,
    PLAYBACK_EVENT_READY,
    PLAYBACK_EVENT_FAILED,
    PLAYBACK_EVENT_EXITED
} PlaybackEventKind;

typedef struct {
    char *url;
    char *title;
    char *source;
    bool reset_attempts;
    unsigned long generation;
} PlaybackJob;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool thread_started;
    bool shutting_down;

    char *queued_url;
    char *queued_title;
    char *queued_source;
    bool queued_reset_attempts;
    bool request_pending;
    unsigned long request_generation;

    pid_t player_pid;
    bool player_ready;
    unsigned long active_generation;
    int requested_volume;

    PlaybackEventKind event_kind;
    unsigned long event_generation;
    bool event_reset_attempts;
    char event_message[160];
} PlaybackWorker;

static PlaybackWorker playback = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .player_pid = -1,
    .requested_volume = 100,
};

extern char **environ;

static char mpv_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)] =
    "";
static char mpv_socket_tmpdir[PATH_MAX] = "";

/* forward declaration for now-playing header */
static void set_now_playing(const char *title);

static int current_volume = 100;
static bool paused = false;
static bool continuous = false;
static EntryList playlist = {0};
static long play_index = -1;
static char *now_playing_title = NULL;
static bool station_playing = false;
static time_t last_playback_progress = 0;
static time_t last_watchdog_poll = 0;
static double last_playback_position = -1.0;
static int reconnect_attempt = 0;
static bool reconnect_pending = false;
static time_t reconnect_at = 0;

typedef struct {
    time_t sampled_at;
    char title[4096];
    bool have_position;
    double position;
    bool have_paused_for_cache;
    bool paused_for_cache;
} MpvPeriodicSnapshot;

static MpvPeriodicSnapshot periodic_mpv_snapshot;

static int NORMAL_ATTR = 0;
static int SELECTED_ATTR = 0;
static int PLAYING_ATTR = 0;
static int PLAYING_SELECTED_ATTR = 0;

static long long monotonic_millis(void);

static ListingStamp listing_stamp(const char *path) {
    ListingStamp stamp = {0};
    struct stat st;

    if (!path || stat(path, &st) != 0)
        return stamp;
    stamp.device = st.st_dev;
    stamp.inode = st.st_ino;
    stamp.size = st.st_size;
    stamp.modified = st.st_mtime;
#ifdef __APPLE__
    stamp.modified_nsec = st.st_mtimespec.tv_nsec;
#else
    stamp.modified_nsec = st.st_mtim.tv_nsec;
#endif
    stamp.valid = true;
    return stamp;
}

static bool listing_stamp_changed(ListingStamp old, ListingStamp current) {
    return old.valid != current.valid ||
           (old.valid &&
            (old.device != current.device || old.inode != current.inode ||
             old.size != current.size || old.modified != current.modified ||
             old.modified_nsec != current.modified_nsec));
}

static void cleanup_mpv_socket_path(void) {
    if (mpv_socket_path[0])
        unlink(mpv_socket_path);
    if (mpv_socket_tmpdir[0]) {
        rmdir(mpv_socket_tmpdir);
        mpv_socket_tmpdir[0] = '\0';
    }
}

static bool set_private_tmp_mpv_socket_path(void) {
    struct sockaddr_un addr;
    const char *bases[2] = { getenv("TMPDIR"), "/tmp" };

    for (size_t i = 0; i < 2; i++) {
        const char *base = bases[i];
        char tmpl[PATH_MAX];
        char *dir;
        int n;

        if (!base || !base[0])
            continue;
        if (i == 1 && bases[0] && bases[0][0] && strcmp(bases[0], "/tmp") == 0)
            continue;

        n = snprintf(tmpl, sizeof(tmpl), "%s/simpleradio-mpv-XXXXXX", base);
        if (n < 0 || (size_t)n >= sizeof(tmpl))
            continue;

        dir = mkdtemp(tmpl);
        if (!dir)
            continue;

        n = snprintf(mpv_socket_path, sizeof(mpv_socket_path), "%s/socket", dir);
        if (n >= 0 && (size_t)n < sizeof(mpv_socket_path) &&
            (size_t)n < sizeof(addr.sun_path)) {
            snprintf(mpv_socket_tmpdir, sizeof(mpv_socket_tmpdir), "%s", dir);
            return true;
        }

        rmdir(dir);
        mpv_socket_path[0] = '\0';
    }

    return false;
}

static void init_mpv_socket_path(void) {
    struct sockaddr_un addr;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int n = -1;

    if (runtime && runtime[0]) {
        n = snprintf(mpv_socket_path, sizeof(mpv_socket_path),
                     "%s/simpleradio-mpv-%ld.sock", runtime, (long)getpid());
    }

    if (n < 0 || (size_t)n >= sizeof(mpv_socket_path) ||
        (size_t)n >= sizeof(addr.sun_path)) {
        if (!set_private_tmp_mpv_socket_path())
            mpv_socket_path[0] = '\0';
    }

    if (mpv_socket_path[0])
        unlink(mpv_socket_path);
    atexit(cleanup_mpv_socket_path);
}

static bool mpv_socket_addr(struct sockaddr_un *addr) {
    size_t len = strlen(mpv_socket_path);

    if (len >= sizeof(addr->sun_path))
        return false;

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, mpv_socket_path, len + 1);
    return true;
}

static bool write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }

        if (n == 0)
            return false;

        buf += n;
        len -= (size_t)n;
    }

    return true;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) {
        perror("strdup");
        exit(1);
    }
    return p;
}

static char *xasprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *out = NULL;
    if (vasprintf(&out, fmt, ap) < 0 || !out) {
        va_end(ap);
        perror("vasprintf");
        exit(1);
    }
    va_end(ap);
    return out;
}

static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p) {
        perror("realloc");
        exit(1);
    }
    return p;
}

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool is_dir_path(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file_path(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *dir_name_dup(const char *path) {
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return xstrdup(".");
    }
    if (slash == copy) {
        slash[1] = '\0';
        return copy;
    }
    *slash = '\0';
    return copy;
}

static char *join_path(const char *a, const char *b) {
    if (!a || !*a) return xstrdup(b);
    size_t alen = strlen(a);
    if (alen && a[alen - 1] == '/') return xasprintf("%s%s", a, b);
    return xasprintf("%s/%s", a, b);
}

static char *parent_path_dup(const char *path) {
    if (!path || !*path) return xstrdup("/");
    char resolved[PATH_MAX];
    const char *p = path;
    if (realpath(path, resolved)) p = resolved;
    char *copy = xstrdup(p);
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return xstrdup(".");
    }
    if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return copy;
}

static char *trim_inplace(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    while ((*s == '"' || *s == '\'') && s[1]) s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == '"' || end[-1] == '\'')) *--end = '\0';
    return s;
}

static char *clean_station_name(const char *name) {
    char *copy = xstrdup(name ? name : "");
    char *t = trim_inplace(copy);
    char *out = malloc(strlen(t) + 1);
    if (!out) exit(1);
    size_t j = 0;
    bool in_space = false;
    for (size_t i = 0; t[i]; i++) {
        if (isspace((unsigned char)t[i])) {
            if (!in_space && j > 0) out[j++] = ' ';
            in_space = true;
        } else {
            out[j++] = t[i];
            in_space = false;
        }
    }
    if (j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
    free(copy);
    return out;
}

static bool has_suffix_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), su = strlen(suffix);
    if (su > sl) return false;
    return strcasecmp(s + sl - su, suffix) == 0;
}

static bool playlist_file(const char *path) {
    return is_file_path(path) &&
           (has_suffix_ci(path, ".m3u") || has_suffix_ci(path, ".m3u8") || has_suffix_ci(path, ".pls"));
}

static bool is_url(const char *value) {
    if (!value) return false;
    const char *colon = strstr(value, "://");
    if (!colon) return false;
    size_t n = (size_t)(colon - value);
    char scheme[16];
    if (n >= sizeof scheme) return false;
    for (size_t i = 0; i < n; i++) scheme[i] = (char)tolower((unsigned char)value[i]);
    scheme[n] = '\0';
    return strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0 ||
           strcmp(scheme, "ftp") == 0 || strcmp(scheme, "ftps") == 0 ||
           strcmp(scheme, "icy") == 0 || strcmp(scheme, "mms") == 0 ||
           strcmp(scheme, "rtmp") == 0 || strcmp(scheme, "rtsp") == 0;
}

static char *expand_home(const char *value) {
    if (!value || value[0] != '~') return xstrdup(value ? value : "");
    const char *home = getenv("HOME");
    if (!home) home = "";
    if (value[1] == '\0') return xstrdup(home);
    if (value[1] == '/') return xasprintf("%s%s", home, value + 1);
    return xstrdup(value);
}

static char *resolve_playlist_target(const char *base_path, const char *value) {
    char *copy = xstrdup(value ? value : "");
    char *t = trim_inplace(copy);
    if (!*t) {
        free(copy);
        return xstrdup("");
    }
    if (is_url(t)) {
        char *out = xstrdup(t);
        free(copy);
        return out;
    }
    char *expanded = expand_home(t);
    free(copy);
    if (expanded[0] == '/') return expanded;
    char *dir = dir_name_dup(base_path);
    char *joined = join_path(dir, expanded);
    free(dir);
    free(expanded);
    char resolved[PATH_MAX];
    if (realpath(joined, resolved)) {
        free(joined);
        return xstrdup(resolved);
    }
    return joined;
}

static char *label_from_url(const char *url) {
    if (!url || !*url) return xstrdup("");
    if (!is_url(url)) return clean_station_name(base_name(url));

    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *path = strchr(start, '/');
    char *host = NULL;
    if (path) host = strndup(start, (size_t)(path - start));
    else host = xstrdup(start);

    const char *tail = path ? base_name(path) : "";
    if (tail && *tail && strcmp(tail, ";") != 0 && strcmp(tail, "/") != 0) {
        char *clean = clean_station_name(tail);
        free(host);
        return clean;
    }
    char *clean = clean_station_name(host);
    free(host);
    if (*clean) return clean;
    free(clean);
    return xstrdup(url);
}

static void stringlist_push(StringList *list, char *s) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = xrealloc(list->items, list->cap * sizeof(char *));
    }
    list->items[list->len++] = s;
}

static void stringlist_free(StringList *list) {
    for (size_t i = 0; i < list->len; i++) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->len = list->cap = 0;
}

static int cmp_string_ptr_ci(const void *a, const void *b) {
    const char *pa = *(char * const *)a;
    const char *pb = *(char * const *)b;
    return strcasecmp(base_name(pa), base_name(pb));
}

static void station_free(Station *s) {
    if (!s) return;
    free(s->title);
    free(s->url);
    free(s->source);
    s->title = s->url = s->source = NULL;
}

static Station station_clone(const Station *s) {
    Station out = {0};
    if (!s) return out;
    out.title = xstrdup(s->title);
    out.url = xstrdup(s->url);
    out.source = xstrdup(s->source);
    return out;
}

static void stationlist_push(StationList *list, Station s) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = xrealloc(list->items, list->cap * sizeof(Station));
    }
    list->items[list->len++] = s;
}

static void stationlist_free(StationList *list) {
    for (size_t i = 0; i < list->len; i++) station_free(&list->items[i]);
    free(list->items);
    list->items = NULL;
    list->len = list->cap = 0;
}

static void entry_free(Entry *e) {
    if (!e) return;
    free(e->path);
    free(e->label);
    if (e->station) {
        station_free(e->station);
        free(e->station);
    }
    memset(e, 0, sizeof *e);
}

static Entry entry_clone(const Entry *e) {
    Entry out = {0};
    out.kind = e->kind;
    out.path = xstrdup(e->path);
    out.label = xstrdup(e->label ? e->label : "");
    if (e->station) {
        out.station = malloc(sizeof(Station));
        if (!out.station) exit(1);
        *out.station = station_clone(e->station);
    }
    return out;
}

static void entrylist_push(EntryList *list, Entry e) {
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 32;
        list->items = xrealloc(list->items, list->cap * sizeof(Entry));
    }
    list->items[list->len++] = e;
}

static void entrylist_clear(EntryList *list) {
    for (size_t i = 0; i < list->len; i++) entry_free(&list->items[i]);
    list->len = 0;
}

static void entrylist_free(EntryList *list) {
    entrylist_clear(list);
    free(list->items);
    list->items = NULL;
    list->cap = 0;
}

static Entry make_entry(EntryKind kind, const char *path, const char *label, const Station *station) {
    Entry e = {0};
    e.kind = kind;
    e.path = xstrdup(path ? path : "");
    e.label = xstrdup(label ? label : "");
    if (station) {
        e.station = malloc(sizeof(Station));
        if (!e.station) exit(1);
        *e.station = station_clone(station);
    }
    return e;
}

static FILE *open_text_file(const char *path) {
    return fopen(path, "r");
}

static StationList parse_m3u(const char *path) {
    StationList stations = {0};
    FILE *f = open_text_file(path);
    if (!f) return stations;

    char *line = NULL;
    size_t n = 0;
    char *pending_title = xstrdup("");

    while (getline(&line, &n, f) != -1) {
        char *t = trim_inplace(line);
        if (!*t) continue;
        if (strncmp(t, "#EXTINF", 7) == 0) {
            char *comma = strchr(t, ',');
            free(pending_title);
            pending_title = comma ? clean_station_name(comma + 1) : xstrdup("");
            continue;
        }
        if (*t == '#') continue;

        char *target = resolve_playlist_target(path, t);
        if (!*target) {
            free(target);
            continue;
        }
        char *fallback = label_from_url(target);
        Station s = {0};
        s.title = *pending_title ? xstrdup(pending_title) : fallback;
        if (*pending_title) free(fallback);
        s.url = target;
        s.source = xstrdup(path);
        stationlist_push(&stations, s);
        free(pending_title);
        pending_title = xstrdup("");
    }

    free(pending_title);
    free(line);
    fclose(f);
    return stations;
}

typedef struct {
    int number;
    char *file;
    char *title;
} PlsItem;

typedef struct {
    PlsItem *items;
    size_t len;
    size_t cap;
} PlsList;

static PlsItem *pls_get(PlsList *list, int number) {
    for (size_t i = 0; i < list->len; i++) {
        if (list->items[i].number == number) return &list->items[i];
    }
    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = xrealloc(list->items, list->cap * sizeof(PlsItem));
    }
    PlsItem *it = &list->items[list->len++];
    it->number = number;
    it->file = NULL;
    it->title = NULL;
    return it;
}

static int cmp_pls_item(const void *a, const void *b) {
    const PlsItem *pa = a;
    const PlsItem *pb = b;
    return (pa->number > pb->number) - (pa->number < pb->number);
}

static void plslist_free(PlsList *list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i].file);
        free(list->items[i].title);
    }
    free(list->items);
}

static StationList parse_pls(const char *path) {
    StationList stations = {0};
    FILE *f = open_text_file(path);
    if (!f) return stations;

    PlsList pls = {0};
    char *line = NULL;
    size_t n = 0;

    while (getline(&line, &n, f) != -1) {
        char *t = trim_inplace(line);
        if (!*t || *t == '[') continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_inplace(t);
        char *value = trim_inplace(eq + 1);

        size_t klen = strlen(key);
        size_t pos = klen;
        while (pos > 0 && isdigit((unsigned char)key[pos - 1])) pos--;
        if (pos == klen || pos == 0) continue;
        int number = atoi(key + pos);
        key[pos] = '\0';

        PlsItem *it = pls_get(&pls, number);
        if (strcasecmp(key, "File") == 0) {
            free(it->file);
            it->file = xstrdup(value);
        } else if (strcasecmp(key, "Title") == 0) {
            free(it->title);
            it->title = xstrdup(value);
        }
    }

    qsort(pls.items, pls.len, sizeof(PlsItem), cmp_pls_item);
    for (size_t i = 0; i < pls.len; i++) {
        if (!pls.items[i].file || !*pls.items[i].file) continue;
        char *target = resolve_playlist_target(path, pls.items[i].file);
        char *clean_title = clean_station_name(pls.items[i].title ? pls.items[i].title : "");
        char *fallback = label_from_url(target);
        Station s = {0};
        s.title = *clean_title ? clean_title : fallback;
        if (*clean_title) free(fallback); else free(clean_title);
        s.url = target;
        s.source = xstrdup(path);
        stationlist_push(&stations, s);
    }

    free(line);
    fclose(f);
    plslist_free(&pls);
    return stations;
}

static StationList parse_playlist(const char *path) {
    if (has_suffix_ci(path, ".m3u") || has_suffix_ci(path, ".m3u8")) return parse_m3u(path);
    if (has_suffix_ci(path, ".pls")) return parse_pls(path);
    StationList empty = {0};
    return empty;
}

static void list_entries(const char *path, StringList *dirs, StringList *playlists) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char *full = join_path(path, de->d_name);
        if (stat(full, &st) != 0) free(full);
        else if (S_ISDIR(st.st_mode)) stringlist_push(dirs, full);
        else if (S_ISREG(st.st_mode) &&
                 (has_suffix_ci(full, ".m3u") ||
                  has_suffix_ci(full, ".m3u8") ||
                  has_suffix_ci(full, ".pls")))
            stringlist_push(playlists, full);
        else free(full);
    }
    closedir(d);
    qsort(dirs->items, dirs->len, sizeof(char *), cmp_string_ptr_ci);
    qsort(playlists->items, playlists->len, sizeof(char *), cmp_string_ptr_ci);
}

static bool stringlist_contains(StringList *list, const char *s) {
    for (size_t i = 0; i < list->len; i++) {
        if (strcmp(list->items[i], s) == 0) return true;
    }
    return false;
}

static StringList choose_start_roots(void) {
    StringList roots = {0};
    const char *home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;
    const char *login = getlogin();
    if (!login) login = getenv("USER");
    if (!login) login = "";

    char *candidates[] = {
        xstrdup(home),
        join_path(home, "Music"),
        join_path(home, "Downloads"),
        xasprintf("/media/%s", login),
        xasprintf("/run/media/%s", login),
        xstrdup("/mnt"),
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char *root = candidates[i];
        if (!path_exists(root)) {
            free(root);
            continue;
        }
        if (i < 3) {
            if (!stringlist_contains(&roots, root)) stringlist_push(&roots, root);
            else free(root);
        } else {
            DIR *d = opendir(root);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                    char *full = join_path(root, de->d_name);
                    if (is_dir_path(full) && !stringlist_contains(&roots, full)) stringlist_push(&roots, full);
                    else free(full);
                }
                closedir(d);
            }
            free(root);
        }
    }
    return roots;
}

static EntryList make_entries(const char *current, StringList *roots, char **title_out) {
    EntryList entries = {0};

    if (!current) {
        for (size_t i = 0; i < roots->len; i++) {
            entrylist_push(&entries, make_entry(ENTRY_DIR, roots->items[i], NULL, NULL));
        }
        *title_out = xstrdup("Choose Playlist Root");
        return entries;
    }

    if (playlist_file(current)) {
        char *parent = parent_path_dup(current);
        entrylist_push(&entries, make_entry(ENTRY_UP, parent, "../", NULL));
        free(parent);

        StationList stations = parse_playlist(current);
        for (size_t i = 0; i < stations.len; i++) {
            entrylist_push(&entries, make_entry(ENTRY_STATION, current, stations.items[i].title, &stations.items[i]));
        }
        stationlist_free(&stations);

        if (entries.len == 1) {
            entrylist_push(&entries, make_entry(ENTRY_EMPTY, current, "No playable stations found", NULL));
        }
        *title_out = xasprintf("RADIO: %s", base_name(current));
        return entries;
    }

    char *parent = parent_path_dup(current);
    entrylist_push(&entries, make_entry(ENTRY_UP, parent, "../", NULL));
    free(parent);

    StringList dirs = {0}, pls = {0};
    list_entries(current, &dirs, &pls);
    for (size_t i = 0; i < dirs.len; i++) entrylist_push(&entries, make_entry(ENTRY_DIR, dirs.items[i], NULL, NULL));
    for (size_t i = 0; i < pls.len; i++) entrylist_push(&entries, make_entry(ENTRY_PLAYLIST, pls.items[i], NULL, NULL));
    stringlist_free(&dirs);
    stringlist_free(&pls);

    *title_out = xstrdup(current);
    return entries;
}

static void reset_stream_watchdog(void) {
    station_playing = false;
    last_playback_progress = 0;
    last_watchdog_poll = 0;
    last_playback_position = -1.0;
    reconnect_attempt = 0;
    reconnect_pending = false;
    reconnect_at = 0;
    memset(&periodic_mpv_snapshot, 0, sizeof(periodic_mpv_snapshot));
}

static void note_player_started(bool reset_attempts) {
    station_playing = true;
    last_playback_progress = time(NULL);
    last_watchdog_poll = 0;
    last_playback_position = -1.0;
    reconnect_pending = false;
    reconnect_at = 0;
    memset(&periodic_mpv_snapshot, 0, sizeof(periodic_mpv_snapshot));
    if (reset_attempts)
        reconnect_attempt = 0;
}

static int connect_mpv_socket(int timeout_ms) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    if (!mpv_socket_addr(&addr)) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0)
        fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        return fd;

    if (errno != EINPROGRESS && errno != EAGAIN) {
        close(fd);
        return -1;
    }

    struct pollfd pfd = {
        .fd = fd,
        .events = POLLOUT,
    };
    int ready;
    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);

    if (ready <= 0) {
        close(fd);
        return -1;
    }

    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                   &error_size) != 0 || socket_error != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool mpv_command_raw(const char *json) {
    int fd = connect_mpv_socket(MPV_IPC_CONNECT_TIMEOUT_MS);
    bool ok = false;

    if (fd < 0)
        return false;

    ok = write_all(fd, json, strlen(json)) &&
         write_all(fd, "\n", 1);
    close(fd);
    return ok;
}

static size_t mpv_read_responses(int fd, char *buf, size_t size,
                                 size_t expected_responses) {
    size_t used = 0;
    size_t responses = 0;
    long long deadline = monotonic_millis() + MPV_IPC_RESPONSE_TIMEOUT_MS;

    if (!buf || size < 2)
        return 0;
    buf[0] = '\0';
    while (used + 1 < size) {
        ssize_t count = read(fd, buf + used, size - used - 1);

        if (count > 0) {
            size_t old_used = used;
            used += (size_t)count;
            buf[used] = '\0';
            for (size_t i = old_used; i < used; i++)
                if (buf[i] == '\n') responses++;
            if (responses >= expected_responses)
                break;
            continue;
        }
        if (count == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            break;

        long long remaining = deadline - monotonic_millis();
        if (remaining <= 0)
            break;
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        int ready;
        do {
            ready = poll(&poll_fd, 1, (int)remaining);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0)
            break;
    }
    return used;
}

static size_t mpv_read_response(int fd, char *buf, size_t size) {
    return mpv_read_responses(fd, buf, size, 1);
}

static char *json_data_string(const char *json) {
    const char *p = strstr(json, "\"data\":");
    if (!p) return NULL;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "null", 4) == 0)
        return NULL;

    if (*p != '"')
        return NULL;

    p++;
    char out[4096];
    size_t j = 0;

    while (*p && j < sizeof(out) - 1) {
        if (*p == '"' && (p == json || p[-1] != '\\'))
            break;

        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') out[j++] = ' ';
            else if (*p == 't') out[j++] = ' ';
            else out[j++] = *p;
        } else {
            out[j++] = *p;
        }

        p++;
    }

    out[j] = '\0';
    if (!out[0]) return NULL;
    return clean_station_name(out);
}

static bool json_data_bool(const char *json, bool *value) {
    const char *p = json;

    while ((p = strstr(p, "\"data\":")) != NULL) {
        p += 7;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "true", 4) == 0) {
            *value = true;
            return true;
        }

        if (strncmp(p, "false", 5) == 0) {
            *value = false;
            return true;
        }

        p++;
    }

    return false;
}

static bool json_data_double(const char *json, double *value) {
    const char *p = json;

    while ((p = strstr(p, "\"data\":")) != NULL) {
        p += 7;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "null", 4) == 0) {
            p++;
            continue;
        }

        char *end = NULL;
        errno = 0;
        double v = strtod(p, &end);
        if (end && end > p && errno != ERANGE) {
            *value = v;
            return true;
        }

        p++;
    }

    return false;
}

static bool copy_mpv_response_line(const char *responses, int request_id,
                                   char *line_out, size_t line_size) {
    char marker[48];
    const char *line = responses;

    if (!responses || !line_out || line_size < 2)
        return false;
    snprintf(marker, sizeof(marker), "\"request_id\":%d", request_id);
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t length;
        const char *match;

        if (!end) end = line + strlen(line);
        match = strstr(line, marker);
        if (match && match < end) {
            length = (size_t)(end - line);
            if (length >= line_size) length = line_size - 1;
            memcpy(line_out, line, length);
            line_out[length] = '\0';
            return true;
        }
        if (!*end) break;
        line = end + 1;
    }
    return false;
}

static const MpvPeriodicSnapshot *refresh_periodic_mpv_snapshot(time_t now) {
    static const char commands[] =
        "{\"command\":[\"get_property\",\"metadata/icy-title\"],\"request_id\":201}\n"
        "{\"command\":[\"get_property\",\"media-title\"],\"request_id\":202}\n"
        "{\"command\":[\"get_property\",\"playback-time\"],\"request_id\":203}\n"
        "{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":204}\n"
        "{\"command\":[\"get_property\",\"paused-for-cache\"],\"request_id\":205}\n";
    char responses[16384];
    char line[8192];
    int fd;

    if (periodic_mpv_snapshot.sampled_at == now)
        return &periodic_mpv_snapshot;
    memset(&periodic_mpv_snapshot, 0, sizeof(periodic_mpv_snapshot));
    periodic_mpv_snapshot.sampled_at = now;

    fd = connect_mpv_socket(MPV_IPC_CONNECT_TIMEOUT_MS);
    if (fd < 0)
        return &periodic_mpv_snapshot;
    if (!write_all(fd, commands, sizeof(commands) - 1)) {
        close(fd);
        return &periodic_mpv_snapshot;
    }
    (void)mpv_read_responses(fd, responses, sizeof(responses), 5);
    close(fd);

    char *title = NULL;
    if (copy_mpv_response_line(responses, 201, line, sizeof(line)))
        title = json_data_string(line);
    if ((!title || !title[0]) &&
        copy_mpv_response_line(responses, 202, line, sizeof(line))) {
        free(title);
        title = json_data_string(line);
    }
    if (title) {
        snprintf(periodic_mpv_snapshot.title,
                 sizeof(periodic_mpv_snapshot.title), "%s", title);
        free(title);
    }

    if (copy_mpv_response_line(responses, 203, line, sizeof(line)))
        periodic_mpv_snapshot.have_position =
            json_data_double(line, &periodic_mpv_snapshot.position);
    if (!periodic_mpv_snapshot.have_position &&
        copy_mpv_response_line(responses, 204, line, sizeof(line)))
        periodic_mpv_snapshot.have_position =
            json_data_double(line, &periodic_mpv_snapshot.position);
    if (copy_mpv_response_line(responses, 205, line, sizeof(line)))
        periodic_mpv_snapshot.have_paused_for_cache =
            json_data_bool(line, &periodic_mpv_snapshot.paused_for_cache);

    return &periodic_mpv_snapshot;
}

static bool mpv_get_bool_property(const char *property, bool *value) {
    int fd = connect_mpv_socket(MPV_IPC_CONNECT_TIMEOUT_MS);
    if (fd < 0)
        return false;

    char *cmd = xasprintf("{\"command\":[\"get_property\",\"%s\"],\"request_id\":100}\n", property);
    bool sent = write_all(fd, cmd, strlen(cmd));
    free(cmd);

    if (!sent) {
        close(fd);
        return false;
    }

    char buf[1024];
    size_t used = mpv_read_response(fd, buf, sizeof(buf));

    close(fd);

    if (!used)
        return false;

    return json_data_bool(buf, value);
}

static bool playback_is_ready(void) {
    bool ready;

    pthread_mutex_lock(&playback.mutex);
    ready = playback.player_ready && playback.player_pid > 0 &&
            !playback.shutting_down;
    pthread_mutex_unlock(&playback.mutex);

    return ready;
}

static bool update_now_playing_from_mpv(void) {
    static time_t last_poll = 0;
    bool changed = false;

    if (!station_playing || !playback_is_ready())
        return false;

    time_t now = time(NULL);
    if (now == last_poll)
        return false;

    last_poll = now;

    const MpvPeriodicSnapshot *snapshot =
        refresh_periodic_mpv_snapshot(now);
    if (snapshot->title[0]) {
        if (!now_playing_title ||
            strcmp(now_playing_title, snapshot->title) != 0) {
            set_now_playing(snapshot->title);
            changed = true;
        }
    }
    return changed;
}

static bool set_volume(int volume) {
    char *cmd = xasprintf("{\"command\":[\"set_property\",\"volume\",%d]}", volume);
    bool ok = mpv_command_raw(cmd);
    free(cmd);
    return ok;
}

static long long monotonic_millis(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return (long long)time(NULL) * 1000LL;

    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void playback_job_free(PlaybackJob *job) {
    if (!job)
        return;
    free(job->url);
    free(job->title);
    free(job->source);
    memset(job, 0, sizeof(*job));
}

static void playback_clear_queued_locked(void) {
    free(playback.queued_url);
    free(playback.queued_title);
    free(playback.queued_source);
    playback.queued_url = NULL;
    playback.queued_title = NULL;
    playback.queued_source = NULL;
    playback.request_pending = false;
}

static bool playback_generation_is_current(unsigned long generation) {
    bool current;

    pthread_mutex_lock(&playback.mutex);
    current = !playback.shutting_down &&
              generation == playback.request_generation;
    pthread_mutex_unlock(&playback.mutex);

    return current;
}

static bool playback_wait_for_worker_tick(unsigned long generation, int delay_ms) {
    struct timespec deadline;
    bool current;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += delay_ms / 1000;
    deadline.tv_nsec += (long)(delay_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&playback.mutex);
    if (!playback.shutting_down &&
        generation == playback.request_generation)
        pthread_cond_timedwait(&playback.cond, &playback.mutex, &deadline);
    current = !playback.shutting_down &&
              generation == playback.request_generation;
    pthread_mutex_unlock(&playback.mutex);

    return current;
}

static void playback_clear_player(pid_t pid) {
    pthread_mutex_lock(&playback.mutex);
    if (playback.player_pid == pid) {
        playback.player_pid = -1;
        playback.player_ready = false;
        playback.active_generation = 0;
    }
    pthread_mutex_unlock(&playback.mutex);
}

static void playback_stop_process(pid_t pid) {
    if (pid <= 0)
        return;

    kill(pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid || (r < 0 && errno == ECHILD))
            return;
        if (r < 0 && errno != EINTR)
            break;

        struct timespec delay = {0, 50000000L};
        nanosleep(&delay, NULL);
    }

    kill(pid, SIGKILL);
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, 0);

        if (r == pid || (r < 0 && errno == ECHILD))
            return;
        if (r < 0 && errno == EINTR)
            continue;
        return;
    }
}

static int playback_spawn_mpv(const char *url, int volume, pid_t *pid_out) {
    posix_spawn_file_actions_t actions;
    char ipc_arg[sizeof(mpv_socket_path) + 32];
    char volume_arg[32];
    int rc;

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0)
        return rc;

    rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                          "/dev/null", O_RDONLY, 0);
    if (rc == 0)
        rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                              "/dev/null", O_WRONLY, 0);
    if (rc == 0)
        rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                              "/dev/null", O_WRONLY, 0);
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return rc;
    }

    snprintf(ipc_arg, sizeof(ipc_arg), "--input-ipc-server=%s",
             mpv_socket_path);
    snprintf(volume_arg, sizeof(volume_arg), "--volume=%d", volume);

    char *const argv[] = {
        "mpv",
        "--no-video",
        "--no-audio-display",
        "--force-window=no",
        "--no-terminal",
        "--quiet",
        "--network-timeout=15",
        "--cache=yes",
        volume_arg,
        ipc_arg,
        (char *)url,
        NULL,
    };

    rc = posix_spawnp(pid_out, "mpv", &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return rc;
}

typedef enum {
    STARTUP_READY,
    STARTUP_CANCELLED,
    STARTUP_EXITED,
    STARTUP_TIMED_OUT
} StartupResult;

static int playback_requested_volume(void) {
    int volume;

    pthread_mutex_lock(&playback.mutex);
    volume = playback.requested_volume;
    pthread_mutex_unlock(&playback.mutex);

    return volume;
}

static StartupResult playback_wait_until_ready(pid_t pid,
                                                unsigned long generation) {
    long long deadline = monotonic_millis() +
                         MPV_STARTUP_TIMEOUT_SECONDS * 1000LL;

    while (monotonic_millis() < deadline) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid || (r < 0 && errno == ECHILD))
            return STARTUP_EXITED;
        if (!playback_generation_is_current(generation))
            return STARTUP_CANCELLED;

        if (mpv_socket_path[0] && path_exists(mpv_socket_path)) {
            bool idle = false;
            if (mpv_get_bool_property("idle-active", &idle)) {
                (void)idle;
                set_volume(playback_requested_volume());
                return STARTUP_READY;
            }
        }

        if (!playback_wait_for_worker_tick(generation, 50))
            return STARTUP_CANCELLED;
    }

    return playback_generation_is_current(generation)
               ? STARTUP_TIMED_OUT
               : STARTUP_CANCELLED;
}

static bool playback_publish_event(PlaybackEventKind kind,
                                   unsigned long generation,
                                   bool reset_attempts,
                                   const char *message) {
    bool published = false;

    pthread_mutex_lock(&playback.mutex);
    if (!playback.shutting_down &&
        generation == playback.request_generation) {
        playback.event_kind = kind;
        playback.event_generation = generation;
        playback.event_reset_attempts = reset_attempts;
        snprintf(playback.event_message, sizeof(playback.event_message),
                 "%s", message ? message : "");
        if (kind == PLAYBACK_EVENT_READY)
            playback.player_ready = true;
        published = true;
    }
    pthread_mutex_unlock(&playback.mutex);

    return published;
}

static void playback_publish_failure(unsigned long generation,
                                     bool reset_attempts,
                                     const char *message) {
    playback_publish_event(PLAYBACK_EVENT_FAILED, generation,
                           reset_attempts, message);
}

static bool playback_take_queued_job(PlaybackJob *job) {
    bool have_job = false;

    pthread_mutex_lock(&playback.mutex);
    if (playback.request_pending) {
        job->url = playback.queued_url;
        job->title = playback.queued_title;
        job->source = playback.queued_source;
        job->reset_attempts = playback.queued_reset_attempts;
        job->generation = playback.request_generation;
        playback.queued_url = NULL;
        playback.queued_title = NULL;
        playback.queued_source = NULL;
        playback.request_pending = false;
        have_job = true;
    }
    pthread_mutex_unlock(&playback.mutex);

    return have_job;
}

static bool playback_worker_should_shutdown(void) {
    bool shutting_down;

    pthread_mutex_lock(&playback.mutex);
    shutting_down = playback.shutting_down;
    pthread_mutex_unlock(&playback.mutex);

    return shutting_down;
}

static void playback_wait_for_work(pid_t player_pid) {
    pthread_mutex_lock(&playback.mutex);
    if (!playback.shutting_down && !playback.request_pending) {
        if (player_pid > 0) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 100000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&playback.cond, &playback.mutex,
                                   &deadline);
        } else {
            pthread_cond_wait(&playback.cond, &playback.mutex);
        }
    }
    pthread_mutex_unlock(&playback.mutex);
}

static void *playback_worker_main(void *unused) {
    pid_t player_pid = -1;
    unsigned long player_generation = 0;

    (void)unused;

    for (;;) {
        PlaybackJob job = {0};

        playback_wait_for_work(player_pid);
        if (playback_worker_should_shutdown())
            break;

        if (!playback_take_queued_job(&job)) {
            if (player_pid > 0) {
                int status;
                pid_t r = waitpid(player_pid, &status, WNOHANG);

                if (r == player_pid || (r < 0 && errno == ECHILD)) {
                    pid_t exited_pid = player_pid;
                    unsigned long exited_generation = player_generation;
                    player_pid = -1;
                    player_generation = 0;
                    playback_clear_player(exited_pid);
                    playback_publish_event(PLAYBACK_EVENT_EXITED,
                                           exited_generation, true,
                                           "Stream ended");
                }
            }
            continue;
        }

        if (player_pid > 0) {
            pid_t old_pid = player_pid;
            player_pid = -1;
            player_generation = 0;
            playback_stop_process(old_pid);
            playback_clear_player(old_pid);
            unlink(mpv_socket_path);
        }

        if (!playback_generation_is_current(job.generation)) {
            playback_job_free(&job);
            continue;
        }

        const char *source = job.source && job.source[0] ? job.source : ".";
        char *target = resolve_playlist_target(source, job.url);
        if (!target || !target[0]) {
            free(target);
            playback_publish_failure(job.generation, job.reset_attempts,
                                     "Unable to resolve station");
            playback_job_free(&job);
            continue;
        }

        if (!playback_generation_is_current(job.generation)) {
            free(target);
            playback_job_free(&job);
            continue;
        }

        unlink(mpv_socket_path);
        int rc = playback_spawn_mpv(target, playback_requested_volume(),
                                    &player_pid);
        free(target);
        if (rc != 0) {
            char message[160];
            snprintf(message, sizeof(message), "Unable to start mpv: %s",
                     strerror(rc));
            player_pid = -1;
            playback_publish_failure(job.generation, job.reset_attempts,
                                     message);
            playback_job_free(&job);
            continue;
        }

        player_generation = job.generation;
        pthread_mutex_lock(&playback.mutex);
        bool current = !playback.shutting_down &&
                       job.generation == playback.request_generation;
        if (current) {
            playback.player_pid = player_pid;
            playback.player_ready = false;
            playback.active_generation = job.generation;
        }
        pthread_mutex_unlock(&playback.mutex);

        if (!current) {
            playback_stop_process(player_pid);
            player_pid = -1;
            player_generation = 0;
            playback_job_free(&job);
            continue;
        }

        StartupResult result = playback_wait_until_ready(player_pid,
                                                          job.generation);
        if (result == STARTUP_READY) {
            if (!playback_publish_event(PLAYBACK_EVENT_READY,
                                        job.generation,
                                        job.reset_attempts,
                                        "Playing")) {
                pid_t stale_pid = player_pid;
                player_pid = -1;
                player_generation = 0;
                playback_stop_process(stale_pid);
                playback_clear_player(stale_pid);
                unlink(mpv_socket_path);
            }
        } else {
            pid_t failed_pid = player_pid;
            player_pid = -1;
            player_generation = 0;
            if (result != STARTUP_EXITED)
                playback_stop_process(failed_pid);
            playback_clear_player(failed_pid);
            unlink(mpv_socket_path);

            if (result == STARTUP_TIMED_OUT)
                playback_publish_failure(job.generation,
                                         job.reset_attempts,
                                         "mpv startup timed out");
            else if (result == STARTUP_EXITED)
                playback_publish_failure(job.generation,
                                         job.reset_attempts,
                                         "mpv exited during startup");
        }

        playback_job_free(&job);
    }

    if (player_pid > 0) {
        pid_t old_pid = player_pid;
        playback_stop_process(old_pid);
        playback_clear_player(old_pid);
    }
    unlink(mpv_socket_path);

    return NULL;
}

static bool playback_worker_start(void) {
    int rc;

    pthread_mutex_lock(&playback.mutex);
    playback.requested_volume = current_volume;
    playback.shutting_down = false;
    pthread_mutex_unlock(&playback.mutex);

    rc = pthread_create(&playback.thread, NULL, playback_worker_main, NULL);
    if (rc != 0)
        return false;

    playback.thread_started = true;
    return true;
}

static void playback_worker_shutdown(void) {
    pthread_mutex_lock(&playback.mutex);
    if (!playback.thread_started) {
        pthread_mutex_unlock(&playback.mutex);
        return;
    }

    playback.shutting_down = true;
    playback.request_generation++;
    playback.player_ready = false;
    playback.event_kind = PLAYBACK_EVENT_NONE;
    playback_clear_queued_locked();
    pthread_cond_broadcast(&playback.cond);
    pthread_mutex_unlock(&playback.mutex);

    pthread_join(playback.thread, NULL);

    pthread_mutex_lock(&playback.mutex);
    playback.thread_started = false;
    playback.player_pid = -1;
    playback.player_ready = false;
    playback.active_generation = 0;
    pthread_mutex_unlock(&playback.mutex);
}

static bool playback_queue_request(const char *url, const char *title,
                                   const char *source,
                                   bool reset_attempts) {
    char *url_copy = xstrdup(url ? url : "");
    char *title_copy = xstrdup(title ? title : "");
    char *source_copy = xstrdup(source ? source : "");
    bool queued = false;

    pthread_mutex_lock(&playback.mutex);
    if (!playback.shutting_down && playback.thread_started && url_copy[0]) {
        playback_clear_queued_locked();
        playback.queued_url = url_copy;
        playback.queued_title = title_copy;
        playback.queued_source = source_copy;
        playback.queued_reset_attempts = reset_attempts;
        playback.request_generation++;
        if (playback.request_generation == 0)
            playback.request_generation++;
        playback.request_pending = true;
        playback.player_ready = false;
        playback.event_kind = PLAYBACK_EVENT_NONE;
        pthread_cond_broadcast(&playback.cond);
        queued = true;
    }
    pthread_mutex_unlock(&playback.mutex);

    if (!queued) {
        free(url_copy);
        free(title_copy);
        free(source_copy);
    }

    return queued;
}

static void playback_set_requested_volume(int volume) {
    pthread_mutex_lock(&playback.mutex);
    playback.requested_volume = volume;
    pthread_cond_broadcast(&playback.cond);
    pthread_mutex_unlock(&playback.mutex);
}

static char *toggle_pause(void) {
    if (!playback_is_ready())
        return xstrdup(station_playing ? "Player control unavailable" : "Player starting");
    bool actual_paused = false;

    if (!mpv_get_bool_property("pause", &actual_paused))
        return xstrdup("Player control unavailable");

    paused = !actual_paused;
    char *cmd = xasprintf("{\"command\":[\"set_property\",\"pause\",%s]}",
                          paused ? "true" : "false");
    bool ok = mpv_command_raw(cmd);
    free(cmd);

    if (!ok) {
        paused = actual_paused;
        return xstrdup("Player control unavailable");
    }

    return xstrdup(paused ? "Paused" : "Playing");
}

static char *play_playlist_item(long index) {
    if (playlist.len == 0) return xstrdup("Nothing to play");
    if (index < 0 || (size_t)index >= playlist.len) return xstrdup("Nothing to play");
    play_index = index;
    Entry *e = &playlist.items[play_index];
    if (e->kind == ENTRY_STATION && e->station) {
        set_now_playing(e->station->title);
        reset_stream_watchdog();
        if (!playback_queue_request(e->station->url, e->station->title,
                                    e->station->source, true)) {
            play_index = -1;
            set_now_playing(NULL);
            return xstrdup("Unable to queue player");
        }
        return xstrdup("Starting...");
    }
    return xstrdup("Nothing to play");
}

static Entry *current_station_entry(void) {
    if (!playlist.len || play_index < 0 || (size_t)play_index >= playlist.len)
        return NULL;

    Entry *e = &playlist.items[play_index];
    if (e->kind != ENTRY_STATION || !e->station || !e->station->url || !e->station->url[0])
        return NULL;

    return e;
}

static int reconnect_backoff_delay(int attempt) {
    if (attempt <= 1)
        return 0;

    int delay = 2;
    for (int i = 2; i < attempt && delay < RECONNECT_BACKOFF_MAX_SECONDS; i++)
        delay *= 2;

    if (delay > RECONNECT_BACKOFF_MAX_SECONDS)
        delay = RECONNECT_BACKOFF_MAX_SECONDS;

    return delay;
}

static bool playback_position_changed(double position) {
    if (last_playback_position < 0.0)
        return true;

    return position > last_playback_position + 0.25 ||
           position + 0.25 < last_playback_position;
}

static char *restart_current_station_from_watchdog(void) {
    Entry *e = current_station_entry();
    if (!e)
        return NULL;

    set_now_playing(e->station->title);
    station_playing = false;
    paused = false;
    if (!playback_queue_request(e->station->url, e->station->title,
                                e->station->source, false))
        return xasprintf("buffering... reconnecting attempt %d failed", reconnect_attempt);

    return xasprintf("buffering... reconnecting attempt %d", reconnect_attempt);
}

static char *check_stream_watchdog(void) {
    if (!station_playing || !playback_is_ready() || !current_station_entry())
        return NULL;

    time_t now = time(NULL);
    if (paused) {
        last_playback_progress = now;
        last_watchdog_poll = now;
        reconnect_pending = false;
        reconnect_at = 0;
        return NULL;
    }

    if (last_playback_progress == 0)
        last_playback_progress = now;

    if (now - last_watchdog_poll < WATCHDOG_POLL_SECONDS)
        return NULL;

    last_watchdog_poll = now;

    const MpvPeriodicSnapshot *snapshot =
        refresh_periodic_mpv_snapshot(now);
    double position = snapshot->position;
    bool have_position = snapshot->have_position;

    if (have_position && playback_position_changed(position)) {
        bool recovered = reconnect_attempt > 0 || reconnect_pending;
        last_playback_position = position;
        last_playback_progress = now;
        reconnect_attempt = 0;
        reconnect_pending = false;
        reconnect_at = 0;
        return recovered ? xstrdup("Stream recovered") : NULL;
    }

    if (!have_position) {
        if (snapshot->have_paused_for_cache &&
            !snapshot->paused_for_cache) {
            bool recovered = reconnect_attempt > 0 || reconnect_pending;
            last_playback_progress = now;
            reconnect_attempt = 0;
            reconnect_pending = false;
            reconnect_at = 0;
            return recovered ? xstrdup("Stream recovered") : NULL;
        }
    }

    if (reconnect_pending) {
        if (now >= reconnect_at) {
            reconnect_pending = false;
            reconnect_at = 0;
            return restart_current_station_from_watchdog();
        }

        return xasprintf("buffering... reconnecting attempt %d", reconnect_attempt);
    }

    if (now - last_playback_progress >= STALL_TIMEOUT_SECONDS) {
        reconnect_attempt++;
        int delay = reconnect_backoff_delay(reconnect_attempt);

        if (delay > 0) {
            reconnect_pending = true;
            reconnect_at = now + delay;
            return xasprintf("buffering... reconnecting attempt %d", reconnect_attempt);
        }

        return restart_current_station_from_watchdog();
    }

    return NULL;
}

static bool playback_take_event(PlaybackEventKind *kind,
                                bool *reset_attempts,
                                char *message, size_t message_size) {
    bool have_event = false;

    pthread_mutex_lock(&playback.mutex);
    if (playback.event_kind != PLAYBACK_EVENT_NONE &&
        playback.event_generation == playback.request_generation) {
        *kind = playback.event_kind;
        *reset_attempts = playback.event_reset_attempts;
        snprintf(message, message_size, "%s", playback.event_message);
        have_event = true;
    }
    playback.event_kind = PLAYBACK_EVENT_NONE;
    pthread_mutex_unlock(&playback.mutex);

    return have_event;
}

static char *check_auto_advance(void) {
    PlaybackEventKind kind;
    bool reset_attempts = true;
    char message[160];

    if (!playback_take_event(&kind, &reset_attempts,
                             message, sizeof(message)))
        return NULL;

    if (kind == PLAYBACK_EVENT_READY) {
        paused = false;
        note_player_started(reset_attempts);
        return xstrdup(message[0] ? message : "Playing");
    }

    reset_stream_watchdog();

    if (kind == PLAYBACK_EVENT_FAILED) {
        play_index = -1;
        set_now_playing(NULL);
        return xstrdup(message[0] ? message : "Unable to start player");
    }

    if (!continuous || play_index < 0) {
        set_now_playing(NULL);
        return xstrdup(message[0] ? message : "Stream ended");
    }
    long next = play_index + 1;
    if ((size_t)next >= playlist.len) {
        play_index = -1;
        set_now_playing(NULL);
        return xstrdup("End of station list");
    }
    return play_playlist_item(next);
}

static bool entry_is_playing(const Entry *entry) {
    if (playlist.len && play_index >= 0 && (size_t)play_index < playlist.len) {
        Entry *p = &playlist.items[play_index];
        if (entry->kind != p->kind) return false;
        if (!entry->station || !p->station) return false;
        return strcmp(entry->station->url, p->station->url) == 0 && strcmp(entry->station->title, p->station->title) == 0;
    }
    return false;
}

static char *shorten_text(const char *text, int width) {
    if (width <= 1) return xstrdup("");
    int len = (int)strlen(text);
    if (len <= width) return xstrdup(text);
    const char *tail = text + (len - (width - 1));
    return xasprintf("…%s", tail);
}

static void set_now_playing(const char *title) {
    free(now_playing_title);
    now_playing_title = title && *title ? xstrdup(title) : NULL;
}

__attribute__((unused)) static void draw_center_text(WINDOW *stdscr, int y, const char *text, int width, int attr) {
    (void)stdscr;
    if (y < 0 || width <= 8 || !text || !*text) return;

    int max_width = width / 2;
    if (max_width < 12) max_width = width - 2;
    if (max_width > width - 2) max_width = width - 2;

    char *s = shorten_text(text, max_width);
    int len = (int)strlen(s);
    int x = (width - len) / 2;
    if (x < 0) x = 0;
    if (x + len >= width) len = width - x - 1;

    attron(attr);
    mvaddnstr(y, x, s, len);
    attroff(attr);
    free(s);
}

static void draw_full_line(WINDOW *stdscr, int y, const char *text, int width, int attr) {
    if (y < 0 || width <= 1) return;
    char *s = shorten_text(text ? text : "", width - 1);
    int len = (int)strlen(s);
    attron(attr);
    mvaddnstr(y, 0, s, width - 1);
    for (int i = len; i < width - 1; i++) addch(' ');
    attroff(attr);
    free(s);
}

static void build_station_playlist_from_entries(EntryList *entries) {
    entrylist_clear(&playlist);
    play_index = -1;
    for (size_t i = 0; i < entries->len; i++) {
        if (entries->items[i].kind == ENTRY_STATION) {
            entrylist_push(&playlist, entry_clone(&entries->items[i]));
        }
    }
}

static long playlist_index_for_entry(const Entry *entry) {
    for (size_t i = 0; i < playlist.len; i++) {
        Entry *p = &playlist.items[i];
        if (p->station && entry->station && strcmp(p->station->url, entry->station->url) == 0 && strcmp(p->station->title, entry->station->title) == 0) {
            return (long)i;
        }
    }
    return -1;
}

static void browser(WINDOW *stdscr, StringList *roots, const char *start_path) {
    char *current = start_path && *start_path ? xstrdup(start_path) : NULL;
    int selected = 0;
    int offset = 0;
    char *status = xstrdup("Choose a folder");
    EntryList entries = {0};
    char *title = NULL;
    bool listing_dirty = true;
    bool redraw = true;
    ListingStamp cached_stamp = {0};
    time_t last_listing_check = 0;
    timeout(200);

    for (;;) {
        time_t now = time(NULL);
        char *auto_status = check_auto_advance();
        if (auto_status) {
            free(status);
            status = auto_status;
            redraw = true;
        }

        if (update_now_playing_from_mpv())
            redraw = true;
        char *watchdog_status = check_stream_watchdog();
        if (watchdog_status) {
            free(status);
            status = watchdog_status;
            redraw = true;
        }

        if (current && now != last_listing_check) {
            ListingStamp latest = listing_stamp(current);
            if (!listing_dirty && listing_stamp_changed(cached_stamp, latest))
                listing_dirty = true;
            last_listing_check = now;
        }

        if (listing_dirty) {
            entrylist_free(&entries);
            free(title);
            title = NULL;
            entries = make_entries(current, roots, &title);
            cached_stamp = listing_stamp(current);
            if (entries.len == 0)
                entrylist_push(&entries, make_entry(
                    ENTRY_EMPTY, current ? current : "", "(empty)", NULL));
            listing_dirty = false;
            redraw = true;
        }

        if (redraw) {
            erase();
            int height, width;
            getmaxyx(stdscr, height, width);

        int visible_rows = height - 2;
        if (visible_rows < 1) visible_rows = 1;

        if (selected < 0) selected = 0;
        if ((size_t)selected >= entries.len)
            selected = entries.len ? (int)entries.len - 1 : 0;

        if (offset < 0)
            offset = 0;
        if (selected < offset)
            offset = selected;
        if (selected >= offset + visible_rows)
            offset = selected - visible_rows + 1;

        int max_offset = (int)entries.len - visible_rows;
        if (max_offset < 0)
            max_offset = 0;
        if (offset > max_offset)
            offset = max_offset;

        const char *mode = continuous ? "Auto-next" : "Stay";
        char *header = NULL;

        const char *playing_name = NULL;

        if (now_playing_title && now_playing_title[0]) {
            playing_name = now_playing_title;
        } else if (playlist.len && play_index >= 0 && (size_t)play_index < playlist.len &&
                   playlist.items[play_index].station &&
                   playlist.items[play_index].station->title &&
                   playlist.items[play_index].station->title[0]) {
            playing_name = playlist.items[play_index].station->title;
        }

        if (playing_name)
            header = xasprintf("%s | Vol: %d%% | %s | Now: %s", title, current_volume, mode, playing_name);
        else
            header = xasprintf("%s | Vol: %d%% | %s", title, current_volume, mode);

        draw_full_line(stdscr, 0, header, width, NORMAL_ATTR);
        free(header);

        int end = offset + visible_rows;
        if ((size_t)end > entries.len) end = (int)entries.len;
        for (int idx = offset, row = 1; idx < end; idx++, row++) {
            Entry *e = &entries.items[idx];
            char *label = NULL;
            if (e->kind == ENTRY_UP) label = xstrdup("../");
            else if (e->kind == ENTRY_DIR) label = xasprintf("[%s]/", base_name(e->path));
            else if (e->kind == ENTRY_PLAYLIST) label = xasprintf("[RADIO] %s", base_name(e->path));
            else if (e->kind == ENTRY_STATION) label = xstrdup(e->station ? e->station->title : e->label);
            else label = xstrdup(e->label ? e->label : "(empty)");

            bool is_selected = idx == selected;
            bool is_playing = entry_is_playing(e);

            if (is_selected) {
                /*
                   Keep the selector visible, but never let selection color
                   override the playing color. If a playing station is selected,
                   the whole visible line stays yellow.
                */
                int attr = is_playing ? PLAYING_ATTR : NORMAL_ATTR;

                char *line = xasprintf("> %s", label);
                draw_full_line(stdscr, row, line, width, attr);
                free(line);
            } else {
                int attr = is_playing ? PLAYING_ATTR : NORMAL_ATTR;
                char *line = xasprintf("  %s", label);
                draw_full_line(stdscr, row, line, width, attr);
                free(line);
            }

            free(label);
        }

        char *footer = xasprintf("Enter=open/play  Space=pause  c=mode  PgUp/PgDn=volume  Backspace=up  q=quit | %s",
                                 status);
        draw_full_line(stdscr, height - 1, footer, width, NORMAL_ATTR);
        free(footer);
            refresh();
            redraw = false;
        }

        int key = getch();
        if (key == ERR) {
            continue;
        }

        redraw = true;

        if (key == 'q' || key == 27) {
            break;
        } else if (key == ' ') {
            free(status);
            status = toggle_pause();
        } else if (key == 'c') {
            continuous = !continuous;
            free(status);
            status = xstrdup(continuous ? "Mode: Auto-next" : "Mode: Stay");
        } else if (key == KEY_UP || key == 'k') {
            if (selected > 0) selected--;
        } else if (key == KEY_DOWN || key == 'j') {
            if ((size_t)selected + 1 < entries.len) selected++;
        } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (current) {
                char *parent = parent_path_dup(current);
                free(current);
                current = parent;
                selected = 0;
                offset = 0;
                listing_dirty = true;
            }
        } else if (key == KEY_PPAGE) {
            current_volume += 5;
            if (current_volume > MAX_VOLUME) current_volume = MAX_VOLUME;
            playback_set_requested_volume(current_volume);
            if (playback_is_ready())
                set_volume(current_volume);
            free(status);
            status = xasprintf("Volume: %d%%", current_volume);
        } else if (key == KEY_NPAGE) {
            current_volume -= 5;
            if (current_volume < 0) current_volume = 0;
            playback_set_requested_volume(current_volume);
            if (playback_is_ready())
                set_volume(current_volume);
            free(status);
            status = xasprintf("Volume: %d%%", current_volume);
        } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            Entry *e = &entries.items[selected];
            if (e->kind == ENTRY_DIR || e->kind == ENTRY_UP || e->kind == ENTRY_PLAYLIST) {
                free(current);
                current = xstrdup(e->path);
                selected = 0;
                offset = 0;
                listing_dirty = true;
                free(status);
                status = xstrdup(current);
            } else if (e->kind == ENTRY_STATION) {
                build_station_playlist_from_entries(&entries);
                long idx = playlist_index_for_entry(e);
                free(status);
                status = play_playlist_item(idx >= 0 ? idx : 0);
            }
        }
    }

    entrylist_free(&entries);
    free(title);
    free(current);
    free(status);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    init_mpv_socket_path();
    if (!playback_worker_start()) {
        fprintf(stderr, "simpleradio: unable to start playback worker\n");
        return 1;
    }
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(SUI_ESCAPE_DELAY_MS);
    notimeout(stdscr, FALSE);
    curs_set(0);
    start_color();
    use_default_colors();

    int bright_green = (COLORS >= 256) ? 46 : COLOR_GREEN;
    int darker_green = (COLORS >= 256) ? 40 : COLOR_GREEN;
    (void)darker_green;
    int yellow = (COLORS >= 256) ? 226 : COLOR_YELLOW;

    /* Keep SimpleRadio's foreground palette without painting the terminal. */
    init_pair(1, bright_green, -1);
    init_pair(2, bright_green, -1);
    init_pair(3, yellow, -1);
    init_pair(4, yellow, -1);

    NORMAL_ATTR = COLOR_PAIR(1);
    SELECTED_ATTR = COLOR_PAIR(2) | A_BOLD;
    PLAYING_ATTR = COLOR_PAIR(3) | A_BOLD;
    PLAYING_SELECTED_ATTR = COLOR_PAIR(4) | A_BOLD;
    bkgd(' ' | NORMAL_ATTR);

    StringList roots = choose_start_roots();
    if (!roots.len) {
        mvaddstr(0, 0, "No browse roots found.");
        getch();
    } else {
        browser(stdscr, &roots, argc > 1 ? argv[1] : NULL);
    }

    endwin();
    playback_worker_shutdown();
    stringlist_free(&roots);
    entrylist_free(&playlist);
    set_now_playing(NULL);
    return 0;
}
