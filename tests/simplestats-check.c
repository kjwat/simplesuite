#define main simplestats_program_main
#include "../simplestats.c"
#undef main

#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

static void test_disk_usage(void) {
    struct statvfs stats = {0};
    Usage usage;
    char text[64];

    stats.f_blocks = 1000;
    stats.f_bfree = 400;
    stats.f_bavail = 350;
    stats.f_frsize = 4096;
    stats.f_bsize = 1024;
    usage = disk_usage_from_statvfs(&stats);
    assert(usage.total == 4096000);
    assert(usage.used == 2457600);
    format_usage(text, sizeof(text), usage);
    assert(!strcmp(text, "2.34 MiB (60.0%)"));

    /* Large disks must not overflow a 32-bit byte count. */
    stats.f_blocks = (uint64_t)1 << 30;
    stats.f_bfree = (uint64_t)1 << 29;
    usage = disk_usage_from_statvfs(&stats);
    assert(usage.total == (uint64_t)4 << 40);
    assert(usage.used == (uint64_t)2 << 40);
    format_usage(text, sizeof(text), usage);
    assert(!strcmp(text, "2.00 TiB (50.0%)"));

    stats.f_blocks = stats.f_bfree = 0;
    format_usage(text, sizeof(text), disk_usage_from_statvfs(&stats));
    assert(!strcmp(text, "n/a"));
}

#if !defined(__FreeBSD__) && !defined(__APPLE__)
static Usage memory_fixture(const char *contents) {
    FILE *f = tmpfile();
    Usage usage;

    assert(f);
    assert(fputs(contents, f) >= 0);
    rewind(f);
    usage = ram_usage_from_meminfo(f);
    fclose(f);
    return usage;
}

static void test_ram_usage(void) {
    Usage usage = memory_fixture(
        "MemTotal: 1048576 kB\n"
        "MemFree: 262144 kB\n"
        "MemAvailable: 900000 kB\n"
        "HugePages_Total: 0\n"
        "Buffers: 65536 kB\n"
        "Cached: 327680 kB\n"
        "SReclaimable: 65536 kB\n"
        "Shmem: 65536 kB\n");
    Usage more_cache = memory_fixture(
        "MemTotal: 1048576 kB\n"
        "MemFree: 196608 kB\n"
        "Buffers: 65536 kB\n"
        "Cached: 393216 kB\n"
        "SReclaimable: 65536 kB\n"
        "Shmem: 65536 kB\n");
    char text[64];

    /* 384 MiB used, including 64 MiB shared memory; cache growth is neutral. */
    assert(usage.total == (uint64_t)1 << 30);
    assert(usage.used == (uint64_t)384 << 20);
    assert(more_cache.used == usage.used);
    format_usage(text, sizeof(text), usage);
    assert(!strcmp(text, "384.00 MiB (37.5%)"));

    assert(!ram_usage_from_meminfo(NULL).total);
    assert(!memory_fixture("MemFree: 512 kB\n").total);
    assert(!memory_fixture("MemTotal: 1024 kB\n").total);
    assert(!memory_fixture("MemTotal: 0 kB\nMemFree: 0 kB\n").total);
    usage = memory_fixture("MemTotal: 1024 kB\nMemFree: 2048 kB\n");
    assert(usage.used == 0);
    usage = memory_fixture("MemTotal: 1024 kB\nMemFree: 512 kB\n");
    assert(usage.used == 512 * 1024);
}

static void supply_attribute(const char *root, const char *supply,
                             const char *attribute, const char *value) {
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", root, supply);
    assert(mkdir(path, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/%s/%s", root, supply, attribute);
    f = fopen(path, "w");
    assert(f);
    assert(fprintf(f, "%s\n", value) > 0);
    assert(fclose(f) == 0);
}

static void remove_supply(const char *root, const char *supply) {
    const char *attributes[] = {"type", "scope", "online", "status"};
    char path[512];

    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", root, supply, attributes[i]);
        assert(unlink(path) == 0 || errno == ENOENT);
    }
    snprintf(path, sizeof(path), "%s/%s", root, supply);
    assert(rmdir(path) == 0);
}

static void test_external_power(void) {
    char directory[] = "/tmp/simplestats-power-XXXXXX";
    char *root = mkdtemp(directory);

    assert(root);
    assert(external_power_from_directory(root) == -1);

    supply_attribute(root, "BAT0", "type", "Battery");
    supply_attribute(root, "BAT0", "online", "1");
    supply_attribute(root, "BAT0", "status", "Discharging");
    assert(external_power_from_directory(root) == 0);
    supply_attribute(root, "BAT0", "status", "Charging");
    assert(external_power_from_directory(root) == 1);
    supply_attribute(root, "BAT0", "status", "Full");
    assert(external_power_from_directory(root) == -1);

    supply_attribute(root, "AC", "type", "Mains");
    supply_attribute(root, "AC", "online", "1");
    assert(external_power_from_directory(root) == 1);
    supply_attribute(root, "BAT0", "status", "Not charging");
    assert(external_power_from_directory(root) == 1);
    supply_attribute(root, "AC", "online", "0");
    assert(external_power_from_directory(root) == 0);

    /* An offline adapter must not hide another connected USB charger. */
    supply_attribute(root, "usb-charger", "type", "USB");
    supply_attribute(root, "usb-charger", "scope", "System");
    supply_attribute(root, "usb-charger", "online", "2");
    assert(external_power_from_directory(root) == 1);
    supply_attribute(root, "usb-charger", "scope", "Device");
    assert(external_power_from_directory(root) == 0);
    remove_supply(root, "usb-charger");

    supply_attribute(root, "AC", "online", "unknown");
    assert(external_power_from_directory(root) == -1);
    remove_supply(root, "AC");
    remove_supply(root, "BAT0");
    assert(rmdir(root) == 0);
    assert(external_power_from_directory(root) == -1);
}
#endif

int main(void) {
    test_disk_usage();
#if !defined(__FreeBSD__) && !defined(__APPLE__)
    test_ram_usage();
    test_external_power();
#endif
    puts("SimpleStats: usage amounts, cache exclusion, and external power checks passed");
    return 0;
}
