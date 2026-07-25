#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

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
    if ((mount_record->f_flags & MNT_AUTOMOUNTED) == 0)
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

#ifndef SIMPLEFILES_FREEBSD_UNMOUNT_TEST
int main(int argc, char **argv)
{
    char mountpoint[PATH_MAX];
    const struct statfs *mount_record;

    if (argc != 2) {
        fprintf(stderr, "usage: simplefiles-freebsd-unmount MOUNTPOINT\n");
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
