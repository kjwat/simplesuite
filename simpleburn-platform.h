#ifndef SIMPLEBURN_PLATFORM_H
#define SIMPLEBURN_PLATFORM_H

/* Native drive discovery. No Python runtime or extra storage daemon needed. */
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/sysmacros.h>

static int linux_mount_path(const char *line, char *path)
{
    int end = 0;
    if (sscanf(line, "%*u %*u %*s %*s %4095s%n", path, &end) != 1 || line[end] != ' ') return -1;
    char *out = path;
    for (const char *p = path; *p; p++) {
        if (*p != '\\') { *out++ = *p; continue; }
        if (p[1] < '0' || p[1] > '7' || p[2] < '0' || p[2] > '7' || p[3] < '0' || p[3] > '7') return -1;
        unsigned int c = (unsigned int)((p[1] - '0') * 64 + (p[2] - '0') * 8 + p[3] - '0');
        if (!c || c > 255) return -1;
        *out++ = (char)c; p += 3;
    }
    *out = 0;
    return 0;
}

static int platform_search_mounts(const char *root, IsoSearch *search)
{
    FILE *fp = fopen("/proc/self/mountinfo", "r");
    if (!fp) return errorf("Cannot inspect mounted filesystems for ISO search.");
    char *line = NULL; size_t capacity = 0;
    int result = 0;
    while (getline(&line, &capacity, fp) >= 0) {
        char path[BURN_PATH];
        if (linux_mount_path(line, path) < 0 || search_mount_add(search, root, path) < 0) {
            result = -1; break;
        }
    }
    if (ferror(fp)) result = -1;
    free(line); fclose(fp);
    return result < 0 ? errorf("Cannot inspect mounted filesystems for ISO search.") : 0;
}

static int pair_value(const char *line, const char *key, char *out, size_t size)
{
    char needle[64]; snprintf(needle, sizeof(needle), "%s=\"", key);
    const char *p = line;
    do {
        p = strstr(p, needle);
        if (!p) return -1;
        if (p == line || p[-1] == ' ') break;
        p++;
    } while (1);
    p += strlen(needle);
    size_t used = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (*p != 'x' || !p[1] || !p[2] || !isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2])) return -1;
            char hex[3] = {p[1], p[2], 0}; c = (unsigned char)strtoul(hex, NULL, 16); p += 3;
        }
        if (!c || used + 1 >= size) return -1;
        out[used++] = (char)c;
    }
    if (*p != '"') return -1;
    out[used] = 0; return 0;
}

static int number_value(const char *line, const char *key, uint64_t *number)
{
    char value[32], *end;
    if (pair_value(line, key, value, sizeof(value)) < 0 || !isdigit((unsigned char)value[0])) return -1;
    errno = 0; *number = strtoull(value, &end, 10);
    return errno || *end ? -1 : 0;
}

static int linux_node(const char *line, BurnNode *node)
{
    char device[32], sequence[32] = "0", filename[256];
    uint64_t readonly;
    unsigned int maj, min; int end = 0;
    memset(node, 0, sizeof(*node));
    if (pair_value(line, "NAME", node->path, sizeof(node->path)) < 0 ||
        pair_value(line, "PKNAME", node->parent, sizeof(node->parent)) < 0 ||
        pair_value(line, "TYPE", node->kind, sizeof(node->kind)) < 0 ||
        pair_value(line, "MODEL", node->model, sizeof(node->model)) < 0 ||
        pair_value(line, "SERIAL", node->serial, sizeof(node->serial)) < 0 ||
        pair_value(line, "MOUNTPOINTS", node->mounts, sizeof(node->mounts)) < 0 ||
        pair_value(line, "MAJ:MIN", device, sizeof(device)) < 0 ||
        number_value(line, "SIZE", &node->size) < 0 || number_value(line, "RO", &readonly) < 0 ||
        readonly > 1 || sscanf(device, "%u:%u%n", &maj, &min, &end) != 2 || device[end] ||
        strncmp(node->path, "/dev/", 5)) return -1;
    node->device = makedev(maj, min); node->readonly = (int)readonly;
    snprintf(filename, sizeof(filename), "/sys/dev/block/%u:%u/diskseq", maj, min);
    FILE *fp = fopen(filename, "r");
    if (fp) { if (!fgets(sequence, sizeof(sequence), fp)) copy_text(sequence, sizeof(sequence), "0"); fclose(fp); }
    sequence[strcspn(sequence, "\n")] = 0;
    char identity[384];
    snprintf(identity, sizeof(identity), "%s:%" PRIu64 ":%s:%s",
        device, node->size, sequence, node->serial);
    copy_text(node->identity, sizeof(node->identity), identity);
    return 0;
}

static int platform_scan(BurnTable *table)
{
    char *args[] = {"lsblk", "--bytes", "--paths", "--pairs", "--exclude", "7",
        "--output", "NAME,PKNAME,TYPE,SIZE,RO,MODEL,SERIAL,MOUNTPOINTS,MAJ:MIN", NULL};
    char *output = capture(args);
    table->count = 0;
    if (!output) return errorf("Cannot list drives. Install util-linux (lsblk).");
    char *save = NULL;
    for (char *p = strtok_r(output, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
        if (table->count == BURN_MAX_NODES || linux_node(p, &table->nodes[table->count]) < 0) {
            free(output); table->count = 0; return errorf("Cannot safely read the drive list.");
        }
        table->count++;
    }
    free(output);
    int zfs = 0;
    FILE *mounts = fopen("/proc/self/mounts", "r");
    if (!mounts) return errorf("Cannot inspect mounted filesystems.");
    char *line = NULL; size_t capacity = 0;
    while (getline(&line, &capacity, mounts) >= 0) if (strstr(line, " zfs ")) zfs = 1;
    int failed = ferror(mounts); free(line); fclose(mounts);
    if (failed) return errorf("Cannot inspect mounted filesystems.");
    return protect_pools(table, zfs);
}

static int platform_unmount(const BurnTable *table, int disk)
{
    unsigned char set[BURN_MAX_NODES]; members(table, disk, set);
    for (int i = table->count - 1; i >= 0; i--) if (set[i] && table->nodes[i].mounts[0]) {
        char *args[] = {"umount", "--all-targets", "--", (char *)table->nodes[i].path, NULL};
        if (command(args)) return errorf("Drive is busy; close files on it and try again.");
    }
    return 0;
}

static int platform_open(const BurnNode *drive)
{
    struct stat before, after;
    if (lstat(drive->path, &before) < 0 || !S_ISBLK(before.st_mode) || before.st_rdev != drive->device)
        return errorf("Target is no longer the selected whole drive.");
    int fd = open(drive->path, O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errorf("Cannot lock drive for writing: %s", strerror(errno));
    uint64_t size = 0; int readonly = 1;
    if (fstat(fd, &after) < 0 || !S_ISBLK(after.st_mode) || after.st_rdev != drive->device ||
        ioctl(fd, BLKGETSIZE64, &size) < 0 || ioctl(fd, BLKROGET, &readonly) < 0 || readonly || size != drive->size) {
        close(fd); return errorf("Drive changed or became read-only. Burn stopped.");
    }
    return fd;
}

static int platform_flush(int fd) { return fsync(fd); }
static int platform_executable(char *out, size_t size)
{
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n < 0 || (size_t)n >= size - 1) return -1;
    out[n] = 0; return 0;
}

#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <sys/disk.h>
#include <sys/mount.h>

static CFPropertyListRef disk_plist(char *const args[])
{
    char *text = capture(args);
    if (!text) return NULL;
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)text, (CFIndex)strlen(text)); free(text);
    if (!data) return NULL;
    CFPropertyListRef result = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data); return result;
}

static CFTypeRef plist_get(CFDictionaryRef dict, CFStringRef key)
{
    return dict && CFGetTypeID(dict) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(dict, key) : NULL;
}

static void plist_string(CFDictionaryRef dict, CFStringRef key, char *out, size_t size)
{
    CFTypeRef value = plist_get(dict, key); *out = 0;
    if (value && CFGetTypeID(value) == CFStringGetTypeID())
        CFStringGetCString(value, out, (CFIndex)size, kCFStringEncodingUTF8);
}

static int plist_bool(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = plist_get(dict, key);
    return value && CFGetTypeID(value) == CFBooleanGetTypeID() ? CFBooleanGetValue(value) : -1;
}

static int mac_node(CFDictionaryRef dict, BurnNode *node)
{
    memset(node, 0, sizeof(*node));
    plist_string(dict, CFSTR("DeviceNode"), node->path, sizeof(node->path));
    char parent[120], virtual_type[32];
    plist_string(dict, CFSTR("ParentWholeDisk"), parent, sizeof(parent));
    if (!*parent) plist_string(dict, CFSTR("PartOfWhole"), parent, sizeof(parent));
    if (*parent) snprintf(node->parent, sizeof(node->parent), "/dev/%s", parent);
    plist_string(dict, CFSTR("MediaName"), node->model, sizeof(node->model));
    plist_string(dict, CFSTR("MountPoint"), node->mounts, sizeof(node->mounts));
    plist_string(dict, CFSTR("DeviceTreePath"), node->serial, sizeof(node->serial));
    plist_string(dict, CFSTR("VirtualOrPhysical"), virtual_type, sizeof(virtual_type));
    copy_text(node->kind, sizeof(node->kind), plist_bool(dict, CFSTR("WholeDisk")) == 1 ? "disk" : "part");
    if (!strcasecmp(virtual_type, "Virtual") || plist_bool(dict, CFSTR("Virtual")) == 1)
        copy_text(node->kind, sizeof(node->kind), "part");
    int writable = plist_bool(dict, CFSTR("Writable"));
    if (writable < 0) writable = plist_bool(dict, CFSTR("WritableMedia"));
    node->readonly = writable != 1;
    CFTypeRef number = plist_get(dict, CFSTR("TotalSize"));
    int64_t size = 0;
    if (number && CFGetTypeID(number) == CFNumberGetTypeID()) CFNumberGetValue(number, kCFNumberSInt64Type, &size);
    node->size = size > 0 ? (uint64_t)size : 0;
    struct stat st;
    if (stat(node->path, &st) == 0) node->device = st.st_rdev;
    char identity[384];
    snprintf(identity, sizeof(identity), "%ju:%" PRIu64 ":%s",
        (uintmax_t)node->device, node->size, node->serial);
    copy_text(node->identity, sizeof(node->identity), identity);
    return strncmp(node->path, "/dev/disk", 9) ? -1 : 0;
}

/* APFS volume devices belong to synthesized containers; connect these to
 * their physical stores so a boot or source volume protects the whole disk. */
static int mac_apfs(BurnTable *table)
{
    int needed = 0;
    for (int i = 0; i < table->count; i++)
        if (!strcmp(table->nodes[i].kind, "part") && !table->nodes[i].parent[0]) needed = 1;
    if (!needed) return 0;
    char *args[] = {"diskutil", "apfs", "list", "-plist", NULL};
    CFPropertyListRef plist = disk_plist(args);
    if (!plist) return errorf("Cannot inspect APFS storage safely.");
    CFTypeRef containers = plist_get((CFDictionaryRef)plist, CFSTR("Containers"));
    if (containers && CFGetTypeID(containers) == CFArrayGetTypeID())
        for (CFIndex i = 0; i < CFArrayGetCount(containers); i++) {
            CFDictionaryRef container = CFArrayGetValueAtIndex(containers, i);
            CFTypeRef stores = plist_get(container, CFSTR("PhysicalStores"));
            char reference[120]; plist_string(container, CFSTR("ContainerReference"), reference, sizeof(reference));
            if (!stores || CFGetTypeID(stores) != CFArrayGetTypeID()) continue;
            for (CFIndex j = 0; j < CFArrayGetCount(stores); j++) {
                char identifier[120], path[128];
                plist_string(CFArrayGetValueAtIndex(stores, j), CFSTR("DeviceIdentifier"), identifier, sizeof(identifier));
                snprintf(path, sizeof(path), "/dev/%s", identifier);
                if (CFArrayGetCount(stores) != 1) {
                    for (int k = 0; k < table->count; k++) if (!strcmp(table->nodes[k].path, path))
                        copy_text(table->nodes[k].kind, sizeof(table->nodes[k].kind), "stack");
                } else for (int k = 0; k < table->count; k++) {
                    if (!strcmp(table->nodes[k].path + 5, reference)) copy_text(table->nodes[k].parent, sizeof(table->nodes[k].parent), path);
                }
            }
        }
    CFRelease(plist); return 0;
}

static int platform_scan(BurnTable *table)
{
    table->count = 0;
    char *args[] = {"diskutil", "list", "-plist", NULL};
    CFPropertyListRef plist = disk_plist(args);
    if (!plist) return errorf("Cannot list drives with diskutil.");
    CFTypeRef disks = plist_get((CFDictionaryRef)plist, CFSTR("AllDisks"));
    if (!disks || CFGetTypeID(disks) != CFArrayGetTypeID()) { CFRelease(plist); return errorf("Cannot read diskutil drive list."); }
    for (CFIndex i = 0; i < CFArrayGetCount(disks); i++) {
        char identifier[128];
        CFTypeRef value = CFArrayGetValueAtIndex(disks, i);
        if (CFGetTypeID(value) != CFStringGetTypeID() ||
            !CFStringGetCString(value, identifier, sizeof(identifier), kCFStringEncodingUTF8) || table->count == BURN_MAX_NODES) goto fail;
        char *info[] = {"diskutil", "info", "-plist", identifier, NULL};
        CFPropertyListRef dict = disk_plist(info);
        if (!dict) goto fail;
        int code = mac_node((CFDictionaryRef)dict, &table->nodes[table->count]); CFRelease(dict);
        if (code < 0) goto fail;
        table->count++;
    }
    CFRelease(plist); return mac_apfs(table);
fail:
    CFRelease(plist); table->count = 0; return errorf("Drive list changed. Refresh and try again.");
}

static int platform_unmount(const BurnTable *table, int disk)
{
    char *args[] = {"diskutil", "unmountDisk", (char *)table->nodes[disk].path, NULL};
    return command(args) ? errorf("Drive is busy; close files on it and try again.") : 0;
}

static int platform_open(const BurnNode *drive)
{
    char raw[136]; struct stat st, block, after;
    if (stat(drive->path, &block) < 0 || !S_ISBLK(block.st_mode) || block.st_rdev != drive->device)
        return errorf("Target is no longer the selected drive.");
    snprintf(raw, sizeof(raw), "/dev/r%s", drive->path + 5);
    if (lstat(raw, &st) < 0 || !S_ISCHR(st.st_mode)) return errorf("Target is no longer a raw drive.");
    int fd = open(raw, O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errorf("Cannot open drive: %s", strerror(errno));
    uint64_t blocks = 0; uint32_t blocksize = 0;
    if (fstat(fd, &after) < 0 || !S_ISCHR(after.st_mode) || after.st_rdev != st.st_rdev ||
        ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) < 0 || ioctl(fd, DKIOCGETBLOCKSIZE, &blocksize) < 0 ||
        !blocksize || blocks > UINT64_MAX / blocksize || blocks * blocksize != drive->size) {
        close(fd); return errorf("Drive capacity changed. Burn stopped.");
    }
    return fd;
}

static int platform_flush(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) return fsync(fd);
    return ioctl(fd, DKIOCSYNCHRONIZECACHE, NULL);
}

static int platform_executable(char *out, size_t size)
{
    char path[BURN_PATH]; uint32_t capacity = sizeof(path);
    return _NSGetExecutablePath(path, &capacity) == 0 && realpath(path, out) && strlen(out) < size ? 0 : -1;
}

#elif defined(__FreeBSD__)
#include <libgeom.h>
#include <sys/disk.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

static const char *geom_config(struct gconf *config, const char *name)
{
    struct gconfig *entry;
    LIST_FOREACH(entry, config, lg_config) if (!strcmp(entry->lg_name, name)) return entry->lg_val;
    return "";
}

static int platform_scan(BurnTable *table)
{
    struct gmesh mesh; table->count = 0;
    if (geom_gettree(&mesh)) return errorf("Cannot inspect GEOM drives.");
    struct gclass *class; struct ggeom *geom; struct gprovider *provider;
    LIST_FOREACH(class, &mesh.lg_class, lg_class)
        LIST_FOREACH(geom, &class->lg_geom, lg_geom)
            LIST_FOREACH(provider, &geom->lg_provider, lg_provider) {
                if (table->count == BURN_MAX_NODES) { geom_deletetree(&mesh); return errorf("Too many GEOM providers."); }
                BurnNode *node = &table->nodes[table->count++]; memset(node, 0, sizeof(*node));
                snprintf(node->path, sizeof(node->path), "/dev/%s", provider->lg_name);
                copy_text(node->kind, sizeof(node->kind), !strcmp(class->lg_name, "DISK") ? "disk" :
                    !strcmp(class->lg_name, "PART") ? "part" : !strcmp(class->lg_name, "LABEL") ? "label" : "stack");
                struct gconsumer *consumer = LIST_FIRST(&geom->lg_consumer);
                if (consumer && consumer->lg_provider) snprintf(node->parent, sizeof(node->parent), "/dev/%s", consumer->lg_provider->lg_name);
                if (consumer && LIST_NEXT(consumer, lg_consumer)) copy_text(node->kind, sizeof(node->kind), "stack");
                node->size = provider->lg_mediasize > 0 ? (uint64_t)provider->lg_mediasize : 0;
                node->readonly = !strncmp(provider->lg_name, "cd", 2) || !strncmp(provider->lg_name, "acd", 3);
                copy_text(node->model, sizeof(node->model), geom_config(&provider->lg_config, "descr"));
                copy_text(node->serial, sizeof(node->serial), geom_config(&provider->lg_config, "ident"));
                if (!node->model[0]) copy_text(node->model, sizeof(node->model), geom_config(&geom->lg_config, "descr"));
                if (!node->serial[0]) copy_text(node->serial, sizeof(node->serial), geom_config(&geom->lg_config, "ident"));
                if (!node->model[0]) copy_text(node->model, sizeof(node->model), geom->lg_name);
                struct stat st; if (stat(node->path, &st) == 0) node->device = st.st_rdev;
                char identity[384];
                snprintf(identity, sizeof(identity), "%ju:%" PRIu64 ":%s:%ju",
                    (uintmax_t)node->device, node->size, node->serial, (uintmax_t)(uintptr_t)provider->lg_id);
                copy_text(node->identity, sizeof(node->identity), identity);
            }
    geom_deletetree(&mesh);
    struct statfs *mounts; int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (!count) return errorf("Cannot inspect mounted filesystems.");
    int zfs = 0;
    for (int m = 0; m < count; m++) {
        if (!strcmp(mounts[m].f_fstypename, "zfs")) zfs = 1;
        for (int i = 0; i < table->count; i++) if (!strcmp(mounts[m].f_mntfromname, table->nodes[i].path)) {
            size_t used = strlen(table->nodes[i].mounts), n = strlen(mounts[m].f_mntonname);
            if (used + n + 2 >= sizeof(table->nodes[i].mounts)) return errorf("Mount list is too long.");
            if (used) table->nodes[i].mounts[used++] = '\n';
            memcpy(table->nodes[i].mounts + used, mounts[m].f_mntonname, n + 1);
        }
    }
    char *swap_args[] = {"swapinfo", "-k", NULL};
    char *swaps = capture(swap_args);
    if (!swaps) return errorf("Cannot inspect swap devices.");
    for (int i = 0; i < table->count; i++) if (strstr(swaps, table->nodes[i].path))
        copy_text(table->nodes[i].mounts, sizeof(table->nodes[i].mounts), "[SWAP]");
    free(swaps);
    return protect_pools(table, zfs);
}

static int platform_unmount(const BurnTable *table, int disk)
{
    unsigned char set[BURN_MAX_NODES]; members(table, disk, set);
    for (int i = table->count - 1; i >= 0; i--) if (set[i] && table->nodes[i].mounts[0]) {
        char points[BURN_PATH]; copy_text(points, sizeof(points), table->nodes[i].mounts);
        char *save = NULL;
        for (char *point = strtok_r(points, "\n", &save); point; point = strtok_r(NULL, "\n", &save)) {
            char *args[] = {"umount", point, NULL};
            if (command(args)) return errorf("Drive is busy; close files on it and try again.");
        }
    }
    return 0;
}

static int platform_open(const BurnNode *drive)
{
    struct stat before, after;
    if (lstat(drive->path, &before) < 0 || !S_ISCHR(before.st_mode) || before.st_rdev != drive->device)
        return errorf("Target is no longer the selected drive.");
    int fd = open(drive->path, O_WRONLY | O_EXCL | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errorf("Cannot open drive: %s", strerror(errno));
    off_t size = 0;
    if (fstat(fd, &after) < 0 || after.st_rdev != before.st_rdev ||
        ioctl(fd, DIOCGMEDIASIZE, &size) < 0 || (uint64_t)size != drive->size) {
        close(fd); return errorf("Drive changed. Burn stopped.");
    }
    return fd;
}

static int platform_flush(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) return fsync(fd);
    return g_flush(fd);
}
static int platform_executable(char *out, size_t size)
{
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    size_t capacity = size;
    return sysctl(mib, 4, out, &capacity, NULL, 0);
}

#else
#error "Simpleburn requires Linux, FreeBSD or macOS."
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
static int platform_search_mounts(const char *root, IsoSearch *search)
{
    struct statfs *mounts;
    /* Cached mount names avoid contacting an unresponsive network server. */
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0) return errorf("Cannot inspect mounted filesystems for ISO search.");
    for (int i = 0; i < count; i++)
        if (search_mount_add(search, root, mounts[i].f_mntonname) < 0)
            return errorf("Cannot inspect mounted filesystems for ISO search.");
    return 0;
}
#endif
#endif
