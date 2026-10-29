#define main simpleserved_program_main
#include "../simpleserved.c"
#undef main

#include <assert.h>

/* Exercise the real nonblocking connect/SO_ERROR path as well as fixtures.
 * All sockets stay on loopback and use kernel-assigned, unprivileged ports. */
static void check_tcp_probes(void)
{
    SSDaemon *daemon = calloc(1, sizeof(*daemon));
    struct sockaddr_in address = {0};
    socklen_t size = sizeof(address);
    int listener, refused;

    assert(daemon);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(listener, (struct sockaddr *)&address, &size) == 0);
    assert(listen(listener, 1) == 0);
    assert(route_service_reachable(daemon, SS_ROUTE_LAN, "127.0.0.1",
                                    ntohs(address.sin_port)));

    /* A bound socket without listen() reserves a definitely closed TCP port. */
    address.sin_port = 0;
    refused = socket(AF_INET, SOCK_STREAM, 0);
    assert(refused >= 0);
    assert(bind(refused, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(refused, (struct sockaddr *)&address, &size) == 0);
    assert(!route_service_reachable(daemon, SS_ROUTE_LAN, "127.0.0.1",
                                     ntohs(address.sin_port)));
    assert(!route_service_reachable(daemon, SS_ROUTE_LAN, "invalid", 2049));
    assert(close(listener) == 0);
    assert(close(refused) == 0);
    free(daemon);
}

static void write_fixture(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");

    assert(file);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}

static char *read_fixture(const char *path)
{
    char *contents = NULL;
    char error[512];
    size_t length;

    assert(ss_read_file(path, 1024 * 1024, &contents, &length, error,
                        sizeof(error)));
    return contents;
}

static void endpoints(const char *path, int old_rpc, int old_nfs,
                       int new_rpc, int new_nfs)
{
    char contents[256];

    assert(snprintf(contents, sizeof(contents),
                     "192.0.2.50 111 %d\n192.0.2.50 2049 %d\n"
                     "192.0.2.51 111 %d\n192.0.2.51 2049 %d\n",
                     old_rpc, old_nfs, new_rpc, new_nfs) < (int)sizeof(contents));
    write_fixture(path, contents);
}

/* Publish through the same generation-checked cache as manifest rediscovery. */
static void rediscover(SSDaemon *daemon, const char *address)
{
    static const char manifest[] =
        "[server]\nversion=1\nname=dhcp-peer\n"
        "hostname=dhcp-peer.local\nprotocol=nfs\n\n"
        "[share Library]\nprotocol=nfs\nexport=/exports/Library\n"
        "access=read-write\nuuid=test-disk\nsize=1000\nfree=500\n";
    SSRemoteServer *remote = malloc(sizeof(*remote));
    SSManifestJob job = {0};
    SSDiscoveredService *service = &daemon->services[0];
    char error[512];

    assert(remote);
    assert(ss_parse_manifest(manifest, address, SS_DEFAULT_PORT, remote,
                              error, sizeof(error)));
    pthread_mutex_lock(&daemon->remote_mutex);
    memset(service, 0, sizeof(*service));
    service->generation = ++daemon->next_service_generation;
    service->manifest_pending = 1;
    assert(ss_copy_string(service->name, sizeof(service->name),
                           "dhcp-peer SimpleServe"));
    assert(ss_copy_string(service->server_name, sizeof(service->server_name),
                           "dhcp-peer"));
    assert(ss_copy_string(service->type, sizeof(service->type), SS_SERVICE_TYPE));
    assert(ss_copy_string(service->domain, sizeof(service->domain), "local"));
    daemon->service_count = 1;
    job.generation = service->generation;
    assert(ss_copy_string(job.name, sizeof(job.name), service->name));
    assert(ss_copy_string(job.type, sizeof(job.type), service->type));
    assert(ss_copy_string(job.domain, sizeof(job.domain), service->domain));
    assert(ss_copy_string(job.advertised_name, sizeof(job.advertised_name),
                           remote->name));
    cache_manifest_result_locked(daemon, &job, remote);
    pthread_mutex_unlock(&daemon->remote_mutex);
    free(remote);
    remote = copy_cached_remote(daemon, "dhcp-peer");
    assert(remote && strcmp(remote->address, address) == 0);
    free(remote);
}

static void check_commands(const char *path, int normal, int lazy, int mount)
{
    char *contents = read_fixture(path);
    const char *normal_at = strstr(contents, "/bin/umount\t-i\t-c\t/");
    const char *lazy_at = strstr(contents, "/bin/umount\t-i\t-c\t-l\t");
    const char *mount_at = strstr(contents, "/bin/mount\t");

    assert((normal_at != NULL) == normal);
    assert((lazy_at != NULL) == lazy);
    assert((mount_at != NULL) == mount);
    if (normal_at && lazy_at)
        assert(normal_at < lazy_at);
    if (normal_at && mount_at)
        assert(normal_at < mount_at);
    if (lazy_at && mount_at)
        assert(lazy_at < mount_at);
    if (mount_at)
        assert(strstr(mount_at, "192.0.2.51:/exports/Library") != NULL);
    assert(strstr(contents, "\t-f\t") == NULL);
    assert(strstr(contents, "soft") == NULL);
    free(contents);
}

static void check_reconnected(SSDaemon *daemon, const char *target)
{
    const SSClientMount *mount = &daemon->mounts.mounts[0];
    SSMountConfig *saved = malloc(sizeof(*saved));
    char error[512];

    assert(saved);
    assert(daemon->mounts.mount_count == 1);
    assert(mount->mounted && mount->available && mount->remembered);
    assert(strcmp(mount->target, target) == 0);
    assert(strcmp(mount->filesystem_id, "test-disk") == 0);
    assert(strcmp(mount->address, "192.0.2.51") == 0);
    assert(strcmp(mount->lan_address, "192.0.2.51") == 0);
    assert(mount->route == SS_ROUTE_LAN && mount->misses == 0);
    assert(ss_load_mount_config(daemon->state_path, saved, error, sizeof(error)));
    assert(saved->mount_count == 1 && saved->mounts[0].remembered);
    assert(strcmp(saved->mounts[0].address, "192.0.2.51") == 0);
    free(saved);
}

int main(void)
{
    char temporary[] = "/tmp/simpleserve-reconnect.XXXXXX";
    char *root = mkdtemp(temporary);
    char home[PATH_MAX], mounts[PATH_MAX], commands[PATH_MAX];
    char probes[PATH_MAX], target[PATH_MAX], table[PATH_MAX * 2], error[1024];
    SSDaemon *daemon = calloc(1, sizeof(*daemon));
    SSClientMount initial = {0};
    SSBuffer message;
    SSRemoteServer *remote;

    check_tcp_probes();
    assert(root && daemon);
    assert(snprintf(home, sizeof(home), "%s/home", root) < (int)sizeof(home));
    assert(snprintf(mounts, sizeof(mounts), "%s/mounts", root) < (int)sizeof(mounts));
    assert(snprintf(commands, sizeof(commands), "%s/commands", root) < (int)sizeof(commands));
    assert(snprintf(probes, sizeof(probes), "%s/endpoints", root) < (int)sizeof(probes));
    assert(snprintf(daemon->state_path, sizeof(daemon->state_path),
                     "%s/state", root) < (int)sizeof(daemon->state_path));
    assert(mkdir(home, 0700) == 0);
    assert(setenv("SIMPLESERVE_TEST_HOME", home, 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_MOUNTS", mounts, 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_COMMAND_LOG", commands, 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_ENDPOINTS", probes, 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_NORMAL_UNMOUNT_BUSY", "1", 1) == 0);
    write_fixture(mounts, "");
    write_fixture(commands, "");
    daemon->platform = SS_PLATFORM_LINUX;
    daemon->test_mode = daemon->no_network = 1;
    assert(pthread_mutex_init(&daemon->remote_mutex, NULL) == 0);
    assert(pthread_cond_init(&daemon->manifest_condition, NULL) == 0);
    daemon->remote_sync_initialized = 1;
    assert(prepare_mount_target(getuid(), getgid(), "dhcp-peer", "Library",
                                target, sizeof(target), 1, error, sizeof(error)));
    assert(snprintf(table, sizeof(table),
                     "%s\t192.0.2.50:/exports/Library\tnfs\ttest-disk\t1000\t500\trw\n",
                     target) < (int)sizeof(table));
    initial.uid = getuid();
    initial.gid = getgid();
    initial.remembered = initial.mounted = initial.available = 1;
    initial.route = SS_ROUTE_LAN;
    initial.access = SS_ACCESS_READ_WRITE;
    assert(ss_copy_string(initial.server, sizeof(initial.server), "dhcp-peer"));
    assert(ss_copy_string(initial.share, sizeof(initial.share), "Library"));
    assert(ss_copy_string(initial.hostname, sizeof(initial.hostname), "dhcp-peer.local"));
    assert(ss_copy_string(initial.address, sizeof(initial.address), "192.0.2.50"));
    assert(ss_copy_string(initial.lan_address, sizeof(initial.lan_address), "192.0.2.50"));
    assert(ss_copy_string(initial.export_path, sizeof(initial.export_path), "/exports/Library"));
    assert(ss_copy_string(initial.filesystem_id, sizeof(initial.filesystem_id), "test-disk"));
    assert(ss_copy_string(initial.target, sizeof(initial.target), target));
    daemon->mounts.mount_count = 1;

    /* A brief outage without an alternate endpoint keeps the original mount.
     * Once the old NFS service returns, no unmount should be attempted. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    endpoints(probes, 0, 0, 0, 0);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    assert(daemon->mounts.mounts[0].mounted);
    assert(!daemon->mounts.mounts[0].available);
    endpoints(probes, 0, 1, 0, 0);
    reconcile_mounts(daemon);
    assert(daemon->mounts.mounts[0].available);
    assert(daemon->mounts.mounts[0].misses == 0);
    check_commands(commands, 0, 0, 0);

    /* Rediscovery can arrive while the old address still accepts NFS TCP
     * connections. A busy mount stays intact; normal migration retries later
     * without losing the newly discovered endpoint or lazily detaching. */
    rediscover(daemon, "192.0.2.51");
    endpoints(probes, 1, 1, 1, 1);
    reconcile_mounts(daemon);
    assert(daemon->mounts.mounts[0].mounted);
    assert(strcmp(daemon->mounts.mounts[0].lan_address, "192.0.2.51") == 0);
    assert(strcmp(daemon->mounts.mounts[0].address, "192.0.2.50") == 0);
    check_commands(commands, 1, 0, 0);
    remote = copy_cached_remote(daemon, "dhcp-peer");
    assert(remote && strcmp(remote->address, "192.0.2.51") == 0);
    free(remote);
    write_fixture(commands, "");
    reconcile_mounts(daemon);
    check_commands(commands, 0, 0, 0); /* bounded retry interval */
    daemon->mounts.mounts[0].last_lan_retry_ms = 0;
    assert(setenv("SIMPLESERVE_TEST_NORMAL_UNMOUNT_BUSY", "0", 1) == 0);
    reconcile_mounts(daemon);
    check_commands(commands, 1, 0, 1);
    check_reconnected(daemon, target);

    /* The explicit connect path must make the same safe migration. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    remote = copy_cached_remote(daemon, "dhcp-peer");
    assert(remote);
    ss_buffer_init(&message);
    assert(perform_mount(daemon, getuid(), getgid(), remote, &remote->shares[0],
                          1, &message, error, sizeof(error)));
    ss_buffer_free(&message);
    free(remote);
    check_commands(commands, 1, 0, 1);
    check_reconnected(daemon, target);

    /* Dead old endpoint, live replacement: a busy mount uses the guarded
     * lazy detach, preserving old descriptors and hard-NFS write behavior. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    assert(setenv("SIMPLESERVE_TEST_NORMAL_UNMOUNT_BUSY", "1", 1) == 0);
    endpoints(probes, 1, 0, 1, 1); /* rpcbind is not NFS liveness */
    reconcile_mounts(daemon);
    check_commands(commands, 1, 1, 1);
    check_reconnected(daemon, target);

    /* A mount failure after detach keeps the remembered replacement address.
     * Rediscovery can retry without mounting over the old filesystem. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    assert(setenv("SIMPLESERVE_TEST_COMMAND_FAIL", "/bin/mount", 1) == 0);
    reconcile_mounts(daemon);
    assert(!daemon->mounts.mounts[0].mounted);
    assert(daemon->mounts.mounts[0].remembered);
    assert(strcmp(daemon->mounts.mounts[0].lan_address, "192.0.2.51") == 0);
    check_commands(commands, 1, 1, 1);
    assert(unsetenv("SIMPLESERVE_TEST_COMMAND_FAIL") == 0);
    write_fixture(mounts, ""); /* simulated detach removed the mount */
    write_fixture(commands, "");
    rediscover(daemon, "192.0.2.51");
    reconcile_mounts(daemon);
    check_commands(commands, 0, 0, 1);
    check_reconnected(daemon, target);

    /* Timeout of normal unmount has the same safe fallback. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    assert(setenv("SIMPLESERVE_TEST_NORMAL_UNMOUNT_TIMEOUT", "1", 1) == 0);
    reconcile_mounts(daemon);
    check_commands(commands, 1, 1, 1);
    check_reconnected(daemon, target);
    assert(unsetenv("SIMPLESERVE_TEST_NORMAL_UNMOUNT_TIMEOUT") == 0);

    /* A failed detach cannot be followed by a mount over the old filesystem.
     * Keep both addresses so rediscovery and the next retry can recover. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    assert(setenv("SIMPLESERVE_TEST_COMMAND_FAIL", "-l", 1) == 0);
    reconcile_mounts(daemon);
    assert(daemon->mounts.mounts[0].mounted);
    assert(daemon->mounts.mounts[0].remembered);
    assert(strcmp(daemon->mounts.mounts[0].address, "192.0.2.50") == 0);
    assert(strcmp(daemon->mounts.mounts[0].lan_address, "192.0.2.51") == 0);
    check_commands(commands, 1, 1, 0);
    assert(unsetenv("SIMPLESERVE_TEST_COMMAND_FAIL") == 0);
    rediscover(daemon, "192.0.2.51");
    write_fixture(commands, "");
    reconcile_mounts(daemon);
    check_commands(commands, 1, 1, 1);
    check_reconnected(daemon, target);

    /* A replacement advertising only rpcbind must not receive a mount.
     * After the outage grace period, remove the stale namespace entry, then
     * reconnect automatically when rediscovery and NFS are both available. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    endpoints(probes, 0, 0, 1, 0);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    check_commands(commands, 0, 0, 0);
    reconcile_mounts(daemon);
    check_commands(commands, 1, 1, 0);
    assert(!daemon->mounts.mounts[0].mounted);
    assert(daemon->mounts.mounts[0].remembered);
    assert(access(target, F_OK) != 0 && errno == ENOENT);
    write_fixture(mounts, ""); /* simulated detach removed the mount */
    write_fixture(commands, "");
    reconcile_mounts(daemon);
    check_commands(commands, 0, 0, 0);
    endpoints(probes, 0, 0, 1, 1);
    rediscover(daemon, "192.0.2.51");
    reconcile_mounts(daemon);
    check_commands(commands, 0, 0, 1);
    check_reconnected(daemon, target);

    assert(unlink(mounts) == 0);
    assert(unlink(commands) == 0);
    assert(unlink(probes) == 0);
    assert(unlink(daemon->state_path) == 0);
    assert(rmdir(target) == 0);
    assert(snprintf(target, sizeof(target), "%s/SimpleServe/dhcp-peer", home) < (int)sizeof(target));
    assert(rmdir(target) == 0);
    assert(snprintf(target, sizeof(target), "%s/SimpleServe", home) < (int)sizeof(target));
    assert(rmdir(target) == 0);
    assert(rmdir(home) == 0);
    assert(rmdir(root) == 0);
    pthread_mutex_destroy(&daemon->remote_mutex);
    pthread_cond_destroy(&daemon->manifest_condition);
    free(daemon);
    puts("OK DHCP address migration, busy/temporary outages, guarded detach, retry, and rediscovery recovery");
    return 0;
}
