#define SIMPLEFILES_TRASH_TEST 1
#define SIMPLEFILES_TRASH_IDLE_MS 150
#define SIMPLEFILES_TRASH_PROBE_MS 100
#define main simplefiles_program_main
#include "../simplefiles.c"
#undef main

#include <assert.h>

static void make_directory(const char *path)
{
    assert(mkdir(path, 0700) == 0);
}

static void make_file(const char *path)
{
    static const char contents[] = "trash test\n";
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);

    assert(fd >= 0);
    assert(write(fd, contents, sizeof(contents) - 1) ==
           (ssize_t)(sizeof(contents) - 1));
    assert(close(fd) == 0);
}

static int directory_is_empty(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;

    assert(dir);
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return 1;
}


#ifdef __linux__
typedef struct {
    pid_t pid;
    unsigned int port;
} TestNFSServer;

static void read_test_socket(int fd, void *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t count = read(fd, (char *)data + done, size - done);
        assert(count > 0);
        done += (size_t)count;
    }
}

/* Exercise real RPC sockets while keeping all filesystem mutations in /tmp.
 * The fixture mount table represents the client mount and cached entries. */
static TestNFSServer start_rpc_server(unsigned int port, int udp, int silent, unsigned int mapped_port)
{
    struct sockaddr_in address = { .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons((uint16_t)port) };
    int listener = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0), reuse = 1;
    assert(listener >= 0);
    assert(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    if (!udp) assert(listen(listener, 16) == 0);
    TestNFSServer server = { .port = ntohs(address.sin_port) };
    server.pid = fork();
    assert(server.pid >= 0);
    if (server.pid == 0) {
        for (;;) {
            uint32_t call[15], reply[8];
            size_t call_size, reply_size;
            struct sockaddr_in client;
            socklen_t client_size = sizeof(client);
            int fd = listener;
            if (udp) {
                ssize_t count = recvfrom(listener, call + 1, sizeof(call) - 4, 0,
                                         (struct sockaddr *)&client, &client_size);
                assert(count >= 40);
                call_size = (size_t)count + 4;
            } else {
                fd = accept(listener, NULL, NULL);
                assert(fd >= 0);
                if (silent) for (;;) pause();
                read_test_socket(fd, call, 4);
                call_size = (ntohl(call[0]) & 0x7fffffffU) + 4;
                assert(call_size <= sizeof(call) && call_size >= 44);
                read_test_socket(fd, call + 1, call_size - 4);
            }
            assert(ntohl(call[3]) == 2);
            if (mapped_port) {
                assert(call_size == 60 && ntohl(call[4]) == 100000 &&
                       ntohl(call[5]) == 2 && ntohl(call[6]) == 3);
                assert(ntohl(call[11]) == 100003 && ntohl(call[12]) >= 2 && ntohl(call[12]) <= 4);
            } else {
                assert(call_size == 44 && ntohl(call[4]) == 100003 && ntohl(call[6]) == 0);
                assert(ntohl(call[5]) >= 2 && ntohl(call[5]) <= 4);
            }
            memset(reply, 0, sizeof(reply));
            reply_size = mapped_port ? 32 : 28;
            reply[0] = htonl(0x80000000U | (uint32_t)(reply_size - 4));
            reply[1] = call[1];
            reply[2] = htonl(1); /* RPC REPLY, MSG_ACCEPTED, AUTH_NONE, SUCCESS. */
            if (mapped_port) reply[7] = htonl(mapped_port);
            if (udp) {
                if (!silent)
                    assert(sendto(listener, reply + 1, reply_size - 4, 0,
                                  (struct sockaddr *)&client, client_size) == (ssize_t)reply_size - 4);
            } else {
                /* Split the record header and body to exercise stream reads. */
                assert(write(fd, reply, 2) == 2);
                assert(write(fd, (char *)reply + 2, reply_size - 2) == (ssize_t)reply_size - 2);
                close(fd);
            }
        }
    }
    close(listener);
    return server;
}

static TestNFSServer start_nfs_server(unsigned int port, int udp, int silent)
{
    return start_rpc_server(port, udp, silent, 0);
}

static void stop_nfs_server(TestNFSServer server)
{
    assert(kill(server.pid, SIGTERM) == 0);
    assert(waitpid(server.pid, NULL, 0) == server.pid);
}

static const char *blocked_volume;
static void block_location(const char *path)
{
    if (blocked_volume && strcmp(path, blocked_volume) == 0)
        for (;;) pause();
}

static void make_trash(const char *root)
{
    char path[PATH_MAX];
    make_directory(root);
    join_path(path, root, "files");
    make_directory(path);
    join_path(path, root, "info");
    make_directory(path);
    join_path(path, root, "files/deleted");
    make_directory(path);
    join_path(path, root, "files/deleted/nested");
    make_file(path);
    join_path(path, root, "info/deleted.trashinfo");
    make_file(path);
}

static void assert_trash(const char *root, int present)
{
    char path[PATH_MAX];
    join_path(path, root, "files");
    assert(directory_is_empty(path) == !present);
    join_path(path, root, "info/deleted.trashinfo");
    assert(path_exists(path) == present);
}

static void refill_trash(const char *root)
{
    char path[PATH_MAX];
    join_path(path, root, "files/deleted");
    make_directory(path);
    join_path(path, root, "files/deleted/nested");
    make_file(path);
    join_path(path, root, "info/deleted.trashinfo");
    make_file(path);
}

static void finish_empty_trash(const char *expected_message)
{
    int64_t started = sui_monotonic_ms();
    empty_trash_now();
    assert(file_operation_pid > 0);
    for (int i = 0; i < 200 && file_operation_pid > 0; i++) {
        (void)check_background_file_operation();
        if (file_operation_pid > 0) usleep(10000);
    }
    assert(file_operation_pid < 0);
    assert(sui_monotonic_ms() - started < 2000);
    assert(strcmp(message, expected_message) == 0);
}

static void write_nfs_mounts(const char *table, const char *remote,
                             const char *local, const char *nested_local,
                             unsigned int port, int udp)
{
    FILE *mounts = fopen(table, "w");
    assert(mounts);
    fprintf(mounts, "server:/export %s %s rw,addr=127.0.0.1,proto=%s,vers=%s,port=%u 0 0\n",
            remote, udp ? "nfs" : "nfs4", udp ? "udp" : "tcp", udp ? "3" : "4.2", port);
    /* Local mounted-volume trash must complete even when NFS precedes it. */
    fprintf(mounts, "local %s ext4 rw 0 0\n", local);
    fprintf(mounts, "local %s ext4 rw 0 0\n", nested_local);
    assert(fclose(mounts) == 0);
}

static void test_nfs_connection_cycle(const char *root, int udp)
{
    char base[PATH_MAX], data[PATH_MAX], home_trash[PATH_MAX], remote[PATH_MAX];
    char local[PATH_MAX], local_trash[PATH_MAX], remote_trash[PATH_MAX];
    char shared[PATH_MAX], shared_trash[PATH_MAX], ordinary[PATH_MAX], table[PATH_MAX];
    char nested_local[PATH_MAX], nested_trash[PATH_MAX], custom_missing[PATH_MAX];
    join_path(base, root, udp ? "nfs-udp" : "nfs-tcp");
    make_directory(base);
    join_path(data, base, "data");
    make_directory(data);
    join_path(home_trash, data, "Trash");
    make_trash(home_trash);
    assert(setenv("XDG_DATA_HOME", data, 1) == 0);
    join_path(remote, base, "remote");
    join_path(local, base, "local");
    make_directory(remote);
    make_directory(local);
    assert(snprintf(remote_trash, sizeof(remote_trash), "%s/.Trash-%lu",
                    remote, (unsigned long)getuid()) < (int)sizeof(remote_trash));
    assert(snprintf(local_trash, sizeof(local_trash), "%s/.Trash-%lu",
                    local, (unsigned long)getuid()) < (int)sizeof(local_trash));
    make_trash(remote_trash);
    make_trash(local_trash);
    join_path(shared, remote, ".Trash");
    make_directory(shared);
    assert(chmod(shared, 01777) == 0);
    assert(snprintf(shared_trash, sizeof(shared_trash), "%s/%lu", shared,
                    (unsigned long)getuid()) < (int)sizeof(shared_trash));
    make_trash(shared_trash);
    join_path(ordinary, remote, "keep-ordinary-file");
    make_file(ordinary);
    join_path(nested_local, remote, "nested-local-mount");
    make_directory(nested_local);
    assert(snprintf(nested_trash, sizeof(nested_trash), "%s/.Trash-%lu",
                    nested_local, (unsigned long)getuid()) < (int)sizeof(nested_trash));
    make_trash(nested_trash);
    join_path(custom_missing, remote, "do-not-create");
    join_path(table, base, "mounts");

    TestNFSServer server = start_nfs_server(0, udp, 0);
    TestNFSServer rpcbind = start_rpc_server(0, udp, 0, server.port);
    char rpcbind_port[16];
    snprintf(rpcbind_port, sizeof(rpcbind_port), "%u", rpcbind.port);
    trash_rpcbind_port_test = rpcbind_port;
    unsigned int port = server.port;
    write_nfs_mounts(table, remote, local, nested_local, port, udp);
    trash_mount_table_test_path = table;
    config_trash_dir[0] = '\0';
    finish_empty_trash("trash emptied");
    assert_trash(home_trash, 0);
    assert_trash(local_trash, 0);
    assert_trash(remote_trash, 0);
    assert_trash(shared_trash, 0);
    assert_trash(nested_trash, 0);
    assert(path_exists(ordinary));

    /* Explicit port=0 discovers the service rather than assuming 2049. */
    write_nfs_mounts(table, remote, local, nested_local, 0, udp);
    assert(empty_default_trash() == 0);

    stop_nfs_server(server);
    refill_trash(home_trash);
    refill_trash(local_trash);
    refill_trash(remote_trash);
    refill_trash(shared_trash);
    refill_trash(nested_trash);
    finish_empty_trash("reachable trash emptied; unavailable trash deferred");
    assert_trash(home_trash, 0);
    assert_trash(local_trash, 0);
    assert_trash(nested_trash, 0);
    /* The cached fixture directories are accessible, but cannot authorize
     * remote deletion or a false 'empty' result while the server is down. */
    assert_trash(remote_trash, 1);
    assert_trash(shared_trash, 1);
    assert(path_exists(ordinary));
    safe_copy(config_trash_dir, sizeof(config_trash_dir), custom_missing);
    assert(empty_trash_bounded(custom_missing, 2) == TRASH_DEFERRED);
    assert(!path_exists(custom_missing));
    safe_copy(config_trash_dir, sizeof(config_trash_dir), remote_trash);
    assert(empty_trash_bounded(remote_trash, 2) == TRASH_DEFERRED);
    assert_trash(remote_trash, 1);
    config_trash_dir[0] = '\0';

    /* A mount removed between enumeration and the worker must never fall
     * back to its ordinary local mount-point directory. */
    FILE *mounts = fopen(table, "w");
    assert(mounts);
    fprintf(mounts, "local %s ext4 rw 0 0\n", remote);
    assert(fclose(mounts) == 0);
    assert(empty_trash_bounded_for_mount(remote, 1, "server:/export") == TRASH_DEFERRED);
    assert_trash(remote_trash, 1);
    assert_trash(shared_trash, 1);
    assert(path_exists(ordinary));
    write_nfs_mounts(table, remote, local, nested_local, port, udp);

    server = start_nfs_server(port, udp, 0);
    refill_trash(home_trash);
    finish_empty_trash("trash emptied");
    assert_trash(home_trash, 0);
    assert_trash(remote_trash, 0);
    assert_trash(shared_trash, 0);
    assert(path_exists(ordinary));
    if (getuid() != 0) {
        char files[PATH_MAX];
        refill_trash(home_trash);
        refill_trash(remote_trash);
        join_path(files, remote_trash, "files");
        assert(chmod(files, 0000) == 0);
        finish_empty_trash("trash partly emptied; some failed");
        assert_trash(home_trash, 0);
        assert(chmod(files, 0700) == 0);
        assert_trash(remote_trash, 1);
        assert(empty_default_trash() == 0);
    }
    stop_nfs_server(server);

    /* A listening endpoint that does not answer RPC is also unavailable. */
    server = start_nfs_server(port, udp, 1);
    refill_trash(home_trash);
    refill_trash(remote_trash);
    finish_empty_trash("reachable trash emptied; unavailable trash deferred");
    assert_trash(home_trash, 0);
    assert_trash(remote_trash, 1);
    assert(path_exists(ordinary));
    stop_nfs_server(server);
    stop_nfs_server(rpcbind);
    trash_rpcbind_port_test = NULL;
    trash_mount_table_test_path = NULL;
}

static void test_offline_trash(const char *root)
{
    TestNFSServer server = start_nfs_server(0, 0, 0);
    char data[PATH_MAX], local[PATH_MAX], online[PATH_MAX], offline[PATH_MAX];
    char online_trash[PATH_MAX], offline_trash[PATH_MAX], table[PATH_MAX];
    join_path(data, root, "data");
    make_directory(data);
    join_path(local, data, "Trash");
    make_trash(local);
    assert(setenv("XDG_DATA_HOME", data, 1) == 0);
    join_path(online, root, "online");
    join_path(offline, root, "offline");
    make_directory(online);
    make_directory(offline);
    assert(snprintf(online_trash, sizeof(online_trash), "%s/.Trash-%lu",
                    online, (unsigned long)getuid()) < (int)sizeof(online_trash));
    assert(snprintf(offline_trash, sizeof(offline_trash), "%s/.Trash-%lu",
                    offline, (unsigned long)getuid()) < (int)sizeof(offline_trash));
    make_trash(online_trash);
    make_trash(offline_trash);
    join_path(table, root, "mounts");
    FILE *mounts = fopen(table, "w");
    assert(mounts);
    fprintf(mounts, "server:/online %s nfs rw,addr=127.0.0.1,proto=tcp,vers=3,port=%u 0 0\n", online, server.port);
    fprintf(mounts, "server:/offline %s nfs rw,addr=127.0.0.1,proto=tcp,vers=3,port=%u 0 0\n", offline, server.port);
    assert(fclose(mounts) == 0);
    trash_mount_table_test_path = table;
    trash_location_test_hook = block_location;
    blocked_volume = offline;
    config_trash_dir[0] = '\0';

    empty_trash_now();
    for (int i = 0; i < 200 && file_operation_pid > 0; i++) {
        (void)check_background_file_operation();
        if (file_operation_pid > 0) usleep(10000);
    }
    assert(file_operation_pid < 0);
    assert(strcmp(message,
                  "reachable trash emptied; unavailable trash deferred") == 0);
    assert_trash(local, 0);
    assert_trash(online_trash, 0);
    assert_trash(offline_trash, 1);

    /* The same normal operation picks up the deferred files on recovery. */
    blocked_volume = NULL;
    assert(empty_default_trash() == 0);
    assert_trash(offline_trash, 0);

    /* Refuse a symlinked payload directory, preserving unrelated data. */
    char payload[PATH_MAX], victim[PATH_MAX];
    join_path(payload, local, "files");
    assert(rmdir(payload) == 0);
    join_path(victim, root, "victim");
    make_directory(victim);
    assert(symlink(victim, payload) == 0);
    join_path(payload, victim, "keep");
    make_file(payload);
    assert(empty_trash_bounded(local, 0) == 1);
    assert(path_exists(payload));

    /* Custom trash on an unavailable filesystem also returns promptly. */
    blocked_volume = offline;
    safe_copy(config_trash_dir, sizeof(config_trash_dir), offline);
    assert(empty_trash_bounded(offline, 2) == TRASH_DEFERRED);
    assert(path_exists(offline_trash));
    trash_location_test_hook = NULL;
    trash_mount_table_test_path = NULL;
    stop_nfs_server(server);
}

static void test_kernel_mount_trash(const char *root)
{
    char data[PATH_MAX], local[PATH_MAX], kernel[PATH_MAX], volume[PATH_MAX];
    char kernel_trash[PATH_MAX], volume_trash[PATH_MAX], table[PATH_MAX];
    join_path(data, root, "kernel-data");
    make_directory(data);
    join_path(local, data, "Trash");
    make_trash(local);
    assert(setenv("XDG_DATA_HOME", data, 1) == 0);
    join_path(kernel, root, "kernel-volume");
    join_path(volume, root, "real-volume");
    make_directory(kernel);
    make_directory(volume);
    assert(snprintf(kernel_trash, sizeof(kernel_trash), "%s/.Trash-%lu",
                    kernel, (unsigned long)getuid()) < (int)sizeof(kernel_trash));
    assert(snprintf(volume_trash, sizeof(volume_trash), "%s/.Trash-%lu",
                    volume, (unsigned long)getuid()) < (int)sizeof(volume_trash));
    make_trash(kernel_trash);
    make_trash(volume_trash);
    join_path(table, root, "kernel-mounts");
    FILE *mounts = fopen(table, "w");
    assert(mounts);
    fprintf(mounts, "none %s bpf rw 0 0\n", kernel);
    fprintf(mounts, "none %s efivarfs rw 0 0\n", kernel);
    fprintf(mounts, "none %s rpc_pipefs rw 0 0\n", kernel);
    fprintf(mounts, "none %s nsfs rw 0 0\n", kernel);
    fprintf(mounts, "none %s tmpfs rw 0 0\n", volume);
    assert(fclose(mounts) == 0);
    trash_mount_table_test_path = table;
    trash_location_test_hook = block_location;
    /* Accessing a kernel mount must be avoided before spawning a worker.
     * Otherwise its permission error or stalled I/O produces a false warning. */
    blocked_volume = kernel;
    config_trash_dir[0] = '\0';
    empty_trash_now();
    for (int i = 0; i < 200 && file_operation_pid > 0; i++) {
        (void)check_background_file_operation();
        if (file_operation_pid > 0) usleep(10000);
    }
    assert(file_operation_pid < 0);
    assert(strcmp(message, "trash emptied") == 0);
    assert_trash(local, 0);
    assert_trash(volume_trash, 0);
    assert_trash(kernel_trash, 1);
    /* Repeating the operation on an empty trash must also report success. */
    assert(empty_default_trash() == 0);

    if (getuid() != 0) {
        char files[PATH_MAX];
        join_path(files, volume_trash, "files");
        assert(chmod(files, 0000) == 0);
        /* A genuine permission error on a data filesystem remains a failure. */
        assert(empty_default_trash() == 2);
        assert(chmod(files, 0700) == 0);
    }
    trash_location_test_hook = NULL;
    trash_mount_table_test_path = NULL;
    blocked_volume = NULL;
}
#endif

int main(void)
{
    char root[] = "/tmp/simplefiles-trash-test.XXXXXX";
    char default_trash[PATH_MAX];
    char default_file[PATH_MAX];
    char default_dir[PATH_MAX];
    char custom_trash[PATH_MAX];
    char custom_file[PATH_MAX];
    char custom_dir[PATH_MAX];
    char nested_file[PATH_MAX];
    char decoy_file[PATH_MAX];


#ifndef __linux__
    assert(strcmp(select_delete_authorizer(1, 1, 1), "sudo") == 0);
    assert(strcmp(select_delete_authorizer(1, 0, 1), "pkexec") == 0);
    assert(strcmp(select_delete_authorizer(0, 1, 1), "pkexec") == 0);
    assert(strcmp(select_delete_authorizer(0, 1, 0), "sudo") == 0);
    assert(select_delete_authorizer(1, 0, 0) == NULL);
#endif

    assert(mkdtemp(root));
    join_path(default_trash, root, "default");
    join_path(default_file, default_trash, "file");
    join_path(default_dir, default_trash, "empty-dir");
    join_path(custom_trash, root, "custom");
    join_path(custom_file, custom_trash, "file");
    join_path(custom_dir, custom_trash, "nested");
    join_path(nested_file, custom_dir, "file");
    join_path(decoy_file, default_trash, "keep");

    make_directory(default_trash);
    make_file(default_file);
    make_directory(default_dir);
    make_directory(custom_trash);

    safe_copy(cwd_path, sizeof(cwd_path), root);
    config_trash_dir[0] = '\0';
    int ok = 0;
    int fail = 0;

    make_file(decoy_file);
    make_file(custom_file);
    make_directory(custom_dir);
    make_file(nested_file);
    safe_copy(config_trash_dir, sizeof(config_trash_dir), custom_trash);

    ok = 0;
    fail = 0;
    assert(empty_configured_trash(&ok, &fail) == 0);
    assert(ok == 2 && fail == 0);
    assert(directory_is_empty(custom_trash));
    assert(path_exists(decoy_file));

    ok = 0;
    fail = 0;
    assert(empty_configured_trash(&ok, &fail) == 0);
    assert(ok == 0 && fail == 0);

    /* The interactive path must hand a potentially large recursive removal
     * to a worker instead of holding the curses event loop. */
    make_file(custom_file);
    empty_trash_now();
    assert(file_operation_pid > 0);
    assert(file_operation_kind == FILE_OPERATION_EMPTY_TRASH);
    for (int i = 0; i < 200 && file_operation_pid > 0; i++) {
        (void)check_background_file_operation();
        if (file_operation_pid > 0) usleep(10000);
    }
    assert(file_operation_pid < 0);
    assert(directory_is_empty(custom_trash));
    assert(strcmp(message, "trash emptied") == 0);

#ifdef __linux__
    test_nfs_connection_cycle(root, 0);
    test_nfs_connection_cycle(root, 1);
    test_offline_trash(root);
    test_kernel_mount_trash(root);
#endif
    assert(remove_recursive(root) == 0);
    return 0;
}
