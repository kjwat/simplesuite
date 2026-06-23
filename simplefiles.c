// simplefiles.c
// build: gcc simplefiles.c -o simplefiles -lncursesw

#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define PANE_GAP 4

#include <locale.h>
#include <wchar.h>
#include <ncurses.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/file.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <sys/statvfs.h>
#include <sys/mman.h>



#define SF_COL_GAP 3
#define SF_PANE_GUTTER 2
#define MAX_ENTRIES 4096
#define MAX_SELECTED 4096
#define MAX_CLIPBOARD 4096
#define MAX_DIR_MEMORY 4096
#define DETAIL_REDRAW_DELAY_MS 150

typedef struct {
    char name[NAME_MAX + 1];
    int is_dir;
} Entry;

static Entry entries[MAX_ENTRIES];
static int entry_count = 0;
static int cursor = 0;
static int top = 0;

static char cwd_path[PATH_MAX];

static char selected[MAX_SELECTED][PATH_MAX];
static int selected_count = 0;

static char clipboard_paths[MAX_CLIPBOARD][PATH_MAX];
static int clipboard_count = 0;
static int clipboard_mode = 0;
static unsigned long clipboard_generation = 0;

typedef struct {
    int ok;
    int fail;
    int renamed;
    char last_pasted_name[NAME_MAX + 1];
} PasteResult;

typedef struct {
    volatile uint64_t total_bytes;
    volatile uint64_t done_bytes;
    volatile int active;
} PasteProgress;

static PasteProgress *paste_progress = NULL;

static pid_t paste_worker_pid = -1;
static int paste_result_fd = -1;
static int paste_worker_mode = 0;
static unsigned long paste_clipboard_generation = 0;
static char paste_destination[PATH_MAX] = "";

static int pending_key = 0;
static int pending_delete = 0;
static int pending_empty_trash = 0;
static int show_hidden = 0;

static int command_mode = 0;
static char command[1024] = "";
static int command_len = 0;

static int search_mode = 0;
static char search_query[1024] = "";
static int search_len = 0;

static int running = 1;
static volatile sig_atomic_t stop_requested = 0;
static const char *exit_reason = "normal";
static FILE *debug_file = NULL;
static int instance_lock_fd = -1;
static char instance_lock_path[PATH_MAX] = "";

static char message[256] = "";

static int config_terminal_mode = 0;
static char config_editor_command[512] = "";

static char config_text_extensions[4096] =
    ".txt,.md,.markdown,.tex,.c,.h,.cpp,.hpp,.cc,.py,.sh,.conf,.ini,.log";

typedef struct {
    char ext[64];
    char cmd[512];
} OpenRule;

static OpenRule open_rules[256];
static int open_rule_count = 0;

static int config_preview = 1;
static int config_preview_lines = 80;
static char config_start_dir[PATH_MAX] = "";
static char config_trash_dir[PATH_MAX] = "";
static int config_confirm_delete = 1;

static WINDOW *parent_win = NULL;
static WINDOW *current_win = NULL;
static WINDOW *preview_win = NULL;
static WINDOW *top_win = NULL;
static WINDOW *status_win = NULL;

static int last_lines = 0;
static int last_cols = 0;

static int single_pane_mode = 0;

typedef struct {
    char dir[PATH_MAX];
    char child[NAME_MAX + 1];
} DirMemory;

static DirMemory dir_memory[MAX_DIR_MEMORY];
static int dir_memory_count = 0;

static int remove_recursive(const char *path);
static void load_dir(const char *path);
static void draw_ui(void);
static void destroy_windows(void);
static void clear_selected(void);
static void arm_delete(void);
static void confirm_delete(void);
static void expand_path(char *out, const char *in);
static void trim_config_value(char *s);

static void request_stop(int signo) {
    (void)signo;
    stop_requested = 1;
}

static int install_signal_handlers(void) {
    struct sigaction sa;
    int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = request_stop;
    sigemptyset(&sa.sa_mask);

    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &sa, NULL) != 0)
            return 0;
    }
    return 1;
}

static int safe_copy(char *dst, size_t dstsz, const char *src) {
    size_t n;

    if (!dst || dstsz == 0)
        return 0;
    if (!src)
        src = "";

    n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return src[n] == '\0';
}

static int safe_join3(char *dst, size_t dstsz, const char *a, const char *sep, const char *b) {
    size_t alen, slen, blen, total;

    if (!dst || dstsz == 0)
        return 0;
    if (!a) a = "";
    if (!sep) sep = "";
    if (!b) b = "";

    alen = strlen(a);
    slen = strlen(sep);
    blen = strlen(b);
    total = alen + slen + blen;
    if (total >= dstsz) {
        dst[0] = '\0';
        return 0;
    }

    memcpy(dst, a, alen);
    memcpy(dst + alen, sep, slen);
    memcpy(dst + alen + slen, b, blen);
    dst[total] = '\0';
    return 1;
}

static int runtime_dir(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    char cache[PATH_MAX];

    if (!home || !home[0])
        return 0;
    if (!safe_join3(cache, sizeof(cache), home, "/", ".cache"))
        return 0;
    if (mkdir(cache, 0700) != 0 && errno != EEXIST)
        return 0;
    if (!safe_join3(out, outsz, cache, "/", "simplefiles"))
        return 0;
    if (mkdir(out, 0700) != 0 && errno != EEXIST)
        return 0;
    return 1;
}

static void debug_log(const char *fmt, ...) {
    va_list ap;

    if (!debug_file)
        return;
    va_start(ap, fmt);
    vfprintf(debug_file, fmt, ap);
    va_end(ap);
    fputc('\n', debug_file);
    fflush(debug_file);
}

static void start_debug_log(const char *argv0) {
    char dir[PATH_MAX];
    char path[PATH_MAX];
    char exe[PATH_MAX] = "unknown";
    char here[PATH_MAX] = "unknown";
    const char *debug = getenv("SIMPLEFILES_DEBUG");
    const char *tty = ttyname(STDIN_FILENO);
    ssize_t n;

    if (!debug || strcmp(debug, "1") != 0 || !runtime_dir(dir, sizeof(dir)))
        return;
    if (!safe_join3(path, sizeof(path), dir, "/", "debug.log"))
        return;
    debug_file = fopen(path, "a");
    if (!debug_file)
        return;
    fcntl(fileno(debug_file), F_SETFD, FD_CLOEXEC);

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n >= 0)
        exe[n] = '\0';
    if (!getcwd(here, sizeof(here)))
        snprintf(here, sizeof(here), "unknown");
    debug_log("start pid=%ld ppid=%ld tty=%s cwd=%s argv0=%s exe=%s",
              (long)getpid(), (long)getppid(), tty ? tty : "none", here,
              argv0 ? argv0 : "", exe);
}

static int acquire_instance_lock(void) {
    char dir[PATH_MAX];
    struct stat st;

    if (!runtime_dir(dir, sizeof(dir)) || fstat(STDIN_FILENO, &st) != 0)
        return 0;
    if (snprintf(instance_lock_path, sizeof(instance_lock_path),
                 "%s/tty-%ju-%ju.lock", dir,
                 (uintmax_t)st.st_dev, (uintmax_t)st.st_ino) >= (int)sizeof(instance_lock_path))
        return 0;
    instance_lock_fd = open(instance_lock_path, O_RDWR | O_CREAT, 0600);
    if (instance_lock_fd < 0)
        return 0;
    fcntl(instance_lock_fd, F_SETFD, FD_CLOEXEC);
    if (flock(instance_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(instance_lock_fd);
        instance_lock_fd = -1;
        return -1;
    }
    return 1;
}

static void release_instance_lock(void) {
    if (instance_lock_fd < 0)
        return;
    unlink(instance_lock_path);
    flock(instance_lock_fd, LOCK_UN);
    close(instance_lock_fd);
    instance_lock_fd = -1;
}

static int terminal_is_available(void) {
    pid_t foreground;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return 0;
    errno = 0;
    foreground = tcgetpgrp(STDIN_FILENO);
    if (foreground < 0 &&
        (errno == ENOTTY || errno == EIO || errno == EBADF))
        return 0;
    return 1;
}


static void hard_redraw_after_shell(void) {
    reset_prog_mode();

    destroy_windows();
    last_lines = 0;
    last_cols = 0;

    clear();
    erase();
    refresh();
    clearok(stdscr, TRUE);

    load_dir(cwd_path);
    draw_ui();
}

static int wait_for_child(pid_t pid, int *status, const char *context) {
    pid_t result;

    debug_log("%s parent entering waitpid child=%ld", context, (long)pid);
    for (;;) {
        if (stop_requested) {
            exit_reason = "signal";
            running = 0;
            debug_log("%s waitpid interrupted by stop request", context);
            return 0;
        }

        result = waitpid(pid, status, 0);
        if (result == pid) {
            debug_log("%s waitpid returned child=%ld status=%d",
                      context, (long)pid, status ? *status : 0);
            return 1;
        }
        if (result < 0 && errno == EINTR)
            continue;

        debug_log("%s waitpid failed child=%ld errno=%d",
                  context, (long)pid, errno);
        return -1;
    }
}

static int redraw_after_child(const char *context) {
    int tty_valid = terminal_is_available();

    debug_log("%s tty valid before redraw=%d stop_requested=%d",
              context, tty_valid, stop_requested ? 1 : 0);
    if (stop_requested) {
        exit_reason = "signal";
        running = 0;
        return 0;
    }
    if (!tty_valid) {
        exit_reason = "lost tty";
        running = 0;
        return 0;
    }

    debug_log("%s before hard_redraw_after_shell", context);
    hard_redraw_after_shell();
    debug_log("%s after hard_redraw_after_shell", context);
    return 1;
}



static void set_cursor_to_name(const char *name) {
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            cursor = i;
            top = cursor - 5;
            if (top < 0) top = 0;
            return;
        }
    }
}

static void remember_dir_child(const char *dir, const char *child) {
    if (!dir || !child || dir[0] == '\0' || child[0] == '\0')
        return;

    for (int i = 0; i < dir_memory_count; i++) {
        if (strcmp(dir_memory[i].dir, dir) == 0) {
            safe_copy(dir_memory[i].child, sizeof(dir_memory[i].child), child);
            return;
        }
    }

    if (dir_memory_count < MAX_DIR_MEMORY) {
        safe_copy(dir_memory[dir_memory_count].dir, sizeof(dir_memory[dir_memory_count].dir), dir);

        safe_copy(dir_memory[dir_memory_count].child, sizeof(dir_memory[dir_memory_count].child), child);

        dir_memory_count++;
    }
}

static void remember_current_cursor(void) {
    if (entry_count <= 0)
        return;

    remember_dir_child(cwd_path, entries[cursor].name);
}

static int restore_dir_cursor(const char *dir) {
    for (int i = 0; i < dir_memory_count; i++) {
        if (strcmp(dir_memory[i].dir, dir) == 0) {
            set_cursor_to_name(dir_memory[i].child);
            return 1;
        }
    }

    return 0;
}

static int cmp_entries(const void *a, const void *b) {
    const Entry *ea = a;
    const Entry *eb = b;

    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir;

    return strcasecmp(ea->name, eb->name);
}

static void set_message(const char *msg) {
    strncpy(message, msg, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
}

static void join_path(char *out, const char *a, const char *b) {
    if (strcmp(a, "/") == 0) {
        if (!safe_join3(out, PATH_MAX, "/", "", b))
            out[0] = '\0';
    } else {
        if (!safe_join3(out, PATH_MAX, a, "/", b))
            out[0] = '\0';
    }
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int path_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static int path_is_regular(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Most local and removable filesystems provide d_type with readdir().  Use it
 * when available so listing a directory does not require a separate stat for
 * every entry.  Follow symlinks and fall back to fstatat() when the filesystem
 * reports an unknown type. */
static int dir_entry_is_dir(DIR *dir, const struct dirent *de) {
#ifdef DT_DIR
    if (de->d_type == DT_DIR)
        return 1;
    if (de->d_type != DT_UNKNOWN && de->d_type != DT_LNK)
        return 0;
#endif

    struct stat st;
    return fstatat(dirfd(dir), de->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);
}

static void unique_paste_path(char *dst, const char *dir, const char *name) {
    char candidate[PATH_MAX];
    char suffix[256] = "";

    for (int i = 0; i < 200; i++) {
        char joined_name[NAME_MAX + 256 + 1];
        if (!safe_join3(joined_name, sizeof(joined_name), name, "", suffix))
            continue;
        if (!safe_join3(candidate, sizeof(candidate), dir, "/", joined_name))
            continue;

        if (!path_exists(candidate)) {
            safe_copy(dst, PATH_MAX, candidate);
            return;
        }

        strncat(suffix, "_", sizeof(suffix) - strlen(suffix) - 1);
    }

    {
        char fallback[NAME_MAX + 64];
        if (snprintf(fallback, sizeof(fallback), "%s.%ld", name, (long)time(NULL)) < (int)sizeof(fallback))
            safe_join3(dst, PATH_MAX, dir, "/", fallback);
        else
            dst[0] = '\0';
    }
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];

    strncpy(tmp, path, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';

    size_t len = strlen(tmp);
    if (len == 0)
        return -1;

    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

static int get_trash_dir(char *out) {
    if (config_trash_dir[0]) {
        expand_path(out, config_trash_dir);
        return mkdir_p(out);
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0')
        return -1;

    snprintf(out, PATH_MAX, "%s/.local/share/simplefiles/trash", home);
    return mkdir_p(out);
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void expand_path(char *out, const char *in) {
    in = skip_spaces(in);

    if (in[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "";

        if (in[1] == '\0')
            snprintf(out, PATH_MAX, "%s", home);
        else if (in[1] == '/')
            snprintf(out, PATH_MAX, "%s/%s", home, in + 2);
        else
            snprintf(out, PATH_MAX, "%s", in);
        return;
    }

    if (in[0] == '/') {
        snprintf(out, PATH_MAX, "%s", in);
        return;
    }

    join_path(out, cwd_path, in);
}

static void start_command(const char *initial) {
    command_mode = 1;
    pending_key = 0;
    pending_delete = 0;
    pending_empty_trash = 0;

    if (!initial) initial = "";

    strncpy(command, initial, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    command_len = (int)strlen(command);
}

static void start_search(void) {
    search_mode = 1;
    pending_key = 0;
    pending_delete = 0;
    pending_empty_trash = 0;
    search_query[0] = '\0';
    search_len = 0;
}

static void start_rename_command(void) {
    if (entry_count <= 0) {
        set_message("nothing to rename");
        return;
    }

    char initial[1024];
    snprintf(initial, sizeof(initial), "rename %s", entries[cursor].name);
    start_command(initial);
}

static void empty_trash_now(void) {
    char trash[PATH_MAX];
    if (get_trash_dir(trash) != 0) {
        set_message("cannot open trash");
        return;
    }

    DIR *dir = opendir(trash);
    if (!dir) {
        set_message("cannot open trash");
        return;
    }

    int ok = 0;
    int fail = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char full[PATH_MAX];
        join_path(full, trash, de->d_name);

        if (remove_recursive(full) == 0)
            ok++;
        else
            fail++;
    }

    closedir(dir);

    if (ok == 0 && fail == 0)
        set_message("trash already empty");
    else if (ok > 0 && fail == 0)
        set_message("trash emptied");
    else if (ok > 0 && fail > 0)
        set_message("trash partly emptied; some failed");
    else
        set_message("empty trash failed");
}


static void add_open_rule(const char *ext, const char *cmd) {
    if (!ext || !cmd || open_rule_count >= 256) return;

    safe_copy(open_rules[open_rule_count].ext,
              sizeof(open_rules[open_rule_count].ext), ext);

    snprintf(open_rules[open_rule_count].cmd,
             sizeof(open_rules[open_rule_count].cmd),
             "%s", cmd);

    open_rule_count++;
}

static const char *find_open_rule(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return NULL;

    for (int i = 0; i < open_rule_count; i++) {
        if (strcasecmp(ext, open_rules[i].ext) == 0)
            return open_rules[i].cmd;
    }

    return NULL;
}

static int extension_in_text_list(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;

    char list[4096];
    snprintf(list, sizeof(list), "%s", config_text_extensions);

    char *tok = strtok(list, ",");
    while (tok) {
        trim_config_value(tok);
        if (strcasecmp(tok, ext) == 0)
            return 1;
        tok = strtok(NULL, ",");
    }

    return 0;
}

static void trim_config_value(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;

    if (p != s)
        memmove(s, p, strlen(p) + 1);

    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }

    len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
                     (s[0] == '\'' && s[len - 1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/simplefiles/config", home);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';

        char key[256];
        char val[512];

        safe_copy(key, sizeof(key), line);
        snprintf(val, sizeof(val), "%s", eq + 1);

        trim_config_value(key);
        trim_config_value(val);

        if (strcasecmp(key, "OPEN_MODE") == 0) {
            if (strcasecmp(val, "terminal") == 0)
                config_terminal_mode = 1;
            else
                config_terminal_mode = 0;
        } else if (strcasecmp(key, "EDITOR_COMMAND") == 0) {
            snprintf(config_editor_command,
                     sizeof(config_editor_command),
                     "%s",
                     val);

        } else if (strcasecmp(key, "TEXT_EXTENSIONS") == 0) {
            snprintf(config_text_extensions,
                     sizeof(config_text_extensions),
                     "%s",
                     val);

        } else if (strcasecmp(key, "PREVIEW") == 0) {
            config_preview =
                (strcasecmp(val, "true") == 0 ||
                 strcasecmp(val, "yes") == 0 ||
                 strcmp(val, "1") == 0);

        } else if (strcasecmp(key, "PREVIEW_LINES") == 0) {
            config_preview_lines = atoi(val);
            if (config_preview_lines < 1)
                config_preview_lines = 1;

        } else if (strcasecmp(key, "START_DIR") == 0) {
            snprintf(config_start_dir,
                     sizeof(config_start_dir),
                     "%s",
                     val);

        } else if (strcasecmp(key, "TRASH_DIR") == 0) {
            snprintf(config_trash_dir,
                     sizeof(config_trash_dir),
                     "%s",
                     val);

        } else if (strcasecmp(key, "CONFIRM_DELETE") == 0) {
            config_confirm_delete =
                (strcasecmp(val, "true") == 0 ||
                 strcasecmp(val, "yes") == 0 ||
                 strcmp(val, "1") == 0);

        } else if (strncasecmp(key, "OPEN_.", 6) == 0) {
            add_open_rule(key + 5, val);
        }
    }

    fclose(f);
}

__attribute__((unused)) static int text_file_type(const char *path) {
    const char *ext = strrchr(path, '.');

    if (!ext)
        return 0;

    return
        strcasecmp(ext, ".txt") == 0  ||
        strcasecmp(ext, ".md") == 0   ||
        strcasecmp(ext, ".markdown") == 0 ||
        strcasecmp(ext, ".c") == 0    ||
        strcasecmp(ext, ".h") == 0    ||
        strcasecmp(ext, ".cpp") == 0  ||
        strcasecmp(ext, ".hpp") == 0  ||
        strcasecmp(ext, ".cc") == 0   ||
        strcasecmp(ext, ".py") == 0   ||
        strcasecmp(ext, ".sh") == 0   ||
        strcasecmp(ext, ".conf") == 0 ||
        strcasecmp(ext, ".ini") == 0  ||
        strcasecmp(ext, ".log") == 0;
}

static void command_cd(const char *arg) {
    arg = skip_spaces(arg);
    if (arg[0] == '\0') {
        set_message("cd needs a path");
        return;
    }

    char path[PATH_MAX];
    expand_path(path, arg);

    if (chdir(path) == 0) {
        if (!getcwd(cwd_path, sizeof(cwd_path)))
            safe_copy(cwd_path, sizeof(cwd_path), "/");
        cursor = 0;
        top = 0;
        load_dir(cwd_path);
        restore_dir_cursor(cwd_path);
        set_message("changed directory");
    } else {
        set_message("cd failed");
    }
}

static void command_mkdir(const char *arg) {
    arg = skip_spaces(arg);
    if (arg[0] == '\0') {
        set_message("mkdir needs a name");
        return;
    }

    char path[PATH_MAX];
    expand_path(path, arg);

    if (mkdir(path, 0755) == 0) {
        load_dir(cwd_path);
        set_message("directory created");
    } else {
        set_message("mkdir failed");
    }
}

static void command_rename(const char *arg) {
    arg = skip_spaces(arg);
    if (entry_count <= 0) {
        set_message("nothing to rename");
        return;
    }

    if (arg[0] == '\0') {
        set_message("rename needs a new name");
        return;
    }

    char src[PATH_MAX];
    char dst[PATH_MAX];

    join_path(src, cwd_path, entries[cursor].name);
    expand_path(dst, arg);

    if (path_exists(dst)) {
        set_message("rename target exists");
        return;
    }

    if (rename(src, dst) == 0) {
        load_dir(cwd_path);
        set_message("renamed");
    } else {
        set_message("rename failed");
    }
}

static const char *relative_to_cwd(const char *path) {
    size_t len = strlen(cwd_path);

    if (strcmp(cwd_path, "/") == 0) {
        if (path[0] == '/')
            return path + 1;
        return path;
    }

    if (strncmp(path, cwd_path, len) == 0 && path[len] == '/')
        return path + len + 1;

    return path;
}

static void command_compress(const char *arg) {
    arg = skip_spaces(arg);

    if (arg[0] == '\0') {
        set_message("compress needs a zip name");
        return;
    }

    char zipname[PATH_MAX];
    expand_path(zipname, arg);

    size_t zlen = strlen(zipname);
    if (zlen < 4 || strcmp(zipname + zlen - 4, ".zip") != 0)
        strncat(zipname, ".zip", sizeof(zipname) - strlen(zipname) - 1);

    if (path_exists(zipname)) {
        set_message("zip target exists");
        return;
    }

    int item_count = selected_count > 0 ? selected_count : (entry_count > 0 ? 1 : 0);

    if (item_count <= 0) {
        set_message("nothing to compress");
        return;
    }

    struct stat cwd_stat;
    if (stat(cwd_path, &cwd_stat) != 0 || !S_ISDIR(cwd_stat.st_mode) ||
        access(cwd_path, X_OK) != 0) {
        set_message("compress failed: directory unavailable");
        return;
    }

    /* zip, -qr, archive, --, items..., NULL */
    char **zip_argv = calloc((size_t)item_count + 5, sizeof(*zip_argv));
    if (!zip_argv) {
        set_message("compress failed: out of memory");
        return;
    }

    zip_argv[0] = "zip";
    zip_argv[1] = "-qr";
    zip_argv[2] = zipname;
    zip_argv[3] = "--";

    for (int i = 0; i < item_count; i++) {
        if (selected_count > 0)
            zip_argv[i + 4] = (char *)relative_to_cwd(selected[i]);
        else
            zip_argv[i + 4] = entries[cursor].name;
    }

    debug_log("compress cwd=%s archive=%s items=%d", cwd_path, zipname,
              item_count);

    reset_shell_mode();

    pid_t pid = fork();
    int status = 0;
    int waited = -1;

    if (pid == 0) {
        if (chdir(cwd_path) != 0)
            _exit(126);
        execvp(zip_argv[0], zip_argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    if (pid > 0)
        waited = wait_for_child(pid, &status, "compress");

    reset_prog_mode();
    free(zip_argv);

    load_dir(cwd_path);

    if (pid > 0 && waited == 1 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0) {
        clear_selected();
        set_message("compressed");
    } else if (pid < 0) {
        set_message("compress failed: could not start");
    } else if (waited != 1) {
        set_message("compress failed: wait interrupted");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        set_message("compress failed: zip not found");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 126) {
        set_message("compress failed: could not execute zip");
    } else if (WIFEXITED(status)) {
        char failure[64];
        snprintf(failure, sizeof(failure), "compress failed: zip exit %d",
                 WEXITSTATUS(status));
        set_message(failure);
    } else if (WIFSIGNALED(status)) {
        char failure[64];
        snprintf(failure, sizeof(failure), "compress failed: signal %d",
                 WTERMSIG(status));
        set_message(failure);
    } else {
        set_message("compress failed");
    }

    destroy_windows();
    last_lines = 0;
    last_cols = 0;
    clear();
    erase();
    refresh();
    clearok(stdscr, TRUE);
    draw_ui();
}


static void command_extract(void) {
    if (entry_count <= 0) {
        set_message("nothing to extract");
        return;
    }

    const char *name = entries[cursor].name;
    size_t len = strlen(name);

    if (len < 5 || strcmp(name + len - 4, ".zip") != 0) {
        set_message("extract needs .zip");
        return;
    }

    char zip_path[PATH_MAX];
    join_path(zip_path, cwd_path, name);

    char outdir[PATH_MAX];
    if (strlen(name) >= sizeof(outdir)) {
        set_message("Archive name too long.");
        return;
    }
    memcpy(outdir, name, strlen(name) + 1);
    outdir[len - 4] = '\0';

    char outpath[PATH_MAX];
    join_path(outpath, cwd_path, outdir);

    if (path_exists(outpath)) {
        set_message("extract target exists");
        return;
    }

    char cmd[65536];
    snprintf(cmd, sizeof(cmd), "cd ");

    strncat(cmd, "'", sizeof(cmd) - strlen(cmd) - 1);
    for (const char *s = cwd_path; *s; s++) {
        if (*s == '\'')
            strncat(cmd, "'\\''", sizeof(cmd) - strlen(cmd) - 1);
        else {
            char one[2] = {*s, '\0'};
            strncat(cmd, one, sizeof(cmd) - strlen(cmd) - 1);
        }
    }
    strncat(cmd, "'", sizeof(cmd) - strlen(cmd) - 1);

    strncat(cmd, " && mkdir -p '", sizeof(cmd) - strlen(cmd) - 1);
    for (const char *s = outdir; *s; s++) {
        if (*s == '\'')
            strncat(cmd, "'\\''", sizeof(cmd) - strlen(cmd) - 1);
        else {
            char one[2] = {*s, '\0'};
            strncat(cmd, one, sizeof(cmd) - strlen(cmd) - 1);
        }
    }
    strncat(cmd, "'", sizeof(cmd) - strlen(cmd) - 1);

    strncat(cmd, " && unzip -q '", sizeof(cmd) - strlen(cmd) - 1);
    for (const char *s = name; *s; s++) {
        if (*s == '\'')
            strncat(cmd, "'\\''", sizeof(cmd) - strlen(cmd) - 1);
        else {
            char one[2] = {*s, '\0'};
            strncat(cmd, one, sizeof(cmd) - strlen(cmd) - 1);
        }
    }
    strncat(cmd, "' -d '", sizeof(cmd) - strlen(cmd) - 1);
    for (const char *s = outdir; *s; s++) {
        if (*s == '\'')
            strncat(cmd, "'\\''", sizeof(cmd) - strlen(cmd) - 1);
        else {
            char one[2] = {*s, '\0'};
            strncat(cmd, one, sizeof(cmd) - strlen(cmd) - 1);
        }
    }
    strncat(cmd, "'", sizeof(cmd) - strlen(cmd) - 1);

    reset_shell_mode();
    int rc = system(cmd);
    reset_prog_mode();

    load_dir(cwd_path);

    if (rc == 0) {
        set_message("extracted");
    } else {
        set_message("extract failed");
    }

    destroy_windows();
    last_lines = 0;
    last_cols = 0;
    clear();
    erase();
    refresh();
    clearok(stdscr, TRUE);
    draw_ui();
}

static int command_starts_with(const char *cmd, const char *name) {
    size_t n = strlen(name);
    return strncmp(cmd, name, n) == 0 &&
           (cmd[n] == '\0' || cmd[n] == ' ' || cmd[n] == '\t');
}

static int openwith_is_tui(const char *cmd) {
    cmd = skip_spaces(cmd);

    return command_starts_with(cmd, "nvim")        ||
           command_starts_with(cmd, "vim")         ||
           command_starts_with(cmd, "vi")          ||
           command_starts_with(cmd, "nano")        ||
           command_starts_with(cmd, "emacs")       ||
           command_starts_with(cmd, "simplepdf")    ||
           command_starts_with(cmd, "simplewords")  ||
           command_starts_with(cmd, "simplefiles")  ||
           command_starts_with(cmd, "htop")         ||
           command_starts_with(cmd, "less")         ||
           command_starts_with(cmd, "more")         ||
           command_starts_with(cmd, "man");
}

static void shell_quote_append(char *dst, size_t dstsz, const char *src) {
    strncat(dst, "'", dstsz - strlen(dst) - 1);

    for (const char *s = src; *s; s++) {
        if (*s == '\'')
            strncat(dst, "'\\''", dstsz - strlen(dst) - 1);
        else {
            char one[2] = {*s, '\0'};
            strncat(dst, one, dstsz - strlen(dst) - 1);
        }
    }

    strncat(dst, "'", dstsz - strlen(dst) - 1);
}

static void exec_openwith_detached(const char *cmd, const char *path) {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2)
            close(devnull);
    }

    char script[4096];
    snprintf(script, sizeof(script), "exec %s \"$1\"", cmd);

    execl("/bin/sh",
          "sh",
          "-c",
          script,
          "simplefiles-openwith",
          path,
          (char *)NULL);

    _exit(127);
}

static void exec_default_open_detached(const char *path) {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2)
            close(devnull);
    }

    execlp("gio", "gio", "open", path, (char *)NULL);
    execlp("xdg-open", "xdg-open", path, (char *)NULL);
    _exit(127);
}

static void command_openwith(const char *arg) {
    arg = skip_spaces(arg);

    if (arg[0] == '\0') {
        set_message("openwith needs a program");
        return;
    }

    if (entry_count <= 0) {
        set_message("nothing to open");
        return;
    }

    char full[PATH_MAX];
    join_path(full, cwd_path, entries[cursor].name);

    int is_tui = openwith_is_tui(arg);

    if (is_tui) {
        char cmd[4096];
        int status = 0;
        pid_t pid;

        snprintf(cmd, sizeof(cmd), "%s ", arg);
        shell_quote_append(cmd, sizeof(cmd), full);

        debug_log("openwith tui before def_prog_mode/endwin command=%s", arg);
        def_prog_mode();
        endwin();

        pid = fork();
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        debug_log("openwith tui child pid=%ld", (long)pid);

        if (pid > 0)
            wait_for_child(pid, &status, "openwith tui");

        if (!redraw_after_child("openwith tui"))
            return;

        if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            set_message("returned");
        else
            set_message("openwith tui failed");

        return;
    }

    pid_t pid = fork();

    if (pid < 0) {
        set_message("openwith failed");
        return;
    }

    if (pid == 0) {
        setsid();

        pid_t grandchild = fork();
        if (grandchild < 0)
            _exit(127);
        if (grandchild > 0)
            _exit(0);

        exec_openwith_detached(arg, full);
    }

    if (pid > 0)
        wait_for_child(pid, NULL, "openwith detached");

    if (running)
        set_message("opened");
}


static void command_unmount(void) {
    if (entry_count <= 0) {
        set_message("nothing selected");
        return;
    }

    char full[PATH_MAX];
    join_path(full, cwd_path, entries[cursor].name);

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_message("not a directory");
        return;
    }

    char cmd[PATH_MAX + 128];

    snprintf(cmd, sizeof(cmd),
             "gio mount -u \"%s\" >/dev/null 2>&1",
             full);

    int rc = system(cmd);

    if (rc != 0) {
        snprintf(cmd, sizeof(cmd),
                 "umount \"%s\" >/dev/null 2>&1",
                 full);
        rc = system(cmd);
    }

    if (rc == 0) {
        load_dir(cwd_path);
        set_message("drive unmounted");
    } else {
        set_message("unmount failed");
    }
}


static void execute_command(const char *raw) {
    const char *cmd = skip_spaces(raw);

    if (cmd[0] == '\0') {
        set_message("");
        return;
    }

    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
        exit_reason = "q";
        running = 0;
        return;
    }

    if (strcmp(cmd, "reload") == 0 || strcmp(cmd, "redraw") == 0) {
        load_dir(cwd_path);
        clearok(stdscr, TRUE);
        set_message("reloaded");
        return;
    }

    if (strcmp(cmd, "hidden") == 0) {
        show_hidden = !show_hidden;
        load_dir(cwd_path);
        set_message(show_hidden ? "hidden files shown" : "hidden files hidden");
        return;
    }

    if (strcmp(cmd, "emptytrash") == 0) {
        pending_empty_trash = 1;
        set_message("empty trash permanently? y/N");
        return;
    }

    if (strcmp(cmd, "delete") == 0) {
        arm_delete();
        return;
    }

    if (strncmp(cmd, "cd ", 3) == 0) {
        command_cd(cmd + 3);
        return;
    }

    if (strncmp(cmd, "mkdir ", 6) == 0) {
        command_mkdir(cmd + 6);
        return;
    }

    if (strncmp(cmd, "rename ", 7) == 0) {
        command_rename(cmd + 7);
        return;
    }

    if (strncmp(cmd, "compress ", 9) == 0) {
        command_compress(cmd + 9);
        return;
    }

    if (strncmp(cmd, "openwith ", 9) == 0) {
        command_openwith(cmd + 9);
        return;
    }

    if (strcmp(cmd, "extract") == 0) {
        command_extract();
        return;
    }

    if (strcmp(cmd, "unmount") == 0) {
        command_unmount();
        return;
    }

    set_message("unknown command");
}

static void load_dir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    entry_count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && entry_count < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        if (!show_hidden && de->d_name[0] == '.')
            continue;

        safe_copy(entries[entry_count].name, sizeof(entries[entry_count].name), de->d_name);
        entries[entry_count].is_dir = dir_entry_is_dir(dir, de);
        entry_count++;
    }

    closedir(dir);

    qsort(entries, entry_count, sizeof(Entry), cmp_entries);

    if (cursor >= entry_count) cursor = entry_count - 1;
    if (cursor < 0) cursor = 0;
    if (top < 0) top = 0;
}

static int is_selected(const char *path) {
    for (int i = 0; i < selected_count; i++) {
        if (strcmp(selected[i], path) == 0)
            return 1;
    }
    return 0;
}

static void toggle_selected(const char *path) {
    for (int i = 0; i < selected_count; i++) {
        if (strcmp(selected[i], path) == 0) {
            for (int j = i; j < selected_count - 1; j++)
                strcpy(selected[j], selected[j + 1]);
            selected_count--;
            return;
        }
    }

    if (selected_count < MAX_SELECTED) {
        strncpy(selected[selected_count], path, PATH_MAX - 1);
        selected[selected_count][PATH_MAX - 1] = '\0';
        selected_count++;
    }
}

static void clear_selected(void) {
    selected_count = 0;
}

static void invert_selection(void) {
    for (int i = 0; i < entry_count; i++) {
        char full[PATH_MAX];
        join_path(full, cwd_path, entries[i].name);
        toggle_selected(full);
    }
}

static void clear_clipboard(void) {
    clipboard_count = 0;
    clipboard_mode = 0;
    clipboard_generation++;
}

static void add_clipboard_path(const char *path) {
    if (clipboard_count >= MAX_CLIPBOARD) return;

    safe_copy(clipboard_paths[clipboard_count], sizeof(clipboard_paths[clipboard_count]), path);
    clipboard_count++;
}

static void yank_or_cut(int mode) {
    clear_clipboard();

    clipboard_mode = mode;

    if (selected_count > 0) {
        for (int i = 0; i < selected_count && i < MAX_CLIPBOARD; i++)
            add_clipboard_path(selected[i]);
    } else if (entry_count > 0) {
        char full[PATH_MAX];
        join_path(full, cwd_path, entries[cursor].name);
        add_clipboard_path(full);
    }

    clear_selected();

    if (clipboard_count == 0) {
        clipboard_mode = 0;
        set_message("clipboard empty");
    } else if (clipboard_mode == 'y') {
        set_message("copied to clipboard");
    } else if (clipboard_mode == 'd') {
        set_message("cut to clipboard");
    }
}

static uint64_t recursive_size(const char *path) {
    struct stat st;

    if (lstat(path, &st) != 0)
        return 0;

    if (S_ISREG(st.st_mode))
        return (uint64_t)st.st_size;

    if (S_ISDIR(st.st_mode)) {
        uint64_t total = 0;
        DIR *dir = opendir(path);
        if (!dir)
            return 0;

        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            char full[PATH_MAX];
            join_path(full, path, de->d_name);
            total += recursive_size(full);
        }

        closedir(dir);
        return total;
    }

    return 0;
}

static uint64_t clipboard_total_size(void) {
    uint64_t total = 0;

    for (int i = 0; i < clipboard_count; i++)
        total += recursive_size(clipboard_paths[i]);

    return total;
}

static int ensure_paste_progress(void) {
    if (paste_progress)
        return 1;

    paste_progress = mmap(NULL, sizeof(*paste_progress),
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (paste_progress == MAP_FAILED) {
        paste_progress = NULL;
        return 0;
    }

    memset((void *)paste_progress, 0, sizeof(*paste_progress));
    return 1;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;

    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (out < 0) {
        close(in);
        return -1;
    }

    char buf[65536];
    ssize_t n;

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        char *p = buf;
        ssize_t left = n;

        while (left > 0) {
            ssize_t written = write(out, p, left);
            if (written < 0) {
                close(in);
                close(out);
                return -1;
            }

            if (paste_progress && paste_progress->active)
                paste_progress->done_bytes += (uint64_t)written;

            p += written;
            left -= written;
        }
    }

    close(in);
    close(out);

    return n < 0 ? -1 : 0;
}

static int copy_recursive(const char *src, const char *dst) {
    struct stat st;

    if (lstat(src, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0)
            return -1;

        DIR *dir = opendir(src);
        if (!dir)
            return -1;

        struct dirent *de;

        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            char sfull[PATH_MAX];
            char dfull[PATH_MAX];

            join_path(sfull, src, de->d_name);
            join_path(dfull, dst, de->d_name);

            if (copy_recursive(sfull, dfull) != 0) {
                closedir(dir);
                return -1;
            }
        }

        closedir(dir);
        return 0;
    }

    if (S_ISREG(st.st_mode))
        return copy_file(src, dst, st.st_mode & 0777);

    return -1;
}

static int remove_recursive(const char *path) {
    struct stat st;

    if (lstat(path, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir)
            return -1;

        struct dirent *de;

        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            char full[PATH_MAX];
            join_path(full, path, de->d_name);

            if (remove_recursive(full) != 0) {
                closedir(dir);
                return -1;
            }
        }

        closedir(dir);
        return rmdir(path);
    }

    return unlink(path);
}

__attribute__((unused)) static int unique_trash_path(char *dst, const char *trash, const char *name) {
    char candidate[PATH_MAX];
    join_path(candidate, trash, name);

    if (!path_exists(candidate)) {
        strncpy(dst, candidate, PATH_MAX - 1);
        dst[PATH_MAX - 1] = '\0';
        return 0;
    }

    time_t now = time(NULL);

    for (int i = 1; i < 10000; i++) {
        {
            char numbered[NAME_MAX + 64];
            if (snprintf(numbered, sizeof(numbered), "%s.%ld.%d", name, (long)now, i) >= (int)sizeof(numbered))
                continue;
            if (!safe_join3(candidate, sizeof(candidate), trash, "/", numbered))
                continue;
        }

        if (!path_exists(candidate)) {
            safe_copy(dst, PATH_MAX, candidate);
            return 0;
        }
    }

    return -1;
}

static int move_to_trash_one(const char *src) {
    char cmd[PATH_MAX * 2 + 128];

    if (snprintf(cmd,
                 sizeof(cmd),
                 "gio trash -- \"%s\" >/dev/null 2>&1",
                 src) >= (int)sizeof(cmd))
        return -1;

    return system(cmd) == 0 ? 0 : -1;
}

static void arm_delete(void) {
    if (!config_confirm_delete) {
        confirm_delete();
        return;
    }

    if (selected_count > 0) {
        pending_delete = 1;
        set_message("delete selected to trash? y/N");
        return;
    }

    if (entry_count > 0) {
        pending_delete = 1;
        set_message("delete current to trash? y/N");
        return;
    }

    set_message("nothing to delete");
}

static void confirm_delete(void) {
    int ok = 0;
    int fail = 0;

    if (selected_count > 0) {
        for (int i = 0; i < selected_count; i++) {
            if (move_to_trash_one(selected[i]) == 0)
                ok++;
            else
                fail++;
        }

        clear_selected();
    } else if (entry_count > 0) {
        char full[PATH_MAX];
        join_path(full, cwd_path, entries[cursor].name);

        if (move_to_trash_one(full) == 0)
            ok++;
        else
            fail++;
    }

    load_dir(cwd_path);

    if (ok > 0 && fail == 0)
        set_message("moved to trash");
    else if (ok > 0 && fail > 0)
        set_message("some moved to trash; some failed");
    else
        set_message("trash move failed");
}

static void perform_paste(const char *destination, PasteResult *result) {
    memset(result, 0, sizeof(*result));

    for (int i = 0; i < clipboard_count; i++) {
        const char *src = clipboard_paths[i];
        const char *name = base_name(src);

        if (name[0] == '\0')
            continue;

        char plain_dst[PATH_MAX];
        char dst[PATH_MAX];
        join_path(plain_dst, destination, name);
        unique_paste_path(dst, destination, name);

        if (strcmp(src, dst) == 0) {
            result->fail++;
            continue;
        }
        if (strcmp(plain_dst, dst) != 0)
            result->renamed++;

        if (clipboard_mode == 'd') {
            if (rename(src, dst) == 0 ||
                (copy_recursive(src, dst) == 0 && remove_recursive(src) == 0)) {
                safe_copy(result->last_pasted_name,
                          sizeof(result->last_pasted_name), base_name(dst));
                result->ok++;
            } else {
                result->fail++;
            }
        } else if (clipboard_mode == 'y') {
            if (copy_recursive(src, dst) == 0) {
                safe_copy(result->last_pasted_name,
                          sizeof(result->last_pasted_name), base_name(dst));
                result->ok++;
            } else {
                result->fail++;
            }
        }
    }
}

static int write_pipe_result(int fd, const PasteResult *result) {
    const char *p = (const char *)result;
    size_t remaining = sizeof(*result);

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n > 0) {
            p += n;
            remaining -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return 0;
    }
    return 1;
}

static int read_pipe_result(int fd, PasteResult *result) {
    char *p = (char *)result;
    size_t remaining = sizeof(*result);

    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n > 0) {
            p += n;
            remaining -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return 0;
    }
    return 1;
}

static int paste_clipboard(void) {
    if (paste_worker_pid > 0) {
        set_message("paste already in progress");
        return -1;
    }
    if (clipboard_count <= 0 || clipboard_mode == 0) {
        set_message("nothing to paste");
        return -1;
    }

    int result_pipe[2];
    if (pipe(result_pipe) != 0) {
        set_message("paste failed: could not start worker");
        return -1;
    }

    safe_copy(paste_destination, sizeof(paste_destination), cwd_path);
    paste_worker_mode = clipboard_mode;
    paste_clipboard_generation = clipboard_generation;

    if (!ensure_paste_progress()) {
        close(result_pipe[0]);
        close(result_pipe[1]);
        set_message("paste failed: no progress memory");
        return -1;
    }
    paste_progress->total_bytes = clipboard_total_size();
    paste_progress->done_bytes = 0;
    paste_progress->active = 1;

    pid_t pid = fork();
    if (pid == 0) {
        close(result_pipe[0]);
        if (instance_lock_fd >= 0)
            close(instance_lock_fd);
        if (debug_file) {
            fclose(debug_file);
            debug_file = NULL;
        }

        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);

        PasteResult result;
        perform_paste(paste_destination, &result);
        write_pipe_result(result_pipe[1], &result);
        close(result_pipe[1]);
        _exit(result.fail > 0 && result.ok == 0 ? 1 : 0);
    }

    close(result_pipe[1]);
    if (pid < 0) {
        close(result_pipe[0]);
        set_message("paste failed: could not start worker");
        return -1;
    }

    paste_worker_pid = pid;
    paste_result_fd = result_pipe[0];
    set_message("pasting in background");
    debug_log("paste worker started pid=%ld destination=%s items=%d mode=%c",
              (long)pid, paste_destination, clipboard_count, clipboard_mode);
    return 0;
}

static int check_background_paste(void) {
    if (paste_worker_pid <= 0)
        return 0;

    int status = 0;
    pid_t pid = waitpid(paste_worker_pid, &status, WNOHANG);
    if (pid == 0 || (pid < 0 && errno == EINTR))
        return 0;

    PasteResult result;
    int have_result = paste_result_fd >= 0 &&
                      read_pipe_result(paste_result_fd, &result);
    if (paste_result_fd >= 0)
        close(paste_result_fd);

    debug_log("paste worker finished pid=%ld status=%d result=%d",
              (long)paste_worker_pid, status, have_result);
    paste_worker_pid = -1;
    paste_result_fd = -1;
    if (paste_progress)
        paste_progress->active = 0;

    if (!have_result) {
        set_message("background paste failed");
        return 1;
    }

    load_dir(cwd_path);
    if (strcmp(cwd_path, paste_destination) == 0 && result.last_pasted_name[0])
        set_cursor_to_name(result.last_pasted_name);

    if (paste_worker_mode == 'd' && result.ok > 0 &&
        clipboard_generation == paste_clipboard_generation)
        clear_clipboard();

    if (result.ok > 0 && result.fail == 0 && result.renamed > 0)
        set_message("paste complete; renamed duplicates");
    else if (result.ok > 0 && result.fail == 0)
        set_message("paste complete");
    else if (result.ok > 0)
        set_message("paste partly complete; some failed");
    else
        set_message("paste failed");

    return 1;
}

static void draw_text(WINDOW *win, int y, int x, int w, const char *s) {
    if (!win || w <= 0) return;

    wmove(win, y, x);
    for (int i = 0; i < w; i++)
        waddch(win, ' ');

    wchar_t ws[8192];
    memset(ws, 0, sizeof(ws));

    mbstate_t state;
    memset(&state, 0, sizeof(state));

    const char *src = s;
    size_t n = mbsrtowcs(ws, &src, 8191, &state);

    wmove(win, y, x);

    if (n == (size_t)-1)
        waddnstr(win, s, w);
    else
        waddnwstr(win, ws, w);
}

static void clear_window(WINDOW *win) {
    if (!win) return;
    werase(win);
}

static void write_terminal_control(const char *sequence, size_t length) {
    while (length > 0) {
        ssize_t written = write(STDOUT_FILENO, sequence, length);
        if (written > 0) {
            sequence += written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        break;
    }
}

static int screen_updates_can_synchronize(void) {
    const char *term = getenv("TERM");
    return isatty(STDOUT_FILENO) && term &&
           term[0] != '\0' && strcmp(term, "dumb") != 0;
}

/* Modern terminals recognize DEC mode 2026 as a synchronized-update fence.
 * Unknown private modes are safely ignored by older terminals. */
static void begin_screen_update(void) {
    static const char sync_begin[] = "\033[?2026h";

    if (screen_updates_can_synchronize())
        write_terminal_control(sync_begin, sizeof(sync_begin) - 1);
}

static void end_screen_update(void) {
    static const char sync_end[] = "\033[?2026l";

    fflush(stdout);
    if (screen_updates_can_synchronize())
        write_terminal_control(sync_end, sizeof(sync_end) - 1);
}

static void present_screen(void) {
    begin_screen_update();
    doupdate();
    end_screen_update();
}

static void draw_parent_pane(WINDOW *win, int w, int h) {
    clear_window(win);
    if (strcmp(cwd_path, "/") == 0) {
        draw_text(win, 0, 0, w, "[root]");
        return;
    }

    char parent[PATH_MAX];
    safe_copy(parent, sizeof(parent), cwd_path);

    char *slash = strrchr(parent, '/');
    char current_name[NAME_MAX + 1] = "";

    if (slash && slash != parent) {
        safe_copy(current_name, sizeof(current_name), slash + 1);
        *slash = '\0';
    } else {
        strcpy(parent, "/");
        if (slash && slash == parent) {
            safe_copy(current_name, sizeof(current_name), cwd_path + 1);
        }
    }

    DIR *dir = opendir(parent);
    if (!dir) return;

    Entry parent_entries[MAX_ENTRIES];
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!show_hidden && de->d_name[0] == '.')
            continue;

        safe_copy(parent_entries[count].name, sizeof(parent_entries[count].name), de->d_name);
        parent_entries[count].is_dir = dir_entry_is_dir(dir, de);
        count++;
    }

    closedir(dir);

    qsort(parent_entries, count, sizeof(Entry), cmp_entries);

    int maxrows = h;

    for (int i = 0; i < count && i < maxrows; i++) {
        char line[PATH_MAX];
        safe_join3(line, sizeof(line),
                   parent_entries[i].is_dir ? "/" : " ",
                   "", parent_entries[i].name);

        if (strcmp(parent_entries[i].name, current_name) == 0)
            wattron(win, A_REVERSE);

        draw_text(win, i, 0, w, line);

        if (strcmp(parent_entries[i].name, current_name) == 0)
            wattroff(win, A_REVERSE);
    }
}

static void adjust_current_view(int view_h) {
    if (cursor < top) top = cursor;
    if (cursor >= top + view_h) top = cursor - view_h + 1;
    if (top < 0) top = 0;
}

static void draw_current_row(WINDOW *win, int w, int row, int idx) {
    if (idx >= entry_count) {
        draw_text(win, row, 0, w, "");
        return;
    }

    char full[PATH_MAX];
    join_path(full, cwd_path, entries[idx].name);

    int sel = is_selected(full);

    char line[PATH_MAX];
    snprintf(line, sizeof(line), "%c %s%s",
             sel ? '*' : ' ',
             entries[idx].is_dir ? "/" : " ",
             entries[idx].name);

    if (idx == cursor)
        wattron(win, A_REVERSE);

    draw_text(win, row, 0, w, line);

    if (idx == cursor)
        wattroff(win, A_REVERSE);
}

static void draw_current_pane(WINDOW *win, int w, int h) {
    clear_window(win);
    adjust_current_view(h);

    for (int i = 0; i < h; i++)
        draw_current_row(win, w, i, top + i);
}

static void preview_directory(WINDOW *win, const char *path, int w, int h) {
    DIR *dir = opendir(path);
    if (!dir) {
        draw_text(win, 0, 0, w, "[cannot open directory]");
        return;
    }

    Entry pentries[MAX_ENTRIES];
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!show_hidden && de->d_name[0] == '.')
            continue;

        safe_copy(pentries[count].name, sizeof(pentries[count].name), de->d_name);
        pentries[count].is_dir = dir_entry_is_dir(dir, de);
        count++;
    }

    closedir(dir);

    qsort(pentries, count, sizeof(Entry), cmp_entries);

    int maxrows = h;

    for (int i = 0; i < count && i < maxrows; i++) {
        char line[PATH_MAX];
        safe_join3(line, sizeof(line),
                   pentries[i].is_dir ? "/" : " ",
                   "", pentries[i].name);
        draw_text(win, i, 0, w, line);
    }
}


static int looks_binary(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 1;

    unsigned char buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0)
            return 1;
    }

    return 0;
}

static int has_ext(const char *path, const char *ext) {
    size_t lp = strlen(path);
    size_t le = strlen(ext);
    if (lp < le) return 0;
    return strcasecmp(path + lp - le, ext) == 0;
}

static void preview_file(WINDOW *win, const char *path, int w, int h) {
    if (has_ext(path, ".pdf") || has_ext(path, ".djvu") || has_ext(path, ".epub") || has_ext(path, ".mobi")) {
        struct stat st;
        if (stat(path, &st) == 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "binary/unknown file");
            draw_text(win, 0, 0, w, buf);
            snprintf(buf, sizeof(buf), "size: %lld bytes", (long long)st.st_size);
            draw_text(win, 1, 0, w, buf);
        }
        return;
    }


    struct stat st;
    if (stat(path, &st) != 0) {
        draw_text(win, 0, 0, w, "[cannot stat file]");
        return;
    }
    if (looks_binary(path)) {
        char line[PATH_MAX];

        snprintf(line, sizeof(line), "binary/unknown file");
        draw_text(win, 0, 0, w, line);

        snprintf(line, sizeof(line), "size: %lld bytes", (long long)st.st_size);
        draw_text(win, 1, 0, w, line);

        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        draw_text(win, 0, 0, w, "[cannot open file]");
        return;
    }

    char line[4096];
    int row = 0;
    int maxrows = h;

    while (row < maxrows && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        draw_text(win, row, 0, w, line);
        row++;
    }

    fclose(f);
}

static void draw_preview_pane(WINDOW *win, int w, int h) {
    clear_window(win);
    if (entry_count == 0) {
        draw_text(win, 0, 0, w, "empty");
        return;
    }

    char full[PATH_MAX];
    join_path(full, cwd_path, entries[cursor].name);

    if (!config_preview) {
        draw_text(win, 0, 0, w, "[preview disabled]");
        return;
    }

    if (config_preview_lines < h)
        h = config_preview_lines;

    if (entries[cursor].is_dir)
        preview_directory(win, full, w, h);
    else if (path_is_regular(full))
        preview_file(win, full, w, h);
    else
        draw_text(win, 0, 0, w, "[not a regular file]");
}

static void mode_string(mode_t mode, char *out) {
    out[0] = S_ISDIR(mode) ? 'd' :
             S_ISLNK(mode) ? 'l' :
             S_ISCHR(mode) ? 'c' :
             S_ISBLK(mode) ? 'b' :
             S_ISFIFO(mode) ? 'p' :
             S_ISSOCK(mode) ? 's' : '-';

    const char chars[] = "rwxrwxrwx";

    for (int i = 0; i < 9; i++)
        out[i + 1] = (mode & (1 << (8 - i))) ? chars[i] : '-';

    out[10] = '\0';
}

static void human_size(long long bytes, char *out, size_t outsz) {
    const char *units[] = {"B", "K", "M", "G", "T"};
    double size = (double)bytes;
    int unit = 0;

    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }

    if (unit == 0)
        snprintf(out, outsz, "%lld %s", bytes, units[unit]);
    else
        snprintf(out, outsz, "%.1f %s", size, units[unit]);
}

static void draw_top_bar(void) {
    if (!top_win) return;

    werase(top_win);
    wattrset(top_win, A_NORMAL);

    for (int x = 0; x < COLS; x++)
        mvwaddch(top_win, 0, x, ' ');

    char header[PATH_MAX + NAME_MAX + 2];

    if (entry_count > 0) {
        if (strcmp(cwd_path, "/") == 0)
            snprintf(header, sizeof(header), "/%s", entries[cursor].name);
        else
            snprintf(header, sizeof(header), "%s/%s", cwd_path, entries[cursor].name);
    } else {
        snprintf(header, sizeof(header), "%s", cwd_path);
    }

    mvwaddnstr(top_win, 0, 0, header, COLS - 1);
}

static void human_size_u64(uint64_t bytes, char *out, size_t outsz) {
    const char *units[] = {"B", "K", "M", "G", "T"};
    double size = (double)bytes;
    int unit = 0;

    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }

    if (unit == 0)
        snprintf(out, outsz, "%llu %s", (unsigned long long)bytes, units[unit]);
    else
        snprintf(out, outsz, "%.1f %s", size, units[unit]);
}

static const char *paste_spinner_frame(void) {
    static const char *frames[] = {"◐", "◓", "◑", "◒"};
    struct timespec ts;
    unsigned long long ticks;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return frames[0];

    ticks = (unsigned long long)ts.tv_sec * 4ULL +
            (unsigned long long)(ts.tv_nsec / 250000000L);

    return frames[ticks % 4ULL];
}

static void progress_bar(char *out, size_t outsz, int pct) {
    int width = 20;
    int filled;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    filled = (pct * width + 50) / 100;
    if (filled > width) filled = width;

    if (outsz == 0)
        return;

    size_t pos = 0;
    if (pos + 1 < outsz) out[pos++] = '[';
    for (int i = 0; i < width && pos + 1 < outsz; i++)
        out[pos++] = i < filled ? '#' : '-';
    if (pos + 1 < outsz) out[pos++] = ']';
    out[pos] = '\0';
}

static void draw_status(WINDOW *win, int w) {
    if (!win) return;

    werase(win);
    wbkgd(win, A_REVERSE);

    if (command_mode) {
        char line[1200];
        snprintf(line, sizeof(line), ":%s", command);
        draw_text(win, 0, 0, w, line);
        return;
    }

    if (search_mode) {
        char line[1200];
        snprintf(line, sizeof(line), "/%s", search_query);
        draw_text(win, 0, 0, w, line);
        return;
    }

    if (pending_delete) {
        draw_text(win, 0, 0, w, "delete to trash? y/N");
        return;
    }

    if (pending_empty_trash) {
        draw_text(win, 0, 0, w, "empty trash permanently? y/N");
        return;
    }

    if (paste_worker_pid > 0 && paste_progress && paste_progress->active) {
        uint64_t done = paste_progress->done_bytes;
        uint64_t total = paste_progress->total_bytes;
        char donebuf[64];
        char totalbuf[64];
        char bar[32];
        char line[256];
        int pct = 0;

        if (total > 0) {
            if (done > total)
                done = total;
            pct = (int)((done * 100ULL) / total);
        }

        progress_bar(bar, sizeof(bar), pct);
        human_size_u64(done, donebuf, sizeof(donebuf));
        human_size_u64(total, totalbuf, sizeof(totalbuf));

        if (total > 0)
            snprintf(line, sizeof(line), "%s paste %d%% %s %s / %s",
                     paste_spinner_frame(), pct, bar, donebuf, totalbuf);
        else
            snprintf(line, sizeof(line), "%s pasting...", paste_spinner_frame());

        draw_text(win, 0, 0, w, line);
        return;
    }

    if (entry_count <= 0) {
        draw_text(win, 0, 0, w, message[0] ? message : "empty");
        return;
    }

    char full[PATH_MAX];
    join_path(full, cwd_path, entries[cursor].name);

    struct stat st;
    if (lstat(full, &st) != 0) {
        draw_text(win, 0, 0, w, message[0] ? message : "[cannot stat]");
        return;
    }

    char perms[16];
    mode_string(st.st_mode, perms);

    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);

    char owner[64];
    char group[64];

    snprintf(owner, sizeof(owner), "%s", pw ? pw->pw_name : "?");
    snprintf(group, sizeof(group), "%s", gr ? gr->gr_name : "?");

    char date[64];
    struct tm *tm = localtime(&st.st_mtime);
    if (tm)
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm);
    else
        snprintf(date, sizeof(date), "?");

    char sizebuf[64];
    human_size((long long)st.st_size, sizebuf, sizeof(sizebuf));

    char freebuf[64] = "";
    struct statvfs vfs;
    if (statvfs(cwd_path, &vfs) == 0) {
        unsigned long long free_bytes = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
        human_size((long long)free_bytes, freebuf, sizeof(freebuf));
    } else {
        snprintf(freebuf, sizeof(freebuf), "?");
    }

    char pending[32] = "";
    if (pending_key == 'y')
        snprintf(pending, sizeof(pending), " pending:y");
    else if (pending_key == 'd')
        snprintf(pending, sizeof(pending), " pending:d");
    else if (pending_key == 'p')
        snprintf(pending, sizeof(pending), " pending:p");
    else if (pending_key == 'c')
        snprintf(pending, sizeof(pending), " pending:c");

    char line[PATH_MAX + 512];
    snprintf(line, sizeof(line),
             "%s  %s %s  %s  %s  selected:%d  free:%s  %d/%d%s  %s",
             perms, owner, group, date, sizebuf,
             selected_count, freebuf,
             entry_count > 0 ? cursor + 1 : 0, entry_count,
             pending, message);

    draw_text(win, 0, 0, w, line);
}

static void destroy_windows(void) {
    if (top_win) {
        delwin(top_win);
        top_win = NULL;
    }

    if (parent_win) {
        delwin(parent_win);
        parent_win = NULL;
    }

    if (current_win) {
        delwin(current_win);
        current_win = NULL;
    }

    if (preview_win) {
        delwin(preview_win);
        preview_win = NULL;
    }

    if (status_win) {
        delwin(status_win);
        status_win = NULL;
    }
}

static void setup_windows(void) {
    if (LINES == last_lines && COLS == last_cols && current_win && status_win)
        return;

    destroy_windows();

    clear();
    erase();
    refresh();
    clearok(stdscr, TRUE);

    last_lines = LINES;
    last_cols = COLS;

    int pane_y = 1;
    int pane_h = LINES - 2;
    if (pane_h < 1)
        pane_h = 1;

    top_win = newwin(1, COLS, 0, 0);
    status_win = newwin(1, COLS, LINES - 1, 0);

    if (COLS < 62) {
        single_pane_mode = 1;
        current_win = newwin(pane_h, COLS, pane_y, 0);
    } else {
        single_pane_mode = 0;

        int gap = 1;
        int w1 = COLS / 4;
        int w2 = COLS / 3;

        if (w1 < 18) w1 = 18;
        if (w2 < 24) w2 = 24;

        int x1 = 0;
        int x2 = x1 + w1 + gap;
        int x3 = x2 + w2 + gap;
        int w3 = COLS - x3;

        if (w3 < 20) {
            single_pane_mode = 1;
            current_win = newwin(pane_h, COLS, pane_y, 0);
        } else {
            parent_win = newwin(pane_h, w1, pane_y, x1);
            current_win = newwin(pane_h, w2, pane_y, x2);
            preview_win = newwin(pane_h, w3, pane_y, x3);
        }
    }

    if (top_win) leaveok(top_win, TRUE);
    if (top_win) idlok(top_win, FALSE);
    if (parent_win) {
        leaveok(parent_win, TRUE);
        idlok(parent_win, FALSE);
    }
    if (current_win) {
        leaveok(current_win, TRUE);
        idlok(current_win, FALSE);
        keypad(current_win, TRUE);
        nodelay(current_win, FALSE);
        wtimeout(current_win, -1);
    }
    if (preview_win) {
        leaveok(preview_win, TRUE);
        idlok(preview_win, FALSE);
    }
    if (status_win) {
        leaveok(status_win, TRUE);
        idlok(status_win, FALSE);
    }
}

static void draw_ui(void) {
    attrset(A_NORMAL);
    bkgdset(' ' | A_NORMAL);
    setup_windows();

    int h = LINES - 2;

    if (single_pane_mode) {
        draw_current_pane(current_win, COLS, h);
        wnoutrefresh(current_win);
    } else {
        int ph, pw, ch, cw, vh, vw;

        getmaxyx(parent_win, ph, pw);
        getmaxyx(current_win, ch, cw);
        getmaxyx(preview_win, vh, vw);

        draw_parent_pane(parent_win, pw, ph);
        draw_current_pane(current_win, cw, ch);
        draw_preview_pane(preview_win, vw, vh);

        wnoutrefresh(parent_win);
        wnoutrefresh(current_win);
        wnoutrefresh(preview_win);
    }

    draw_top_bar();
    wnoutrefresh(top_win);

    draw_status(status_win, COLS);
    wnoutrefresh(status_win);

    present_screen();
}

/* When the viewport advances, many middle-pane lines shift by one row.
 * ncurses otherwise turns that pattern into a terminal scrolling-region
 * command, which also moves the static left pane before repairing it.  Commit
 * one middle-pane row at a time so the optimizer cannot scroll shared rows. */
static void draw_scrolled_current_pane(WINDOW *win, int w, int h) {
    begin_screen_update();

    draw_top_bar();
    wnoutrefresh(top_win);
    doupdate();

    untouchwin(win);
    for (int row = 0; row < h; row++) {
        draw_current_row(win, w, row, top + row);
        wnoutrefresh(win);
        doupdate();
        untouchwin(win);
    }

    end_screen_update();
}

/* Cursor movement must not wait for removable-drive I/O.  Paint the list and
 * path immediately; the preview and metadata are refreshed after input has
 * been idle for a short time. */
static void draw_navigation_ui(void) {
    attrset(A_NORMAL);
    bkgdset(' ' | A_NORMAL);
    setup_windows();

    int ch, cw;
    getmaxyx(current_win, ch, cw);
    int old_top = top;
    adjust_current_view(ch);

    if (top != old_top) {
        draw_scrolled_current_pane(current_win, cw, ch);
        return;
    }

    draw_current_pane(current_win, cw, ch);
    wnoutrefresh(current_win);

    draw_top_bar();
    wnoutrefresh(top_win);
    present_screen();
}

static void draw_deferred_details(void) {
    attrset(A_NORMAL);
    bkgdset(' ' | A_NORMAL);
    setup_windows();

    if (!single_pane_mode && preview_win) {
        int vh, vw;
        getmaxyx(preview_win, vh, vw);
        draw_preview_pane(preview_win, vw, vh);
        wnoutrefresh(preview_win);
    }

    draw_status(status_win, COLS);
    wnoutrefresh(status_win);
    present_screen();
}

static void enter_dir(void) {
    if (entry_count == 0) return;
    if (!entries[cursor].is_dir) return;

    remember_current_cursor();

    char next[PATH_MAX];
    join_path(next, cwd_path, entries[cursor].name);

    if (chdir(next) == 0) {
        if (!getcwd(cwd_path, sizeof(cwd_path)))
            safe_copy(cwd_path, sizeof(cwd_path), "/");
        cursor = 0;
        top = 0;
        load_dir(cwd_path);
        restore_dir_cursor(cwd_path);
    }
}

__attribute__((unused)) static int nvim_file_type(const char *path) {
    const char *ext = strrchr(path, '.');

    if (!ext)
        return 0;

    return
        strcasecmp(ext, ".txt") == 0  ||
        strcasecmp(ext, ".md") == 0   ||
        strcasecmp(ext, ".markdown") == 0 ||
        strcasecmp(ext, ".c") == 0    ||
        strcasecmp(ext, ".h") == 0    ||
        strcasecmp(ext, ".cpp") == 0  ||
        strcasecmp(ext, ".hpp") == 0  ||
        strcasecmp(ext, ".cc") == 0   ||
        strcasecmp(ext, ".py") == 0   ||
        strcasecmp(ext, ".sh") == 0   ||
        strcasecmp(ext, ".conf") == 0 ||
        strcasecmp(ext, ".ini") == 0  ||
        strcasecmp(ext, ".log") == 0;
}

static void launch_file(void) {
    if (entry_count == 0)
        return;

    if (entries[cursor].is_dir) {
        enter_dir();
        return;
    }

    char full[PATH_MAX];
    join_path(full, cwd_path, entries[cursor].name);

    debug_log("launch file before def_prog_mode/endwin path=%s", full);
    def_prog_mode();
    endwin();

    pid_t pid = fork();

    if (pid == 0) {
        const char *ext = strrchr(full, '.');

        if (config_terminal_mode && (extension_in_text_list(full) || (!ext && !looks_binary(full)))) {

            if (config_editor_command[0]) {
                char cmd[PATH_MAX + 1024];
                snprintf(cmd,
                         sizeof(cmd),
                         "%s \"%s\"",
                         config_editor_command,
                         full);

                execl("/bin/sh",
                      "sh",
                      "-c",
                      cmd,
                      (char *)NULL);
            }

            execlp("nvim", "nvim", full, (char *)NULL);
            execlp("vim",  "vim",  full, (char *)NULL);
            execlp("vi",   "vi",   full, (char *)NULL);
            execlp("nano", "nano", full, (char *)NULL);

            _exit(127);
        }

        const char *rule = find_open_rule(full);

        if (rule) {
            char cmd[PATH_MAX + 1024];

            snprintf(cmd,
                     sizeof(cmd),
                     "%s \"%s\"",
                     rule,
                     full);

            execl("/bin/sh",
                  "sh",
                  "-c",
                  cmd,
                  (char *)NULL);

            _exit(127);
        }

        setsid();

        pid_t grandchild = fork();
        if (grandchild < 0)
            _exit(127);
        if (grandchild > 0)
            _exit(0);

        exec_default_open_detached(full);
    }

    debug_log("launch file child pid=%ld", (long)pid);

    if (pid > 0) {
        int status = 0;
        wait_for_child(pid, &status, "launch file");
    }

    if (!redraw_after_child("launch file"))
        return;

    if (pid < 0)
        set_message("launch failed");
    else
        set_message("returned");
}

static void go_parent(void) {
    if (strcmp(cwd_path, "/") == 0) return;

    remember_current_cursor();

    char old[PATH_MAX];
    strncpy(old, cwd_path, PATH_MAX - 1);
    old[PATH_MAX - 1] = '\0';

    char *base = strrchr(old, '/');
    char old_name[NAME_MAX + 1] = "";

    if (base) {
        strncpy(old_name, base + 1, NAME_MAX);
        old_name[NAME_MAX] = '\0';
    }

    if (chdir("..") == 0) {
        if (!getcwd(cwd_path, sizeof(cwd_path)))
            safe_copy(cwd_path, sizeof(cwd_path), "/");
        load_dir(cwd_path);

        for (int i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].name, old_name) == 0) {
                cursor = i;
                break;
            }
        }

        top = cursor - 5;
        if (top < 0) top = 0;
    }
}




static void search_step(int direction) {
    if (search_len <= 0) {
        set_message("no search");
        return;
    }

    if (entry_count <= 0) {
        set_message("nothing to search");
        return;
    }

    for (int step = 1; step <= entry_count; step++) {
        int idx = cursor + (direction * step);

        while (idx < 0)
            idx += entry_count;

        idx %= entry_count;

        if (strcasestr(entries[idx].name, search_query)) {
            cursor = idx;
            set_message("found");
            return;
        }
    }

    set_message("not found");
}

static void execute_search(void) {
    if (search_len <= 0) {
        set_message("search canceled");
        return;
    }

    search_step(1);
}

static void handle_search_input(int ch) {
    if (ch == 27) {
        search_mode = 0;
        search_query[0] = '\0';
        search_len = 0;
        set_message("search canceled");
        return;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        search_mode = 0;
        execute_search();
        return;
    }

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (search_len > 0) {
            search_len--;
            search_query[search_len] = '\0';
        }
        return;
    }

    if (ch >= 32 && ch <= 126) {
        if (search_len < (int)sizeof(search_query) - 1) {
            search_query[search_len++] = (char)ch;
            search_query[search_len] = '\0';
        }
    }
}


static void launch_shell_here(void) {
    const char *shell = getenv("SHELL");
    if (!shell || shell[0] == '\0')
        shell = "/bin/sh";

    debug_log("shell before def_prog_mode/endwin shell=%s", shell);
    def_prog_mode();
    endwin();

    pid_t pid = fork();

    if (pid == 0) {
        if (chdir(cwd_path) != 0)
            _exit(127);

        printf("\n");
        printf("========================================\n");
        printf("  SIMPLEFILES SHELL\n");
        printf("  Type: exit\n");
        printf("  to return to Simplefiles\n");
        printf("========================================\n\n");
        fflush(stdout);

        execlp(shell, shell, (char *)NULL);
        _exit(127);
    }

    debug_log("shell child pid=%ld", (long)pid);

    if (pid > 0) {
        int status = 0;
        wait_for_child(pid, &status, "shell");
    }

    if (!redraw_after_child("shell"))
        return;

    if (pid < 0)
        set_message("shell failed");
    else
        set_message("returned from shell");
}

static void handle_normal_input(int ch) {
    switch (ch) {
        case KEY_UP:
        case 'k':
            if (cursor > 0) cursor--;
            break;

        case KEY_DOWN:
        case 'j':
            if (cursor < entry_count - 1) cursor++;
            break;

        case KEY_NPAGE:
            cursor += 10;
            if (cursor >= entry_count) cursor = entry_count - 1;
            break;

        case KEY_PPAGE:
            cursor -= 10;
            if (cursor < 0) cursor = 0;
            break;

        case 'h':
        case KEY_LEFT:
            go_parent();
            break;

        case 'l':
        case KEY_RIGHT:
        case '\n':
            launch_file();
            break;

        case ' ':
            if (entry_count > 0) {
                char full[PATH_MAX];
                join_path(full, cwd_path, entries[cursor].name);
                toggle_selected(full);

                if (cursor < entry_count - 1)
                    cursor++;
            }
            break;

        case 'v':
            invert_selection();
            break;

        case 'V':
            clear_selected();
            break;

        case 'y':
        case 'd':
        case 'p':
        case 'c':
            pending_key = ch;
            break;

        case ':':
            start_command("");
            break;

        case 'o':
            start_command("openwith ");
            break;

        case 't':
            launch_shell_here();
            break;

        case '/':
            start_search();
            break;

        case 'n':
            search_step(1);
            break;

        case 'N':
            search_step(-1);
            break;

        case '.':
            show_hidden = !show_hidden;
            load_dir(cwd_path);
            break;

        case 'q':
            exit_reason = "q";
            running = 0;
            break;
    }
}

static void handle_command_input(int ch) {
    if (ch == 27) {
        command_mode = 0;
        command[0] = '\0';
        command_len = 0;
        set_message("command canceled");
        return;
    }

    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        char cmd[sizeof(command)];
        strncpy(cmd, command, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';

        command_mode = 0;
        command[0] = '\0';
        command_len = 0;

        execute_command(cmd);
        return;
    }

    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (command_len > 0) {
            command_len--;
            command[command_len] = '\0';
        }
        return;
    }

    if (ch >= 32 && ch <= 126) {
        if (command_len < (int)sizeof(command) - 1) {
            command[command_len++] = (char)ch;
            command[command_len] = '\0';
        }
    }
}

static void handle_input(int ch) {
    if (command_mode) {
        handle_command_input(ch);
        return;
    }

    if (search_mode) {
        handle_search_input(ch);
        return;
    }

    if (ch == 'a') {
        start_command("mkdir ");
        return;
    }

    if (pending_empty_trash) {
        pending_empty_trash = 0;

        if (ch == 'y')
            empty_trash_now();
        else
            set_message("empty trash canceled");

        return;
    }

    if (pending_delete) {
        pending_delete = 0;

        if (ch == 'y')
            confirm_delete();
        else
            set_message("delete canceled");

        return;
    }

    if (pending_key) {
        int old_pending = pending_key;
        pending_key = 0;

        if (old_pending == 'y' && ch == 'y') {
            yank_or_cut('y');
            return;
        }

        if (old_pending == 'd' && ch == 'd') {
            yank_or_cut('d');
            return;
        }

        if (old_pending == 'd' && ch == 'D') {
            arm_delete();
            return;
        }

        if (old_pending == 'p' && ch == 'p') {
            paste_clipboard();
            return;
        }

        if (old_pending == 'c' && ch == 'w') {
            start_rename_command();
            return;
        }

        handle_normal_input(ch);
        return;
    }

    handle_normal_input(ch);
}

int main(int argc, char **argv) {
    int curses_started = 0;
    int consecutive_errors = 0;
    int details_pending = 0;
    int lock_result;

    (void)argc;
    setlocale(LC_ALL, "");

    if (!getcwd(cwd_path, sizeof(cwd_path)))
        safe_copy(cwd_path, sizeof(cwd_path), "/");

    load_config();

    if (config_start_dir[0]) {
        char tmp[PATH_MAX];
        expand_path(tmp, config_start_dir);

        if (chdir(tmp) == 0)
            if (!getcwd(cwd_path, sizeof(cwd_path)))
            safe_copy(cwd_path, sizeof(cwd_path), "/");
    }

    start_debug_log(argv[0]);

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        exit_reason = "non-tty startup";
        debug_log("exit reason=%s", exit_reason);
        fprintf(stderr, "simplefiles: refusing to run without a terminal on stdin and stdout\n");
        if (debug_file)
            fclose(debug_file);
        return 1;
    }

    lock_result = acquire_instance_lock();
    if (lock_result <= 0) {
        exit_reason = lock_result < 0 ? "same tty instance" : "instance lock error";
        debug_log("exit reason=%s", exit_reason);
        fprintf(stderr, "simplefiles: %s\n",
                lock_result < 0 ? "already running on this terminal" :
                                  "could not create same-terminal instance lock");
        if (debug_file)
            fclose(debug_file);
        return 1;
    }

    initscr();
    debug_log("after initscr");
    curses_started = 1;
    if (!install_signal_handlers()) {
        exit_reason = "signal setup failed";
        running = 0;
    }
    clearok(stdscr, FALSE);
    noecho();
    cbreak();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, FALSE);
    wtimeout(stdscr, -1);  // block forever until keypress

    if (has_colors()) {
        start_color();
        use_default_colors();
    }

    curs_set(0);
    leaveok(stdscr, TRUE);

    load_dir(cwd_path);
    debug_log("after load_dir");

    debug_log("before initial draw_ui");
    clearok(stdscr, TRUE);
    draw_ui();
    debug_log("after initial draw_ui");

    int ch;
    int first_getch = 1;

    while (running && !stop_requested) {
        if (check_background_paste()) {
            details_pending = 0;
            draw_ui();
        }

        if (!terminal_is_available()) {
            exit_reason = "lost tty";
            break;
        }

        if (details_pending)
            wtimeout(current_win, DETAIL_REDRAW_DELAY_MS);
        else if (paste_worker_pid > 0)
            wtimeout(current_win, 100);
        else
            wtimeout(current_win, -1);

        if (first_getch) {
            debug_log("before first getch");
            first_getch = 0;
        }
        ch = wgetch(current_win);

        if (ch == ERR) {
            if (details_pending) {
                details_pending = 0;
                wtimeout(current_win, -1);
                draw_deferred_details();
                continue;
            }
            if (stop_requested) {
                exit_reason = "signal";
                break;
            }
            if (!terminal_is_available()) {
                exit_reason = "lost tty";
                break;
            }
            if (paste_worker_pid > 0) {
                consecutive_errors = 0;
                if (status_win) {
                    draw_status(status_win, COLS);
                    wnoutrefresh(status_win);
                    present_screen();
                }
                continue;
            }
            consecutive_errors++;
            if (consecutive_errors >= 20) {
                exit_reason = "getch ERR storm";
                break;
            }
            napms(25);
            continue;
        }
        consecutive_errors = 0;

        if (ch == KEY_RESIZE) {
            details_pending = 0;
            destroy_windows();
            last_lines = 0;
            last_cols = 0;
            clear();
            clearok(stdscr, TRUE);
            draw_ui();
            continue;
        }

        int old_cursor = cursor;
        int old_selected_count = selected_count;
        char old_cwd[PATH_MAX];
        safe_copy(old_cwd, sizeof(old_cwd), cwd_path);
        int scroll_key = !command_mode && !search_mode && !pending_delete &&
                         !pending_empty_trash && !pending_key &&
                         (ch == KEY_UP || ch == KEY_DOWN ||
                          ch == KEY_NPAGE || ch == KEY_PPAGE ||
                          ch == 'j' || ch == 'k');

        handle_input(ch);

        if (stop_requested) {
            exit_reason = "signal";
            break;
        }
        if (running) {
            int same_directory = strcmp(old_cwd, cwd_path) == 0;
            int list_interaction = cursor != old_cursor ||
                                   selected_count != old_selected_count ||
                                   scroll_key;

            if (same_directory && list_interaction) {
                draw_navigation_ui();
                details_pending = 1;
                wtimeout(current_win, DETAIL_REDRAW_DELAY_MS);
            } else {
                details_pending = 0;
                wtimeout(current_win, -1);
                draw_ui();
            }
        }
    }

    if (stop_requested)
        exit_reason = "signal";
    if (curses_started) {
        destroy_windows();
        endwin();
    }
    if (paste_result_fd >= 0)
        close(paste_result_fd);
    debug_log("exit reason=%s", exit_reason);
    release_instance_lock();
    if (debug_file)
        fclose(debug_file);
    return 0;
}
