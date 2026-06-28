#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define STATE_DIR ".local/state/simpleclock"
#define ALARM_FILE "alarm"
#define WORKER_FILE "worker"
#define REMINDERS_FILE "reminders"
#define CHECK_LOCK_FILE "check.lock"
#define ALARM_PID_FILE "alarm.pid"
#define LEGACY_ALARM_FILE ".simpleclock-alarm"
#define LEGACY_WORKER_FILE ".simpleclock-alarm-worker"
#define LEGACY_ALARM_TEMP_FILE ".simpleclock-alarm.tmp"

#define CLOCK_ID_LEN 64
#define CLOCK_KIND_LEN 16
#define CLOCK_STATUS_LEN 16
#define CLOCK_TIMESTAMP_LEN 20
#define CLOCK_FIRED_ON_LEN 128

typedef enum {
    ALARM_START_FAILED = 0,
    ALARM_START_RUNNING = 1,
    ALARM_START_EXITED_OK = 2
} AlarmStartResult;

typedef struct {
    char id[CLOCK_ID_LEN];
    char kind[CLOCK_KIND_LEN];
    char due[CLOCK_TIMESTAMP_LEN];
    char status[CLOCK_STATUS_LEN];
    char fired_at[CLOCK_TIMESTAMP_LEN];
    char fired_on[CLOCK_FIRED_ON_LEN];
} ClockReminder;

typedef struct {
    ClockReminder *items;
    size_t len;
    size_t cap;
} ReminderList;

static bool write_clock_reminder(const char *id, const char *kind, time_t due);
static int install_reminders(int quiet);

static bool home_path(char *path, size_t size, const char *suffix) {
    const char *home = getenv("HOME");
    int written;

    if (!home || !*home || !suffix || !*suffix) {
        return false;
    }

    written = snprintf(path, size, "%s/%s", home, suffix);
    return written > 0 && (size_t)written < size;
}

static bool ensure_dir(const char *path) {
    struct stat st;

    if (!path || !*path) {
        return false;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path, 0700) == 0) {
        return true;
    }
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdirs(const char *path) {
    char tmp[4096];
    size_t len;

    if (!path || !*path) {
        return false;
    }

    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len >= sizeof tmp) {
        return false;
    }
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!ensure_dir(tmp)) {
                return false;
            }
            *p = '/';
        }
    }

    return ensure_dir(tmp);
}

static bool state_dir(char *path, size_t size) {
    if (!home_path(path, size, STATE_DIR)) {
        return false;
    }
    return mkdirs(path);
}

static bool state_path(char *path, size_t size, const char *name) {
    char dir[4096];
    int written;

    if (!state_dir(dir, sizeof dir)) {
        return false;
    }
    written = snprintf(path, size, "%s/%s", dir, name);
    return written > 0 && (size_t)written < size;
}

static bool file_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

static bool snprintf_ok(int written, size_t size) {
    return written >= 0 && (size_t)written < size;
}

static void trim(char *s) {
    size_t len;

    if (!s) {
        return;
    }
    while (isspace((unsigned char)*s)) {
        memmove(s, s + 1, strlen(s));
    }
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void clean_field(char *s) {
    if (!s) {
        return;
    }
    for (char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n' || (unsigned char)*p < 32) {
            *p = ' ';
        }
    }
    trim(s);
}

static void copy_field(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) {
        return;
    }
    snprintf(dst, size, "%s", src ? src : "");
    clean_field(dst);
}

static bool atomic_open_temp(const char *path, char *tmp, size_t tmp_size, FILE **out) {
    int written = snprintf(tmp, tmp_size, "%s.tmp.%ld", path, (long)getpid());

    if (!snprintf_ok(written, tmp_size)) {
        return false;
    }
    *out = fopen(tmp, "w");
    return *out != NULL;
}

static bool finish_atomic_write(FILE *file, const char *tmp, const char *path) {
    bool ok = true;

    if (fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (ok && rename(tmp, path) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
    }
    return ok;
}

static bool reminders_path(char *path, size_t size) {
    return state_path(path, size, REMINDERS_FILE);
}

static bool check_lock_path(char *path, size_t size) {
    return state_path(path, size, CHECK_LOCK_FILE);
}

static bool alarm_pid_path(char *path, size_t size) {
    return state_path(path, size, ALARM_PID_FILE);
}

static bool alarm_media_path(char *path, size_t size) {
    return home_path(path, size, ".local/share/simplesuite/simplecal-alarm.mp3");
}

static int reminderlist_push(ReminderList *list, ClockReminder reminder) {
    ClockReminder *items;
    size_t newcap;

    if (list->len == list->cap) {
        newcap = list->cap ? list->cap * 2 : 8;
        items = realloc(list->items, newcap * sizeof *items);
        if (!items) {
            return 0;
        }
        list->items = items;
        list->cap = newcap;
    }
    list->items[list->len++] = reminder;
    return 1;
}

static void reminderlist_free(ReminderList *list) {
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void parse_reminder_line(ClockReminder *reminder, const char *key, const char *value) {
    if (!strcmp(key, "ID")) {
        copy_field(reminder->id, sizeof reminder->id, value);
    } else if (!strcmp(key, "KIND")) {
        copy_field(reminder->kind, sizeof reminder->kind, value);
    } else if (!strcmp(key, "DUE")) {
        copy_field(reminder->due, sizeof reminder->due, value);
    } else if (!strcmp(key, "STATUS")) {
        copy_field(reminder->status, sizeof reminder->status, value);
    } else if (!strcmp(key, "FIRED_AT")) {
        copy_field(reminder->fired_at, sizeof reminder->fired_at, value);
    } else if (!strcmp(key, "FIRED_ON")) {
        copy_field(reminder->fired_on, sizeof reminder->fired_on, value);
    }
}

static int reminder_status_valid(const char *status) {
    return !strcmp(status, "pending") ||
           !strcmp(status, "ringing") ||
           !strcmp(status, "fired") ||
           !strcmp(status, "missed") ||
           !strcmp(status, "error");
}

static int reminder_valid(const ClockReminder *r) {
    return r->id[0] && r->kind[0] && r->due[0] && reminder_status_valid(r->status) &&
           (!strcmp(r->kind, "timer") || !strcmp(r->kind, "alarm"));
}

static int load_reminders(ReminderList *list) {
    char path[4096];
    FILE *file;
    char line[1024];
    ClockReminder current;
    int have = 0;

    memset(list, 0, sizeof *list);
    if (!reminders_path(path, sizeof path)) {
        return 0;
    }
    file = fopen(path, "r");
    if (!file) {
        return errno == ENOENT;
    }

    memset(&current, 0, sizeof current);
    while (fgets(line, sizeof line, file)) {
        char *eq;
        char *key;
        char *value;

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            if (have) {
                if (!current.status[0]) {
                    snprintf(current.status, sizeof current.status, "pending");
                }
                if (reminder_valid(&current) && !reminderlist_push(list, current)) {
                    fclose(file);
                    reminderlist_free(list);
                    return 0;
                }
            }
            memset(&current, 0, sizeof current);
            have = 0;
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        key = line;
        value = eq + 1;
        trim(key);
        parse_reminder_line(&current, key, value);
        have = 1;
    }

    if (have) {
        if (!current.status[0]) {
            snprintf(current.status, sizeof current.status, "pending");
        }
        if (reminder_valid(&current) && !reminderlist_push(list, current)) {
            fclose(file);
            reminderlist_free(list);
            return 0;
        }
    }

    fclose(file);
    return 1;
}

static int write_reminders(ReminderList *list) {
    char path[4096];
    char tmp[4096 + 64];
    FILE *file;

    if (!reminders_path(path, sizeof path)) {
        return 0;
    }
    if (!atomic_open_temp(path, tmp, sizeof tmp, &file)) {
        return 0;
    }

    for (size_t i = 0; i < list->len; i++) {
        ClockReminder *r = &list->items[i];

        if (!reminder_valid(r)) {
            continue;
        }
        if (fprintf(file,
                    "ID=%s\n"
                    "KIND=%s\n"
                    "DUE=%s\n"
                    "STATUS=%s\n"
                    "FIRED_AT=%s\n"
                    "FIRED_ON=%s\n\n",
                    r->id, r->kind, r->due, r->status,
                    r->fired_at, r->fired_on) < 0) {
            fclose(file);
            unlink(tmp);
            return 0;
        }
    }

    return finish_atomic_write(file, tmp, path);
}

static int find_reminder_id(ReminderList *list, const char *id) {
    for (size_t i = 0; i < list->len; i++) {
        if (!strcmp(list->items[i].id, id)) {
            return (int)i;
        }
    }
    return -1;
}

static time_t parse_due_time(const char *due) {
    struct tm tmv;
    int y, mo, d, h, mi;
    int sec = 0;
    char tail;

    if (!due) {
        return (time_t)-1;
    }
    if (sscanf(due, "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &sec, &tail) != 6) {
        if (sscanf(due, "%4d-%2d-%2dT%2d:%2d%c", &y, &mo, &d, &h, &mi, &tail) != 5) {
            return (time_t)-1;
        }
        sec = 0;
    }
    if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) {
        return (time_t)-1;
    }

    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

static void format_timestamp(time_t value, char *out, size_t size) {
    struct tm *tmv = localtime(&value);

    if (!out || size == 0) {
        return;
    }
    if (tmv) {
        strftime(out, size, "%Y-%m-%dT%H:%M:%S", tmv);
    } else {
        snprintf(out, size, "unknown");
    }
}

static void set_fired_fields(ClockReminder *r, time_t now) {
    char host[96] = "unknown";

    format_timestamp(now, r->fired_at, sizeof r->fired_at);
    gethostname(host, sizeof host);
    host[sizeof(host) - 1] = '\0';
    snprintf(r->fired_on, sizeof r->fired_on, "%s", host);
}

static int reminder_is_ringing(const ClockReminder *r) {
    return !strcmp(r->status, "ringing");
}

static int reminder_is_clearable(const ClockReminder *r, time_t now) {
    time_t due;

    if (!strcmp(r->status, "ringing") ||
        !strcmp(r->status, "error") ||
        !strcmp(r->status, "missed")) {
        return 1;
    }

    due = parse_due_time(r->due);
    return !strcmp(r->status, "pending") && due != (time_t)-1 && due <= now;
}

static int count_ringing_reminders_list(const ReminderList *list) {
    int count = 0;

    for (size_t i = 0; i < list->len; i++) {
        if (reminder_is_ringing(&list->items[i])) {
            count++;
        }
    }
    return count;
}

static int active_reminder_kind_exists(const char *kind) {
    ReminderList reminders = {0};
    int found = 0;

    if (!load_reminders(&reminders)) {
        return 0;
    }
    for (size_t i = 0; i < reminders.len; i++) {
        ClockReminder *r = &reminders.items[i];

        if (!strcmp(r->kind, kind) && strcmp(r->status, "fired")) {
            found = 1;
            break;
        }
    }
    reminderlist_free(&reminders);
    return found;
}

static bool write_clock_reminder(const char *id, const char *kind, time_t due) {
    ReminderList reminders = {0};
    ClockReminder reminder;
    int index;
    int ok;

    if (!id || !kind || due <= 0 || !load_reminders(&reminders)) {
        return false;
    }

    index = find_reminder_id(&reminders, id);
    memset(&reminder, 0, sizeof reminder);
    snprintf(reminder.id, sizeof reminder.id, "%s", id);
    snprintf(reminder.kind, sizeof reminder.kind, "%s", kind);
    format_timestamp(due, reminder.due, sizeof reminder.due);
    snprintf(reminder.status, sizeof reminder.status, "pending");

    if (index >= 0) {
        reminders.items[index] = reminder;
    } else if (!reminderlist_push(&reminders, reminder)) {
        reminderlist_free(&reminders);
        return false;
    }

    ok = write_reminders(&reminders);
    reminderlist_free(&reminders);
    return ok != 0;
}

static bool remove_clock_reminder_kind(const char *kind) {
    ReminderList reminders = {0};
    size_t out = 0;
    int ok;

    if (!load_reminders(&reminders)) {
        return false;
    }
    for (size_t i = 0; i < reminders.len; i++) {
        ClockReminder r = reminders.items[i];

        if (strcmp(r.kind, kind) || !strcmp(r.status, "fired")) {
            reminders.items[out++] = r;
        }
    }
    reminders.len = out;
    ok = write_reminders(&reminders);
    reminderlist_free(&reminders);
    return ok != 0;
}

static bool clear_clock_scheduled_reminders(void) {
    ReminderList reminders = {0};
    time_t now = time(NULL);
    size_t out = 0;
    int ok;

    if (!load_reminders(&reminders)) {
        return false;
    }
    for (size_t i = 0; i < reminders.len; i++) {
        ClockReminder r = reminders.items[i];
        int is_clock = !strcmp(r.kind, "timer") || !strcmp(r.kind, "alarm");

        if (!is_clock) {
            reminders.items[out++] = r;
            continue;
        }
        if (reminder_is_clearable(&r, now)) {
            snprintf(r.status, sizeof r.status, "fired");
            set_fired_fields(&r, now);
            reminders.items[out++] = r;
        } else if (!strcmp(r.status, "fired")) {
            reminders.items[out++] = r;
        }
    }
    reminders.len = out;
    ok = write_reminders(&reminders);
    reminderlist_free(&reminders);
    return ok != 0;
}

static int command_exists(const char *cmd) {
    char command[256];
    int rc;

    snprintf(command, sizeof command, "command -v %s >/dev/null 2>&1", cmd);
    rc = system(command);
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static void shell_quote(const char *src, char *dst, size_t size) {
    size_t j = 0;

    if (size == 0) {
        return;
    }
    dst[j++] = '\'';
    for (size_t i = 0; src && src[i] && j + 5 < size; i++) {
        if (src[i] == '\'') {
            memcpy(dst + j, "'\\''", 4);
            j += 4;
        } else {
            dst[j++] = src[i];
        }
    }
    if (j + 1 < size) {
        dst[j++] = '\'';
    }
    dst[j] = '\0';
}

static int alarm_debug_enabled(void) {
    const char *v = getenv("SIMPLECLOCK_ALARM_DEBUG");

    return v && *v && strcmp(v, "0") && strcmp(v, "false") && strcmp(v, "no");
}

static int process_alive(pid_t pid) {
    if (pid <= 1) {
        return 0;
    }
    if (kill(pid, 0) == 0) {
        return 1;
    }
    return errno == EPERM;
}

static int write_alarm_pid(pid_t pid) {
    char path[4096];
    FILE *file;

    if (!alarm_pid_path(path, sizeof path)) {
        return 0;
    }
    file = fopen(path, "w");
    if (!file) {
        return 0;
    }
    fprintf(file, "PID=%ld\n", (long)pid);
    return fclose(file) == 0;
}

static int read_alarm_pid(pid_t *pid) {
    char path[4096];
    FILE *file;
    char line[128];
    long value = -1;

    if (pid) {
        *pid = -1;
    }
    if (!alarm_pid_path(path, sizeof path)) {
        return 0;
    }
    file = fopen(path, "r");
    if (!file) {
        return 0;
    }
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "PID=%ld", &value) == 1 && value > 1) {
            fclose(file);
            if (pid) {
                *pid = (pid_t)value;
            }
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static void remove_alarm_pid(void) {
    char path[4096];

    if (alarm_pid_path(path, sizeof path)) {
        unlink(path);
    }
}

static int poll_alarm_exit(pid_t pid, const char *label, int *exit_ok, int quiet) {
    int status = 0;
    pid_t rc;

    if (exit_ok) {
        *exit_ok = 0;
    }
    do {
        rc = waitpid(pid, &status, WNOHANG);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        if (errno == ECHILD && process_alive(pid)) {
            return 0;
        }
        if (!quiet) {
            fprintf(stderr, "simpleclock: %s PID %ld is no longer running (%s)\n",
                    label, (long)pid, strerror(errno));
        }
        return 1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);

        if (!quiet) {
            fprintf(stderr, "simpleclock: %s PID %ld exit status: %d\n",
                    label, (long)pid, code);
        }
        if (exit_ok) {
            *exit_ok = code == 0;
        }
    } else if (WIFSIGNALED(status)) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: %s PID %ld terminated by signal: %d\n",
                    label, (long)pid, WTERMSIG(status));
        }
    }
    return 1;
}

static int alarm_player_running(pid_t *pid_out, int quiet) {
    pid_t pid;
    int exit_ok = 0;

    if (pid_out) {
        *pid_out = -1;
    }
    if (!read_alarm_pid(&pid)) {
        return 0;
    }
    if (poll_alarm_exit(pid, "alarm player", &exit_ok, quiet)) {
        (void)exit_ok;
        remove_alarm_pid();
        return 0;
    }
    if (!process_alive(pid)) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: alarm player PID %ld is stale\n", (long)pid);
        }
        remove_alarm_pid();
        return 0;
    }
    if (pid_out) {
        *pid_out = pid;
    }
    return 1;
}

static void stop_alarm_player(int quiet) {
    pid_t pid;

    if (!read_alarm_pid(&pid)) {
        return;
    }
    if (process_alive(pid)) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: stopping alarm player PID=%ld\n", (long)pid);
        }
        if (kill(-pid, SIGTERM) != 0 && errno == ESRCH) {
            kill(pid, SIGTERM);
        }
    }
    remove_alarm_pid();
}

static void redirect_child_stdio_to_devnull(void) {
    int fd = open("/dev/null", O_RDWR);

    if (fd < 0) {
        return;
    }
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) {
        close(fd);
    }
}

static AlarmStartResult start_alarm_argv(const char *label, char *const argv[],
                                         pid_t *pid_out, int quiet) {
    pid_t pid;
    int errpipe[2];
    int flags;
    int child_errno = 0;
    ssize_t nread;
    struct timespec delay = { 0, 20000000L };

    if (pid_out) {
        *pid_out = -1;
    }
    if (pipe(errpipe) != 0) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: could not create alarm exec pipe for %s: %s\n",
                    label, strerror(errno));
        }
        return ALARM_START_FAILED;
    }
    flags = fcntl(errpipe[1], F_GETFD);
    if (flags >= 0) {
        fcntl(errpipe[1], F_SETFD, flags | FD_CLOEXEC);
    }

    if (alarm_debug_enabled()) {
        fprintf(stderr, "simpleclock: trying alarm player: %s\n", label);
    }
    pid = fork();
    if (pid == 0) {
        close(errpipe[0]);
        setpgid(0, 0);
        redirect_child_stdio_to_devnull();
        execvp(argv[0], argv);
        child_errno = errno;
        {
            ssize_t ignored = write(errpipe[1], &child_errno, sizeof child_errno);
            (void)ignored;
        }
        _exit(127);
    }
    if (pid < 0) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: could not fork for %s: %s\n", label, strerror(errno));
        }
        close(errpipe[0]);
        close(errpipe[1]);
        return ALARM_START_FAILED;
    }

    close(errpipe[1]);
    nread = read(errpipe[0], &child_errno, sizeof child_errno);
    close(errpipe[0]);
    if (nread > 0) {
        int status = 0;

        if (!quiet) {
            fprintf(stderr, "simpleclock: exec failed for %s: %s\n",
                    argv[0], strerror(child_errno));
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return ALARM_START_FAILED;
    }

    if (!write_alarm_pid(pid) && !quiet) {
        fprintf(stderr, "simpleclock: could not write alarm PID file\n");
    }
    if (alarm_debug_enabled()) {
        fprintf(stderr, "simpleclock: started alarm player PID=%ld (%s)\n", (long)pid, label);
    }

    for (int i = 0; i < 10; i++) {
        int exit_ok = 0;

        if (poll_alarm_exit(pid, label, &exit_ok, 1)) {
            remove_alarm_pid();
            return exit_ok ? ALARM_START_EXITED_OK : ALARM_START_FAILED;
        }
        nanosleep(&delay, NULL);
    }

    if (pid_out) {
        *pid_out = pid;
    }
    return ALARM_START_RUNNING;
}

static AlarmStartResult start_alarm_shell_player(const char *player, const char *path,
                                                 pid_t *pid_out, int quiet) {
    char quoted_path[4096 + 16];
    char command[4096 * 2];
    char label[512];
    char *argv[] = { "sh", "-c", command, NULL };

    shell_quote(path, quoted_path, sizeof quoted_path);
    if (!snprintf_ok(snprintf(command, sizeof command, "exec %s %s", player, quoted_path),
                     sizeof command)) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: SIMPLECLOCK_ALARM_PLAYER command is too long\n");
        }
        return ALARM_START_FAILED;
    }
    snprintf(label, sizeof label, "SIMPLECLOCK_ALARM_PLAYER=%s", player);
    return start_alarm_argv(label, argv, pid_out, quiet);
}

static void ensure_alarm_audio_environment(void) {
    char runtime_dir[128];
    char bus[192];
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");

    if (!xdg_runtime || !*xdg_runtime) {
        snprintf(runtime_dir, sizeof runtime_dir, "/run/user/%ld", (long)getuid());
        setenv("XDG_RUNTIME_DIR", runtime_dir, 0);
        xdg_runtime = getenv("XDG_RUNTIME_DIR");
    }

    if ((!getenv("DBUS_SESSION_BUS_ADDRESS") || !*getenv("DBUS_SESSION_BUS_ADDRESS")) &&
        xdg_runtime && *xdg_runtime) {
        snprintf(bus, sizeof bus, "unix:path=%s/bus", xdg_runtime);
        setenv("DBUS_SESSION_BUS_ADDRESS", bus, 0);
    }
}

static int ensure_alarm_player_running(pid_t *pid_out, int quiet) {
    char path[4096];
    const char *custom_player = getenv("SIMPLECLOCK_ALARM_PLAYER");
    int have_alarm_file = 0;
    int tried = 0;
    pid_t pid = -1;
    AlarmStartResult result;

    if (!custom_player || !*custom_player) {
        custom_player = getenv("SIMPLECAL_ALARM_PLAYER");
    }
    if (alarm_player_running(&pid, 1)) {
        if (pid_out) {
            *pid_out = pid;
        }
        return 1;
    }

    have_alarm_file = alarm_media_path(path, sizeof path) && access(path, R_OK) == 0;
    ensure_alarm_audio_environment();

    if (custom_player && *custom_player) {
        if (!have_alarm_file) {
            if (!quiet) {
                fprintf(stderr, "simpleclock: alarm MP3 missing; custom alarm player needs a path\n");
            }
            return 0;
        }
        result = start_alarm_shell_player(custom_player, path, pid_out, quiet);
        return result != ALARM_START_FAILED;
    }

    if (have_alarm_file && command_exists("mpv")) {
        char *argv[] = {
            "mpv", "--no-config", "--no-video", "--audio-display=no",
            "--audio-device=pipewire", "--really-quiet",
            "--volume=100", path, NULL
        };

        tried = 1;
        result = start_alarm_argv("mpv pipewire", argv, pid_out, quiet);
        if (result != ALARM_START_FAILED) {
            return 1;
        }
        {
            char *pulse_argv[] = {
                "mpv", "--no-config", "--no-video", "--audio-display=no",
                "--audio-device=pulse", "--really-quiet",
                "--volume=100", path, NULL
            };

            result = start_alarm_argv("mpv pulse", pulse_argv, pid_out, quiet);
            if (result != ALARM_START_FAILED) {
                return 1;
            }
        }
        {
            char *auto_argv[] = {
                "mpv", "--no-config", "--no-video", "--audio-display=no",
                "--really-quiet", "--volume=100", path, NULL
            };

            result = start_alarm_argv("mpv auto", auto_argv, pid_out, quiet);
            if (result != ALARM_START_FAILED) {
                return 1;
            }
        }
    }

    if (have_alarm_file && command_exists("pw-play")) {
        char *argv[] = { "pw-play", path, NULL };

        tried = 1;
        result = start_alarm_argv("pw-play", argv, pid_out, quiet);
        if (result != ALARM_START_FAILED) {
            return 1;
        }
    }

    if (have_alarm_file && command_exists("paplay")) {
        char *argv[] = { "paplay", path, NULL };

        tried = 1;
        result = start_alarm_argv("paplay", argv, pid_out, quiet);
        if (result != ALARM_START_FAILED) {
            return 1;
        }
    }

    if (have_alarm_file && command_exists("ffplay")) {
        char *argv[] = { "ffplay", "-nodisp", "-autoexit", path, NULL };

        tried = 1;
        result = start_alarm_argv("ffplay", argv, pid_out, quiet);
        if (result != ALARM_START_FAILED) {
            return 1;
        }
    }

    if (command_exists("speaker-test")) {
        char *argv[] = { "speaker-test", "-t", "sine", "-f", "523", NULL };

        tried = 1;
        result = start_alarm_argv("speaker-test", argv, pid_out, quiet);
        if (result != ALARM_START_FAILED) {
            return 1;
        }
    }

    if (!quiet) {
        if (!tried) {
            fprintf(stderr, "simpleclock: no alarm player found\n");
        } else {
            fprintf(stderr, "simpleclock: all alarm players failed\n");
        }
    }
    return 0;
}

static int acquire_check_lock(int *fd_out, int quiet) {
    char path[4096];
    int fd;

    if (fd_out) {
        *fd_out = -1;
    }
    if (!check_lock_path(path, sizeof path)) {
        return -1;
    }
    fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: could not open reminder lock: %s\n", strerror(errno));
        }
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (!quiet && errno != EWOULDBLOCK && errno != EAGAIN) {
            fprintf(stderr, "simpleclock: could not lock reminder checker: %s\n", strerror(errno));
        } else if (!quiet) {
            fprintf(stderr, "simpleclock: reminder checker already running\n");
        }
        close(fd);
        return 0;
    }
    if (fd_out) {
        *fd_out = fd;
    }
    return 1;
}

static void mark_ringing_reminders_error(ReminderList *reminders) {
    for (size_t i = 0; i < reminders->len; i++) {
        ClockReminder *r = &reminders->items[i];

        if (reminder_is_ringing(r)) {
            snprintf(r->status, sizeof r->status, "error");
        }
    }
}

static int process_due_reminders_once(int start_player, int quiet) {
    ReminderList reminders = {0};
    time_t now = time(NULL);
    int changed = 0;
    int ringing_count;

    if (!load_reminders(&reminders)) {
        return 1;
    }

    for (size_t i = 0; i < reminders.len; i++) {
        ClockReminder *r = &reminders.items[i];
        time_t due;

        if (strcmp(r->status, "pending")) {
            continue;
        }
        due = parse_due_time(r->due);
        if (due == (time_t)-1 || due > now) {
            continue;
        }

        snprintf(r->status, sizeof r->status, "ringing");
        r->fired_at[0] = '\0';
        r->fired_on[0] = '\0';
        changed = 1;
    }

    ringing_count = count_ringing_reminders_list(&reminders);
    if (changed && !write_reminders(&reminders)) {
        reminderlist_free(&reminders);
        return 1;
    }

    if (ringing_count > 0 && start_player) {
        pid_t player_pid = -1;

        if (!ensure_alarm_player_running(&player_pid, quiet) && !quiet) {
            fprintf(stderr, "simpleclock: alarm playback failed while %d reminder(s) ringing\n",
                    ringing_count);
        }
    }

    reminderlist_free(&reminders);
    return 0;
}

static void ui_check_due_reminders(void) {
    static time_t last_tick = 0;
    time_t now = time(NULL);
    int lock_fd = -1;
    int lock_state;

    if (now == last_tick) {
        return;
    }
    last_tick = now;

    lock_state = acquire_check_lock(&lock_fd, 1);
    if (lock_state <= 0) {
        return;
    }
    process_due_reminders_once(1, 1);
    if (lock_fd >= 0) {
        close(lock_fd);
    }
}

static int check_reminders(void) {
    int lock_fd = -1;
    int lock_state;
    int consecutive_player_failures = 0;
    int exit_code = 0;

    lock_state = acquire_check_lock(&lock_fd, 0);
    if (lock_state == 0) {
        return 0;
    }
    if (lock_state < 0) {
        return 1;
    }

    for (;;) {
        ReminderList reminders = {0};
        int ringing_count;

        if (process_due_reminders_once(0, 0) != 0 || !load_reminders(&reminders)) {
            exit_code = 1;
            break;
        }

        ringing_count = count_ringing_reminders_list(&reminders);
        if (ringing_count <= 0) {
            stop_alarm_player(0);
            reminderlist_free(&reminders);
            break;
        }

        {
            pid_t player_pid = -1;

            if (ensure_alarm_player_running(&player_pid, 0)) {
                consecutive_player_failures = 0;
                if (alarm_debug_enabled()) {
                    fprintf(stderr, "simpleclock: ringing %d reminder(s); player PID=%ld\n",
                            ringing_count, (long)player_pid);
                }
            } else {
                consecutive_player_failures++;
                fprintf(stderr,
                        "simpleclock: alarm playback failed while %d reminder(s) ringing; failure %d\n",
                        ringing_count, consecutive_player_failures);
                if (consecutive_player_failures >= 3) {
                    mark_ringing_reminders_error(&reminders);
                    write_reminders(&reminders);
                    stop_alarm_player(0);
                    reminderlist_free(&reminders);
                    exit_code = 1;
                    break;
                }
            }
        }

        reminderlist_free(&reminders);
        sleep(1);
    }

    if (lock_fd >= 0) {
        close(lock_fd);
    }
    return exit_code;
}

static int clear_reminders(int *cleared_count, int quiet) {
    ReminderList reminders = {0};
    time_t now = time(NULL);
    int cleared = 0;
    int ok;

    if (cleared_count) {
        *cleared_count = 0;
    }
    if (!load_reminders(&reminders)) {
        return 1;
    }

    for (size_t i = 0; i < reminders.len; i++) {
        ClockReminder *r = &reminders.items[i];

        if (!reminder_is_clearable(r, now)) {
            continue;
        }
        snprintf(r->status, sizeof r->status, "fired");
        set_fired_fields(r, now);
        if (!quiet) {
            fprintf(stderr, "simpleclock: cleared reminder ID=%s KIND=%s at %s\n",
                    r->id, r->kind, r->fired_at);
        }
        cleared++;
    }

    ok = write_reminders(&reminders);
    if (cleared_count) {
        *cleared_count = cleared;
    }
    if (cleared > 0 && count_ringing_reminders_list(&reminders) == 0) {
        stop_alarm_player(quiet);
    }
    reminderlist_free(&reminders);

    if (!ok) {
        return 1;
    }
    if (!quiet && cleared == 0) {
        fprintf(stderr, "simpleclock: no ringing reminders to clear\n");
    }
    return 0;
}

static int run_command_ok(const char *cmd) {
    int rc = system(cmd);

    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static int run_command_ok_maybe_quiet(const char *cmd, int quiet) {
    char quiet_cmd[1024];

    if (!quiet) {
        return run_command_ok(cmd);
    }
    if (!snprintf_ok(snprintf(quiet_cmd, sizeof quiet_cmd, "%s >/dev/null 2>&1", cmd),
                     sizeof quiet_cmd)) {
        return 0;
    }
    return run_command_ok(quiet_cmd);
}

static int write_text_atomic(const char *path, const char *text) {
    char tmp[4096 + 64];
    FILE *file;

    if (!atomic_open_temp(path, tmp, sizeof tmp, &file)) {
        return 0;
    }
    if (fputs(text, file) == EOF) {
        fclose(file);
        unlink(tmp);
        return 0;
    }
    return finish_atomic_write(file, tmp, path);
}

static int install_systemd_reminders(int quiet) {
    char dir[4096];
    char service_path[4096];
    char timer_path[4096];
    const char *service_text =
        "[Unit]\n"
        "Description=SimpleClock reminder check\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "Environment=XDG_RUNTIME_DIR=%t\n"
        "Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=%t/bus\n"
        "PassEnvironment=PULSE_SERVER PIPEWIRE_REMOTE WAYLAND_DISPLAY DISPLAY XAUTHORITY SIMPLECLOCK_ALARM_PLAYER SIMPLECLOCK_ALARM_DEBUG SIMPLECAL_ALARM_PLAYER\n"
        "ExecStart=%h/.local/bin/simpleclock --check-reminders\n";
    const char *timer_text =
        "[Unit]\n"
        "Description=Run SimpleClock reminder alarms\n"
        "\n"
        "[Timer]\n"
        "OnBootSec=10s\n"
        "OnUnitActiveSec=1s\n"
        "AccuracySec=1s\n"
        "Persistent=true\n"
        "Unit=simpleclock-reminders.service\n"
        "\n"
        "[Install]\n"
        "WantedBy=timers.target\n";
    int written;

    if (!home_path(dir, sizeof dir, ".config/systemd/user")) {
        return 0;
    }
    if (!mkdirs(dir)) {
        return 0;
    }

    written = snprintf(service_path, sizeof service_path, "%s/simpleclock-reminders.service", dir);
    if (!snprintf_ok(written, sizeof service_path)) {
        return 0;
    }
    written = snprintf(timer_path, sizeof timer_path, "%s/simpleclock-reminders.timer", dir);
    if (!snprintf_ok(written, sizeof timer_path)) {
        return 0;
    }

    if (!write_text_atomic(service_path, service_text)) {
        return 0;
    }
    if (!write_text_atomic(timer_path, timer_text)) {
        return 0;
    }
    if (!run_command_ok_maybe_quiet("systemctl --user daemon-reload", quiet)) {
        return 0;
    }
    if (!run_command_ok_maybe_quiet("systemctl --user enable --now simpleclock-reminders.timer", quiet)) {
        return 0;
    }

    if (!quiet) {
        printf("simpleclock: installed systemd user timer backend\n");
    }
    return 1;
}

static int install_cron_reminders(int quiet) {
    char tmp[4096];
    FILE *in;
    FILE *out;
    char line[2048];
    const char *cron_line = "* * * * * ~/.local/bin/simpleclock --check-reminders >/dev/null 2>&1\n";
    int fd;
    int rc;
    int ok = 1;

    snprintf(tmp, sizeof tmp, "/tmp/simpleclock-cron.XXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0) {
        return 0;
    }
    out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(tmp);
        return 0;
    }

    in = popen("crontab -l 2>/dev/null", "r");
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (strstr(line, "simpleclock --check-reminders")) {
                continue;
            }
            fputs(line, out);
        }
        pclose(in);
    }

    fputs(cron_line, out);
    if (fclose(out) != 0) {
        ok = 0;
    }

    if (ok) {
        pid_t pid = fork();
        int status = 0;

        if (pid == 0) {
            if (quiet) {
                int nullfd = open("/dev/null", O_WRONLY);

                if (nullfd >= 0) {
                    dup2(nullfd, STDOUT_FILENO);
                    dup2(nullfd, STDERR_FILENO);
                    close(nullfd);
                }
            }
            execlp("crontab", "crontab", tmp, (char *)NULL);
            _exit(127);
        }
        if (pid < 0) {
            ok = 0;
        }
        while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (pid > 0) {
            ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }

    rc = unlink(tmp);
    (void)rc;
    if (ok && !quiet) {
        printf("simpleclock: installed cron reminder backend\n");
    }
    return ok;
}

static int install_reminders(int quiet) {
    char dir[4096];

    if (!state_dir(dir, sizeof dir)) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: could not create ~/.local/state/simpleclock\n");
        }
        return 1;
    }

    if (run_command_ok("systemctl --user show-environment >/dev/null 2>&1")) {
        if (install_systemd_reminders(quiet)) {
            return 0;
        }
        if (!quiet) {
            fprintf(stderr, "simpleclock: systemd user timer install failed; trying cron fallback\n");
        }
    }

    if (!command_exists("crontab")) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: systemd user is unavailable and crontab was not found\n");
        }
        return 1;
    }
    if (!install_cron_reminders(quiet)) {
        if (!quiet) {
            fprintf(stderr, "simpleclock: cron reminder install failed\n");
        }
        return 1;
    }
    return 0;
}

static bool read_alarm_file_path(const char *path, long *alarm_time) {
    FILE *file;
    long saved_time;

    if (!path || !file_exists(path)) {
        return false;
    }
    file = fopen(path, "r");
    if (!file) {
        return false;
    }
    if (fscanf(file, "%ld", &saved_time) != 1 || saved_time <= 0) {
        fclose(file);
        return false;
    }
    fclose(file);
    *alarm_time = saved_time;
    return true;
}

static void migrate_alarm_file_to_reminder(const char *path) {
    long alarm_time = 0;

    if (!read_alarm_file_path(path, &alarm_time)) {
        return;
    }
    if (!active_reminder_kind_exists("alarm")) {
        write_clock_reminder("alarm", "alarm", (time_t)alarm_time);
    }
    unlink(path);
}

static void migrate_legacy_state(void) {
    static int attempted = 0;
    char path[4096];

    if (attempted) {
        return;
    }
    attempted = 1;

    if (state_path(path, sizeof path, ALARM_FILE)) {
        migrate_alarm_file_to_reminder(path);
    }
    if (home_path(path, sizeof path, LEGACY_ALARM_FILE)) {
        migrate_alarm_file_to_reminder(path);
    }
    if (state_path(path, sizeof path, WORKER_FILE)) {
        unlink(path);
    }
    if (home_path(path, sizeof path, LEGACY_WORKER_FILE)) {
        unlink(path);
    }
    if (home_path(path, sizeof path, LEGACY_ALARM_TEMP_FILE)) {
        unlink(path);
    }
}

static void sync_ui_from_reminders(long *timer_end, bool *timer_on, bool timer_paused,
                                   bool *timer_ringing, long *alarm_time, bool *alarm_on,
                                   bool *alarm_ringing) {
    ReminderList reminders = {0};
    time_t now = time(NULL);
    int saw_timer = 0;
    int saw_alarm = 0;

    if (!load_reminders(&reminders)) {
        return;
    }

    for (size_t i = 0; i < reminders.len; i++) {
        ClockReminder *r = &reminders.items[i];
        time_t due = parse_due_time(r->due);
        int clearable = reminder_is_clearable(r, now);

        if (due == (time_t)-1) {
            continue;
        }

        if (!strcmp(r->kind, "timer")) {
            if (!strcmp(r->status, "pending")) {
                if (!timer_paused) {
                    *timer_end = due;
                    *timer_on = true;
                    *timer_ringing = false;
                }
                saw_timer = 1;
            } else if (reminder_is_ringing(r) || clearable) {
                if (!timer_paused) {
                    *timer_on = false;
                }
                *timer_ringing = true;
                saw_timer = 1;
            }
        } else if (!strcmp(r->kind, "alarm")) {
            if (!strcmp(r->status, "pending")) {
                *alarm_time = due;
                *alarm_on = true;
                *alarm_ringing = false;
                saw_alarm = 1;
            } else if (reminder_is_ringing(r) || clearable) {
                *alarm_on = false;
                *alarm_ringing = true;
                saw_alarm = 1;
            }
        }
    }

    if (!timer_paused && !saw_timer) {
        *timer_on = false;
        *timer_ringing = false;
    }
    if (!saw_alarm) {
        *alarm_on = false;
        *alarm_ringing = false;
    }

    reminderlist_free(&reminders);
}

long nowsec(void) {
    return time(NULL);
}

long parse_duration(const char *s) {
    long value = 0;
    char unit = 's';

    if (sscanf(s, "%ld%c", &value, &unit) < 1) {
        return -1;
    }

    switch (unit) {
        case 'd':
        case 'D':
            return value * 86400L;

        case 'h':
        case 'H':
            return value * 3600L;

        case 'm':
        case 'M':
            return value * 60L;

        case 's':
        case 'S':
        default:
            return value;
    }
}

void ask(char *buf, int n, const char *prompt) {
    timeout(-1);
    echo();
    curs_set(1);

    mvprintw(LINES - 2, 2, "%s", prompt);
    clrtoeol();
    move(LINES - 1, 2);
    clrtoeol();

    getnstr(buf, n - 1);

    noecho();
    curs_set(0);
    timeout(1000);
}

int main(int argc, char **argv) {
    migrate_legacy_state();

    if (argc > 1) {
        if (!strcmp(argv[1], "--check-reminders")) {
            return check_reminders();
        }
        if (!strcmp(argv[1], "--clear-reminders")) {
            int cleared = 0;

            return clear_reminders(&cleared, 0);
        }
        if (!strcmp(argv[1], "--install-reminders")) {
            return install_reminders(0);
        }
        fprintf(stderr,
                "usage: %s [--check-reminders|--clear-reminders|--install-reminders]\n",
                argv[0]);
        return 1;
    }

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    leaveok(stdscr, TRUE);
    timeout(1000);

    long timer_end = 0;
    bool timer_on = false;
    bool timer_paused = false;
    bool timer_ringing = false;
    long timer_left_paused = 0;

    long alarm_time = 0;
    bool alarm_on = false;
    bool alarm_ringing = false;

    bool sw_on = false;
    long sw_start = 0;
    long sw_elapsed = 0;

    ui_check_due_reminders();
    sync_ui_from_reminders(&timer_end, &timer_on, timer_paused, &timer_ringing,
                           &alarm_time, &alarm_on, &alarm_ringing);

    while (1) {
        long t = nowsec();

        ui_check_due_reminders();
        sync_ui_from_reminders(&timer_end, &timer_on, timer_paused, &timer_ringing,
                               &alarm_time, &alarm_on, &alarm_ringing);

        struct tm *lt = localtime(&t);
        char dt[128];
        strftime(dt, sizeof dt, "%A, %B %d, %Y   %I:%M:%S %p", lt);

        erase();

        mvprintw(1, 2, "SIMPLECLOCK");
        mvprintw(3, 2, "%s", dt);

        long sw = sw_elapsed;
        if (sw_on) {
            sw += t - sw_start;
        }

        mvprintw(6, 2, "Stopwatch: %02ld:%02ld:%02ld  [%s]",
                 sw / 3600, (sw / 60) % 60, sw % 60,
                 sw_on ? "running" : "stopped");

        if (timer_ringing) {
            mvprintw(8, 2, "Timer: RINGING - press x to stop");
        } else if (timer_on && timer_paused) {
            long left = timer_left_paused;
            mvprintw(8, 2, "Timer: %02ld:%02ld:%02ld  [paused]",
                     left / 3600, (left / 60) % 60, left % 60);
        } else if (timer_on) {
            long left = timer_end - t;
            mvprintw(8, 2, "Timer: %02ld:%02ld:%02ld  [running]",
                     left / 3600, (left / 60) % 60, left % 60);
        } else {
            mvprintw(8, 2, "Timer: off");
        }

        if (alarm_ringing) {
            mvprintw(10, 2, "Alarm: RINGING - press x to stop");
        } else if (alarm_on) {
            struct tm *at = localtime(&alarm_time);
            char abuf[64];
            strftime(abuf, sizeof abuf, "%I:%M %p", at);
            mvprintw(10, 2, "Alarm: %s", abuf);
        } else {
            mvprintw(10, 2, "Alarm: off");
        }

        mvprintw(14, 2, "[s] start/stop stopwatch");
        mvprintw(15, 2, "[r] reset stopwatch");
        mvprintw(16, 2, "[t] set timer (30s, 5m, 2h, 1d)");
        mvprintw(17, 2, "[space] pause/resume timer");
        mvprintw(18, 2, "[a] set alarm HH:MM 24h");
        mvprintw(19, 2, "[x] stop ringing");
        mvprintw(20, 2, "[c] clear timer/alarm");
        mvprintw(21, 2, "[q] quit");

        wnoutrefresh(stdscr);
        doupdate();

        int ch = getch();

        if (ch == 'q') {
            break;
        }

        if (ch == 'x') {
            clear_reminders(NULL, 1);
            timer_ringing = false;
            alarm_ringing = false;
            sync_ui_from_reminders(&timer_end, &timer_on, timer_paused, &timer_ringing,
                                   &alarm_time, &alarm_on, &alarm_ringing);
        }

        if (ch == ' ') {
            if (timer_on && !timer_paused) {
                timer_left_paused = timer_end - t;
                if (timer_left_paused < 0) {
                    timer_left_paused = 0;
                }
                timer_paused = true;
                remove_clock_reminder_kind("timer");
            } else if (timer_on && timer_paused) {
                timer_end = nowsec() + timer_left_paused;
                timer_paused = false;
                write_clock_reminder("timer", "timer", (time_t)timer_end);
                install_reminders(1);
            }
        }

        if (ch == 's') {
            if (sw_on) {
                sw_elapsed += t - sw_start;
                sw_on = false;
            } else {
                sw_start = t;
                sw_on = true;
            }
        }

        if (ch == 'r') {
            sw_on = false;
            sw_elapsed = 0;
        }

        if (ch == 't') {
            char buf[32];

            ask(buf, sizeof buf, "Timer (examples: 30s 5m 2h 1d): ");

            long seconds = parse_duration(buf);

            if (seconds > 0) {
                timer_end = nowsec() + seconds;
                timer_on = true;
                timer_paused = false;
                timer_left_paused = 0;
                timer_ringing = false;
                write_clock_reminder("timer", "timer", (time_t)timer_end);
                install_reminders(1);
            }
        }

        if (ch == 'a') {
            char buf[32];
            ask(buf, sizeof buf, "Alarm time HH:MM 24h: ");

            int h, m;
            if (sscanf(buf, "%d:%d", &h, &m) == 2 &&
                h >= 0 && h < 24 &&
                m >= 0 && m < 60) {

                time_t raw = time(NULL);
                struct tm tm_alarm = *localtime(&raw);

                tm_alarm.tm_hour = h;
                tm_alarm.tm_min = m;
                tm_alarm.tm_sec = 0;

                alarm_time = mktime(&tm_alarm);

                if (alarm_time <= nowsec()) {
                    alarm_time += 24 * 60 * 60;
                }

                alarm_on = true;
                alarm_ringing = false;
                write_clock_reminder("alarm", "alarm", (time_t)alarm_time);
                install_reminders(1);
            }
        }

        if (ch == 'c') {
            timer_on = false;
            timer_paused = false;
            timer_left_paused = 0;
            alarm_on = false;
            timer_ringing = false;
            alarm_ringing = false;
            clear_clock_scheduled_reminders();
            stop_alarm_player(1);
        }
    }

    endwin();
    return 0;
}
