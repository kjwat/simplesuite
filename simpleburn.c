#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <ncurses.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "simpleproc.h"
#include "simpleui.h"

#define BURN_MAX_NODES 512
#define BURN_PATH 4096
#define BURN_SYNC_BYTES (16 * 1024 * 1024ULL)
typedef struct {
    char path[128], parent[128], kind[24], model[160], serial[160];
    char mounts[BURN_PATH], identity[384];
    uint64_t size;
    dev_t device;
    int readonly;
} BurnNode;
typedef struct { BurnNode nodes[BURN_MAX_NODES]; int count; } BurnTable;
static volatile sig_atomic_t interrupted;
static char message[512] = "Choose a drive. Enter to continue.";
static void stop(int sig) { (void)sig; interrupted = 1; }

static int errorf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    return -1;
}

static void copy_text(char *to, size_t size, const char *from)
{
    snprintf(to, size, "%s", from ? from : "");
}

static void display_text(char *to, size_t size, const char *from)
{
    copy_text(to, size, from);
    for (char *p = to; *p; p++)
        if ((unsigned char)*p < 32 || *p == 127) *p = '?';
}

static int wait_child(pid_t pid)
{
    int status;
    pid_t got;
    do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    return got == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* No shell interpolation: filenames and device names always stay arguments. */
static int command(char *const args[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (!pid) { execvp(args[0], args); perror(args[0]); _exit(127); }
    return wait_child(pid);
}

static char *capture(char *const args[])
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    if (!pid) {
        dup2(pipefd[1], STDOUT_FILENO);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        close(pipefd[0]); close(pipefd[1]);
        execvp(args[0], args); _exit(127);
    }
    close(pipefd[1]);
    size_t used = 0, capacity = 8192;
    char *data = malloc(capacity);
    if (data) for (;;) {
        if (used == capacity - 1) {
            if (capacity >= 4 * 1024 * 1024) { free(data); data = NULL; break; }
            char *grown = realloc(data, capacity * 2);
            if (!grown) { free(data); data = NULL; break; }
            data = grown; capacity *= 2;
        }
        ssize_t n = read(pipefd[0], data + used, capacity - used - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { free(data); data = NULL; }
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(pipefd[0]);
    if (!data) kill(pid, SIGTERM);
    if (wait_child(pid) != 0) { free(data); return NULL; }
    if (data) data[used] = 0;
    return data;
}

static int path_inside(const char *path, const char *directory)
{
    size_t n = strlen(directory);
    return n && strncmp(path, directory, n) == 0 &&
        (directory[n - 1] == '/' || path[n] == '/' || !path[n]);
}

typedef struct { char **mounts; size_t count; } IsoSearch;

static int search_mount_add(IsoSearch *search, const char *root, const char *path)
{
    if (!strcmp(path, root) || !path_inside(path, root)) return 0;
    char *name = strdup(path);
    if (!name) return -1;
    char **grown = realloc(search->mounts, (search->count + 1) * sizeof(*grown));
    if (!grown) { free(name); return -1; }
    search->mounts = grown;
    search->mounts[search->count++] = name;
    return 0;
}

static void search_mount_free(IsoSearch *search)
{
    for (size_t i = 0; i < search->count; i++) free(search->mounts[i]);
    free(search->mounts);
}

static int search_mount_blocked(const IsoSearch *search, const char *path)
{
    for (size_t i = 0; i < search->count; i++)
        if (path_inside(path, search->mounts[i])) return 1;
    return 0;
}

static void members(const BurnTable *table, int disk, unsigned char *set)
{
    memset(set, 0, BURN_MAX_NODES);
    set[disk] = 1;
    for (int pass = 0; pass < table->count; pass++) {
        int changed = 0;
        for (int i = 0; i < table->count; i++) {
            if (set[i] || !table->nodes[i].parent[0]) continue;
            for (int j = 0; j < table->count; j++)
                if (set[j] && !strcmp(table->nodes[i].parent, table->nodes[j].path)) {
                    set[i] = 1; changed = 1; break;
                }
        }
        if (!changed) break;
    }
}

/* Only ordinary removable-volume mount locations may be auto-unmounted. */
static int safe_mounts(const char *mounts, const char *image)
{
    char buffer[BURN_PATH];
    copy_text(buffer, sizeof(buffer), mounts);
    char *save = NULL;
    for (char *p = strtok_r(buffer, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
        if (path_inside(image, p)) return 0;
        if (!path_inside(p, "/media") && !path_inside(p, "/run/media") &&
            !path_inside(p, "/mnt") && !path_inside(p, "/Volumes")) return 0;
    }
    return 1;
}

static int eligible(const BurnTable *table, int disk, const char *image,
                    const struct stat *image_stat)
{
    const BurnNode *drive = &table->nodes[disk];
    if (strcmp(drive->kind, "disk") || drive->readonly ||
        drive->size < (uint64_t)image_stat->st_size) return 0;
    unsigned char set[BURN_MAX_NODES];
    members(table, disk, set);
    for (int i = 0; i < table->count; i++) if (set[i]) {
        const BurnNode *node = &table->nodes[i];
        if (node->device && node->device == image_stat->st_dev) return 0;
        if (i != disk && strcmp(node->kind, "part") && strcmp(node->kind, "label")) return 0;
        if (!safe_mounts(node->mounts, image)) return 0;
    }
    return 1;
}

#if defined(__linux__) || defined(__FreeBSD__)
static int protect_pools(BurnTable *table, int required)
{
    if (!ssp_command_available("zpool")) return required ? errorf("Cannot inspect active ZFS storage; install zpool.") : 0;
    char *args[] = {"zpool", "status", "-LP", NULL};
    char *pools = capture(args);
    if (!pools) return required ? errorf("Cannot inspect active ZFS storage.") : 0;
    for (int i = 0; i < table->count; i++) if (strstr(pools, table->nodes[i].path))
        copy_text(table->nodes[i].kind, sizeof(table->nodes[i].kind), "stack");
    free(pools); return 0;
}
#endif

#include "simpleburn-platform.h"

static void image_identity(const struct stat *st, char *out, size_t size)
{
#if defined(__APPLE__)
    struct timespec modified = st->st_mtimespec, changed = st->st_ctimespec;
#else
    struct timespec modified = st->st_mtim, changed = st->st_ctim;
#endif
    snprintf(out, size, "%ju:%ju:%ju:%jd:%ld:%jd:%ld", (uintmax_t)st->st_dev,
        (uintmax_t)st->st_ino, (uintmax_t)st->st_size,
        (intmax_t)modified.tv_sec, modified.tv_nsec,
        (intmax_t)changed.tv_sec, changed.tv_nsec);
}

static int open_image(const char *path, struct stat *st)
{
    size_t n = strlen(path);
    if (n < 4 || strcasecmp(path + n - 4, ".iso")) return errorf("Choose an .iso file.");
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errorf("Cannot read ISO: %s", strerror(errno));
    if (fstat(fd, st) < 0 || !S_ISREG(st->st_mode) || st->st_size <= 0 || st->st_size % 2048) {
        close(fd);
        return errorf("ISO must be a nonempty regular file with complete 2048-byte sectors.");
    }
    return fd;
}

/* dd inherits the pinned descriptors, preserving their offsets between
 * batches. Classic operands work with GNU, BusyBox, FreeBSD and macOS dd. */
static int dd_batch(int in, int out, uint64_t size)
{
    char count[64];
    snprintf(count, sizeof(count), "count=%" PRIu64, size / 2048);
    fflush(NULL);
    sigset_t blocked, original;
    sigemptyset(&blocked); sigaddset(&blocked, SIGINT); sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, &original);
    pid_t pid = fork();
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &original, NULL);
        return errorf("Cannot start dd: %s", strerror(errno));
    }
    if (!pid) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
        sigprocmask(SIG_SETMASK, &original, NULL);
        if (dup2(in, STDIN_FILENO) < 0 || dup2(out, STDOUT_FILENO) < 0) _exit(126);
        close(in); close(out);
        char *args[] = {"dd", "ibs=2048", "obs=4194304", count, "conv=notrunc", NULL};
        execvp(args[0], args); perror("dd"); _exit(127);
    }
    sigprocmask(SIG_SETMASK, &original, NULL);
    int status = 0, cancelled = 0;
    pid_t got;
    do {
        if (interrupted && !cancelled) { kill(pid, SIGTERM); cancelled = 1; }
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    if (cancelled || interrupted) return errorf("Burn cancelled; the drive contains an incomplete image.");
    if (got < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
        return errorf("dd failed; the drive contains an incomplete image. See the dd error above.");
    return 0;
}

/* Progress counts successfully flushed bytes, not data waiting in the OS
 * cache. Bounded batches keep the final flush from holding an entire ISO. */
static int run_dd(int in, int out, uint64_t size)
{
    uint64_t committed = 0;
    puts("STATUS Writing and syncing drive...\nPROGRESS 0"); fflush(stdout);
    while (committed < size) {
        if (interrupted) return errorf("Burn cancelled; the drive contains an incomplete image.");
        uint64_t batch = size - committed;
        if (batch > BURN_SYNC_BYTES) batch = BURN_SYNC_BYTES;
        if (dd_batch(in, out, batch) < 0) return -1;
        uint64_t end = committed + batch;
        if (lseek(in, 0, SEEK_CUR) != (off_t)end || lseek(out, 0, SEEK_CUR) != (off_t)end)
            return errorf("ISO changed or dd stopped short; the drive contains an incomplete image.");
        if (platform_flush(out) < 0) return errorf("Cannot flush drive: %s", strerror(errno));
        if (interrupted) return errorf("Burn cancelled; the drive contains an incomplete image.");
        committed = end;
        printf("PROGRESS %" PRIu64 "\n", committed); fflush(stdout);
    }
    return 0;
}

static int write_image(const char *image, const char *device,
                       const char *expected_drive, const char *expected_image)
{
    int in = -1, out = -1, result = 1, disk = -1;
    BurnTable *table = calloc(1, sizeof(*table));
    struct stat st, final_stat;
    char identity[256];
    if (!table) { errorf("Out of memory."); goto done; }
    if (geteuid() != 0) { errorf("Administrator privileges are required."); goto done; }
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    in = open_image(image, &st);
    if (in < 0) goto done;
    image_identity(&st, identity, sizeof(identity));
    if (strcmp(identity, expected_image)) { errorf("ISO changed since selection. Choose it again."); goto done; }
    if (platform_scan(table) < 0) goto done;
    for (int i = 0; i < table->count; i++) if (!strcmp(table->nodes[i].path, device)) { disk = i; break; }
    if (disk < 0 || strcmp(table->nodes[disk].identity, expected_drive) ||
        !eligible(table, disk, image, &st)) {
        errorf("Drive changed or is unsafe to erase. Choose it again."); goto done;
    }
    puts("STATUS Unmounting selected drive..."); fflush(stdout);
    if (platform_unmount(table, disk) < 0 || interrupted) goto done;
    if (platform_scan(table) < 0) goto done;
    disk = -1;
    for (int i = 0; i < table->count; i++) if (!strcmp(table->nodes[i].path, device)) { disk = i; break; }
    if (disk < 0 || strcmp(table->nodes[disk].identity, expected_drive) ||
        !eligible(table, disk, image, &st)) {
        errorf("Drive changed during unmount. Burn stopped."); goto done;
    }
    unsigned char set[BURN_MAX_NODES];
    members(table, disk, set);
    for (int i = 0; i < table->count; i++) if (set[i] && table->nodes[i].mounts[0]) {
        errorf("Drive is still mounted or in use. Burn stopped."); goto done;
    }
    out = platform_open(&table->nodes[disk]);
    if (out < 0) goto done;
    if (run_dd(in, out, (uint64_t)st.st_size) < 0) goto done;
    if (fstat(in, &final_stat) < 0) { errorf("Cannot recheck ISO."); goto done; }
    image_identity(&final_stat, identity, sizeof(identity));
    if (strcmp(identity, expected_image)) { errorf("ISO changed while burning. Burn failed."); goto done; }
    if (close(out) < 0) { out = -1; errorf("Cannot close drive: %s", strerror(errno)); goto done; }
    out = -1;
    puts("DONE"); result = 0;
done:
    if (out >= 0) close(out);
    if (in >= 0) close(in);
    free(table);
    if (result) printf("ERROR %s\n", interrupted ? "Burn cancelled; the drive may contain an incomplete image." : message);
    return result;
}

static int skipped_directory(const char *name)
{
    return !strcmp(name, ".git") || !strcmp(name, ".cache") ||
        !strcmp(name, "node_modules") || !strcmp(name, "build");
}

static int find_isos(const char *directory, int depth, const IsoSearch *search)
{
    if (depth > 64 || search_mount_blocked(search, directory)) return 0;
    DIR *dir = opendir(directory);
    if (!dir) return 0;
    int result = 0;
    /* Downloads must be visible before a large home directory is traversed. */
    if (!depth) {
        char downloads[BURN_PATH];
        int n = snprintf(downloads, sizeof(downloads), "%s/Downloads", directory);
        if (n > 0 && (size_t)n < sizeof(downloads) && !search_mount_blocked(search, downloads)) {
            struct stat st;
            if (lstat(downloads, &st) == 0 && S_ISDIR(st.st_mode))
                result = find_isos(downloads, 1, search);
        }
    }
    struct dirent *entry;
    while (!result && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!depth && !strcmp(entry->d_name, "Downloads")) continue;
        char path[BURN_PATH];
        int n = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        /* Check the mount table before stat: an offline NFS stat can hang. */
        if (search_mount_blocked(search, path)) continue;
        struct stat st;
        if (lstat(path, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!skipped_directory(entry->d_name)) result = find_isos(path, depth + 1, search);
        } else {
            size_t length = strlen(path);
            if (length >= 4 && !strcasecmp(path + length - 4, ".iso") &&
                (S_ISREG(st.st_mode) || (S_ISLNK(st.st_mode) && stat(path, &st) == 0 && S_ISREG(st.st_mode)))) {
                if (fwrite(path, 1, length + 1, stdout) != length + 1 || fflush(stdout) == EOF) result = -1;
            }
        }
    }
    closedir(dir);
    return result;
}

static void stop_finder(pid_t finder)
{
    kill(finder, SIGTERM);
    pid_t got;
    do { got = waitpid(finder, NULL, WNOHANG); } while (got < 0 && errno == EINTR);
    /* Cancellation must also work if an explicitly searched share is offline. */
    if (!got) ssp_manage_deferred_child(finder);
}

static int choose_iso(const char *root, char *image, struct stat *st)
{
    ssp_manage_deferred_child(0);
    IsoSearch search = {0};
    if (platform_search_mounts(root, &search) < 0) {
        search_mount_free(&search); return -1;
    }
    int files[2], selection[2];
    if (pipe(files) < 0) { search_mount_free(&search); return errorf("Cannot start ISO picker."); }
    if (pipe(selection) < 0) {
        close(files[0]); close(files[1]); search_mount_free(&search);
        return errorf("Cannot start ISO picker.");
    }
    pid_t finder = fork();
    if (!finder) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        dup2(files[1], STDOUT_FILENO);
        close(files[0]); close(files[1]); close(selection[0]); close(selection[1]);
        int result = find_isos(root, 0, &search);
        _exit(result < 0 ? 1 : 0);
    }
    search_mount_free(&search);
    if (finder < 0) goto fail;
    pid_t picker = fork();
    if (!picker) {
        dup2(files[0], STDIN_FILENO); dup2(selection[1], STDOUT_FILENO);
        close(files[0]); close(files[1]); close(selection[0]); close(selection[1]);
        unsetenv("FZF_DEFAULT_OPTS"); unsetenv("FZF_DEFAULT_OPTS_FILE");
        char *args[] = {"fzf", "--read0", "--print0", "--no-multi", "--layout=reverse",
            "--height=90%", "--border=rounded", "--color=bw", "--prompt=ISO > ",
            "--header=simpleburn | Enter choose ISO | Esc cancel", NULL};
        execvp(args[0], args); perror("fzf"); _exit(127);
    }
    if (picker < 0) { stop_finder(finder); goto fail; }
    close(files[0]); close(files[1]); close(selection[1]);
    char selected[BURN_PATH];
    size_t used = 0;
    ssize_t n;
    while (!interrupted && used < sizeof(selected) && (n = read(selection[0], selected + used, sizeof(selected) - used)) != 0) {
        if (n < 0) { if (errno == EINTR && !interrupted) continue; break; }
        used += (size_t)n;
    }
    close(selection[0]);
    if (interrupted) { kill(picker, SIGTERM); kill(finder, SIGTERM); }
    int code = wait_child(picker);
    stop_finder(finder);
    if (code == 1 || code == 130 || interrupted) return 0;
    if (code != 0 || !used || used >= sizeof(selected) || selected[used - 1] ||
        strlen(selected) != used - 1) return errorf("ISO picker failed.");
    if (!realpath(selected, image)) return errorf("Cannot resolve ISO: %s", strerror(errno));
    int fd = open_image(image, st);
    if (fd < 0) return -1;
    close(fd);
    return 1;
fail:
    close(files[0]); close(files[1]); close(selection[0]); close(selection[1]);
    return errorf("Cannot start ISO picker: %s", strerror(errno));
}

static void size_text(uint64_t bytes, char *out, size_t size)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double number = (double)bytes;
    int unit = 0;
    while (number >= 1024 && unit < 4) { number /= 1024; unit++; }
    snprintf(out, size, "%.1f %s", number, units[unit]);
}

static int progress_percent(uint64_t committed, uint64_t size, int done)
{
    if (done) return 100;
    if (!size) return 0;
    /* Floor the percentage and reserve 100% for the completed helper. */
    if (committed >= size) return 99;
    int percent = (int)(100.0L * (long double)committed / (long double)size);
    return percent < 100 ? percent : 99;
}

static void draw(const BurnTable *table, const int *drives, int count, int selected,
                 int *top, const char *image, uint64_t image_size, int confirming,
                 int burning, uint64_t written, int done)
{
    erase();
    if (LINES < 13 || COLS < 48) {
        addnstr("simpleburn: enlarge terminal (q quits)", COLS > 0 ? COLS - 1 : 0);
        refresh(); return;
    }
    attron(A_BOLD); mvaddstr(1, 2, "simpleburn"); attroff(A_BOLD);
    char text[BURN_PATH + 64], name[BURN_PATH], size[32];
    display_text(name, sizeof(name), image); size_text(image_size, size, sizeof(size));
    snprintf(text, sizeof(text), "ISO: %s", name); mvaddnstr(3, 2, text, COLS - 4);
    snprintf(text, sizeof(text), "Size: %s", size); mvaddnstr(4, 2, text, COLS - 4);
    if (burning || done) {
        int percent = progress_percent(written, image_size, done);
        snprintf(text, sizeof(text), done ? "Burn complete: %s" : "Burning to %s",
            table->nodes[drives[selected]].path);
        mvaddnstr(6, 2, text, COLS - 4);
        mvprintw(8, 2, "%d%%", percent);
        int width = COLS - 12, filled = (int)(width * percent / 100);
        mvaddch(9, 2, '['); mvhline(9, 3, ' ', width); mvaddch(9, width + 3, ']');
        attron(A_REVERSE); mvhline(9, 3, ' ', filled); attroff(A_REVERSE);
    } else {
        mvaddstr(6, 2, "Drive              Size         Model");
        int visible = LINES - 12;
        if (selected < *top) *top = selected;
        if (selected >= *top + visible) *top = selected - visible + 1;
        for (int i = *top, row = 7; i < count && i < *top + visible; i++, row++) {
            const BurnNode *drive = &table->nodes[drives[i]];
            display_text(name, sizeof(name), drive->model);
            size_text(drive->size, size, sizeof(size));
            snprintf(text, sizeof(text), "%-18s %-12s %s", drive->path, size, name);
            if (i == selected) attron(A_REVERSE);
            mvaddnstr(row, 2, text, COLS - 4);
            if (i == selected) attroff(A_REVERSE);
        }
        if (!count) mvaddnstr(7, 2, "No suitable drives. Connect a drive and press r.", COLS - 4);
    }
    display_text(text, sizeof(text), message);
    mvaddnstr(LINES - 3, 2, text, COLS - 4);
    mvaddnstr(LINES - 2, 2, done ? "r drives   f ISO   q quit" : burning ? "q cancel burn" : confirming ?
        "y erase selected drive and burn   n cancel" :
        "Up/Down choose   Enter burn   r refresh   f ISO   q quit", COLS - 4);
    refresh();
}

static pid_t begin_burn(const BurnNode *drive, const char *image,
                        const struct stat *st, int *output)
{
    char executable[BURN_PATH], identity[256];
    if (platform_executable(executable, sizeof(executable)) < 0) {
        errorf("Cannot locate simpleburn executable."); return 0;
    }
    if (geteuid() != 0) {
        if (!ssp_command_available("sudo")) { errorf("Install sudo to authorize the write."); return 0; }
        endwin();
        puts("simpleburn: authorize writing the selected drive."); fflush(stdout);
        char *auth[] = {"sudo", "-v", NULL};
        int code = command(auth);
        reset_prog_mode(); refresh();
        if (code || interrupted) { errorf("Administrator authentication cancelled."); return 0; }
    }
    int pipefd[2];
    if (pipe(pipefd) < 0) { errorf("Cannot start burner: %s", strerror(errno)); return 0; }
    image_identity(st, identity, sizeof(identity));
    fflush(NULL);
    pid_t pid = fork();
    if (!pid) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
        dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        char *args[] = {"sudo", "-n", "--", executable, "--write", (char *)image,
            (char *)drive->path, (char *)drive->identity, identity, NULL};
        execvp(geteuid() == 0 ? executable : "sudo", geteuid() == 0 ? args + 3 : args);
        perror("simpleburn"); _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); errorf("Cannot start burner."); return 0; }
    *output = pipefd[0];
    fcntl(*output, F_SETFL, O_NONBLOCK);
    copy_text(message, sizeof(message), "Preparing drive...");
    return pid;
}

static void read_progress(int fd, char *line, size_t *used, uint64_t *written,
                          int *done, int *failed)
{
    char bytes[4096];
    ssize_t n;
    while ((n = read(fd, bytes, sizeof(bytes))) > 0) for (ssize_t i = 0; i < n; i++) {
        if (bytes[i] == '\n' || bytes[i] == '\r') {
            line[*used] = 0;
            if (!strncmp(line, "PROGRESS ", 9)) *written = strtoull(line + 9, NULL, 10);
            else if (!strncmp(line, "STATUS ", 7)) copy_text(message, sizeof(message), line + 7);
            else if (!strncmp(line, "ERROR ", 6)) { copy_text(message, sizeof(message), line + 6); *failed = 1; }
            else if (!strcmp(line, "DONE")) *done = 1;
            else if (strstr(line, "dd:") || strstr(line, "sudo:")) {
                copy_text(message, sizeof(message), line); *failed = 1;
            }
            *used = 0;
        } else if (*used < 1023) line[(*used)++] = bytes[i];
    }
}

int main(int argc, char **argv)
{
    struct sigaction action = {0};
    action.sa_handler = stop; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    if (argc == 6 && !strcmp(argv[1], "--write")) return write_image(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        puts("Usage: simpleburn [SEARCH_DIRECTORY]\n       simpleburn --list-drives IMAGE.iso\n\nFind an ISO with fzf, choose a drive, confirm erasure, and write it with dd.\nSearches HOME by default, checking Downloads first and skipping nested mounts.\nTo search a mounted share, specify its directory. Linux, FreeBSD and macOS are supported."); return 0;
    }
    int listing = argc == 3 && !strcmp(argv[1], "--list-drives");
    if (!listing && (argc > 2 || (argc == 2 && argv[1][0] == '-'))) {
        fprintf(stderr, "Usage: simpleburn [SEARCH_DIRECTORY]\n"); return 2;
    }
    BurnTable *table = calloc(1, sizeof(*table));
    if (!table) { fprintf(stderr, "simpleburn: out of memory\n"); return 1; }
    char image[BURN_PATH] = "", root[BURN_PATH];
    struct stat st;
    if (listing) {
        if (!realpath(argv[2], image)) { perror("ISO"); free(table); return 1; }
        int fd = open_image(image, &st);
        if (fd < 0 || platform_scan(table) < 0) {
            if (fd >= 0) close(fd);
            fprintf(stderr, "simpleburn: %s\n", message); free(table); return 1;
        }
        close(fd);
        for (int i = 0; i < table->count; i++) if (eligible(table, i, image, &st)) {
            char model[160]; display_text(model, sizeof(model), table->nodes[i].model);
            printf("%s\t%" PRIu64 "\t%s\n", table->nodes[i].path, table->nodes[i].size, model);
        }
        free(table); return 0;
    }
    const char *search = argc == 2 ? argv[1] : getenv("HOME");
    if (!search || !realpath(search, root) || stat(root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "simpleburn: search directory is unavailable\n"); free(table); return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        !ssp_command_available("fzf") || !ssp_command_available("dd")) {
        fprintf(stderr, "simpleburn: run in a terminal with fzf and dd installed\n"); free(table); return 1;
    }
    setlocale(LC_ALL, "");
    int picked = choose_iso(root, image, &st);
    if (picked <= 0) {
        if (picked < 0) fprintf(stderr, "simpleburn: %s\n", message);
        free(table); return interrupted ? 130 : picked < 0 ? 1 : 0;
    }
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    set_escdelay(SUI_ESCAPE_DELAY_MS); timeout(100);
    int drives[BURN_MAX_NODES], count = 0, selected = 0, top = 0, confirming = 0;
    int rescan = 1, output = -1, done = 0, failed = 0, cancelled = 0;
    pid_t worker = 0;
    uint64_t written = 0;
    char line[1024]; size_t used = 0;
    int64_t next_scan = 0;
    BurnNode chosen = {0};
    while (!interrupted) {
        ssp_manage_deferred_child(0);
        if (worker) {
            read_progress(output, line, &used, &written, &done, &failed);
            int code = 0;
            pid_t got = waitpid(worker, &code, WNOHANG);
            if (got == worker || (got < 0 && errno != EINTR)) {
                read_progress(output, line, &used, &written, &done, &failed);
                close(output); output = -1; worker = 0;
                int succeeded = !cancelled && got > 0 && WIFEXITED(code) && !WEXITSTATUS(code) && done && !failed;
                if (succeeded)
                    copy_text(message, sizeof(message), "Done. ISO burned successfully; drive flushed.");
                else if (cancelled) copy_text(message, sizeof(message), "Burn cancelled; the drive contains an incomplete image.");
                else if (!failed) copy_text(message, sizeof(message), "Burn failed. The drive may contain an incomplete image.");
                done = succeeded;
                rescan = 0; next_scan = INT64_MAX;
            }
        }
        if (!worker && !confirming && (rescan || sui_monotonic_ms() >= next_scan)) {
            char previous[128] = "";
            if (count) copy_text(previous, sizeof(previous), table->nodes[drives[selected]].path);
            count = 0;
            if (platform_scan(table) == 0) {
                for (int i = 0; i < table->count; i++) if (eligible(table, i, image, &st)) drives[count++] = i;
                selected = 0;
                for (int i = 0; i < count; i++) if (!strcmp(table->nodes[drives[i]].path, previous)) selected = i;
            }
            rescan = 0; next_scan = sui_monotonic_ms() + 3000;
        }
        draw(table, drives, count, selected, &top, image, (uint64_t)st.st_size,
             confirming, worker != 0, written, done && !failed && !cancelled);
        int key = getch();
        if (worker) {
            if ((key == 'q' || key == 27) && !cancelled) {
                kill(worker, SIGTERM); cancelled = 1;
                copy_text(message, sizeof(message), "Stopping burn...");
            }
            continue;
        }
        if (key == 'q' || key == 27) break;
        if (done && key != ERR && key != KEY_RESIZE) done = 0;
        if (confirming && key != ERR && key != KEY_RESIZE) {
            confirming = 0;
            if ((key == 'y' || key == 'Y') && LINES >= 13 && COLS >= 48) {
                written = 0; done = failed = cancelled = 0; used = 0;
                worker = begin_burn(&chosen, image, &st, &output);
            } else copy_text(message, sizeof(message), "Burn cancelled.");
        } else if ((key == KEY_UP || key == 'k') && selected > 0) selected--;
        else if ((key == KEY_DOWN || key == 'j') && selected + 1 < count) selected++;
        else if (key == KEY_PPAGE) selected = selected > 5 ? selected - 5 : 0;
        else if (key == KEY_NPAGE && count) selected = selected + 5 < count ? selected + 5 : count - 1;
        else if (key == 'r') { rescan = 1; copy_text(message, sizeof(message), "Drive list refreshed."); }
        else if (key == 'f') {
            endwin();
            char next_image[BURN_PATH]; struct stat next_stat;
            picked = choose_iso(root, next_image, &next_stat);
            reset_prog_mode(); refresh();
            if (picked > 0) {
                copy_text(image, sizeof(image), next_image); st = next_stat;
                rescan = 1; copy_text(message, sizeof(message), "Choose a drive. Enter to continue.");
            }
        } else if ((key == '\n' || key == KEY_ENTER) && count && LINES >= 13 && COLS >= 48) {
            chosen = table->nodes[drives[selected]];
            confirming = 1;
            char model[160], size[32]; display_text(model, sizeof(model), chosen.model);
            size_text(chosen.size, size, sizeof(size));
            errorf("Erase ALL data on %s (%s, %s)? Press y to burn.", chosen.path, model, size);
        }
    }
    if (worker) { kill(worker, SIGTERM); wait_child(worker); }
    if (output >= 0) close(output);
    endwin(); free(table);
    return interrupted ? 130 : 0;
}
