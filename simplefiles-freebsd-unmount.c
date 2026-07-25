#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

typedef struct {
    char device[PATH_MAX];
    char fstype[64];
    char mountprog[PATH_MAX];
    char options[256];
} FreeBSDMediaMapEntry;

static int freebsd_path_has_prefix_component(const char *path,
                                             const char *prefix)
{
    size_t len;

    if (!path || !prefix)
        return 0;
    len = strlen(prefix);
    return strncmp(path, prefix, len) == 0 && path[len] != '\0';
}

static int freebsd_media_mount_path_allowed(const char *path)
{
    const char *run_media = "/run/media/";
    const char *rest;
    const char *slash;

    if (freebsd_path_has_prefix_component(path, "/media/"))
        return 1;

    if (strncmp(path, run_media, strlen(run_media)) != 0)
        return 0;
    rest = path + strlen(run_media);
    slash = strchr(rest, '/');
    return slash && slash[1] != '\0';
}

static int freebsd_media_mount_key(const char *path, char *key, size_t keysz)
{
    const char *prefix = "/media/";
    const char *start;

    if (!path || !key || keysz == 0 ||
        strncmp(path, prefix, strlen(prefix)) != 0)
        return 0;
    start = path + strlen(prefix);
    if (!*start || strchr(start, '/') || strlen(start) >= keysz)
        return 0;
    if (strcmp(start, ".") == 0 || strcmp(start, "..") == 0)
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

    if (!source || strncmp(source, "/dev/", 5) != 0)
        return 0;
    if (stat(source, &st) != 0)
        return 0;
    return S_ISCHR(st.st_mode);
}

static int freebsd_mount_record_allowed(const struct statfs *mount_record,
                                        const char *mountpoint)
{
    struct stat st;

    if (!mount_record || !mountpoint)
        return 0;
    if (strcmp(mount_record->f_mntonname, mountpoint) != 0)
        return 0;
    if (!freebsd_media_mount_path_allowed(mount_record->f_mntonname))
        return 0;
    if (!freebsd_device_source_allowed(mount_record->f_mntfromname))
        return 0;
    if (access(mountpoint, X_OK) != 0)
        return 0;
    if (stat(mountpoint, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;
    return 1;
}

static const struct statfs *freebsd_find_mount_record(const char *mountpoint)
{
    struct statfs *mounts;
    int count;

    count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0)
        return NULL;

    for (int i = 0; i < count; i++) {
        if (strcmp(mounts[i].f_mntonname, mountpoint) == 0)
            return &mounts[i];
    }
    return NULL;
}

static int freebsd_mountpoint_is_mounted(const char *mountpoint)
{
    return freebsd_find_mount_record(mountpoint) != NULL;
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

static int append_option(char *out, size_t outsz, const char *option)
{
    if (!option || !*option)
        return 1;
    if (strlcat(out, out[0] ? "," : "", outsz) >= outsz)
        return 0;
    return strlcat(out, option, outsz) < outsz;
}

static int freebsd_media_label_matches_key(const char *label, const char *key)
{
    char cleaned[NAME_MAX + 1];
    size_t len;

    if (!label || !key)
        return 0;
    len = strlen(label);
    if (len == 0 || len >= sizeof(cleaned))
        return 0;
    for (size_t i = 0; i < len; i++) {
        cleaned[i] = (label[i] == '+' || label[i] == '/') ? '-' : label[i];
    }
    cleaned[len] = '\0';
    return strcmp(cleaned, key) == 0;
}

static int parse_fstyp_label(char *line, char *label, size_t labelsz)
{
    char *space;

    if (!line || !label || labelsz == 0)
        return 0;
    label[0] = '\0';
    line[strcspn(line, "\r\n")] = '\0';

    space = line;
    while (*space && !isspace((unsigned char)*space))
        space++;
    while (*space && isspace((unsigned char)*space))
        space++;
    if (!*space)
        return 0;
    strlcpy(label, space, labelsz);
    return label[0] != '\0';
}

static int capture_fstyp_label(const char *device, char *label, size_t labelsz)
{
    int fds[2];
    pid_t pid;
    char output[NAME_MAX + 128];
    size_t used = 0;
    int status = 0;

    if (!device || !label || labelsz == 0)
        return 0;
    label[0] = '\0';
    if (pipe(fds) != 0)
        return 0;

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        if (fds[1] > 2)
            close(fds[1]);
        execl("/usr/sbin/fstyp", "fstyp", "-l", device, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    while (used + 1 < sizeof(output)) {
        ssize_t n = read(fds[0], output + used, sizeof(output) - used - 1);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fds[0]);
    output[used] = '\0';

    do {
        if (waitpid(pid, &status, 0) == pid)
            break;
    } while (errno == EINTR);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0)
        return 0;
    return parse_fstyp_label(output, label, labelsz);
}

static int freebsd_geom_provider_for_label(const char *key, char *provider,
                                           size_t providersz)
{
    char *confdot;
    size_t confdot_len = 0;
    const char *scan;

    if (!key || !key[0] || !provider || providersz == 0)
        return 0;
    provider[0] = '\0';

    if (sysctlbyname("kern.geom.confdot", NULL, &confdot_len, NULL, 0) != 0 ||
        confdot_len == 0)
        return 0;
    confdot = malloc(confdot_len + 1);
    if (!confdot)
        return 0;
    if (sysctlbyname("kern.geom.confdot", confdot, &confdot_len, NULL, 0) !=
        0) {
        free(confdot);
        return 0;
    }
    confdot[confdot_len] = '\0';

    scan = confdot;
    while ((scan = strstr(scan, "label=\"")) != NULL) {
        const char *name_start = scan + strlen("label=\"");
        const char *label_end = strchr(name_start, '"');
        const char *name_end = strstr(name_start, "\\n");
        char candidate[NAME_MAX + 1];
        char device[PATH_MAX];
        char label[NAME_MAX + 1];
        size_t len;

        if (!label_end)
            break;
        scan = label_end + 1;
        if (!name_end || name_end > label_end)
            continue;
        len = (size_t)(name_end - name_start);
        if (len == 0 || len >= sizeof(candidate))
            continue;
        memcpy(candidate, name_start, len);
        candidate[len] = '\0';
        if (strchr(candidate, '/'))
            continue;
        if (snprintf(device, sizeof(device), "/dev/%s", candidate) >=
            (int)sizeof(device))
            continue;
        if (capture_fstyp_label(device, label, sizeof(label)) &&
            freebsd_media_label_matches_key(label, key)) {
            strlcpy(provider, candidate, providersz);
            free(confdot);
            return provider[0] != '\0';
        }
    }

    free(confdot);
    return 0;
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
    int fds[2];
    pid_t pid;
    char output[1024];
    size_t used = 0;
    int status = 0;

    if (!key || !*key || !entry || strchr(key, '/'))
        return 0;
    if (pipe(fds) != 0)
        return 0;

    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        if (fds[1] > 2)
            close(fds[1]);
        execl("/etc/autofs/special_media", "special_media", key, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    while (used + 1 < sizeof(output)) {
        ssize_t n = read(fds[0], output + used, sizeof(output) - used - 1);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fds[0]);
    output[used] = '\0';

    do {
        if (waitpid(pid, &status, 0) == pid)
            break;
    } while (errno == EINTR);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0)
        return 0;
    return parse_media_map_entry(output, entry);
}

static int freebsd_capture_media_map_entry_for_key(const char *key,
                                                   FreeBSDMediaMapEntry *entry)
{
    char provider[NAME_MAX + 1];

    if (freebsd_capture_media_map_entry(key, entry))
        return 1;
    if (!freebsd_geom_provider_for_label(key, provider, sizeof(provider)))
        return 0;
    return freebsd_capture_media_map_entry(provider, entry);
}

static int freebsd_mountprog_allowed(const FreeBSDMediaMapEntry *entry)
{
    if (!entry->mountprog[0])
        return 1;
    if (strcmp(entry->fstype, "exfat") == 0 &&
        strcmp(entry->mountprog, "/usr/local/sbin/mount.exfat") == 0)
        return 1;
    if (strcmp(entry->fstype, "ntfs") == 0 &&
        strcmp(entry->mountprog, "/usr/local/bin/ntfs-3g") == 0)
        return 1;
    return 0;
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

static int run_mount_program(const FreeBSDMediaMapEntry *entry,
                             const char *mountpoint)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        if (setgid(0) != 0 || setuid(0) != 0)
            _exit(126);

        if (entry->mountprog[0]) {
            if (entry->options[0]) {
                execl(entry->mountprog, entry->mountprog, "-o",
                      entry->options, entry->device, mountpoint, (char *)NULL);
            } else {
                execl(entry->mountprog, entry->mountprog, entry->device,
                      mountpoint, (char *)NULL);
            }
        } else if (entry->options[0]) {
            execl("/sbin/mount", "mount", "-t", entry->fstype, "-o",
                  entry->options, entry->device, mountpoint, (char *)NULL);
        } else {
            execl("/sbin/mount", "mount", "-t", entry->fstype, entry->device,
                  mountpoint, (char *)NULL);
        }
        _exit(127);
    }

    do {
        if (waitpid(pid, &status, 0) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    } while (errno == EINTR);

    return 0;
}

static int freebsd_mount_media(const char *requested)
{
    char mountpoint[PATH_MAX];
    char key[NAME_MAX + 1];
    struct stat st;
    FreeBSDMediaMapEntry entry;
    int created_mountpoint = 0;

    if (!freebsd_media_mount_request(requested, mountpoint, sizeof(mountpoint),
                                     key, sizeof(key))) {
        fprintf(stderr, "simplefiles-freebsd-unmount: bad media path: %s\n",
                requested ? requested : "");
        return EX_USAGE;
    }
    if (freebsd_mountpoint_is_mounted(mountpoint))
        return EX_OK;
    if (geteuid() != 0) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: helper not privileged\n");
        return EX_NOPERM;
    }
    if (!freebsd_capture_media_map_entry_for_key(key, &entry) ||
        !freebsd_mountprog_allowed(&entry) ||
        !freebsd_add_user_mount_options(&entry)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: refusing media mount: %s\n",
                mountpoint);
        return EX_UNAVAILABLE;
    }
    if (stat(mountpoint, &st) != 0) {
        if (errno != ENOENT || mkdir(mountpoint, 0755) != 0) {
            fprintf(stderr,
                    "simplefiles-freebsd-unmount: cannot create mountpoint: %s\n",
                    mountpoint);
            return EX_CANTCREAT;
        }
        created_mountpoint = 1;
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
            if (freebsd_mountpoint_is_mounted(mountpoint))
                return EX_OK;
            usleep(100000);
        }
    }
    if (!freebsd_mountpoint_is_mounted(mountpoint)) {
        fprintf(stderr, "simplefiles-freebsd-unmount: mount failed: %s\n",
                mountpoint);
        if (created_mountpoint)
            rmdir(mountpoint);
        return EX_OSERR;
    }
    return EX_OK;
}

static int freebsd_run_automount_unmount(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);

        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        execl("/usr/sbin/automount", "automount", "-u", (char *)NULL);
        _exit(127);
    }

    do {
        if (waitpid(pid, &status, 0) == pid)
            return WIFEXITED(status);
    } while (errno == EINTR);

    return 0;
}

#ifndef SIMPLEFILES_FREEBSD_UNMOUNT_TEST
int main(int argc, char **argv)
{
    char mountpoint[PATH_MAX];
    const struct statfs *mount_record;

    if (argc == 3 && strcmp(argv[1], "--mount") == 0)
        return freebsd_mount_media(argv[2]);

    if (argc != 2) {
        fprintf(stderr,
                "usage: simplefiles-freebsd-unmount [--mount] MOUNTPOINT\n");
        return EX_USAGE;
    }

    if (!realpath(argv[1], mountpoint)) {
        fprintf(stderr, "simplefiles-freebsd-unmount: %s: %s\n", argv[1],
                strerror(errno));
        return EX_UNAVAILABLE;
    }

    mount_record = freebsd_find_mount_record(mountpoint);
    if (!freebsd_mount_record_allowed(mount_record, mountpoint)) {
        fprintf(stderr,
                "simplefiles-freebsd-unmount: refusing non-media mount: %s\n",
                mountpoint);
        return EX_UNAVAILABLE;
    }

    if (unmount(mountpoint, 0) != 0) {
        int saved_errno = errno;

        if (saved_errno == EBUSY) {
            if (freebsd_run_automount_unmount() &&
                !freebsd_mountpoint_is_mounted(mountpoint))
                return EX_OK;
            if ((mount_record->f_flags & MNT_AUTOMOUNTED) != 0) {
                sync();
                if (unmount(mountpoint, MNT_FORCE) == 0)
                    return EX_OK;
            }
        }

        fprintf(stderr, "simplefiles-freebsd-unmount: unmount %s: %s\n",
                mountpoint, strerror(saved_errno));
        if (saved_errno == EBUSY)
            return EX_TEMPFAIL;
        if (saved_errno == EPERM || saved_errno == EACCES)
            return EX_NOPERM;
        return EX_OSERR;
    }

    return EX_OK;
}
#endif
