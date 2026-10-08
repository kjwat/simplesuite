#define _POSIX_C_SOURCE 200809L
#include <ncurses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "simpleui.h"

static const char *folders[] = {"writing", "scriptorium", "simplesuite", "website"};
static char home[3996], destination[4096], temporary[4096], staging[4096];
static char status[512] = "Ready. Press b to back up.";
static pid_t worker;
static int logfd = -1, result;
static volatile sig_atomic_t interrupted;
static void stop(int sig) { (void)sig; interrupted = 1; }
static int directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static int source_exists(int i) {
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", home, folders[i]);
    return directory(path);
}

static char *joined(const char *directory_path, const char *name) {
    size_t size = strlen(directory_path) + strlen(name) + 2;
    char *path = malloc(size);
    if (path) snprintf(path, size, "%s/%s", directory_path, name);
    return path;
}

static int ends_with(const char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && strcmp(name + n - s, suffix) == 0;
}

/* These rules never apply inside .git, including objects, refs and hooks. */
static int generated_name(const char *name, int is_directory) {
    static const char *dirs[] = {
        "build", "_build", "dist", "target", "CMakeFiles", "node_modules",
        "__pycache__", ".cache", ".pytest_cache", ".mypy_cache", ".ruff_cache",
        ".tox", ".venv", "venv", ".next", ".nuxt", ".parcel-cache", ".turbo",
        "autosave", ".auto-save-list", NULL
    };
    static const char *suffixes[] = {
        ".o", ".obj", ".a", ".so", ".dylib", ".dll", ".exe", ".pyc",
        ".pyo", ".class", ".gcda", ".gcno", ".swp", ".swo", ".tmp",
        ".temp", ".autosave", "~", NULL
    };
    if (is_directory) {
        for (int i = 0; dirs[i]; i++) if (strcmp(name, dirs[i]) == 0) return 1;
        return 0;
    }
    if (strcmp(name, ".DS_Store") == 0 || strcmp(name, ".tramp") == 0 ||
        strncmp(name, ".#", 2) == 0 || strncmp(name, ".~lock.", 7) == 0 ||
        (name[0] == '#' && ends_with(name, "#"))) return 1;
    for (int i = 0; suffixes[i]; i++) if (ends_with(name, suffixes[i])) return 1;
    const char *version = strstr(name, ".so.");
    return version && strspn(version + 4, "0123456789.") == strlen(version + 4);
}

/* Recognize native executables even when they have no filename extension. */
static int compiled_file(const char *path) {
    unsigned char bytes[64];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    do { n = read(fd, bytes, sizeof(bytes)); } while (n < 0 && errno == EINTR);
    if (n < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    int compiled = n >= 4 &&
        (memcmp(bytes, "\177ELF", 4) == 0 ||
         memcmp(bytes, "\xfe\xed\xfa\xce", 4) == 0 ||
         memcmp(bytes, "\xce\xfa\xed\xfe", 4) == 0 ||
         memcmp(bytes, "\xfe\xed\xfa\xcf", 4) == 0 ||
         memcmp(bytes, "\xcf\xfa\xed\xfe", 4) == 0 ||
         memcmp(bytes, "\xca\xfe\xba\xbe", 4) == 0 ||
         memcmp(bytes, "\xbe\xba\xfe\xca", 4) == 0 ||
         memcmp(bytes, "\xca\xfe\xba\xbf", 4) == 0 ||
         memcmp(bytes, "\xbf\xba\xfe\xca", 4) == 0);
    if (n >= 8 && memcmp(bytes, "!<arch>\n", 8) == 0) compiled = 1;
    if (n == 64 && bytes[0] == 'M' && bytes[1] == 'Z') {
        unsigned long offset = (unsigned long)bytes[60] | (unsigned long)bytes[61] << 8 |
            (unsigned long)bytes[62] << 16 | (unsigned long)bytes[63] << 24;
        if (lseek(fd, (off_t)offset, SEEK_SET) >= 0 && read(fd, bytes, 4) == 4 &&
            memcmp(bytes, "PE\0\0", 4) == 0) compiled = 1;
    }
    close(fd);
    return compiled;
}

static int stage_file(const char *source, const char *target, const struct stat *st) {
    /* Hard links avoid copying large repositories when sources share a disk. */
    if (link(source, target) == 0) return 0;
    int in = open(source, O_RDONLY), out = -1, saved;
    if (in < 0) return -1;
    out = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) goto fail;
    char buffer[65536];
    ssize_t n;
    while ((n = read(in, buffer, sizeof(buffer))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; goto fail; }
        ssize_t done = 0;
        while (done < n) {
            ssize_t written = write(out, buffer + done, (size_t)(n - done));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) goto fail;
            done += written;
        }
    }
    if (fchmod(out, st->st_mode & 0777) != 0) goto fail;
    struct timespec times[2] = {{st->st_atime, 0}, {st->st_mtime, 0}};
    if (futimens(out, times) != 0) goto fail;
    close(in);
    return close(out);
fail:
    saved = errno;
    close(in);
    if (out >= 0) close(out);
    errno = saved;
    return -1;
}

static int stage_tree(const char *source, const char *target, int git, int root) {
    struct stat st;
    if (lstat(source, &st) != 0) return -1;
    /* Follow a selected folder symlink, but preserve links within the folder. */
    if (root && S_ISLNK(st.st_mode) && stat(source, &st) != 0) return -1;
    const char *name = strrchr(source, '/');
    name = name ? name + 1 : source;
    git = git || strcmp(name, ".git") == 0;
    if (!git && generated_name(name, S_ISDIR(st.st_mode))) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(source);
        if (!dir) return -1;
        if (mkdir(target, 0700) != 0) { int saved = errno; closedir(dir); errno = saved; return -1; }
        struct dirent *entry;
        int error = 0;
        for (;;) {
            errno = 0;
            entry = readdir(dir);
            if (!entry) { error = errno; break; }
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char *from = joined(source, entry->d_name), *to = joined(target, entry->d_name);
            if (!from || !to) error = ENOMEM;
            else if (stage_tree(from, to, git, 0) != 0) {
                error = errno;
                fprintf(stderr, "Cannot back up %s: %s\n", from, strerror(error));
            }
            free(from); free(to);
            if (error) break;
        }
        closedir(dir);
        if (error) { errno = error; return -1; }
        struct timespec times[2] = {{st.st_atime, 0}, {st.st_mtime, 0}};
        if (utimensat(AT_FDCWD, target, times, 0) != 0) return -1;
        return chmod(target, st.st_mode & 0777);
    }
    if (S_ISREG(st.st_mode)) {
        if (!git) {
            int compiled = compiled_file(source);
            if (compiled < 0) return -1;
            if (compiled) return 0;
        }
        return stage_file(source, target, &st);
    }
    if (S_ISLNK(st.st_mode)) {
        size_t size = st.st_size > 0 ? (size_t)st.st_size + 1 : 4096;
        char *link_target = malloc(size + 1);
        if (!link_target) return -1;
        ssize_t n = readlink(source, link_target, size);
        if (n < 0 || (size_t)n == size) {
            int saved = n < 0 ? errno : ENAMETOOLONG;
            free(link_target); errno = saved; return -1;
        }
        link_target[n] = 0;
        int code = symlink(link_target, target), saved = errno;
        free(link_target); errno = saved;
        return code;
    }
    /* Sockets, pipes and devices are runtime state rather than source. */
    return 0;
}

static void remove_staging(const char *path) {
    struct stat st;
    if (!*path || lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
    chmod(path, 0700);
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char *child = joined(path, entry->d_name);
        if (child) { remove_staging(child); free(child); }
    }
    closedir(dir);
    rmdir(path);
}

static int begin(int replace) {
    char dir[4096], logfile[] = "/tmp/simplesave-log-XXXXXX";
    char *args[14] = {"zip", "-r", "-y", "-q", NULL};
    struct stat st;
    int n = 5;
    result = 0;
    staging[0] = 0;
    for (int i = 0; i < 3; i++) {
        if (!source_exists(i)) {
            snprintf(status, sizeof(status), "Missing required folder: ~/%s", folders[i]);
            return 0;
        }
    }
    if (!replace && lstat(destination, &st) == 0) {
        snprintf(status, sizeof(status), "Today's backup exists. Press r to replace, then y to confirm.");
        return 0;
    }
    snprintf(dir, sizeof(dir), "%s/backups", home);
    if (mkdir(dir, 0700) != 0 && (errno != EEXIST || !directory(dir))) {
        snprintf(status, sizeof(status), "Cannot create ~/backups: %s", strerror(errno));
        return 0;
    }
    snprintf(staging, sizeof(staging), "%s/backups/.simplesave-XXXXXX", home);
    if (!mkdtemp(staging)) { staging[0] = 0; goto fail; }
    strcpy(temporary, staging);
    strcat(temporary, "/archive.zip");
    logfd = mkstemp(logfile);
    if (logfd < 0) goto fail;
    unlink(logfile);
    args[4] = temporary;
    for (int i = 0; i < 4; i++) if (source_exists(i)) args[n++] = (char *)folders[i];
    args[n] = NULL;
    worker = fork();
    if (worker < 0) goto fail;
    if (worker == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(logfd);
        for (int i = 5; args[i]; i++) {
            char *source = joined(home, args[i]), *target = joined(staging, args[i]);
            if (!source || !target || stage_tree(source, target, 0, 1) != 0) {
                perror("Preparing source backup");
                _exit(126);
            }
            free(source); free(target);
        }
        if (chdir(staging) != 0) _exit(126);
        /* -y preserves symlinks instead of following them outside the sources. */
        execvp("zip", args);
        perror("zip");
        _exit(127);
    }
    snprintf(status, sizeof(status), "Backing up... Press q to cancel.");
    result = replace ? 2 : 1;
    return 1;
fail:
    snprintf(status, sizeof(status), "Cannot start backup: %s", strerror(errno));
    if (logfd >= 0) close(logfd);
    logfd = -1;
    remove_staging(staging);
    return 0;
}
static void finish(int cancel) {
    int code;
    pid_t waited;
    if (cancel) kill(worker, SIGTERM);
    do { waited = waitpid(worker, &code, cancel ? 0 : WNOHANG); } while (waited < 0 && errno == EINTR);
    if (!waited) return;
    worker = 0;
    if (cancel || waited < 0 || !WIFEXITED(code) || WEXITSTATUS(code) != 0) {
        char detail[300] = "";
        lseek(logfd, 0, SEEK_SET);
        ssize_t count = read(logfd, detail, sizeof(detail) - 1);
        if (count > 0) { detail[count] = 0; detail[strcspn(detail, "\r\n")] = 0; }
        snprintf(status, sizeof(status), "%s%s", cancel ? "Backup cancelled." : "Backup failed. ", cancel ? "" : detail);
        result = 0;
    } else {
        int ok = result == 2 ? rename(temporary, destination) : link(temporary, destination);
        if (ok != 0) {
            snprintf(status, sizeof(status), "Cannot publish backup: %s", strerror(errno));
            result = 0;
        } else {
            snprintf(status, sizeof(status), "Backup saved successfully.");
            result = 1;
        }
    }
    remove_staging(staging);
    close(logfd);
    logfd = -1;
}
int main(int argc, char **argv) {
    int batch = argc == 2 && strcmp(argv[1], "--backup") == 0;
    const char *env = getenv("HOME");
    time_t now = time(NULL);
    char date[32];
    struct tm local;
    if (argc > 1 && !batch) {
        fprintf(stderr, "Usage: simplesave [--backup]\n");
        return 2;
    }
    if (!env || env[0] != '/' || strlen(env) >= sizeof(home)) {
        fprintf(stderr, "HOME must be an absolute path shorter than 3996 bytes.\n");
        return 1;
    }
    strcpy(home, env);
    localtime_r(&now, &local);
    strftime(date, sizeof(date), "%m-%d-%y", &local);
    snprintf(destination, sizeof(destination), "%s/backups/%s.zip", home, date);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    if (batch) {
        if (begin(0)) while (worker && !interrupted) { finish(0); if (worker) sui_sleep_ms(100); }
        if (worker) finish(1);
        printf("%s\n%s\n", status, destination);
        return result == 1 ? 0 : 1;
    }
    setlocale(LC_ALL, "");
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    set_escdelay(SUI_ESCAPE_DELAY_MS); timeout(100);
    int confirm = 0;
    while (!interrupted) {
        if (worker) finish(0);
        erase();
        if (LINES < 14 || COLS < 30) {
            addnstr("simplesave: enlarge terminal (q quits)", COLS > 0 ? COLS - 1 : 0);
            refresh();
            int key = getch();
            if (key == 'q' || key == 27) break;
            continue;
        }
        box(stdscr, 0, 0);
        mvaddnstr(1, 2, "simplesave", COLS - 4);
        mvaddnstr(2, 2, "Writing, source and Git repos; skips build output and caches.", COLS - 4);
        char line[8192];
        snprintf(line, sizeof(line), "Save to: %s", destination);
        mvaddnstr(3, 2, line, COLS - 4);
        for (int i = 0; i < 4 && 5 + i < LINES - 4; i++) {
            snprintf(line, sizeof(line), "~/%s  [%s]", folders[i], source_exists(i) ? "included" : i == 3 ? "absent; skipped" : "missing");
            mvaddnstr(5 + i, 2, line, COLS - 4);
        }
        mvaddnstr(LINES - 4, 2, "[b] back up   [r] replace today's backup   [q] quit", COLS - 4);
        mvaddnstr(LINES - 2, 2, status, COLS - 4);
        refresh();
        int ch = getch();
        if (ch == 'q' || ch == 27) break;
        if (worker) continue;
        if (confirm && ch != ERR && ch != KEY_RESIZE) {
            confirm = 0;
            if (ch == 'y' || ch == 'Y') begin(1);
            else snprintf(status, sizeof(status), "Replacement cancelled.");
        } else if (ch == 'b' || ch == '\n') begin(0);
        else if (ch == 'r') {
            confirm = 1;
            snprintf(status, sizeof(status), "Replace today's backup? Press y to confirm, any other key to cancel.");
        }
    }
    if (worker) finish(1);
    endwin();
    return 0;
}
