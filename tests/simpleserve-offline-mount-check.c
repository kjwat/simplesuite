#define main simpleserved_program_main
#include "../simpleserved.c"
#undef main

#include <assert.h>

static void write_fixture(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");

    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static char *read_fixture(const char *path)
{
    char *text = NULL;
    char error[512];
    size_t length;

    assert(ss_read_file(path, 1024 * 1024, &text, &length, error,
                        sizeof(error)));
    return text;
}

int main(void)
{
    char temporary[] = "/tmp/simpleserve-offline-mount.XXXXXX";
    char *root = mkdtemp(temporary);
    char home[PATH_MAX], mounts[PATH_MAX], commands[PATH_MAX];
    char target[PATH_MAX], table[PATH_MAX * 2], error[1024];
    SSBuffer message;
    SSDaemon *daemon = calloc(1, sizeof(*daemon));
    SSClientMount initial;
    char *text;

    assert(root && daemon);
    assert(snprintf(home, sizeof(home), "%s/home", root) < (int)sizeof(home));
    assert(snprintf(mounts, sizeof(mounts), "%s/mounts", root) < (int)sizeof(mounts));
    assert(snprintf(commands, sizeof(commands), "%s/commands", root) < (int)sizeof(commands));
    assert(snprintf(daemon->state_path, sizeof(daemon->state_path),
                     "%s/state", root) < (int)sizeof(daemon->state_path));
    assert(mkdir(home, 0700) == 0);
    assert(setenv("SIMPLESERVE_TEST_HOME", home, 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_MOUNTS", mounts, 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_COMMAND_LOG", commands, 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_LAN_REACHABLE", "0", 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_TAILSCALE_REACHABLE", "0", 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_NORMAL_UNMOUNT_BUSY", "1", 1) == 0);
    write_fixture(mounts, "");
    write_fixture(commands, "");
    daemon->platform = SS_PLATFORM_LINUX;
    daemon->test_mode = 1;
    daemon->no_network = 1;
    daemon->remote_sync_initialized = 1;
    assert(pthread_mutex_init(&daemon->remote_mutex, NULL) == 0);
    assert(pthread_cond_init(&daemon->manifest_condition, NULL) == 0);
    assert(prepare_mount_target(getuid(), getgid(), "offline-peer", "Library",
                                target, sizeof(target), 1, error, sizeof(error)));
    assert(snprintf(table, sizeof(table),
                     "%s\t192.0.2.50:/exports/Library\tnfs\ttest-disk\t1000\t500\trw\n",
                     target) < (int)sizeof(table));
    write_fixture(mounts, table);
    memset(&initial, 0, sizeof(initial));
    initial.uid = getuid();
    initial.gid = getgid();
    initial.remembered = initial.mounted = initial.available = 1;
    initial.route = SS_ROUTE_LAN;
    initial.access = SS_ACCESS_READ_WRITE;
    assert(ss_copy_string(initial.server, sizeof(initial.server), "offline-peer"));
    assert(ss_copy_string(initial.share, sizeof(initial.share), "Library"));
    assert(ss_copy_string(initial.hostname, sizeof(initial.hostname), "offline-peer.local"));
    assert(ss_copy_string(initial.lan_address, sizeof(initial.lan_address), "192.0.2.50"));
    assert(ss_copy_string(initial.address, sizeof(initial.address), "192.0.2.50"));
    assert(ss_copy_string(initial.export_path, sizeof(initial.export_path), "/exports/Library"));
    assert(ss_copy_string(initial.filesystem_id, sizeof(initial.filesystem_id), "test-disk"));
    assert(ss_copy_string(initial.target, sizeof(initial.target), target));
    daemon->mounts.mount_count = 1;
    daemon->mounts.mounts[0] = initial;

    /* No alternate server/route exists. A busy remembered mount must still
     * leave the namespace after the normal outage grace period. */
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    assert(daemon->mounts.mounts[0].mounted);
    reconcile_mounts(daemon);
    assert(!daemon->mounts.mounts[0].mounted);
    assert(!daemon->mounts.mounts[0].available);
    assert(daemon->mounts.mounts[0].remembered);
    assert(access(target, F_OK) != 0 && errno == ENOENT);
    text = read_fixture(commands);
    assert(strstr(text, "/bin/umount\t-i\t-c\t") != NULL);
    assert(strstr(text, "/bin/umount\t-i\t-c\t-l\t") != NULL);
    assert(strstr(text, "/bin/mount\t") == NULL);
    free(text);
    text = read_fixture(daemon->state_path);
    assert(strstr(text, "server=offline-peer\n") != NULL);
    free(text);

    /* An offline remembered share stays quiet until its endpoint responds;
     * returning service restores the same share and remembered identity. */
    write_fixture(mounts, "");
    write_fixture(commands, "");
    reconcile_mounts(daemon);
    assert(access(target, F_OK) != 0 && errno == ENOENT);
    text = read_fixture(commands);
    assert(text[0] == '\0');
    free(text);
    assert(setenv("SIMPLESERVE_TEST_LAN_REACHABLE", "1", 1) == 0);
    reconcile_mounts(daemon);
    assert(daemon->mounts.mounts[0].mounted);
    assert(daemon->mounts.mounts[0].available);
    assert(daemon->mounts.mounts[0].remembered);
    assert(strcmp(daemon->mounts.mounts[0].target, target) == 0);
    text = read_fixture(commands);
    assert(strstr(text, "192.0.2.50:/exports/Library") != NULL);
    free(text);

    /* rpcbind can disappear while established NFS traffic still works.
     * Conversely, a running rpcbind does not make a dead NFS server usable. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    assert(setenv("SIMPLESERVE_TEST_LAN_REACHABLE", "0", 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_LAN_NFS_REACHABLE", "1", 1) == 0);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    assert(daemon->mounts.mounts[0].mounted);
    assert(daemon->mounts.mounts[0].available);
    ss_buffer_init(&message);
    assert(!perform_unmount(daemon, getuid(), getgid(), "offline-peer", "Library",
                             &message, error, sizeof(error)));
    ss_buffer_free(&message);
    text = read_fixture(commands);
    assert(strstr(text, "\t-l\t") == NULL);
    free(text);
    write_fixture(commands, "");
    assert(setenv("SIMPLESERVE_TEST_LAN_REACHABLE", "1", 1) == 0);
    assert(setenv("SIMPLESERVE_TEST_LAN_NFS_REACHABLE", "0", 1) == 0);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    assert(!daemon->mounts.mounts[0].mounted);
    write_fixture(mounts, "");
    write_fixture(commands, "");
    reconcile_mounts(daemon);
    text = read_fixture(commands);
    assert(text[0] == '\0');
    free(text);
    assert(unsetenv("SIMPLESERVE_TEST_LAN_NFS_REACHABLE") == 0);

    /* Explicit disconnect must work for the same offline/busy case, but a
     * healthy busy mount must never be detached behind an active transfer. */
    daemon->mounts.mounts[0] = initial;
    write_fixture(mounts, table);
    write_fixture(commands, "");
    ss_buffer_init(&message);
    assert(!perform_unmount(daemon, getuid(), getgid(), "offline-peer", "Library",
                             &message, error, sizeof(error)));
    assert(daemon->mounts.mount_count == 1);
    text = read_fixture(commands);
    assert(strstr(text, "\t-l\t") == NULL);
    free(text);
    assert(setenv("SIMPLESERVE_TEST_LAN_REACHABLE", "0", 1) == 0);
    assert(perform_unmount(daemon, getuid(), getgid(), "offline-peer", "Library",
                            &message, error, sizeof(error)));
    assert(daemon->mounts.mount_count == 0);
    ss_buffer_free(&message);

    /* Do not detach a replacement mount belonging to another server. */
    daemon->mounts.mount_count = 1;
    daemon->mounts.mounts[0] = initial;
    write_fixture(commands, "");
    assert(snprintf(table, sizeof(table),
                     "%s\t192.0.2.99:/exports/Library\tnfs\ttest-disk\t1000\t500\trw\n",
                     target) < (int)sizeof(table));
    write_fixture(mounts, table);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    reconcile_mounts(daemon);
    ss_buffer_init(&message);
    assert(!perform_unmount(daemon, getuid(), getgid(), "offline-peer", "Library",
                             &message, error, sizeof(error)));
    ss_buffer_free(&message);
    text = read_fixture(commands);
    assert(text[0] == '\0');
    free(text);
    assert(unlink(mounts) == 0);
    assert(unlink(commands) == 0);
    assert(unlink(daemon->state_path) == 0);
    (void)rmdir(target);
    assert(snprintf(target, sizeof(target), "%s/SimpleServe/offline-peer", home) < (int)sizeof(target));
    assert(rmdir(target) == 0);
    assert(snprintf(target, sizeof(target), "%s/SimpleServe", home) < (int)sizeof(target));
    assert(rmdir(target) == 0);
    assert(rmdir(home) == 0);
    assert(rmdir(root) == 0);
    pthread_mutex_destroy(&daemon->remote_mutex);
    pthread_cond_destroy(&daemon->manifest_condition);
    free(daemon);
    puts("OK offline busy mount release, remembered reconnect, explicit disconnect, and healthy/unmanaged mount protection");
    return 0;
}
