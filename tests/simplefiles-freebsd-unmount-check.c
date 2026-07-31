#define SIMPLEFILES_FREEBSD_UNMOUNT_TEST
#include "../simplefiles-freebsd-unmount.c"
#undef SIMPLEFILES_FREEBSD_UNMOUNT_TEST

#include <assert.h>
#include <stdint.h>

static void write_test_script(const char *path, const char *marker)
{
    char script[PATH_MAX * 2];
    int fd;
    int length;

    length = snprintf(script, sizeof(script),
                      "#!/bin/sh\n: > \"%s\"\nexit 1\n", marker);
    assert(length > 0 && (size_t)length < sizeof(script));
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0700);
    assert(fd >= 0);
    assert(write(fd, script, (size_t)length) == length);
    assert(close(fd) == 0);
}

static void set_mount_record(struct statfs *record, const char *from,
                             const char *on, uint64_t flags)
{
    memset(record, 0, sizeof(*record));
    strlcpy(record->f_mntfromname, from, sizeof(record->f_mntfromname));
    strlcpy(record->f_mntonname, on, sizeof(record->f_mntonname));
    record->f_flags = flags;
}

static void test_privileged_child_environment(void)
{
    char temp[] = "/tmp/simplefiles-helper-env.XXXXXX";
    char fake_sysctl[PATH_MAX];
    char marker[PATH_MAX];
    const char *old_path;
    char *saved_path = NULL;
    FreeBSDMediaMapEntry entry;

    assert(mkdtemp(temp));
    assert(snprintf(fake_sysctl, sizeof(fake_sysctl), "%s/sysctl", temp) <
           (int)sizeof(fake_sysctl));
    assert(snprintf(marker, sizeof(marker), "%s/caller-path-was-used", temp) <
           (int)sizeof(marker));
    write_test_script(fake_sysctl, marker);

    old_path = getenv("PATH");
    if (old_path) {
        saved_path = strdup(old_path);
        assert(saved_path);
    }
    assert(setenv("PATH", temp, 1) == 0);
    (void)freebsd_capture_media_map_entry(
        "SIMPLESUITE_ENVIRONMENT_TEST_DO_NOT_MOUNT", &entry);
    assert(access(marker, F_OK) != 0);
    if (saved_path) {
        assert(setenv("PATH", saved_path, 1) == 0);
        free(saved_path);
    } else {
        assert(unsetenv("PATH") == 0);
    }

    assert(unlink(fake_sysctl) == 0);
    assert(rmdir(temp) == 0);
}

static void test_capture_timeout(void)
{
    const char *const argv[] = {
        "sh", "-c", "sleep 5; echo late", NULL
    };
    char output[64];
    long long started = freebsd_monotonic_milliseconds();

    assert(!freebsd_capture_safe_argv(
        "/bin/sh", argv, output, sizeof(output), 100));
    assert(freebsd_monotonic_milliseconds() - started < 2000);
}

int main(void)
{
    struct statfs record;
    struct statfs mounts[2];
    struct statfs copied;
    FreeBSDMediaMapEntry entry;
    FreeBSDFilesystemChecker checker;

    (void)freebsd_mount_record_allowed;
    (void)freebsd_mount_media;
    (void)freebsd_unmount_media;

    assert(freebsd_media_mount_path_allowed("/media/T7"));
    assert(freebsd_media_mount_path_allowed("/media/New Volume"));
    assert(!freebsd_media_mount_path_allowed("/media"));
    assert(!freebsd_media_mount_path_allowed("/mediax/T7"));
    assert(!freebsd_media_mount_path_allowed("/media/.hidden"));
    assert(!freebsd_media_mount_path_allowed("/media/T7/music"));
    assert(!freebsd_media_mount_path_allowed("/run/media"));
    assert(!freebsd_media_mount_path_allowed_for_user(
        "/run/media/alice", "alice"));
    assert(freebsd_media_mount_path_allowed_for_user(
        "/run/media/alice/T7", "alice"));
    assert(!freebsd_media_mount_path_allowed_for_user(
        "/run/media/bob/T7", "alice"));
    assert(!freebsd_media_mount_path_allowed_for_user(
        "/run/media/alice/T7/music", "alice"));

    assert(freebsd_device_source_allowed("/dev/null"));
    assert(!freebsd_device_source_allowed("/etc/passwd"));
    assert(!freebsd_device_source_allowed("dev/null"));
    assert(!freebsd_device_source_allowed("/dev/../etc/passwd"));
    assert(freebsd_same_device("/dev/null", "/dev/null"));
    assert(!freebsd_same_device("/dev/null", "/dev/zero"));

    set_mount_record(&record, "/dev/null", "/media/T7", MNT_AUTOMOUNTED);
    assert(freebsd_mount_record_policy_allowed(&record, "/media/T7"));

    set_mount_record(&record, "/dev/null", "/media/T7", 0);
    assert(!freebsd_mount_record_policy_allowed(&record, "/media/T7"));

    set_mount_record(&record, "/dev/null", "/media/T7",
                     MNT_NOSUID | MNT_NOATIME | MNT_AUTOMOUNTED |
                     MNT_UNTRUSTED);
    strlcpy(record.f_fstypename, "ext2fs",
            sizeof(record.f_fstypename));
    assert(freebsd_mount_record_has_required_safety(&record));
    record.f_flags &= ~MNT_NOSUID;
    assert(!freebsd_mount_record_has_required_safety(&record));
    record.f_flags |= MNT_NOSUID;
    record.f_flags &= ~MNT_UNTRUSTED;
    assert(freebsd_mount_record_has_required_safety(&record));
    record.f_flags &= ~MNT_NOATIME;
    assert(!freebsd_mount_record_has_required_safety(&record));
    record.f_flags |= MNT_NOATIME | MNT_RDONLY;
    assert(!freebsd_mount_record_has_required_safety(&record));

    set_mount_record(&record, "/dev/null", "/mnt/T7", MNT_AUTOMOUNTED);
    assert(!freebsd_mount_record_policy_allowed(&record, "/mnt/T7"));

    set_mount_record(&record, "/etc/passwd", "/media/T7", MNT_AUTOMOUNTED);
    assert(!freebsd_mount_record_policy_allowed(&record, "/media/T7"));

    set_mount_record(&record, "/dev/null", "/media/T7", MNT_AUTOMOUNTED);
    assert(!freebsd_mount_record_policy_allowed(&record, "/media/Other"));

    set_mount_record(&mounts[0], "/dev/null", "/media/T7",
                     MNT_AUTOMOUNTED);
    set_mount_record(&mounts[1], "/dev/zero", "/media/Other", 0);
    assert(freebsd_copy_mount_record(mounts, 2, "/media/T7", &copied));
    memset(&mounts[0], 0, sizeof(mounts[0]));
    assert(strcmp(copied.f_mntfromname, "/dev/null") == 0);
    assert(strcmp(copied.f_mntonname, "/media/T7") == 0);
    assert((copied.f_flags & MNT_AUTOMOUNTED) != 0);
    assert(!freebsd_copy_mount_record(mounts, 2, "/media/Missing", &copied));

    memset(&entry, 0, sizeof(entry));
    strlcpy(entry.fstype, "ext2fs", sizeof(entry.fstype));
    strlcpy(entry.options, "async", sizeof(entry.options));
    assert(freebsd_add_safe_mount_options(&entry));
    assert(strstr(entry.options, "async"));
    assert(strstr(entry.options, "nosuid"));
    assert(strstr(entry.options, "noatime"));
    assert(strstr(entry.options, "automounted"));
    assert(strstr(entry.options, "rw"));
    assert(!strstr(entry.options, "autoro"));
    assert(!strstr(entry.options, ",ro"));

    assert(freebsd_filesystem_checker("ext2fs", &checker));
    assert(checker.kind == FREEBSD_CHECKER_E2FSCK);
    assert(strcmp(checker.path, "/usr/local/sbin/e2fsck") == 0);
    assert(strcmp(checker.check_option, "-n") == 0);
    assert(strcmp(checker.repair_option, "-p") == 0);
    assert(checker.repair_after_clean_check);
    assert(freebsd_checker_repair_exit_success(&checker, 0));
    assert(freebsd_checker_repair_exit_success(&checker, 1));
    assert(freebsd_checker_repair_exit_success(&checker, 2));
    assert(freebsd_checker_repair_exit_success(&checker, 3));
    assert(!freebsd_checker_repair_exit_success(&checker, 4));
    assert(!freebsd_checker_repair_exit_success(&checker, 8));

    assert(freebsd_filesystem_checker("ufs", &checker));
    assert(checker.kind == FREEBSD_CHECKER_UFS);
    assert(strcmp(checker.path, "/sbin/fsck_ufs") == 0);
    assert(checker.repair_after_clean_check);
    assert(freebsd_filesystem_checker("ffs", &checker));
    assert(checker.kind == FREEBSD_CHECKER_UFS);
    assert(freebsd_filesystem_checker("msdosfs", &checker));
    assert(checker.kind == FREEBSD_CHECKER_MSDOSFS);
    assert(strcmp(checker.path, "/sbin/fsck_msdosfs") == 0);
    assert(!checker.repair_after_clean_check);
    assert(freebsd_filesystem_checker("exfat", &checker));
    assert(checker.kind == FREEBSD_CHECKER_EXFAT);
    assert(strcmp(checker.path, "/usr/local/sbin/exfatfsck") == 0);
    assert(!checker.repair_after_clean_check);
    assert(freebsd_filesystem_checker("ntfs", &checker));
    assert(checker.kind == FREEBSD_CHECKER_NTFS);
    assert(strcmp(checker.path, "/usr/local/bin/ntfsfix") == 0);
    assert(strcmp(checker.repair_option, "-d") == 0);
    assert(checker.repair_after_clean_check);
    assert(freebsd_checker_repair_exit_success(&checker, 0));
    assert(!freebsd_checker_repair_exit_success(&checker, 1));
    assert(!freebsd_filesystem_checker("zfs", &checker));
    assert(!freebsd_filesystem_checker("../evil", &checker));

    memset(&entry, 0, sizeof(entry));
    strlcpy(entry.fstype, "../evil", sizeof(entry.fstype));
    assert(!freebsd_add_safe_mount_options(&entry));

    test_privileged_child_environment();
    test_capture_timeout();

    return 0;
}
