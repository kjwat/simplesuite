#define main simpleburn_app_main
/* Gate fsync on temporary regular files to emulate a slow or failed flush. */
#define fsync simpleburn_check_fsync
#include "../simpleburn.c"
#undef fsync
#undef main
#include <assert.h>

extern int fsync(int fd);
static int flush_notify = -1, flush_release = -1;

int simpleburn_check_fsync(int fd)
{
    char result = 's';
    if (flush_notify >= 0) {
        off_t offset = lseek(fd, 0, SEEK_CUR); assert(offset >= 0);
        uint64_t queued = (uint64_t)offset;
        assert(write(flush_notify, &queued, sizeof(queued)) == sizeof(queued));
        assert(read(flush_release, &result, 1) == 1);
        if (result == 'e') { errno = EIO; return -1; }
    }
    int code = fsync(fd);
    if (result == 'c') interrupted = 1;
    return code;
}

static void drive_policy(void)
{
    BurnTable *table = calloc(1, sizeof(*table)); assert(table);
    table->count = 3;
    BurnNode *disk = &table->nodes[0], *part = &table->nodes[1], *label = &table->nodes[2];
    copy_text(disk->path, sizeof(disk->path), "/dev/test0");
    copy_text(disk->kind, sizeof(disk->kind), "disk"); disk->size = 8 * 1024 * 1024;
    copy_text(part->path, sizeof(part->path), "/dev/test0p1");
    copy_text(part->parent, sizeof(part->parent), disk->path); copy_text(part->kind, sizeof(part->kind), "part");
    copy_text(label->path, sizeof(label->path), "/dev/label/test");
    copy_text(label->parent, sizeof(label->parent), part->path); copy_text(label->kind, sizeof(label->kind), "label");
    struct stat iso = {0}; iso.st_size = 4096; iso.st_dev = 123;
    assert(eligible(table, 0, "/home/test/image.iso", &iso));
    assert(!eligible(table, 1, "/home/test/image.iso", &iso));
    disk->readonly = 1; assert(!eligible(table, 0, "/home/test/image.iso", &iso)); disk->readonly = 0;
    iso.st_size = (off_t)disk->size + 2048; assert(!eligible(table, 0, "/home/test/image.iso", &iso)); iso.st_size = 4096;
    const char *unsafe[] = {"/", "/home", "/boot/efi", "/usr", "/var", "/srv/data", "[SWAP]", "/mnt/data\n/"};
    for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
        copy_text(label->mounts, sizeof(label->mounts), unsafe[i]);
        assert(!eligible(table, 0, "/home/test/image.iso", &iso));
    }
    copy_text(label->mounts, sizeof(label->mounts), "/media/test/stick\n/mnt/bind");
    assert(eligible(table, 0, "/home/test/image.iso", &iso));
    assert(!eligible(table, 0, "/media/test/stick/image.iso", &iso));
    assert(!eligible(table, 0, "/mnt/bind/nested/image.iso", &iso));
    assert(eligible(table, 0, "/media/test/sticker/image.iso", &iso));
    label->mounts[0] = 0; part->device = iso.st_dev;
    assert(!eligible(table, 0, "/unrelated/image.iso", &iso)); part->device = 0;
    copy_text(part->kind, sizeof(part->kind), "crypt");
    assert(!eligible(table, 0, "/home/test/image.iso", &iso));
    copy_text(part->kind, sizeof(part->kind), "stack");
    assert(!eligible(table, 0, "/home/test/image.iso", &iso));
    free(table);
}

static void dd_files(void)
{
    char source[] = "/tmp/simpleburn-source-XXXXXX", target[] = "/tmp/simpleburn-target-XXXXXX";
    int in = mkstemp(source), out = mkstemp(target); assert(in >= 3 && out >= 3);
    unlink(source); unlink(target);
    unsigned char block[2048], readback[2048];
    for (size_t i = 0; i < sizeof(block); i++) block[i] = (unsigned char)(i * 17);
    /* More than one dd output block, plus an incomplete final output block. */
    size_t blocks = 2051;
    for (size_t i = 0; i < blocks; i++) assert(write(in, block, sizeof(block)) == sizeof(block));
    uint64_t bytes = blocks * sizeof(block);
    assert(ftruncate(out, (off_t)bytes + 2048) == 0);
    const char tail[] = "PRESERVE DISK TAIL";
    assert(pwrite(out, tail, sizeof(tail), (off_t)bytes) == sizeof(tail));
    assert(lseek(in, 0, SEEK_SET) == 0);
    assert(run_dd(in, out, bytes) == 0);
    for (size_t i = 0; i < blocks; i++) {
        assert(pread(out, readback, sizeof(readback), (off_t)(i * sizeof(block))) == sizeof(readback));
        assert(!memcmp(block, readback, sizeof(block)));
    }
    char kept[sizeof(tail)];
    assert(pread(out, kept, sizeof(kept), (off_t)bytes) == sizeof(kept)); assert(!memcmp(kept, tail, sizeof(tail)));
    assert(lseek(in, 0, SEEK_SET) == 0 && lseek(out, 0, SEEK_SET) == 0);
    assert(run_dd(in, out, bytes + 2048) < 0); /* Short input cannot report success. */
    assert(strstr(message, "stopped short"));
    assert(lseek(in, 0, SEEK_SET) == 0 && lseek(out, 0, SEEK_SET) == 0);
    interrupted = 1;
    assert(run_dd(in, out, bytes) < 0); interrupted = 0;
    assert(strstr(message, "cancelled"));
#if defined(__linux__)
    int full = open("/dev/full", O_WRONLY); assert(full >= 0);
    assert(lseek(in, 0, SEEK_SET) == 0);
    assert(run_dd(in, full, bytes) < 0); close(full);
#endif
    close(in); close(out);
}

static void image_checks(void)
{
    char directory[] = "/tmp/simpleburn-image-XXXXXX"; assert(mkdtemp(directory));
    char path[BURN_PATH], linkpath[BURN_PATH];
    snprintf(path, sizeof(path), "%s/quote' and newline\n.ISO", directory);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    assert(ftruncate(fd, 2048) == 0); close(fd);
    struct stat st;
    fd = open_image(path, &st); assert(fd >= 0);
    char original[256], changed[256]; image_identity(&st, original, sizeof(original)); close(fd);
    snprintf(linkpath, sizeof(linkpath), "%s/link.iso", directory);
    assert(symlink(path, linkpath) == 0); assert(open_image(linkpath, &st) < 0); unlink(linkpath);
    fd = open(path, O_WRONLY); assert(fd >= 0); assert(ftruncate(fd, 4096) == 0); close(fd);
    fd = open_image(path, &st); assert(fd >= 0); image_identity(&st, changed, sizeof(changed)); close(fd);
    assert(strcmp(original, changed));
    fd = open(path, O_WRONLY); assert(fd >= 0); assert(ftruncate(fd, 2047) == 0); close(fd);
    assert(open_image(path, &st) < 0);
    unlink(path); rmdir(directory);
}

static void synced_progress(char outcome)
{
    char source[] = "/tmp/simpleburn-sync-source-XXXXXX", target[] = "/tmp/simpleburn-sync-target-XXXXXX";
    int in = mkstemp(source), out = mkstemp(target); assert(in >= 3 && out >= 3);
    unlink(source); unlink(target);
    uint64_t size = 2 * BURN_SYNC_BYTES + 6144;
    assert(ftruncate(in, (off_t)size) == 0);
    assert(ftruncate(out, (off_t)size + 2048) == 0);
    const char data[] = "KEEP DATA ACROSS DD BATCHES", tail[] = "PRESERVE DISK TAIL";
    off_t boundary = BURN_SYNC_BYTES - 8;
    assert(pwrite(in, data, sizeof(data), boundary) == sizeof(data));
    assert(pwrite(out, tail, sizeof(tail), (off_t)size) == sizeof(tail));
    int output[2], notify[2], release[2];
    assert(pipe(output) == 0 && pipe(notify) == 0 && pipe(release) == 0);
    fflush(stdout);
    flush_notify = notify[1]; flush_release = release[0];
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(output[0]); close(notify[0]); close(release[1]);
        assert(dup2(output[1], STDOUT_FILENO) >= 0); close(output[1]);
        int result = run_dd(in, out, size);
        _exit(result < 0 ? 1 : 0);
    }
    flush_notify = flush_release = -1;
    close(output[1]); close(notify[1]); close(release[0]);
    assert(fcntl(output[0], F_SETFL, O_NONBLOCK) == 0);
    uint64_t committed = 0, queued;
    char line[1024]; size_t used = 0;
    int done = 0, failed = 0;
    for (uint64_t previous = 0; previous < size; previous = queued) {
        struct pollfd ready = {notify[0], POLLIN, 0};
        assert(poll(&ready, 1, 5000) == 1 && (ready.revents & POLLIN));
        assert(read(notify[0], &queued, sizeof(queued)) == sizeof(queued));
        uint64_t next = size - previous > BURN_SYNC_BYTES ? previous + BURN_SYNC_BYTES : size;
        assert(queued == next);
        read_progress(output[0], line, &used, &committed, &done, &failed);
        assert(committed == previous); /* Pending flush must not advance progress. */
        assert(progress_percent(committed, size, 0) < 100);
        char response = queued == size ? outcome : 's';
        assert(write(release[1], &response, 1) == 1);
    }
    assert(wait_child(child) == (outcome == 's' ? 0 : 1));
    read_progress(output[0], line, &used, &committed, &done, &failed);
    assert(committed == (outcome == 's' ? size : 2 * BURN_SYNC_BYTES));
    assert(progress_percent(committed, size, 0) < 100);
    if (outcome == 's') {
        char copied[sizeof(data)], kept[sizeof(tail)];
        assert(pread(out, copied, sizeof(copied), boundary) == sizeof(copied));
        assert(!memcmp(copied, data, sizeof(data)));
        assert(pread(out, kept, sizeof(kept), (off_t)size) == sizeof(kept));
        assert(!memcmp(kept, tail, sizeof(tail)));
        assert(progress_percent(committed, size, 1) == 100);
    }
    close(output[0]); close(notify[0]); close(release[1]); close(in); close(out);
}

static void streamed_search(const char *root, const IsoSearch *search,
                            const char *const *expected, size_t count)
{
    int records[2]; assert(pipe(records) == 0);
    fflush(stdout);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(records[0]); assert(dup2(records[1], STDOUT_FILENO) >= 0); close(records[1]);
        assert(find_isos(root, 0, search) == 0);
        /* Keep stdout open and never flush here: matches must already be sent. */
        for (;;) pause();
    }
    close(records[1]);
    char found[BURN_PATH]; size_t used = 0, total = 0;
    for (size_t i = 0; i < count; i++) total += strlen(expected[i]) + 1;
    assert(total < sizeof(found));
    while (used < total) {
        struct pollfd ready = {records[0], POLLIN, 0};
        assert(poll(&ready, 1, 1500) == 1 && (ready.revents & POLLIN));
        ssize_t n = read(records[0], found + used, sizeof(found) - used);
        assert(n > 0); used += (size_t)n;
    }
    assert(used == total && !strcmp(found, expected[0])); /* Downloads first. */
    size_t offset = 0, matches = 0;
    while (offset < used) {
        assert(memchr(found + offset, 0, used - offset));
        int known = 0;
        for (size_t i = 0; i < count; i++) if (!strcmp(found + offset, expected[i])) known++;
        assert(known == 1); matches++;
        offset += strlen(found + offset) + 1;
    }
    assert(matches == count);
    struct pollfd extra = {records[0], POLLIN, 0};
    assert(poll(&extra, 1, 50) == 0); /* No duplicates or mounted-share matches. */
    kill(child, SIGKILL); wait_child(child); close(records[0]);
}

static void discovery_checks(void)
{
    char root[] = "/tmp/simpleburn-search-XXXXXX"; assert(mkdtemp(root));
    char downloads[BURN_PATH], share[BURN_PATH], hidden[BURN_PATH];
    snprintf(downloads, sizeof(downloads), "%s/Downloads", root); assert(mkdir(downloads, 0700) == 0);
    snprintf(share, sizeof(share), "%s/mounted share", root); assert(mkdir(share, 0700) == 0);
    snprintf(hidden, sizeof(hidden), "%s/.cache", root); assert(mkdir(hidden, 0700) == 0);
    char iso[BURN_PATH], local[BURN_PATH], remote[BURN_PATH], cached[BURN_PATH];
    snprintf(iso, sizeof(iso), "%s/Downloads/mint quote'\n.ISO", root);
    snprintf(local, sizeof(local), "%s/local.iso", root);
    snprintf(remote, sizeof(remote), "%s/mounted share/remote.iso", root);
    snprintf(cached, sizeof(cached), "%s/.cache/cached.iso", root);
    const char *paths[] = {iso, local, remote, cached};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0); close(fd);
    }
    IsoSearch search = {0};
    assert(search_mount_add(&search, root, root) == 0 && search.count == 0);
    assert(search_mount_add(&search, root, "/unrelated") == 0 && search.count == 0);
    assert(search_mount_add(&search, root, share) == 0 && search.count == 1);
    assert(search_mount_blocked(&search, remote));
    assert(!search_mount_blocked(&search, iso));
    const char *expected[] = {iso, local};
    streamed_search(root, &search, expected, 2);
    search_mount_free(&search);
    search = (IsoSearch){0};
    assert(search_mount_add(&search, share, share) == 0 && search.count == 0);
    /* Explicitly selecting a mount as the search root must include its ISOs. */
    const char *remote_expected[] = {remote};
    streamed_search(share, &search, remote_expected, 1);
    search_mount_free(&search);
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) unlink(paths[i]);
    rmdir(downloads); rmdir(share); rmdir(hidden); rmdir(root);

    int ready[2]; assert(pipe(ready) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(ready[0]); signal(SIGTERM, SIG_IGN);
        assert(write(ready[1], "1", 1) == 1); close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]); char byte; assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
    int64_t started = ssp_monotonic_ms();
    stop_finder(child); /* Simulate a scanner that cannot respond to SIGTERM. */
    assert(ssp_monotonic_ms() - started < 500);
    kill(child, SIGKILL); wait_child(child); ssp_manage_deferred_child(0);
}

int main(void)
{
    signal(SIGINT, stop); signal(SIGTERM, stop);
    drive_policy(); image_checks(); discovery_checks();
    assert(progress_percent(995, 1000, 0) == 99);
    assert(progress_percent(1000, 1000, 0) == 99);
    assert(progress_percent(1000, 1000, 1) == 100);
    assert(progress_percent(UINT64_MAX - 1, UINT64_MAX, 0) == 99);
    synced_progress('s'); synced_progress('e'); synced_progress('c');
#if defined(__linux__)
    char mount[BURN_PATH];
    assert(linux_mount_path("495 32 0:72 / /home/test/mounted\\040share\\134name rw - nfs server:/share rw", mount) == 0);
    assert(!strcmp(mount, "/home/test/mounted share\\name"));
    assert(linux_mount_path("1 2 3:4 / /bad\\000path rw - nfs server:/ rw", mount) < 0);
    assert(linux_mount_path("malformed", mount) < 0);
    BurnNode node;
    const char *row = "NAME=\"/dev/fake\" PKNAME=\"\" TYPE=\"disk\" SIZE=\"8192\" RO=\"0\" MODEL=\"USB\\x20Test\" SERIAL=\"a\\x22b\" MOUNTPOINTS=\"/media/Test\\x20Disk\\x0a/mnt/bind\" MAJ:MIN=\"999:99\"";
    assert(linux_node(row, &node) == 0);
    assert(!strcmp(node.model, "USB Test") && !strcmp(node.serial, "a\"b"));
    assert(!strcmp(node.mounts, "/media/Test Disk\n/mnt/bind"));
    assert(linux_node("NAME=\"/dev/fake\"", &node) < 0);
    assert(pair_value("NAME=\"bad\\x00value\"", "NAME", node.path, sizeof(node.path)) < 0);
    assert(pair_value("NAME=\"bad\\xQ1\"", "NAME", node.path, sizeof(node.path)) < 0);
    assert(pair_value("NAME=\"unterminated", "NAME", node.path, sizeof(node.path)) < 0);
    uint64_t number;
    assert(number_value("SIZE=\"-1\"", "SIZE", &number) < 0);
    assert(number_value("SIZE=\"18446744073709551616\"", "SIZE", &number) < 0);
    char regular[] = "/tmp/simpleburn-refuse-XXXXXX"; int fd = mkstemp(regular); assert(fd >= 0); close(fd);
    memset(&node, 0, sizeof(node)); copy_text(node.path, sizeof(node.path), regular);
    assert(platform_open(&node) < 0); unlink(regular);
#endif
    dd_files();
    puts("OK simpleburn synced progress, delayed and failed flushes, cancellation, ISO discovery, drive protection and real dd copies");
    return 0;
}
