#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main

#include <assert.h>

#ifdef __FreeBSD__
#define APPEND_UNMOUNTED_DRIVES(listing, count, capacity, snapshot,           \
                                snapshot_count, context, skip_existing)       \
    append_unmounted_drives_from_snapshot((listing), (count), (capacity),      \
                                          (snapshot), (snapshot_count),        \
                                          (context), (skip_existing))
#else
#define APPEND_UNMOUNTED_DRIVES(listing, count, capacity, snapshot,           \
                                snapshot_count, context, skip_existing)       \
    append_unmounted_drives_from_snapshot((listing), (count), (capacity),      \
                                          (snapshot), (snapshot_count),        \
                                          (context))
#endif

static void set_drive(DriveRecord *record, const char *id, const char *name,
                      const char *device, int mounted, int can_mount,
                      int removable)
{
    memset(record, 0, sizeof(*record));
    safe_copy(record->id, sizeof(record->id), id);
    safe_copy(record->name, sizeof(record->name), name);
    safe_copy(record->device, sizeof(record->device), device);
    record->mounted = mounted;
    record->can_mount = can_mount;
    record->removable = removable;
}

int main(void)
{
    DriveRecord snapshot[4];
    Entry listing[8] = {0};
    char ignored_device[PATH_MAX] = "";
    char media_root[PATH_MAX] = "";
    char media_key[NAME_MAX + 1] = "";
    const char *media_rest = NULL;
    int includes_user = -1;
    int count;

    assert(media_boundary_for_user("/media", "alice", &includes_user) == 0);
    assert(includes_user == 0);
    assert(media_boundary_for_user("/media/", "alice", &includes_user) == 0);
    assert(includes_user == 0);
    assert(media_boundary_for_user("/run/media/alice", "alice",
                                   &includes_user) == 1);
    assert(includes_user == 1);
    assert(media_boundary_for_user("/run/media/alice/", "alice",
                                   &includes_user) == 1);
    assert(includes_user == 1);
    assert(media_boundary_for_user("/run/media/alice/T7", "alice",
                                   &includes_user) == -1);
    assert(media_boundary_for_user("/media/alice/empty", "alice",
                                   &includes_user) == -1);

    assert(choose_media_root(0, 0, 1, 1, 1) == 1);
    assert(choose_media_root(1, 1, 0, 1, 1) == 0);
    assert(choose_media_root(0, 0, 0, 1, 1) == 0);
    assert(choose_media_root(0, 0, 0, 0, 1) == 1);
    assert(choose_media_root(1, 0, 0, 0, 0) == 1);

    assert(path_is_at_or_below("/run/media/alice/T7", "/run/media/alice/T7"));
    assert(path_is_at_or_below("/run/media/alice/T7/music",
                               "/run/media/alice/T7"));
    assert(!path_is_at_or_below("/run/media/alice/T7-backup",
                                "/run/media/alice/T7"));
#ifdef __FreeBSD__
    assert(freebsd_media_mount_path_allowed("/media/T7"));
    assert(freebsd_media_mount_path_allowed("/media/New Volume"));
    assert(!freebsd_media_mount_path_allowed("/media"));
    assert(!freebsd_media_mount_path_allowed("/mediax/T7"));
    assert(!freebsd_media_mount_path_allowed("/run/media/alice"));
    assert(freebsd_media_mount_path_allowed("/run/media/alice/T7"));
    assert(freebsd_media_request_parts("/media/New Volume/Flac",
                                       media_root, sizeof(media_root),
                                       media_key, sizeof(media_key),
                                       &media_rest));
    assert(strcmp(media_root, "/media") == 0);
    assert(strcmp(media_key, "New Volume") == 0);
    assert(strcmp(media_rest, "/Flac") == 0);
    assert(!freebsd_media_request_parts("/media", media_root,
                                        sizeof(media_root), media_key,
                                        sizeof(media_key), &media_rest));
    assert(freebsd_media_label_matches_key("New Volume", "New Volume"));
    assert(freebsd_media_label_matches_key("A+B/C", "A-B-C"));
    assert(!freebsd_media_label_matches_key("New Volume", "New"));
    assert(!freebsd_exact_automounted_media_device("/", ignored_device,
                                                  sizeof(ignored_device)));
#endif

    safe_copy(listing[0].name, sizeof(listing[0].name), "T7");
    listing[0].is_dir = 1;
    listing[0].kind = ENTRY_FILESYSTEM;
    listing[0].drive_index = -1;

    set_drive(&snapshot[0], "uuid:first", "T7", "/dev/sdc1", 0, 1, 1);
    set_drive(&snapshot[1], "uuid:second", "T7", "/dev/sdd1", 0, 1, 1);
    set_drive(&snapshot[2], "uuid:mounted", "Mounted", "/dev/sde1", 1, 0, 1);
    set_drive(&snapshot[3], "uuid:internal", "Internal", "/dev/nvme0n1p3",
              0, 1, 0);

    count = APPEND_UNMOUNTED_DRIVES(
        listing, 1, 8, snapshot, 4, "test", 0);
    assert(count == 3);
    assert(listing[1].kind == ENTRY_UNMOUNTED_DRIVE);
    assert(listing[1].drive_index == 0);
    assert(strcmp(listing[1].name, "T7 [sdc1]") == 0);
    assert(listing[2].kind == ENTRY_UNMOUNTED_DRIVE);
    assert(listing[2].drive_index == 1);
    assert(strcmp(listing[2].name, "T7 [sdd1]") == 0);
    assert(strcmp(listing[0].name, listing[1].name) != 0);
    assert(strcmp(listing[1].name, listing[2].name) != 0);

    count = APPEND_UNMOUNTED_DRIVES(
        listing, 1, 8, snapshot, 4, "skip-existing-test", 1);
    assert(count == 1);

    count = APPEND_UNMOUNTED_DRIVES(
        listing, 1, 2, snapshot, 4, "capacity-test", 0);
    assert(count == 2);

    suppress_drive_id("uuid:first");
    suppress_drive_id("uuid:first");
    assert(suppressed_drive_count == 1);
    memset(&listing[1], 0, sizeof(listing) - sizeof(listing[0]));
    count = APPEND_UNMOUNTED_DRIVES(
        listing, 1, 8, snapshot, 4, "suppressed-test", 0);
    assert(count == 2);
    assert(listing[1].drive_index == 1);
    assert(strcmp(listing[1].name, "T7 [sdd1]") == 0);

    prune_suppressed_drive_ids(snapshot, 4);
    assert(drive_id_is_suppressed("uuid:first"));
    snapshot[0].mounted = 1;
    prune_suppressed_drive_ids(snapshot, 4);
    assert(drive_id_is_suppressed("uuid:first"));
    unsuppress_drive_id("uuid:first");
    assert(!drive_id_is_suppressed("uuid:first"));

    snapshot[0].mounted = 0;
    suppress_drive_id("uuid:first");
    prune_suppressed_drive_ids(&snapshot[1], 3);
    assert(!drive_id_is_suppressed("uuid:first"));

    return 0;
}
