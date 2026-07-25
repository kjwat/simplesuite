#define SIMPLEFILES_FREEBSD_UNMOUNT_TEST
#include "../simplefiles-freebsd-unmount.c"
#undef SIMPLEFILES_FREEBSD_UNMOUNT_TEST

#include <assert.h>
#include <stdint.h>

static void set_mount_record(struct statfs *record, const char *from,
                             const char *on, uint64_t flags)
{
    memset(record, 0, sizeof(*record));
    strlcpy(record->f_mntfromname, from, sizeof(record->f_mntfromname));
    strlcpy(record->f_mntonname, on, sizeof(record->f_mntonname));
    record->f_flags = flags;
}

int main(void)
{
    struct statfs record;

    assert(freebsd_media_mount_path_allowed("/media/T7"));
    assert(freebsd_media_mount_path_allowed("/media/New Volume"));
    assert(!freebsd_media_mount_path_allowed("/media"));
    assert(!freebsd_media_mount_path_allowed("/mediax/T7"));
    assert(!freebsd_media_mount_path_allowed("/run/media"));
    assert(!freebsd_media_mount_path_allowed("/run/media/alice"));
    assert(freebsd_media_mount_path_allowed("/run/media/alice/T7"));

    assert(freebsd_device_source_allowed("/dev/null"));
    assert(!freebsd_device_source_allowed("/etc/passwd"));
    assert(!freebsd_device_source_allowed("dev/null"));
    assert(freebsd_media_label_matches_key("New Volume", "New Volume"));
    assert(freebsd_media_label_matches_key("A+B/C", "A-B-C"));
    assert(!freebsd_media_label_matches_key("New Volume", "New"));

    set_mount_record(&record, "/dev/null", "/media/T7", MNT_AUTOMOUNTED);
    assert(freebsd_mount_record_allowed(&record, "/media/T7"));

    set_mount_record(&record, "/dev/null", "/media/T7", 0);
    assert(freebsd_mount_record_allowed(&record, "/media/T7"));

    set_mount_record(&record, "/dev/null", "/mnt/T7", MNT_AUTOMOUNTED);
    assert(!freebsd_mount_record_allowed(&record, "/mnt/T7"));

    set_mount_record(&record, "/etc/passwd", "/media/T7", MNT_AUTOMOUNTED);
    assert(!freebsd_mount_record_allowed(&record, "/media/T7"));

    set_mount_record(&record, "/dev/null", "/media/T7", MNT_AUTOMOUNTED);
    assert(!freebsd_mount_record_allowed(&record, "/media/Other"));

    return 0;
}
