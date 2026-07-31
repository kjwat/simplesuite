#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char device[PATH_MAX];
    char fstype[64];
    char mountprog[PATH_MAX];
    char options[1024];
} FreeBSDMediaMapEntry;

typedef enum {
    FREEBSD_CHECKER_E2FSCK = 1,
    FREEBSD_CHECKER_UFS,
    FREEBSD_CHECKER_MSDOSFS,
    FREEBSD_CHECKER_EXFAT,
    FREEBSD_CHECKER_NTFS
} FreeBSDFilesystemCheckerKind;

typedef struct {
    FreeBSDFilesystemCheckerKind kind;
    const char *path;
    const char *check_option;
    const char *repair_option;
    int repair_after_clean_check;
} FreeBSDFilesystemChecker;

#define FREEBSD_CAPTURE_TIMEOUT_MS 3000
#define FREEBSD_MOUNT_TIMEOUT_MS 30000
#define FREEBSD_UNMOUNT_TIMEOUT_MS 30000
#define FREEBSD_FILESYSTEM_CHECK_TIMEOUT_MS (30 * 60 * 1000)

/*
 * This executable is installed setuid-root.  Never pass the caller's
 * environment to a privileged child: FreeBSD's special_media map is a shell
 * script and invokes several utilities by name.
 */
static char freebsd_safe_path[] =
    "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin";
static char freebsd_safe_ifs[] = "IFS= \t\n";
static char freebsd_safe_locale[] = "LC_ALL=C";
static char freebsd_safe_lang[] = "LANG=C";
static char freebsd_safe_home[] = "HOME=/";
static char *const freebsd_safe_environment[] = {
    freebsd_safe_path,
    freebsd_safe_ifs,
    freebsd_safe_locale,
    freebsd_safe_lang,
    freebsd_safe_home,
    NULL
};

static int freebsd_trusted_root_executable(const char *path)
{
    char resolved[PATH_MAX];
    struct stat st;

    if (!path || !realpath(path, resolved))
        return 0;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return st.st_uid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

static void freebsd_execve_safe(const char *path, const char *const argv[])
{
    execve(path, (char *const *)argv, freebsd_safe_environment);
}

static long long freebsd_monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (long long)now.tv_sec * 1000LL +
           (long long)now.tv_nsec / 1000000LL;
}

static void freebsd_kill_child_group(pid_t pid)
{
    if (pid <= 0)
        return;
    if (kill(-pid, SIGKILL) != 0 && errno != ESRCH)
        (void)kill(pid, SIGKILL);
    else
        (void)kill(pid, SIGKILL);
}

static void freebsd_reap_killed_child_bounded(pid_t pid, int *status)
{
    long long deadline = freebsd_monotonic_milliseconds() + 1000;

    while (freebsd_monotonic_milliseconds() < deadline) {
        pid_t result = waitpid(pid, status, WNOHANG);

        if (result == pid || (result < 0 && errno != EINTR))
            return;
        usleep(10000);
    }
}

static int freebsd_wait_child_timeout(pid_t pid, int timeout_ms, int *status)
{
    long long deadline;

    if (pid <= 0 || !status || timeout_ms <= 0)
        return 0;
    deadline = freebsd_monotonic_milliseconds() + timeout_ms;
    for (;;) {
        pid_t result = waitpid(pid, status, WNOHANG);

        if (result == pid)
            return 1;
        if (result < 0 && errno != EINTR)
            return 0;
        if (freebsd_monotonic_milliseconds() >= deadline)
            break;
        usleep(10000);
    }

    freebsd_kill_child_group(pid);
    freebsd_reap_killed_child_bounded(pid, status);
    errno = ETIMEDOUT;
    return 0;
}

static int freebsd_capture_safe_argv(const char *path,
                                     const char *const argv[],
                                     char *output, size_t outputsz,
                                     int timeout_ms)
{
    int fds[2];
    int status = 0;
    int flags;
    int child_done = 0;
    int eof = 0;
    int overflow = 0;
    size_t used = 0;
    pid_t pid;
    long long deadline;

    if (!path || !argv || !output || outputsz < 2 ||
        !freebsd_trusted_root_executable(path) || pipe(fds) != 0)
        return 0;
    output[0] = '\0';

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        int devnull;

        (void)setpgid(0, 0);
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(126);
        devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        if (fds[1] > STDERR_FILENO)
            close(fds[1]);
        if (devnull > STDERR_FILENO && devnull != fds[1])
            close(devnull);
        closefrom(3);
        freebsd_execve_safe(path, argv);
        _exit(127);
    }

    (void)setpgid(pid, pid);
    close(fds[1]);
    flags = fcntl(fds[0], F_GETFL);
    if (flags >= 0)
        (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    deadline = freebsd_monotonic_milliseconds() + timeout_ms;

    while (!child_done || !eof) {
        char discard[256];
        char *destination = used + 1 < outputsz ? output + used : discard;
        size_t available = used + 1 < outputsz ?
            outputsz - used - 1 : sizeof(discard);
        ssize_t count = read(fds[0], destination, available);

        if (count > 0) {
            if (destination == discard)
                overflow = 1;
            else
                used += (size_t)count;
        } else if (count == 0) {
            eof = 1;
        } else if (errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK) {
            eof = 1;
        }

        if (!child_done) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
                child_done = 1;
            else if (result < 0 && errno != EINTR) {
                close(fds[0]);
                return 0;
            }
        }
        if (child_done && eof)
            break;
        if (freebsd_monotonic_milliseconds() >= deadline) {
            freebsd_kill_child_group(pid);
            if (!child_done)
                freebsd_reap_killed_child_bounded(pid, &status);
            close(fds[0]);
            errno = ETIMEDOUT;
            return 0;
        }

        struct pollfd pollfd = {
            .fd = fds[0],
            .events = POLLIN | POLLHUP
        };
        (void)poll(&pollfd, 1, 25);
    }

    close(fds[0]);
    output[used] = '\0';
    return !overflow && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           used > 0;
}

static int freebsd_media_component_allowed(const char *component)
{
    if (!component || !component[0] || component[0] == '.' ||
        strlen(component) > NAME_MAX || strchr(component, '/'))
        return 0;
    for (const unsigned char *p = (const unsigned char *)component; *p; p++) {
        if (iscntrl(*p))
            return 0;
    }
    return 1;
}

static int freebsd_media_mount_path_allowed_for_user(const char *path,
                                                     const char *user)
{
    const char *media = "/media/";
    const char *run_media = "/run/media/";
    const char *rest;
    const char *slash;

    if (!path || !user || !user[0])
        return 0;
    if (strncmp(path, media, strlen(media)) == 0)
        return freebsd_media_component_allowed(path + strlen(media));

    if (strncmp(path, run_media, strlen(run_media)) != 0)
        return 0;
    rest = path + strlen(run_media);
    slash = strchr(rest, '/');
    if (!slash || slash == rest ||
        (size_t)(slash - rest) != strlen(user) ||
        strncmp(rest, user, strlen(user)) != 0)
        return 0;
    return freebsd_media_component_allowed(slash + 1);
}

static int freebsd_media_mount_path_allowed(const char *path)
{
    struct passwd *pw = getpwuid(getuid());

    return pw && freebsd_media_mount_path_allowed_for_user(path, pw->pw_name);
}

static int freebsd_media_mount_key(const char *path, char *key, size_t keysz)
{
    const char *prefix = "/media/";
    const char *start;

    if (!path || !key || keysz == 0 ||
        strncmp(path, prefix, strlen(prefix)) != 0)
        return 0;
    start = path + strlen(prefix);
    if (!freebsd_media_component_allowed(start) || strlen(start) >= keysz)
        return 0;
    strlcpy(key, start, keysz);
    return 1;
}

static int freebsd_media_mount_request(const char *requested, char *mountpoint,
                                       size_t mountsz, char *key, size_t keysz)
{
    char media_root[PATH_MAX];

    if (!requested || !mountpoint || mountsz == 0 ||
        !freebsd_media_mount_key(requested, key, keysz))
        return 0;
    if (!realpath("/media", media_root) || strcmp(media_root, "/media") != 0)
        return 0;
    return snprintf(mountpoint, mountsz, "/media/%s", key) < (int)mountsz;
}

static int freebsd_device_source_allowed(const char *source)
{
    struct stat st;
    const char *name;

    if (!source || strncmp(source, "/dev/", 5) != 0)
        return 0;
    name = source + 5;
    if (!freebsd_media_component_allowed(name))
        return 0;
    if (stat(source, &st) != 0)
        return 0;
    return S_ISCHR(st.st_mode);
}

static int freebsd_same_device(const char *a, const char *b)
{
    struct stat sa;
    struct stat sb;

    return freebsd_device_source_allowed(a) &&
           freebsd_device_source_allowed(b) &&
           stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_rdev == sb.st_rdev;
}

static int freebsd_caller_can_read_device(const char *device)
{
    /*
     * access(2) checks the real uid and supplementary groups even though this
     * executable has an effective uid of root.  The installed operator-only
     * mode and this check keep the helper from expanding device access.
     */
    return freebsd_device_source_allowed(device) &&
           access(device, R_OK) == 0;
}

static int freebsd_mount_record_policy_allowed(
    const struct statfs *mount_record, const char *mountpoint)
{
    if (!mount_record || !mountpoint)
        return 0;
    if (strcmp(mount_record->f_mntonname, mountpoint) != 0)
        return 0;
    if (!freebsd_media_mount_path_allowed(mount_record->f_mntonname))
        return 0;
    if ((mount_record->f_flags & MNT_AUTOMOUNTED) == 0)
        return 0;
    return freebsd_device_source_allowed(mount_record->f_mntfromname);
}

static int freebsd_mount_record_allowed(const struct statfs *mount_record,
                                        const char *mountpoint)
{
    struct stat st;

    if (!freebsd_mount_record_policy_allowed(mount_record, mountpoint))
        return 0;
    if (!freebsd_caller_can_read_device(mount_record->f_mntfromname))
        return 0;
    if (access(mountpoint, X_OK) != 0)
        return 0;
    if (stat(mountpoint, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    return 1;
}

static int freebsd_copy_mount_record(const struct statfs *mounts, int count,
                                     const char *mountpoint,
                                     struct statfs *mount_record)
{
    if (!mounts || count <= 0 || !mountpoint || !mount_record)
        return 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(mounts[i].f_mntonname, mountpoint) == 0) {
            *mount_record = mounts[i];
            return 1;
        }
    }
    return 0;
}

static int freebsd_find_mount_record(const char *mountpoint,
                                     struct statfs *mount_record)
{
    struct statfs *mounts;
    int count;

    count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0)
        return 0;
    return freebsd_copy_mount_record(mounts, count, mountpoint, mount_record);
}

static int freebsd_mountpoint_is_mounted(const char *mountpoint)
{
    struct statfs mount_record;

    return freebsd_find_mount_record(mountpoint, &mount_record);
}

static int freebsd_device_is_mounted_elsewhere(const char *device,
                                               const char *mountpoint)
{
    struct statfs *mounts;
    int count;

    count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0)
        return 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(mounts[i].f_mntfromname, device) == 0 &&
            strcmp(mounts[i].f_mntonname, mountpoint) != 0)
            return 1;
    }
    return 0;
}

static int freebsd_unmount_media(const char *mountpoint);

static int append_option(char *out, size_t outsz, const char *option)
{
    if (!option || !*option)
        return 1;
    if (strlcat(out, out[0] ? "," : "", outsz) >= outsz)
        return 0;
    return strlcat(out, option, outsz) < outsz;
}

static int parse_media_map_options(char *opts, FreeBSDMediaMapEntry *entry)
{
    char *saveptr = NULL;
    char *token;

    if (!opts || opts[0] != '-' || !entry)
        return 0;
    opts++;

    for (token = strtok_r(opts, ",", &saveptr);
         token;
         token = strtok_r(NULL, ",", &saveptr)) {
        if (strncmp(token, "fstype=", 7) == 0) {
            strlcpy(entry->fstype, token + 7, sizeof(entry->fstype));
        } else if (strncmp(token, "mountprog=", 10) == 0) {
            strlcpy(entry->mountprog, token + 10, sizeof(entry->mountprog));
        } else if (!append_option(entry->options, sizeof(entry->options),
                                  token)) {
            return 0;
        }
    }
    return entry->fstype[0] != '\0';
}

static int parse_media_map_entry(char *line, FreeBSDMediaMapEntry *entry)
{
    char *opts;
    char *location;

    if (!line || !entry)
        return 0;
    memset(entry, 0, sizeof(*entry));

    line[strcspn(line, "\r\n")] = '\0';
    opts = line;
    location = strpbrk(opts, " \t");
    if (!location)
        return 0;
    *location++ = '\0';
    while (*location == ' ' || *location == '\t')
        location++;

    if (strncmp(location, ":/dev/", 6) != 0)
        return 0;
    strlcpy(entry->device, location + 1, sizeof(entry->device));
    if (!freebsd_device_source_allowed(entry->device))
        return 0;
    return parse_media_map_options(opts, entry);
}

static int freebsd_capture_media_map_entry(const char *key,
                                           FreeBSDMediaMapEntry *entry)
{
    char output[1024];
    const char *const argv[] = {
        "special_media", key, NULL
    };

    if (!key || !*key || !entry || strchr(key, '/') ||
        !freebsd_trusted_root_executable("/etc/autofs/special_media"))
        return 0;
    if (!freebsd_capture_safe_argv(
            "/etc/autofs/special_media", argv, output, sizeof(output),
            FREEBSD_CAPTURE_TIMEOUT_MS))
        return 0;
    return parse_media_map_entry(output, entry);
}

static int freebsd_mountprog_allowed(const FreeBSDMediaMapEntry *entry)
{
    if (!entry || !freebsd_trusted_root_executable("/sbin/mount"))
        return 0;
    if (!entry->mountprog[0])
        return 1;
    if (strcmp(entry->fstype, "exfat") == 0 &&
        strcmp(entry->mountprog, "/usr/local/sbin/mount.exfat") == 0)
        return freebsd_trusted_root_executable(entry->mountprog);
    if (strcmp(entry->fstype, "ntfs") == 0 &&
        strcmp(entry->mountprog, "/usr/local/bin/ntfs-3g") == 0)
        return freebsd_trusted_root_executable(entry->mountprog);
    return 0;
}

static int freebsd_fstype_allowed(const char *fstype)
{
    if (!fstype || !fstype[0] || strlen(fstype) >= 64)
        return 0;
    for (const unsigned char *p = (const unsigned char *)fstype; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-')
            return 0;
    }
    return 1;
}

static int freebsd_add_user_mount_options(FreeBSDMediaMapEntry *entry)
{
    char option[64];

    if (!entry || !entry->mountprog[0])
        return 1;
    if (strcmp(entry->fstype, "exfat") != 0 &&
        strcmp(entry->fstype, "ntfs") != 0)
        return 1;

    if (snprintf(option, sizeof(option), "uid=%lu",
                 (unsigned long)getuid()) >= (int)sizeof(option))
        return 0;
    if (!append_option(entry->options, sizeof(entry->options), option))
        return 0;
    if (snprintf(option, sizeof(option), "gid=%lu",
                 (unsigned long)getgid()) >= (int)sizeof(option))
        return 0;
    return append_option(entry->options, sizeof(entry->options), option);
}

static int freebsd_add_safe_mount_options(FreeBSDMediaMapEntry *entry)
{
    char mountprog_option[PATH_MAX + 32];
    static const char *const safety_options[] = {
        "nosuid",
        "noatime",
        "automounted",
        "rw"
    };

    if (!entry || !freebsd_fstype_allowed(entry->fstype) ||
        !freebsd_mountprog_allowed(entry) ||
        !freebsd_add_user_mount_options(entry))
        return 0;

    if (entry->mountprog[0]) {
        if (snprintf(mountprog_option, sizeof(mountprog_option),
                     "mountprog=%s", entry->mountprog) >=
            (int)sizeof(mountprog_option) ||
            !append_option(entry->options, sizeof(entry->options),
                           mountprog_option))
            return 0;
    }
    for (size_t i = 0;
         i < sizeof(safety_options) / sizeof(safety_options[0]); i++) {
        if (!append_option(entry->options, sizeof(entry->options),
                           safety_options[i]))
            return 0;
    }
    return 1;
}

static int freebsd_filesystem_checker(const char *fstype,
                                      FreeBSDFilesystemChecker *checker)
{
    if (!fstype || !checker)
        return 0;
    memset(checker, 0, sizeof(*checker));

    if (strcmp(fstype, "ext2fs") == 0) {
        checker->kind = FREEBSD_CHECKER_E2FSCK;
        checker->path = "/usr/local/sbin/e2fsck";
        checker->check_option = "-n";
        checker->repair_option = "-p";
        /*
         * e2fsck -n can return zero for a structurally sound filesystem whose
         * superblock still says "not clean".  Preen is what clears that state.
         */
        checker->repair_after_clean_check = 1;
    } else if (strcmp(fstype, "ufs") == 0 ||
               strcmp(fstype, "ffs") == 0) {
        checker->kind = FREEBSD_CHECKER_UFS;
        checker->path = "/sbin/fsck_ufs";
        checker->check_option = "-n";
        checker->repair_option = "-p";
        checker->repair_after_clean_check = 1;
    } else if (strcmp(fstype, "msdosfs") == 0) {
        checker->kind = FREEBSD_CHECKER_MSDOSFS;
        checker->path = "/sbin/fsck_msdosfs";
        checker->check_option = "-n";
        checker->repair_option = "-p";
    } else if (strcmp(fstype, "exfat") == 0) {
        checker->kind = FREEBSD_CHECKER_EXFAT;
        checker->path = "/usr/local/sbin/exfatfsck";
        checker->check_option = "-n";
        checker->repair_option = "-p";
    } else if (strcmp(fstype, "ntfs") == 0) {
        checker->kind = FREEBSD_CHECKER_NTFS;
        checker->path = "/usr/local/bin/ntfsfix";
        checker->check_option = "-n";
        checker->repair_option = "-d";
        /*
         * ntfsfix --no-action reports mountability, not the dirty flag.
         * A successful -d pass is required to clear that flag when possible.
         */
        checker->repair_after_clean_check = 1;
    } else {
        return 0;
    }
    return 1;
}

static int freebsd_checker_repair_exit_success(
    const FreeBSDFilesystemChecker *checker, int exit_code)
{
    if (!checker || exit_code < 0)
        return 0;
    /*
     * e2fsck uses a bit mask: 1 means errors corrected and 2 means a reboot
     * would be needed for an in-use system filesystem.  This helper only
     * checks an unmounted removable filesystem and verifies it again before
     * mounting, so 0 through 3 are successful repair outcomes here.
     */
    if (checker->kind == FREEBSD_CHECKER_E2FSCK)
        return (exit_code & ~3) == 0;
    return exit_code == 0;
}

static int freebsd_run_filesystem_checker(
    const FreeBSDFilesystemChecker *checker, const char *device, int repair,
    int *exit_code)
{
    pid_t pid;
    int status = 0;

    if (!checker || !checker->path || !device || !exit_code ||
        !freebsd_device_source_allowed(device) ||
        !freebsd_trusted_root_executable(checker->path))
        return 0;

    pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        const char *option =
            repair ? checker->repair_option : checker->check_option;
        const char *const argv_with_option[] = {
            checker->path, option, device, NULL
        };
        const char *const argv_without_option[] = {
            checker->path, device, NULL
        };
        int devnull = open("/dev/null", O_RDWR);

        (void)setpgid(0, 0);
        if (setgid(0) != 0 || setuid(0) != 0)
            _exit(126);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        closefrom(3);
        freebsd_execve_safe(
            checker->path,
            option ? argv_with_option : argv_without_option);
        _exit(127);
    }

    (void)setpgid(pid, pid);
    if (!freebsd_wait_child_timeout(
            pid, FREEBSD_FILESYSTEM_CHECK_TIMEOUT_MS, &status) ||
        !WIFEXITED(status))
        return 0;
    *exit_code = WEXITSTATUS(status);
    return 1;
}

static int freebsd_filesystem_is_unmounted(const char *device,
                                           const char *mountpoint)
{
    return !freebsd_mountpoint_is_mounted(mountpoint) &&
           !freebsd_device_is_mounted_elsewhere(device, mountpoint);
}

static int freebsd_check_repair_filesystem(const FreeBSDMediaMapEntry *entry,
                                           const char *mountpoint)
{
    FreeBSDFilesystemChecker checker;
    int exit_code;

    if (!entry || !mountpoint ||
        !freebsd_filesystem_checker(entry->fstype, &checker) ||
        !freebsd_trusted_root_executable(checker.path)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: no trusted checker for %s\n",
                entry ? entry->fstype : "");
        return EX_CONFIG;
    }
    if (!freebsd_filesystem_is_unmounted(entry->device, mountpoint))
        return EX_TEMPFAIL;

    if (!freebsd_run_filesystem_checker(
            &checker, entry->device, 0, &exit_code))
        return EX_DATAERR;
    if (exit_code == 0 && !checker.repair_after_clean_check)
        return EX_OK;

    if (!freebsd_filesystem_is_unmounted(entry->device, mountpoint))
        return EX_TEMPFAIL;
    if (!freebsd_run_filesystem_checker(
            &checker, entry->device, 1, &exit_code) ||
        !freebsd_checker_repair_exit_success(&checker, exit_code))
        return EX_DATAERR;

    if (!freebsd_filesystem_is_unmounted(entry->device, mountpoint) ||
        !freebsd_run_filesystem_checker(
            &checker, entry->device, 0, &exit_code) ||
        exit_code != 0)
        return EX_DATAERR;
    return EX_OK;
}

static int run_mount_program(const FreeBSDMediaMapEntry *entry,
                             const char *mountpoint)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);

        (void)setpgid(0, 0);
        if (setgid(0) != 0 || setuid(0) != 0)
            _exit(126);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        closefrom(3);

        if (entry->options[0]) {
            const char *const argv[] = {
                "mount", "-t", entry->fstype, "-o", entry->options,
                entry->device, mountpoint, NULL
            };
            freebsd_execve_safe("/sbin/mount", argv);
        } else {
            const char *const argv[] = {
                "mount", "-t", entry->fstype, entry->device,
                mountpoint, NULL
            };
            freebsd_execve_safe("/sbin/mount", argv);
        }
        _exit(127);
    }

    (void)setpgid(pid, pid);
    return freebsd_wait_child_timeout(
               pid, FREEBSD_MOUNT_TIMEOUT_MS, &status) &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int freebsd_run_mount_update(const char *mountpoint,
                                    const char *options)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        const char *const argv[] = {
            "mount", "-u", "-o", options, mountpoint, NULL
        };
        int devnull = open("/dev/null", O_RDWR);

        (void)setpgid(0, 0);
        if (setgid(0) != 0 || setuid(0) != 0)
            _exit(126);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        closefrom(3);
        freebsd_execve_safe("/sbin/mount", argv);
        _exit(127);
    }

    (void)setpgid(pid, pid);
    return freebsd_wait_child_timeout(
               pid, FREEBSD_MOUNT_TIMEOUT_MS, &status) &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int freebsd_update_mount_safety(const char *mountpoint)
{
    /*
     * MNT_UNTRUSTED is useful where a filesystem accepts it, but ext2fs on
     * this FreeBSD release rejects it as an unknown option.  Retry with the
     * universally supported persistent flags instead of making that media
     * impossible to mount.
     */
    return freebsd_run_mount_update(
               mountpoint,
               "rw,nosuid,noatime,untrusted,automounted") ||
           freebsd_run_mount_update(
               mountpoint,
               "rw,nosuid,noatime,automounted");
}

static int
freebsd_mount_record_has_required_safety(const struct statfs *mount_record)
{
    uint64_t required = MNT_NOSUID | MNT_NOATIME | MNT_AUTOMOUNTED;

    return mount_record &&
           (mount_record->f_flags & required) == required &&
           (mount_record->f_flags & MNT_RDONLY) == 0;
}

static int freebsd_mount_has_required_safety(const char *mountpoint,
                                             const char *device)
{
    struct statfs mount_record;

    return freebsd_find_mount_record(mountpoint, &mount_record) &&
           freebsd_same_device(device, mount_record.f_mntfromname) &&
           freebsd_mount_record_has_required_safety(&mount_record);
}

static int freebsd_mount_media(const char *requested,
                               const char *requested_device)
{
    char mountpoint[PATH_MAX];
    char key[NAME_MAX + 1];
    const char *provider;
    struct stat st;
    struct statfs existing_mount;
    FreeBSDMediaMapEntry entry;
    int created_mountpoint = 0;
    int health_result;
    int mounted = 0;

    if (!freebsd_media_mount_request(requested, mountpoint, sizeof(mountpoint),
                                     key, sizeof(key))) {
        fprintf(stderr, "simplefiles-freebsd-unmount: bad media path: %s\n",
                requested ? requested : "");
        return EX_USAGE;
    }
    if (freebsd_find_mount_record(mountpoint, &existing_mount)) {
        if (!requested_device ||
            !freebsd_same_device(requested_device,
                                 existing_mount.f_mntfromname))
            return EX_TEMPFAIL;
        if (freebsd_mount_has_required_safety(mountpoint,
                                              requested_device) ||
            (geteuid() == 0 &&
             freebsd_update_mount_safety(mountpoint) &&
             freebsd_mount_has_required_safety(mountpoint,
                                               requested_device)))
            return EX_OK;
        return EX_OSERR;
    }
    if (geteuid() != 0) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: helper not privileged\n");
        return EX_NOPERM;
    }
    if (!freebsd_caller_can_read_device(requested_device)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: caller cannot access device: %s\n",
                requested_device ? requested_device : "");
        return EX_NOPERM;
    }
    provider = requested_device + strlen("/dev/");
    if (!freebsd_capture_media_map_entry(provider, &entry) ||
        !freebsd_same_device(requested_device, entry.device) ||
        !freebsd_add_safe_mount_options(&entry)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: refusing media mount: %s\n",
                mountpoint);
        return EX_UNAVAILABLE;
    }
    if (!freebsd_filesystem_is_unmounted(entry.device, mountpoint)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: device already mounted: %s\n",
                entry.device);
        return EX_TEMPFAIL;
    }
    health_result = freebsd_check_repair_filesystem(&entry, mountpoint);
    if (health_result != EX_OK) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: filesystem check/repair "
                "failed: %s\n",
                entry.device);
        return health_result;
    }
    if (!freebsd_filesystem_is_unmounted(entry.device, mountpoint))
        return EX_TEMPFAIL;

    if (stat(mountpoint, &st) != 0) {
        if (errno != ENOENT || mkdir(mountpoint, 0755) != 0) {
            fprintf(stderr,
                    "simplefiles-freebsd-unmount: cannot create mountpoint: %s\n",
                    mountpoint);
            return EX_CANTCREAT;
        }
        created_mountpoint = 1;
        (void)chmod(mountpoint, 0755);
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "simplefiles-freebsd-unmount: not a directory: %s\n",
                mountpoint);
        return EX_UNAVAILABLE;
    }
    if (freebsd_device_is_mounted_elsewhere(entry.device, mountpoint)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: device already mounted: %s\n",
                entry.device);
        return EX_TEMPFAIL;
    }
    if (run_mount_program(&entry, mountpoint)) {
        for (int i = 0; i < 20; i++) {
            if (freebsd_mountpoint_is_mounted(mountpoint)) {
                mounted = 1;
                break;
            }
            usleep(100000);
        }
    }
    if (!mounted && !freebsd_mountpoint_is_mounted(mountpoint)) {
        fprintf(stderr, "simplefiles-freebsd-unmount: mount failed: %s\n",
                mountpoint);
        if (created_mountpoint)
            rmdir(mountpoint);
        return EX_OSERR;
    }

    if (!freebsd_mount_has_required_safety(mountpoint, entry.device) &&
        (!freebsd_update_mount_safety(mountpoint) ||
         !freebsd_mount_has_required_safety(mountpoint, entry.device))) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: unsafe mount flags: %s\n",
                mountpoint);
        (void)freebsd_unmount_media(mountpoint);
        if (created_mountpoint && !freebsd_mountpoint_is_mounted(mountpoint))
            rmdir(mountpoint);
        return EX_OSERR;
    }
    return EX_OK;
}

static int freebsd_unmount_media(const char *mountpoint)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
        return EX_OSERR;
    if (pid == 0) {
        (void)setpgid(0, 0);
        if (setgid(0) != 0 || setuid(0) != 0)
            _exit(EX_NOPERM);
        closefrom(3);
        if (unmount(mountpoint, 0) == 0)
            _exit(EX_OK);
        if (errno == EBUSY)
            _exit(EX_TEMPFAIL);
        if (errno == EPERM || errno == EACCES)
            _exit(EX_NOPERM);
        _exit(EX_OSERR);
    }

    (void)setpgid(pid, pid);
    if (!freebsd_wait_child_timeout(
            pid, FREEBSD_UNMOUNT_TIMEOUT_MS, &status))
        return EX_TEMPFAIL;
    if (!WIFEXITED(status))
        return EX_OSERR;
    return WEXITSTATUS(status);
}

#ifndef SIMPLEFILES_FREEBSD_UNMOUNT_TEST
int main(int argc, char **argv)
{
    char mountpoint[PATH_MAX];
    struct statfs mount_record;
    int result;

    (void)umask(022);

    if (argc == 4 && strcmp(argv[1], "--mount") == 0)
        return freebsd_mount_media(argv[2], argv[3]);

    if (argc != 2) {
        fprintf(stderr,
                "usage: simplefiles-freebsd-unmount MOUNTPOINT\n"
                "       simplefiles-freebsd-unmount --mount MOUNTPOINT DEVICE\n");
        return EX_USAGE;
    }

    if (!realpath(argv[1], mountpoint)) {
        fprintf(stderr, "simplefiles-freebsd-unmount: %s: %s\n", argv[1],
                strerror(errno));
        return EX_UNAVAILABLE;
    }

    if (!freebsd_find_mount_record(mountpoint, &mount_record) ||
        !freebsd_mount_record_allowed(&mount_record, mountpoint)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: refusing non-media mount: %s\n",
                mountpoint);
        return EX_UNAVAILABLE;
    }

    if (geteuid() != 0) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: helper not privileged\n");
        return EX_NOPERM;
    }

    result = freebsd_unmount_media(mountpoint);
    if (result == EX_OK && freebsd_mountpoint_is_mounted(mountpoint))
        result = EX_OSERR;
    if (result != EX_OK) {
        fprintf(stderr, "simplefiles-freebsd-unmount: unmount failed: %s\n",
                mountpoint);
    }
    return result;
}
#endif
