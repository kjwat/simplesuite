#define _GNU_SOURCE

#include "simpleserve.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define SS_DAEMON_CONFIG_MAX (1024U * 1024U)

typedef struct {
    SSPlatform platform;
    char socket_path[PATH_MAX];
    char config_path[PATH_MAX];
    char state_path[PATH_MAX];
    char exports_path[PATH_MAX];
    SSServerConfig config;
    SSMountConfig mounts;
    SSRemoteServer remotes[SS_MAX_SERVERS];
    size_t remote_count;
    int control_fd;
    int manifest_fd;
    pid_t publisher_pid;
    int test_mode;
    int no_network;
    time_t last_local_refresh;
    time_t last_remote_refresh;
} SSDaemon;

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void daemon_error(char *error, size_t error_size,
                         const char *format, ...)
{
    va_list arguments;

    if (!error || error_size == 0)
        return;
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int set_close_on_exec(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFD, 0);

    return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static int trusted_executable(const char *path)
{
    struct stat status;

    if (!path || path[0] != '/')
        return 0;
    if (getenv("SIMPLESERVE_TEST_MODE"))
        return 1;
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        access(path, X_OK) != 0)
        return 0;
    return status.st_uid == 0 && (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

static const char *first_command(const SSDaemon *daemon,
                                 const char *const candidates[])
{
    if (daemon->test_mode)
        return candidates[0];
    for (size_t index = 0; candidates[index]; index++) {
        if (trusted_executable(candidates[index]))
            return candidates[index];
    }
    return NULL;
}

static int append_command_log(const SSDaemon *daemon, const SSCommand *command,
                              char *error, size_t error_size)
{
    const char *log_path = getenv("SIMPLESERVE_TEST_COMMAND_LOG");
    int descriptor;

    (void)daemon;
    if (!log_path || !*log_path)
        return 1;
    descriptor = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (descriptor < 0) {
        daemon_error(error, error_size, "cannot open command log: %s",
                     strerror(errno));
        return 0;
    }
    for (size_t index = 0; index < command->argc; index++) {
        if (index && write(descriptor, "\t", 1) != 1)
            goto write_failed;
        if (write(descriptor, command->argv[index],
                  strlen(command->argv[index])) !=
            (ssize_t)strlen(command->argv[index]))
            goto write_failed;
    }
    if (write(descriptor, "\n", 1) != 1)
        goto write_failed;
    close(descriptor);
    return 1;

write_failed:
    daemon_error(error, error_size, "cannot write command log: %s",
                 strerror(errno));
    close(descriptor);
    return 0;
}

static int run_command_capture(const SSDaemon *daemon, SSCommand *command,
                               int timeout_ms, char *output,
                               size_t output_size, char *error,
                               size_t error_size)
{
    static char safe_path[] =
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin";
    static char safe_locale[] = "LC_ALL=C";
    static char safe_lang[] = "LANG=C";
    static char safe_home[] = "HOME=/";
    static char *environment[] = {
        safe_path, safe_locale, safe_lang, safe_home, NULL
    };
    int pipes[2];
    int status = 0;
    int flags;
    int done = 0;
    int eof = 0;
    size_t used = 0;
    pid_t child;
    long long deadline;

    if (!daemon || !command || command->argc == 0 || !output ||
        output_size < 2 || timeout_ms < 1) {
        daemon_error(error, error_size, "invalid command request");
        return 0;
    }
    output[0] = '\0';
    if (!append_command_log(daemon, command, error, error_size))
        return 0;
    if (daemon->test_mode) {
        const char *failure = getenv("SIMPLESERVE_TEST_COMMAND_FAIL");

        if (failure && strstr(command->argv[0], failure)) {
            daemon_error(error, error_size, "test command failure: %s",
                         command->argv[0]);
            return 0;
        }
        return 1;
    }
    if (!trusted_executable(command->argv[0])) {
        daemon_error(error, error_size, "required trusted command is missing: %s",
                     command->argv[0]);
        return 0;
    }
    if (pipe(pipes) != 0) {
        daemon_error(error, error_size, "cannot capture command output: %s",
                     strerror(errno));
        return 0;
    }
    child = fork();
    if (child == 0) {
        int null_fd;

        setpgid(0, 0);
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0 ||
            dup2(pipes[1], STDERR_FILENO) < 0)
            _exit(127);
        if (pipes[1] > STDERR_FILENO)
            close(pipes[1]);
        null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execve(command->argv[0], command->argv, environment);
        _exit(127);
    }
    if (child < 0) {
        daemon_error(error, error_size, "cannot start %s: %s",
                     command->argv[0], strerror(errno));
        close(pipes[0]);
        close(pipes[1]);
        return 0;
    }
    close(pipes[1]);
    flags = fcntl(pipes[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK);
    deadline = monotonic_ms() + timeout_ms;
    while (!done || !eof) {
        struct pollfd descriptor = {pipes[0], POLLIN | POLLHUP, 0};
        long long remaining = deadline - monotonic_ms();
        int wait_ms = remaining > 100 ? 100 : (remaining > 0 ? (int)remaining : 0);

        if (remaining <= 0)
            break;
        (void)poll(&descriptor, 1, wait_ms);
        for (;;) {
            char chunk[1024];
            ssize_t received = read(pipes[0], chunk, sizeof(chunk));

            if (received > 0) {
                size_t available = output_size - 1 - used;
                size_t copy = (size_t)received < available ?
                    (size_t)received : available;

                if (copy) {
                    memcpy(output + used, chunk, copy);
                    used += copy;
                }
                continue;
            }
            if (received == 0)
                eof = 1;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                eof = 1;
            break;
        }
        if (!done) {
            pid_t result = waitpid(child, &status, WNOHANG);

            if (result == child)
                done = 1;
            else if (result < 0 && errno != EINTR)
                done = 1;
        }
    }
    if (!done) {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
        close(pipes[0]);
        output[used] = '\0';
        daemon_error(error, error_size, "%s timed out", command->argv[0]);
        return 0;
    }
    close(pipes[0]);
    output[used] = '\0';
    while (used > 0 && (output[used - 1] == '\r' || output[used - 1] == '\n'))
        output[--used] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        daemon_error(error, error_size, "%s failed%s%s", command->argv[0],
                     output[0] ? ": " : "", output);
        return 0;
    }
    return 1;
}

static int run_command(const SSDaemon *daemon, SSCommand *command,
                       int timeout_ms, char *error, size_t error_size)
{
    char output[4096];

    return run_command_capture(daemon, command, timeout_ms, output,
                               sizeof(output), error, error_size);
}

static int command_from(SSCommand *command, const char *program,
                        const char *const arguments[])
{
    ss_command_init(command);
    if (!program || !ss_command_add(command, program))
        return 0;
    for (size_t index = 0; arguments && arguments[index]; index++) {
        if (!ss_command_add(command, arguments[index]))
            return 0;
    }
    return 1;
}

static size_t active_share_count(const SSDaemon *daemon)
{
    size_t count = 0;

    for (size_t index = 0; index < daemon->config.share_count; index++) {
        if (daemon->config.shares[index].active)
            count++;
    }
    return count;
}

static int refresh_local_shares(SSDaemon *daemon, int *changed)
{
    char ignored[512];

    *changed = 0;
    for (size_t index = 0; index < daemon->config.share_count; index++) {
        SSLocalShare *share = &daemon->config.shares[index];
        SSMountInfo mount;
        int old_active = share->active;
        char old_path[PATH_MAX];
        int found;

        ss_copy_string(old_path, sizeof(old_path), share->current_path);
        memset(&mount, 0, sizeof(mount));
        found = ss_mount_info_exact(share->configured_path, &mount, ignored,
                                    sizeof(ignored));
        if (!found || strcmp(mount.identity, share->filesystem_id) != 0)
            found = ss_find_mount_by_identity(share->filesystem_id, &mount,
                                              ignored, sizeof(ignored));
        share->active = 0;
        share->current_path[0] = '\0';
        if (found && (!mount.read_only ||
                      share->access == SS_ACCESS_READ_ONLY) &&
            ss_user_can_access(share->owner_uid, share->owner_gid,
                               mount.target, share->access,
                               ignored, sizeof(ignored))) {
            share->active = 1;
            ss_copy_string(share->current_path, sizeof(share->current_path),
                           mount.target);
            ss_copy_string(share->source, sizeof(share->source), mount.source);
            ss_copy_string(share->fstype, sizeof(share->fstype), mount.fstype);
            share->total_bytes = mount.total_bytes;
            share->free_bytes = mount.free_bytes;
        }
        if (old_active != share->active ||
            strcmp(old_path, share->current_path) != 0)
            *changed = 1;
    }
    daemon->last_local_refresh = time(NULL);
    return 1;
}

static int file_contents_or_empty(const char *path, char **contents,
                                  size_t *length, char *error,
                                  size_t error_size)
{
    if (access(path, F_OK) != 0 && errno == ENOENT) {
        *contents = strdup("");
        *length = 0;
        if (!*contents) {
            daemon_error(error, error_size, "out of memory");
            return 0;
        }
        return 1;
    }
    return ss_read_file(path, 4U * 1024U * 1024U, contents, length,
                        error, error_size);
}

static int ensure_freebsd_nfs(SSDaemon *daemon, char *error,
                              size_t error_size)
{
    SSCommand command;
    const char *sysrc_args[] = {
        "-q", "rpcbind_enable=YES", "nfs_server_enable=YES",
        "mountd_enable=YES", "rpc_statd_enable=YES",
        "rpc_lockd_enable=YES", NULL
    };
    const char *start_rpcbind[] = {"rpcbind", "onestart", NULL};
    const char *start_nfsd[] = {"nfsd", "onestart", NULL};
    const char *start_lockd[] = {"lockd", "onestart", NULL};

    if (!command_from(&command, "/usr/sbin/sysrc", sysrc_args) ||
        !run_command(daemon, &command, 10000, error, error_size))
        return 0;
    if (!command_from(&command, "/usr/sbin/service", start_rpcbind))
        return 0;
    (void)run_command(daemon, &command, 10000, error, error_size);
    if (!command_from(&command, "/usr/sbin/service", start_nfsd))
        return 0;
    if (!run_command(daemon, &command, 15000, error, error_size)) {
        const char *status_args[] = {"nfsd", "onestatus", NULL};

        if (!command_from(&command, "/usr/sbin/service", status_args) ||
            !run_command(daemon, &command, 5000, error, error_size))
            return 0;
    }
    if (!command_from(&command, "/usr/sbin/service", start_lockd))
        return 0;
    (void)run_command(daemon, &command, 10000, error, error_size);
    return 1;
}

static int reload_freebsd_exports(SSDaemon *daemon, char *error,
                                  size_t error_size)
{
    SSCommand command;
    const char *reload_args[] = {"mountd", "reload", NULL};

    if (!command_from(&command, "/usr/sbin/service", reload_args))
        return 0;
    return run_command(daemon, &command, 10000, error, error_size);
}

static int ensure_linux_nfs(SSDaemon *daemon, char *error,
                            size_t error_size)
{
    static const char *const systemctl_paths[] = {
        "/bin/systemctl", "/usr/bin/systemctl", NULL
    };
    static const char *const service_paths[] = {
        "/usr/sbin/service", "/sbin/service", NULL
    };
    static const char *const rc_service_paths[] = {
        "/sbin/rc-service", "/usr/sbin/rc-service",
        "/bin/rc-service", "/usr/bin/rc-service", NULL
    };
    const char *test_init = getenv("SIMPLESERVE_TEST_INIT");
    const char *systemctl = first_command(daemon, systemctl_paths);
    SSCommand command;

    if ((daemon->test_mode && (!test_init || strcmp(test_init, "systemd") == 0)) ||
        (!daemon->test_mode && systemctl && access("/run/systemd/system", F_OK) == 0)) {
        const char *nfs_server[] = {"enable", "--now", "nfs-server.service", NULL};
        const char *nfs_kernel[] = {
            "enable", "--now", "nfs-kernel-server.service", NULL
        };

        if (!command_from(&command, systemctl ? systemctl : "/bin/systemctl",
                          nfs_server))
            return 0;
        if (run_command(daemon, &command, 30000, error, error_size))
            return 1;
        if (!command_from(&command, systemctl ? systemctl : "/bin/systemctl",
                          nfs_kernel))
            return 0;
        return run_command(daemon, &command, 30000, error, error_size);
    }
    {
        const char *rc_service = first_command(daemon, rc_service_paths);

        if (rc_service) {
            const char *rpcbind_args[] = {"rpcbind", "start", NULL};
            static const char *const server_names[] = {
                "nfs", "nfs-server", "nfs-kernel-server", NULL
            };

            if (!command_from(&command, rc_service, rpcbind_args))
                return 0;
            (void)run_command(daemon, &command, 30000, error, error_size);
            for (size_t index = 0; server_names[index]; index++) {
                const char *arguments[] = {
                    server_names[index], "start", NULL
                };

                if (!command_from(&command, rc_service, arguments))
                    return 0;
                if (run_command(daemon, &command, 30000, error, error_size))
                    return 1;
            }
            return 0;
        }
    }
    {
        const char *service = first_command(daemon, service_paths);
        const char *kernel_args[] = {"nfs-kernel-server", "start", NULL};
        const char *server_args[] = {"nfs-server", "start", NULL};

        if (!service) {
            daemon_error(error, error_size,
                         "no supported Linux service manager found for NFS");
            return 0;
        }
        if (!command_from(&command, service, kernel_args))
            return 0;
        if (run_command(daemon, &command, 30000, error, error_size))
            return 1;
        if (!command_from(&command, service, server_args))
            return 0;
        return run_command(daemon, &command, 30000, error, error_size);
    }
}

static int reload_linux_exports(SSDaemon *daemon, char *error,
                                size_t error_size)
{
    static const char *const exportfs_paths[] = {
        "/usr/sbin/exportfs", "/sbin/exportfs", NULL
    };
    const char *program = first_command(daemon, exportfs_paths);
    const char *arguments[] = {"-ra", NULL};
    SSCommand command;

    if (!program) {
        daemon_error(error, error_size,
                     "exportfs is missing; install the Linux NFS server package");
        return 0;
    }
    if (!command_from(&command, program, arguments))
        return 0;
    return run_command(daemon, &command, 15000, error, error_size);
}

static int sync_exports(SSDaemon *daemon, char *error, size_t error_size)
{
    SSBuffer generated;
    SSBuffer final;
    char *old_contents = NULL;
    size_t old_length = 0;
    const char *managed = "";
    int have_shares = active_share_count(daemon) > 0;
    int wrote = 0;
    int ok = 0;

    ss_buffer_init(&generated);
    ss_buffer_init(&final);
    if (have_shares &&
        !ss_render_exports(daemon->platform, &daemon->config, &generated,
                           error, error_size))
        goto done;
    if (have_shares)
        managed = generated.data;
    if (!file_contents_or_empty(daemon->exports_path, &old_contents,
                                &old_length, error, error_size))
        goto done;
    if (daemon->platform == SS_PLATFORM_FREEBSD) {
        if (!ss_replace_managed_exports(old_contents, managed, &final,
                                        error, error_size))
            goto done;
    } else if (!ss_buffer_append(&final, managed)) {
        daemon_error(error, error_size, "out of memory");
        goto done;
    }
    if (old_length != final.length ||
        memcmp(old_contents, final.data ? final.data : "", final.length) != 0) {
        if (!ss_atomic_write(daemon->exports_path,
                             final.data ? final.data : "", final.length,
                             0644, error, error_size))
            goto done;
        wrote = 1;
    }
    if (have_shares) {
        if (daemon->platform == SS_PLATFORM_FREEBSD) {
            if (!ensure_freebsd_nfs(daemon, error, error_size))
                goto rollback;
        } else if (!ensure_linux_nfs(daemon, error, error_size)) {
            goto rollback;
        }
    }
    if (wrote || have_shares) {
        if (daemon->platform == SS_PLATFORM_FREEBSD) {
            if (!reload_freebsd_exports(daemon, error, error_size))
                goto rollback;
        } else if (!reload_linux_exports(daemon, error, error_size)) {
            goto rollback;
        }
    }
    ok = 1;
    goto done;

rollback:
    if (wrote) {
        char rollback_error[512];

        if (ss_atomic_write(daemon->exports_path, old_contents, old_length,
                            0644, rollback_error, sizeof(rollback_error))) {
            if (daemon->platform == SS_PLATFORM_FREEBSD)
                (void)reload_freebsd_exports(daemon, rollback_error,
                                             sizeof(rollback_error));
            else
                (void)reload_linux_exports(daemon, rollback_error,
                                           sizeof(rollback_error));
        } else {
            fprintf(stderr, "simpleserved: export rollback failed: %s\n",
                    rollback_error);
        }
    }

done:
    free(old_contents);
    ss_buffer_free(&generated);
    ss_buffer_free(&final);
    return ok;
}

static int ensure_avahi(SSDaemon *daemon, char *error, size_t error_size)
{
    SSCommand command;

    if (daemon->platform == SS_PLATFORM_FREEBSD) {
        const char *sysrc_args[] = {
            "-q", "dbus_enable=YES", "avahi_daemon_enable=YES", NULL
        };
        const char *dbus_args[] = {"dbus", "onestart", NULL};
        const char *avahi_args[] = {"avahi-daemon", "onestart", NULL};

        if (!command_from(&command, "/usr/sbin/sysrc", sysrc_args) ||
            !run_command(daemon, &command, 10000, error, error_size))
            return 0;
        if (!command_from(&command, "/usr/sbin/service", dbus_args))
            return 0;
        (void)run_command(daemon, &command, 10000, error, error_size);
        if (!command_from(&command, "/usr/sbin/service", avahi_args))
            return 0;
        if (run_command(daemon, &command, 10000, error, error_size))
            return 1;
        {
            const char *status_args[] = {"avahi-daemon", "onestatus", NULL};

            if (!command_from(&command, "/usr/sbin/service", status_args))
                return 0;
            return run_command(daemon, &command, 5000, error, error_size);
        }
    }
    if (daemon->platform == SS_PLATFORM_LINUX) {
        static const char *const systemctl_paths[] = {
            "/bin/systemctl", "/usr/bin/systemctl", NULL
        };
        static const char *const service_paths[] = {
            "/usr/sbin/service", "/sbin/service", NULL
        };
        static const char *const rc_service_paths[] = {
            "/sbin/rc-service", "/usr/sbin/rc-service",
            "/bin/rc-service", "/usr/bin/rc-service", NULL
        };
        const char *test_init = getenv("SIMPLESERVE_TEST_INIT");
        const char *systemctl = first_command(daemon, systemctl_paths);

        if ((daemon->test_mode &&
             (!test_init || strcmp(test_init, "systemd") == 0)) ||
            (!daemon->test_mode && systemctl &&
             access("/run/systemd/system", F_OK) == 0)) {
            const char *arguments[] = {
                "enable", "--now", "avahi-daemon.service", NULL
            };

            if (!command_from(&command,
                              systemctl ? systemctl : "/bin/systemctl",
                              arguments))
                return 0;
            return run_command(daemon, &command, 30000, error, error_size);
        }
        {
            const char *rc_service = first_command(daemon, rc_service_paths);

            if (rc_service) {
                const char *arguments[] = {"avahi-daemon", "start", NULL};

                if (!command_from(&command, rc_service, arguments))
                    return 0;
                return run_command(daemon, &command, 30000, error, error_size);
            }
        }
        {
            const char *service = first_command(daemon, service_paths);
            const char *arguments[] = {"avahi-daemon", "start", NULL};

            if (!service) {
                daemon_error(error, error_size,
                             "no supported service manager found for Avahi");
                return 0;
            }
            if (!command_from(&command, service, arguments))
                return 0;
            return run_command(daemon, &command, 30000, error, error_size);
        }
    }
    daemon_error(error, error_size, "unsupported Avahi platform");
    return 0;
}

static void stop_publisher(SSDaemon *daemon)
{
    if (daemon->publisher_pid <= 0)
        return;
    (void)kill(daemon->publisher_pid, SIGTERM);
    for (int attempt = 0; attempt < 100; attempt++) {
        pid_t result = waitpid(daemon->publisher_pid, NULL, WNOHANG);

        if (result == daemon->publisher_pid ||
            (result < 0 && errno != EINTR)) {
            daemon->publisher_pid = 0;
            return;
        }
        usleep(10000);
    }
    (void)kill(daemon->publisher_pid, SIGKILL);
    (void)waitpid(daemon->publisher_pid, NULL, 0);
    daemon->publisher_pid = 0;
}

static int start_publisher(SSDaemon *daemon, char *error, size_t error_size)
{
    static const char *const publisher_paths[] = {
        "/usr/local/bin/avahi-publish-service",
        "/usr/bin/avahi-publish-service",
        "/bin/avahi-publish-service", NULL
    };
    static char safe_path[] =
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin";
    static char safe_locale[] = "LC_ALL=C";
    static char safe_home[] = "HOME=/";
    static char *environment[] = {safe_path, safe_locale, safe_home, NULL};
    const char *program;
    char service_name[SS_MAX_NAME + 32];
    char port[16];
    char version[32];
    char server[SS_MAX_NAME + 16];
    pid_t child;

    if (daemon->no_network || active_share_count(daemon) == 0) {
        stop_publisher(daemon);
        return 1;
    }
    if (daemon->publisher_pid > 0) {
        pid_t result = waitpid(daemon->publisher_pid, NULL, WNOHANG);

        if (result == 0)
            return 1;
        daemon->publisher_pid = 0;
    }
    if (!ensure_avahi(daemon, error, error_size))
        return 0;
    if (daemon->test_mode)
        return 1;
    program = first_command(daemon, publisher_paths);
    if (!program) {
        daemon_error(error, error_size,
                     "avahi-publish-service is missing; install Avahi utilities");
        return 0;
    }
    (void)snprintf(service_name, sizeof(service_name), "%s SimpleServe",
                   daemon->config.server_name);
    (void)snprintf(port, sizeof(port), "%u", daemon->config.port);
    (void)snprintf(version, sizeof(version), "version=%d",
                   SS_PROTOCOL_VERSION);
    (void)snprintf(server, sizeof(server), "server=%s",
                   daemon->config.server_name);
    child = fork();
    if (child == 0) {
        char *arguments[] = {
            (char *)"avahi-publish-service", (char *)"--no-fail",
            service_name, (char *)SS_SERVICE_TYPE, port, version, server, NULL
        };
        int null_fd = open("/dev/null", O_RDWR);

        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        execve(program, arguments, environment);
        _exit(127);
    }
    if (child < 0) {
        daemon_error(error, error_size, "cannot start mDNS publisher: %s",
                     strerror(errno));
        return 0;
    }
    daemon->publisher_pid = child;
    for (int attempt = 0; attempt < 10; attempt++) {
        int status = 0;
        pid_t result = waitpid(child, &status, WNOHANG);

        if (result == child) {
            daemon->publisher_pid = 0;
            if (WIFEXITED(status))
                daemon_error(error, error_size,
                             "mDNS publisher exited immediately with status %d",
                             WEXITSTATUS(status));
            else
                daemon_error(error, error_size,
                             "mDNS publisher terminated immediately");
            return 0;
        }
        if (result < 0 && errno != EINTR) {
            daemon->publisher_pid = 0;
            daemon_error(error, error_size,
                         "cannot monitor mDNS publisher: %s", strerror(errno));
            return 0;
        }
        if (result == 0)
            usleep(10000);
    }
    return 1;
}

static int open_control_socket(SSDaemon *daemon, char *error,
                               size_t error_size)
{
    struct sockaddr_un address;
    struct stat status;
    char parent[PATH_MAX];
    char *slash;
    int descriptor;

    if (strlen(daemon->socket_path) >= sizeof(address.sun_path) ||
        !ss_copy_string(parent, sizeof(parent), daemon->socket_path)) {
        daemon_error(error, error_size, "control socket path is too long");
        return 0;
    }
    slash = strrchr(parent, '/');
    if (!slash)
        return 0;
    if (slash == parent)
        slash[1] = '\0';
    else
        *slash = '\0';
    if (!ss_mkdir_parents(parent, 0755, 0, 0, error, error_size))
        return 0;
    if (lstat(daemon->socket_path, &status) == 0) {
        if (!S_ISSOCK(status.st_mode) ||
            (!daemon->test_mode && status.st_uid != 0)) {
            daemon_error(error, error_size,
                         "refusing to replace non-SimpleServe socket path %s",
                         daemon->socket_path);
            return 0;
        }
        if (unlink(daemon->socket_path) != 0) {
            daemon_error(error, error_size, "cannot replace control socket: %s",
                         strerror(errno));
            return 0;
        }
    } else if (errno != ENOENT) {
        daemon_error(error, error_size, "cannot inspect control socket: %s",
                     strerror(errno));
        return 0;
    }
    descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        daemon_error(error, error_size, "cannot create control socket: %s",
                     strerror(errno));
        return 0;
    }
    if (!set_close_on_exec(descriptor)) {
        daemon_error(error, error_size, "cannot secure control socket: %s",
                     strerror(errno));
        close(descriptor);
        return 0;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    ss_copy_string(address.sun_path, sizeof(address.sun_path), daemon->socket_path);
    if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(daemon->socket_path, 0666) != 0 || listen(descriptor, 16) != 0) {
        daemon_error(error, error_size, "cannot activate control socket: %s",
                     strerror(errno));
        close(descriptor);
        unlink(daemon->socket_path);
        return 0;
    }
    daemon->control_fd = descriptor;
    return 1;
}

static int open_manifest_socket(SSDaemon *daemon, char *error,
                                size_t error_size)
{
    struct sockaddr_in address;
    int descriptor;
    int enabled = 1;

    if (daemon->no_network) {
        daemon->manifest_fd = -1;
        return 1;
    }
    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        daemon_error(error, error_size, "cannot create manifest socket: %s",
                     strerror(errno));
        return 0;
    }
    if (!set_close_on_exec(descriptor)) {
        daemon_error(error, error_size, "cannot secure manifest socket: %s",
                     strerror(errno));
        close(descriptor);
        return 0;
    }
    (void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)daemon->config.port);
    if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(descriptor, 16) != 0) {
        daemon_error(error, error_size, "cannot listen on manifest port %u: %s",
                     daemon->config.port, strerror(errno));
        close(descriptor);
        return 0;
    }
    daemon->manifest_fd = descriptor;
    return 1;
}

static int socket_send_all(int descriptor, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t sent = send(descriptor, data, length, 0);

        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (sent == 0)
            return 0;
        data += (size_t)sent;
        length -= (size_t)sent;
    }
    return 1;
}

static void serve_manifest(SSDaemon *daemon, int descriptor)
{
    char request[4096];
    size_t used = 0;
    struct timeval timeout = {2, 0};
    SSBuffer body;
    SSBuffer response;
    char error[512];

    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));
    while (used + 1 < sizeof(request)) {
        ssize_t received = recv(descriptor, request + used,
                                sizeof(request) - 1 - used, 0);

        if (received < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (received == 0)
            break;
        used += (size_t)received;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") || strstr(request, "\n\n"))
            break;
    }
    request[used] = '\0';
    ss_buffer_init(&body);
    ss_buffer_init(&response);
    if ((strncmp(request, "GET /v1/manifest HTTP/1.0", 25) == 0 ||
         strncmp(request, "GET /v1/manifest HTTP/1.1", 25) == 0) &&
        (request[25] == '\r' || request[25] == '\n') &&
        ss_render_manifest(&daemon->config, &body, error, sizeof(error))) {
        (void)ss_buffer_appendf(&response,
                               "HTTP/1.0 200 OK\r\n"
                               "Content-Type: text/plain; charset=utf-8\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n\r\n",
                               body.length);
        (void)ss_buffer_append_n(&response, body.data, body.length);
    } else {
        const char not_found[] = "Not found\n";

        (void)ss_buffer_appendf(&response,
                               "HTTP/1.0 404 Not Found\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n\r\n%s",
                               sizeof(not_found) - 1, not_found);
    }
    if (response.data)
        (void)socket_send_all(descriptor, response.data, response.length);
    ss_buffer_free(&body);
    ss_buffer_free(&response);
}

static int connect_ipv4_timeout(const char *address, unsigned int port,
                                int timeout_ms, char *error,
                                size_t error_size)
{
    struct sockaddr_in endpoint;
    struct pollfd poll_descriptor;
    int descriptor;
    int flags;
    int result;
    int socket_error = 0;
    socklen_t socket_error_size = sizeof(socket_error);

    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, address, &endpoint.sin_addr) != 1) {
        daemon_error(error, error_size, "invalid discovered IPv4 address");
        return -1;
    }
    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        daemon_error(error, error_size, "cannot create manifest client: %s",
                     strerror(errno));
        return -1;
    }
    flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(descriptor);
        return -1;
    }
    result = connect(descriptor, (struct sockaddr *)&endpoint, sizeof(endpoint));
    if (result != 0 && errno != EINPROGRESS) {
        daemon_error(error, error_size, "cannot connect to %s:%u: %s", address,
                     port, strerror(errno));
        close(descriptor);
        return -1;
    }
    if (result != 0) {
        poll_descriptor.fd = descriptor;
        poll_descriptor.events = POLLOUT;
        poll_descriptor.revents = 0;
        if (poll(&poll_descriptor, 1, timeout_ms) <= 0 ||
            getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_size) != 0 || socket_error != 0) {
            daemon_error(error, error_size, "cannot connect to %s:%u", address,
                         port);
            close(descriptor);
            return -1;
        }
    }
    (void)fcntl(descriptor, F_SETFL, flags);
    return descriptor;
}

static int fetch_manifest(const char *address, unsigned int port,
                          SSRemoteServer *server, char *error,
                          size_t error_size)
{
    char request[256];
    SSBuffer response;
    int descriptor;
    long long deadline;
    char *body;
    int ok = 0;

    descriptor = connect_ipv4_timeout(address, port, 2500, error, error_size);
    if (descriptor < 0)
        return 0;
    (void)snprintf(request, sizeof(request),
                   "GET /v1/manifest HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                   address);
    if (!socket_send_all(descriptor, request, strlen(request))) {
        daemon_error(error, error_size, "cannot request manifest from %s", address);
        close(descriptor);
        return 0;
    }
    ss_buffer_init(&response);
    deadline = monotonic_ms() + 3000;
    for (;;) {
        struct pollfd poll_descriptor = {descriptor, POLLIN | POLLHUP, 0};
        long long remaining = deadline - monotonic_ms();
        char chunk[4096];
        ssize_t received;

        if (remaining <= 0) {
            daemon_error(error, error_size, "manifest request to %s timed out",
                         address);
            goto done;
        }
        if (poll(&poll_descriptor, 1, remaining > 500 ? 500 : (int)remaining) < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        received = recv(descriptor, chunk, sizeof(chunk), 0);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            goto done;
        }
        if (received == 0)
            break;
        if (response.length + (size_t)received > SS_MAX_FRAME ||
            !ss_buffer_append_n(&response, chunk, (size_t)received)) {
            daemon_error(error, error_size, "manifest response is too large");
            goto done;
        }
    }
    if (!response.data ||
        (strncmp(response.data, "HTTP/1.0 200 ", 13) != 0 &&
         strncmp(response.data, "HTTP/1.1 200 ", 13) != 0) ||
        (!(body = strstr(response.data, "\r\n\r\n")))) {
        daemon_error(error, error_size, "invalid manifest HTTP response from %s",
                     address);
        goto done;
    }
    body += 4;
    ok = ss_parse_manifest(body, address, port, server, error, error_size);

done:
    close(descriptor);
    ss_buffer_free(&response);
    return ok;
}

static int discover_servers(SSDaemon *daemon, char *error, size_t error_size)
{
    const char *test_manifest = getenv("SIMPLESERVE_TEST_MANIFEST");

    daemon->remote_count = 0;
    if (test_manifest && *test_manifest) {
        char *contents = NULL;
        size_t length = 0;
        const char *address = getenv("SIMPLESERVE_TEST_REMOTE_ADDRESS");

        if (!address)
            address = "127.0.0.2";
        if (!ss_read_file(test_manifest, SS_DAEMON_CONFIG_MAX, &contents, &length,
                          error, error_size))
            return 0;
        (void)length;
        if (!ss_parse_manifest(contents, address, daemon->config.port,
                               &daemon->remotes[0], error, error_size)) {
            free(contents);
            return 0;
        }
        free(contents);
        daemon->remote_count = 1;
        daemon->last_remote_refresh = time(NULL);
        return 1;
    }
    if (daemon->no_network) {
        daemon_error(error, error_size, "network discovery is disabled");
        return 0;
    }
    if (!ensure_avahi(daemon, error, error_size))
        return 0;
    {
        static const char *const browse_paths[] = {
            "/usr/local/bin/avahi-browse", "/usr/bin/avahi-browse",
            "/bin/avahi-browse", NULL
        };
        const char *program = first_command(daemon, browse_paths);
        const char *arguments[] = {
            "-r", "-t", "-p", "-k", SS_SERVICE_TYPE, NULL
        };
        SSCommand command;
        char output[256 * 1024];
        char *line;
        char *save = NULL;

        if (!program) {
            daemon_error(error, error_size,
                         "avahi-browse is missing; install Avahi utilities");
            return 0;
        }
        if (!command_from(&command, program, arguments) ||
            !run_command_capture(daemon, &command, 8000, output,
                                 sizeof(output), error, error_size))
            return 0;
        for (line = strtok_r(output, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char hostname[256];
            char address[64];
            char advertised_name[SS_MAX_NAME + 1];
            unsigned int port;
            SSRemoteServer remote;
            int duplicate = 0;
            int fetched = 0;
            char fetch_error[512];

            if (!ss_parse_avahi_resolved(line, hostname, sizeof(hostname),
                                         address, sizeof(address), &port,
                                         advertised_name,
                                         sizeof(advertised_name)))
                continue;
            for (size_t index = 0; index < daemon->remote_count; index++) {
                if (strcmp(daemon->remotes[index].name, advertised_name) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate || daemon->remote_count >= SS_MAX_SERVERS)
                continue;
            if (strcmp(advertised_name, daemon->config.server_name) == 0) {
                SSBuffer local_manifest;

                ss_buffer_init(&local_manifest);
                if (ss_render_manifest(&daemon->config, &local_manifest,
                                       fetch_error, sizeof(fetch_error)) &&
                    ss_parse_manifest(local_manifest.data, address, port,
                                      &remote, fetch_error,
                                      sizeof(fetch_error)))
                    fetched = 1;
                ss_buffer_free(&local_manifest);
            } else if (fetch_manifest(address, port, &remote, fetch_error,
                                      sizeof(fetch_error))) {
                fetched = 1;
            }
            if (!fetched ||
                strcmp(remote.name, advertised_name) != 0)
                continue;
            ss_copy_string(remote.hostname, sizeof(remote.hostname), hostname);
            daemon->remotes[daemon->remote_count++] = remote;
        }
    }
    daemon->last_remote_refresh = time(NULL);
    return 1;
}

static int peer_credentials(int descriptor, uid_t *uid, gid_t *gid,
                            char *error, size_t error_size)
{
#ifdef __FreeBSD__
    if (getpeereid(descriptor, uid, gid) != 0) {
        daemon_error(error, error_size, "cannot identify control client: %s",
                     strerror(errno));
        return 0;
    }
    return 1;
#elif defined(__linux__)
    struct ucred credentials;
    socklen_t size = sizeof(credentials);

    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
        size != sizeof(credentials)) {
        daemon_error(error, error_size, "cannot identify control client: %s",
                     strerror(errno));
        return 0;
    }
    *uid = credentials.uid;
    *gid = credentials.gid;
    return 1;
#else
    (void)descriptor;
    (void)uid;
    (void)gid;
    daemon_error(error, error_size, "peer credentials are unsupported");
    return 0;
#endif
}

static SSRemoteServer *find_remote_server(SSDaemon *daemon, const char *name)
{
    for (size_t index = 0; index < daemon->remote_count; index++) {
        if (strcmp(daemon->remotes[index].name, name) == 0)
            return &daemon->remotes[index];
    }
    return NULL;
}

static SSRemoteShare *find_remote_share(SSRemoteServer *server,
                                        const char *name)
{
    if (!server)
        return NULL;
    for (size_t index = 0; index < server->share_count; index++) {
        if (strcmp(server->shares[index].name, name) == 0)
            return &server->shares[index];
    }
    return NULL;
}

static SSClientMount *find_client_mount(SSDaemon *daemon, uid_t uid,
                                        const char *server, const char *share)
{
    for (size_t index = 0; index < daemon->mounts.mount_count; index++) {
        SSClientMount *mount = &daemon->mounts.mounts[index];

        if (mount->uid == uid && strcmp(mount->server, server) == 0 &&
            strcmp(mount->share, share) == 0)
            return mount;
    }
    return NULL;
}

static int owned_directory(const char *path, uid_t uid, char *error,
                           size_t error_size)
{
    struct stat status;

    if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != uid) {
        daemon_error(error, error_size,
                     "%s must be a real directory owned by uid %llu", path,
                     (unsigned long long)uid);
        return 0;
    }
    return 1;
}

static int prepare_mount_target(uid_t uid, gid_t gid, const char *server,
                                const char *share, char *target,
                                size_t target_size, char *error,
                                size_t error_size)
{
    struct passwd account;
    struct passwd *result = NULL;
    char account_buffer[16384];
    char home[PATH_MAX];
    char base[PATH_MAX];
    char server_path[PATH_MAX];
    struct stat status;
    SSMountInfo mounted;
    char ignored[256];
    const char *test_home = getenv("SIMPLESERVE_TEST_HOME");

    if (!ss_valid_name(server) || !ss_valid_name(share) ||
        getpwuid_r(uid, &account, account_buffer, sizeof(account_buffer),
                   &result) != 0 || !result ||
        !realpath(test_home && *test_home ? test_home : result->pw_dir, home)) {
        daemon_error(error, error_size, "cannot resolve the requesting user's home");
        return 0;
    }
    if (snprintf(base, sizeof(base), "%s/SimpleServe", home) >=
            (int)sizeof(base) ||
        snprintf(server_path, sizeof(server_path), "%s/%s", base, server) >=
            (int)sizeof(server_path) ||
        snprintf(target, target_size, "%s/%s", server_path, share) >=
            (int)target_size) {
        daemon_error(error, error_size, "mount target path is too long");
        return 0;
    }
    if (!ss_mkdir_parents(target, 0755, uid, gid, error, error_size) ||
        !owned_directory(base, uid, error, error_size) ||
        !owned_directory(server_path, uid, error, error_size))
        return 0;
    if (!ss_mount_info_exact(target, &mounted, ignored, sizeof(ignored)) ||
        strncmp(mounted.fstype, "nfs", 3) != 0) {
        if (!owned_directory(target, uid, error, error_size))
            return 0;
    }
    if (lstat(target, &status) != 0 || !S_ISDIR(status.st_mode))
        return 0;
    return 1;
}

static int directory_empty(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (!directory)
        return 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            closedir(directory);
            return 0;
        }
    }
    closedir(directory);
    return 1;
}

static int ensure_nfs_client(SSDaemon *daemon, char *error, size_t error_size)
{
    SSCommand command;

    if (daemon->platform == SS_PLATFORM_LINUX)
        return 1;
    if (daemon->platform != SS_PLATFORM_FREEBSD) {
        daemon_error(error, error_size, "unsupported NFS client platform");
        return 0;
    }
    {
        const char *sysrc_args[] = {
            "-q", "nfs_client_enable=YES", "rpc_statd_enable=YES",
            "rpc_lockd_enable=YES", NULL
        };
        const char *client_args[] = {"nfsclient", "onestart", NULL};
        const char *lock_args[] = {"lockd", "onestart", NULL};

        if (!command_from(&command, "/usr/sbin/sysrc", sysrc_args) ||
            !run_command(daemon, &command, 10000, error, error_size))
            return 0;
        if (!command_from(&command, "/usr/sbin/service", client_args))
            return 0;
        (void)run_command(daemon, &command, 10000, error, error_size);
        if (!command_from(&command, "/usr/sbin/service", lock_args))
            return 0;
        (void)run_command(daemon, &command, 10000, error, error_size);
    }
    return 1;
}

static int mounted_nfs_at(const SSDaemon *daemon, const char *target,
                          SSMountInfo *mount)
{
    char ignored[256];

    if (daemon->test_mode)
        return 0;
    if (!ss_mount_info_exact(target, mount, ignored, sizeof(ignored)))
        return 0;
    return strncmp(mount->fstype, "nfs", 3) == 0;
}

static int save_remembered_mounts(SSDaemon *daemon, char *error,
                                  size_t error_size)
{
    return ss_save_mount_config(daemon->state_path, &daemon->mounts,
                                error, error_size);
}

static void fill_mount_record(SSClientMount *record, uid_t uid, gid_t gid,
                              const SSRemoteServer *server,
                              const SSRemoteShare *share, const char *target,
                              int remember)
{
    record->uid = uid;
    record->gid = gid;
    ss_copy_string(record->server, sizeof(record->server), server->name);
    ss_copy_string(record->share, sizeof(record->share), share->name);
    ss_copy_string(record->address, sizeof(record->address), server->address);
    ss_copy_string(record->export_path, sizeof(record->export_path),
                   share->export_path);
    ss_copy_string(record->filesystem_id, sizeof(record->filesystem_id),
                   share->filesystem_id);
    ss_copy_string(record->target, sizeof(record->target), target);
    record->access = share->access;
    record->remembered = remember || record->remembered;
    record->mounted = 1;
    record->available = 1;
    record->misses = 0;
}

static int rollback_mount(SSDaemon *daemon, const char *target,
                          char *error, size_t error_size)
{
    SSCommand command;
    SSMountInfo mounted;

    if (!ss_build_unmount_command(daemon->platform, target, 0, &command,
                                  error, error_size) ||
        !run_command(daemon, &command, 15000, error, error_size))
        return 0;
    if (!daemon->test_mode && mounted_nfs_at(daemon, target, &mounted)) {
        daemon_error(error, error_size, "%s is still mounted after rollback",
                     target);
        return 0;
    }
    return 1;
}

static int perform_mount(SSDaemon *daemon, uid_t uid, gid_t gid,
                         SSRemoteServer *server, SSRemoteShare *share,
                         int remember, SSBuffer *message, char *error,
                         size_t error_size)
{
    SSClientMount *record;
    SSClientMount previous_record;
    char target[PATH_MAX];
    char expected_source[PATH_MAX];
    char original_error[512];
    char rollback_error[512];
    SSMountInfo existing;
    SSCommand command;
    int had_record;

    if (!server || !share ||
        !prepare_mount_target(uid, gid, server->name, share->name, target,
                              sizeof(target), error, error_size))
        return 0;
    record = find_client_mount(daemon, uid, server->name, share->name);
    if (!record && daemon->mounts.mount_count >= SS_MAX_MOUNTS) {
        daemon_error(error, error_size, "too many managed mounts");
        return 0;
    }
    if (snprintf(expected_source, sizeof(expected_source), "%s:%s",
                 server->address, share->export_path) >=
        (int)sizeof(expected_source)) {
        daemon_error(error, error_size, "NFS source path is too long");
        return 0;
    }
    had_record = record != NULL;
    if (record)
        previous_record = *record;
    if (mounted_nfs_at(daemon, target, &existing)) {
        if (strcmp(existing.source, expected_source) != 0) {
            daemon_error(error, error_size,
                         "refusing to adopt unexpected NFS mount %s at %s",
                         existing.source, target);
            return 0;
        }
        if (!record) {
            record = &daemon->mounts.mounts[daemon->mounts.mount_count++];
            memset(record, 0, sizeof(*record));
        }
        fill_mount_record(record, uid, gid, server, share, target, remember);
        if (record->remembered &&
            !save_remembered_mounts(daemon, error, error_size)) {
            if (had_record)
                *record = previous_record;
            else
                daemon->mounts.mount_count--;
            return 0;
        }
        return ss_buffer_appendf(message, "%s:%s is already mounted at %s\n",
                                 server->name, share->name, target);
    }
    if (!daemon->test_mode && !directory_empty(target)) {
        daemon_error(error, error_size,
                     "refusing to hide files in non-empty mountpoint %s", target);
        return 0;
    }
    if (!ensure_nfs_client(daemon, error, error_size) ||
        !ss_build_mount_command(daemon->platform, server->address,
                                share->export_path, target, share->access,
                                &command, error, error_size))
        return 0;
    if (!run_command(daemon, &command, 30000, error, error_size)) {
        ss_copy_string(original_error, sizeof(original_error), error);
        if (!daemon->test_mode && mounted_nfs_at(daemon, target, &existing) &&
            !rollback_mount(daemon, target, rollback_error,
                            sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: mount rollback failed: %s\n",
                    rollback_error);
        ss_copy_string(error, error_size, original_error);
        return 0;
    }
    if (!daemon->test_mode && !mounted_nfs_at(daemon, target, &existing)) {
        char ignored[256];

        daemon_error(error, error_size,
                     "mount command returned success but %s is not an NFS mount",
                     target);
        ss_copy_string(original_error, sizeof(original_error), error);
        if (ss_mount_info_exact(target, &existing, ignored, sizeof(ignored)) &&
            !rollback_mount(daemon, target, rollback_error,
                            sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: mount rollback failed: %s\n",
                    rollback_error);
        ss_copy_string(error, error_size, original_error);
        return 0;
    }
    if (!record) {
        record = &daemon->mounts.mounts[daemon->mounts.mount_count++];
        memset(record, 0, sizeof(*record));
    }
    fill_mount_record(record, uid, gid, server, share, target, remember);
    if (record->remembered &&
        !save_remembered_mounts(daemon, error, error_size)) {
        ss_copy_string(original_error, sizeof(original_error), error);
        if (rollback_mount(daemon, target, rollback_error,
                           sizeof(rollback_error))) {
            if (had_record)
                *record = previous_record;
            else
                daemon->mounts.mount_count--;
        } else {
            record->remembered = 0;
            fprintf(stderr, "simpleserved: mount rollback failed: %s\n",
                    rollback_error);
        }
        ss_copy_string(error, error_size, original_error);
        return 0;
    }
    return ss_buffer_appendf(message, "Mounted %s:%s at %s%s\n", server->name,
                             share->name, target,
                             record->remembered ? " (remembered)" : "");
}

static void remove_mount_record(SSDaemon *daemon, size_t index)
{
    if (index >= daemon->mounts.mount_count)
        return;
    if (index + 1 < daemon->mounts.mount_count) {
        memmove(&daemon->mounts.mounts[index],
                &daemon->mounts.mounts[index + 1],
                (daemon->mounts.mount_count - index - 1) *
                    sizeof(daemon->mounts.mounts[0]));
    }
    daemon->mounts.mount_count--;
}

static int perform_unmount(SSDaemon *daemon, uid_t uid, gid_t gid,
                           const char *server, const char *share,
                           SSBuffer *message, char *error, size_t error_size)
{
    char target[PATH_MAX];
    SSMountInfo mounted;
    SSCommand command;
    size_t record_index = SIZE_MAX;

    if (!prepare_mount_target(uid, gid, server, share, target, sizeof(target),
                              error, error_size))
        return 0;
    for (size_t index = 0; index < daemon->mounts.mount_count; index++) {
        SSClientMount *record = &daemon->mounts.mounts[index];

        if (record->uid == uid && strcmp(record->server, server) == 0 &&
            strcmp(record->share, share) == 0) {
            record_index = index;
            break;
        }
    }
    if (!daemon->test_mode && !mounted_nfs_at(daemon, target, &mounted)) {
        if (record_index != SIZE_MAX) {
            remove_mount_record(daemon, record_index);
            if (!save_remembered_mounts(daemon, error, error_size))
                return 0;
        }
        return ss_buffer_appendf(message, "%s:%s is not mounted\n", server, share);
    }
    if (record_index == SIZE_MAX) {
        daemon_error(error, error_size,
                     "%s is an NFS mount not managed by SimpleServe", target);
        return 0;
    }
    if (!daemon->test_mode &&
        daemon->mounts.mounts[record_index].address[0] &&
        daemon->mounts.mounts[record_index].export_path[0]) {
        char expected_source[PATH_MAX];

        if (snprintf(expected_source, sizeof(expected_source), "%s:%s",
                     daemon->mounts.mounts[record_index].address,
                     daemon->mounts.mounts[record_index].export_path) >=
                (int)sizeof(expected_source) ||
            strcmp(mounted.source, expected_source) != 0) {
            daemon_error(error, error_size,
                         "refusing to unmount unexpected NFS source at %s",
                         target);
            return 0;
        }
    }
    if (!ss_build_unmount_command(daemon->platform, target, 0, &command,
                                  error, error_size) ||
        !run_command(daemon, &command, 15000, error, error_size))
        return 0;
    if (!daemon->test_mode && mounted_nfs_at(daemon, target, &mounted)) {
        daemon_error(error, error_size, "%s is still mounted", target);
        return 0;
    }
    if (record_index != SIZE_MAX)
        remove_mount_record(daemon, record_index);
    if (!save_remembered_mounts(daemon, error, error_size))
        return 0;
    (void)rmdir(target);
    {
        char parent[PATH_MAX];
        char *slash;

        if (ss_copy_string(parent, sizeof(parent), target) &&
            (slash = strrchr(parent, '/')) != NULL) {
            *slash = '\0';
            (void)rmdir(parent);
        }
    }
    return ss_buffer_appendf(message, "Unmounted %s:%s from %s\n", server,
                             share, target);
}

static SSLocalShare *find_local_share(SSDaemon *daemon, const char *name,
                                      size_t *index_out)
{
    for (size_t index = 0; index < daemon->config.share_count; index++) {
        if (strcmp(daemon->config.shares[index].name, name) == 0) {
            if (index_out)
                *index_out = index;
            return &daemon->config.shares[index];
        }
    }
    return NULL;
}

static int share_local(SSDaemon *daemon, uid_t uid, gid_t gid,
                       const char *name, SSAccess access_mode,
                       const char *path, SSBuffer *message, char *error,
                       size_t error_size)
{
    SSMountInfo mount;
    SSServerConfig *old_config;
    SSLocalShare *share;
    char original_error[512];
    char rollback_error[512];
    int changed;

    if (!ss_valid_name(name) || !ss_valid_absolute_path(path) ||
        !ss_mount_info_exact(path, &mount, error, error_size) ||
        !ss_user_can_access(uid, gid, mount.target, access_mode,
                            error, error_size))
        return 0;
    if (strcmp(mount.fstype, "autofs") == 0 ||
        strncmp(mount.fstype, "nfs", 3) == 0) {
        daemon_error(error, error_size,
                     "%s filesystems cannot be registered as local shares",
                     mount.fstype);
        return 0;
    }
    if (access_mode == SS_ACCESS_READ_WRITE && mount.read_only) {
        daemon_error(error, error_size, "%s is mounted read-only", mount.target);
        return 0;
    }
    for (size_t index = 0; index < daemon->config.share_count; index++) {
        SSLocalShare *entry = &daemon->config.shares[index];

        if (strcmp(entry->filesystem_id, mount.identity) == 0 &&
            strcmp(entry->name, name) != 0) {
            daemon_error(error, error_size,
                         "filesystem is already shared as %s", entry->name);
            return 0;
        }
    }
    old_config = malloc(sizeof(*old_config));
    if (!old_config) {
        daemon_error(error, error_size, "out of memory");
        return 0;
    }
    *old_config = daemon->config;
    share = find_local_share(daemon, name, NULL);
    if (share && share->owner_uid != uid && uid != 0) {
        daemon_error(error, error_size, "share %s belongs to another user", name);
        free(old_config);
        return 0;
    }
    if (!share) {
        if (daemon->config.share_count >= SS_MAX_SHARES) {
            daemon_error(error, error_size, "too many local shares");
            free(old_config);
            return 0;
        }
        share = &daemon->config.shares[daemon->config.share_count++];
    }
    memset(share, 0, sizeof(*share));
    ss_copy_string(share->name, sizeof(share->name), name);
    ss_copy_string(share->configured_path, sizeof(share->configured_path),
                   mount.target);
    ss_copy_string(share->current_path, sizeof(share->current_path), mount.target);
    ss_copy_string(share->filesystem_id, sizeof(share->filesystem_id),
                   mount.identity);
    ss_copy_string(share->source, sizeof(share->source), mount.source);
    ss_copy_string(share->fstype, sizeof(share->fstype), mount.fstype);
    share->access = access_mode;
    share->owner_uid = uid;
    share->owner_gid = gid;
    share->total_bytes = mount.total_bytes;
    share->free_bytes = mount.free_bytes;
    share->active = 1;
    if (!ss_save_server_config(daemon->config_path, &daemon->config,
                               error, error_size) ||
        !sync_exports(daemon, error, error_size) ||
        !start_publisher(daemon, error, error_size)) {
        ss_copy_string(original_error, sizeof(original_error), error);
        daemon->config = *old_config;
        if (!ss_save_server_config(daemon->config_path, &daemon->config,
                                   rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: configuration rollback failed: %s\n",
                    rollback_error);
        (void)refresh_local_shares(daemon, &changed);
        if (!sync_exports(daemon, rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: share rollback failed: %s\n",
                    rollback_error);
        ss_copy_string(error, error_size, original_error);
        free(old_config);
        return 0;
    }
    free(old_config);
    return ss_buffer_appendf(message,
                             "Shared %s as %s (%s, UUID %s)\n",
                             mount.target, name, ss_access_name(access_mode),
                             mount.identity);
}

static int unshare_local(SSDaemon *daemon, uid_t uid, const char *name,
                         SSBuffer *message, char *error, size_t error_size)
{
    size_t index;
    SSLocalShare *share = find_local_share(daemon, name, &index);
    SSServerConfig *old_config;
    char original_error[512];
    char rollback_error[512];
    int changed;

    if (!share) {
        daemon_error(error, error_size, "unknown local share: %s", name);
        return 0;
    }
    if (share->owner_uid != uid && uid != 0) {
        daemon_error(error, error_size, "share %s belongs to another user", name);
        return 0;
    }
    old_config = malloc(sizeof(*old_config));
    if (!old_config) {
        daemon_error(error, error_size, "out of memory");
        return 0;
    }
    *old_config = daemon->config;
    if (index + 1 < daemon->config.share_count) {
        memmove(&daemon->config.shares[index], &daemon->config.shares[index + 1],
                (daemon->config.share_count - index - 1) *
                    sizeof(daemon->config.shares[0]));
    }
    daemon->config.share_count--;
    if (!ss_save_server_config(daemon->config_path, &daemon->config,
                               error, error_size) ||
        !sync_exports(daemon, error, error_size) ||
        !start_publisher(daemon, error, error_size)) {
        ss_copy_string(original_error, sizeof(original_error), error);
        daemon->config = *old_config;
        if (!ss_save_server_config(daemon->config_path, &daemon->config,
                                   rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: configuration rollback failed: %s\n",
                    rollback_error);
        (void)refresh_local_shares(daemon, &changed);
        if (!sync_exports(daemon, rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: unshare rollback failed: %s\n",
                    rollback_error);
        ss_copy_string(error, error_size, original_error);
        free(old_config);
        return 0;
    }
    free(old_config);
    return ss_buffer_appendf(message, "Stopped sharing %s\n", name);
}

static int format_discovery(const SSDaemon *daemon, SSBuffer *message)
{
    if (!ss_buffer_append(message, "AVAILABLE SHARES\n\n"))
        return 0;
    if (daemon->remote_count == 0)
        return ss_buffer_append(message, "No SimpleServe shares found on this LAN.\n");
    for (size_t server_index = 0; server_index < daemon->remote_count;
         server_index++) {
        const SSRemoteServer *server = &daemon->remotes[server_index];

        if (!ss_buffer_appendf(message, "%s\n", server->name))
            return 0;
        if (server->share_count == 0) {
            if (!ss_buffer_append(message, "  (no active shares)\n"))
                return 0;
            continue;
        }
        for (size_t share_index = 0; share_index < server->share_count;
             share_index++) {
            const SSRemoteShare *share = &server->shares[share_index];
            char size[64];

            ss_human_size(share->total_bytes, size, sizeof(size));
            if (!ss_buffer_appendf(message, "  %-12s %-12s %s\n", share->name,
                                   share->access == SS_ACCESS_READ_WRITE ?
                                       "read-write" : "read-only",
                                   size))
                return 0;
        }
        if (!ss_buffer_append(message, "\n"))
            return 0;
    }
    return 1;
}

static int format_status(SSDaemon *daemon, uid_t uid, SSBuffer *message)
{
    if (!ss_buffer_appendf(message, "SIMPLESERVE STATUS\n\nServer: %s\n",
                           daemon->config.server_name))
        return 0;
    if (!ss_buffer_append(message, "\nLocal shares:\n"))
        return 0;
    if (daemon->config.share_count == 0) {
        if (!ss_buffer_append(message, "  (none)\n"))
            return 0;
    }
    for (size_t index = 0; index < daemon->config.share_count; index++) {
        SSLocalShare *share = &daemon->config.shares[index];

        if (!ss_buffer_appendf(message, "  %-12s %-12s %s%s\n", share->name,
                               ss_access_name(share->access),
                               share->active ? share->current_path :
                                               share->configured_path,
                               share->active ? "" : " (drive unavailable)"))
            return 0;
    }
    if (!ss_buffer_append(message, "\nManaged mounts:\n"))
        return 0;
    {
        size_t visible = 0;

        for (size_t index = 0; index < daemon->mounts.mount_count; index++) {
            SSClientMount *mount = &daemon->mounts.mounts[index];
            char target[PATH_MAX];
            char ignored[256];

            if (uid != 0 && mount->uid != uid)
                continue;
            visible++;
            if (mount->target[0])
                ss_copy_string(target, sizeof(target), mount->target);
            else if (!prepare_mount_target(mount->uid, mount->gid,
                                           mount->server, mount->share,
                                           target, sizeof(target), ignored,
                                           sizeof(ignored)))
                ss_copy_string(target, sizeof(target), "(target unavailable)");
            if (!ss_buffer_appendf(
                    message, "  %s:%s -> %s  %s%s%s\n", mount->server,
                    mount->share, target,
                    mount->mounted ? "mounted" : "not mounted",
                    mount->available ? "" : ", server unavailable",
                    mount->remembered ? ", remembered" : ""))
                return 0;
        }
        if (visible == 0 && !ss_buffer_append(message, "  (none)\n"))
            return 0;
    }
    return 1;
}

static int split_request(char *request, char *fields[], size_t maximum,
                         size_t *count)
{
    char *cursor = request;

    *count = 0;
    if (!request || !*request)
        return 0;
    for (;;) {
        char *tab;

        if (*count >= maximum)
            return 0;
        fields[(*count)++] = cursor;
        tab = strchr(cursor, '\t');
        if (!tab)
            break;
        *tab = '\0';
        cursor = tab + 1;
        if (!*cursor)
            return 0;
    }
    for (size_t index = 0; index < *count; index++) {
        if (!fields[index][0] || strchr(fields[index], '\n') ||
            strchr(fields[index], '\r'))
            return 0;
    }
    return 1;
}

static int process_request(SSDaemon *daemon, uid_t uid, gid_t gid,
                           char *request, SSBuffer *message, char *error,
                           size_t error_size)
{
    char *fields[8];
    size_t count;

    if (!split_request(request, fields, sizeof(fields) / sizeof(fields[0]),
                       &count)) {
        daemon_error(error, error_size, "malformed control request");
        return 0;
    }
    if (strcmp(fields[0], "SHARE") == 0 && count == 4) {
        SSAccess access_mode;

        if (!ss_access_parse(fields[2], &access_mode)) {
            daemon_error(error, error_size, "invalid share access mode");
            return 0;
        }
        return share_local(daemon, uid, gid, fields[1], access_mode, fields[3],
                           message, error, error_size);
    }
    if (strcmp(fields[0], "UNSHARE") == 0 && count == 2 &&
        ss_valid_name(fields[1]))
        return unshare_local(daemon, uid, fields[1], message, error, error_size);
    if (strcmp(fields[0], "DISCOVER") == 0 && count == 1) {
        if (!discover_servers(daemon, error, error_size))
            return 0;
        if (!format_discovery(daemon, message)) {
            daemon_error(error, error_size, "out of memory");
            return 0;
        }
        return 1;
    }
    if (strcmp(fields[0], "MOUNT") == 0 && count == 4 &&
        ss_valid_name(fields[1]) && ss_valid_name(fields[2]) &&
        (strcmp(fields[3], "once") == 0 ||
         strcmp(fields[3], "remember") == 0)) {
        SSRemoteServer *server;
        SSRemoteShare *share;

        if (!discover_servers(daemon, error, error_size))
            return 0;
        server = find_remote_server(daemon, fields[1]);
        share = find_remote_share(server, fields[2]);
        if (!server || !share) {
            daemon_error(error, error_size, "share %s:%s is not available",
                         fields[1], fields[2]);
            return 0;
        }
        return perform_mount(daemon, uid, gid, server, share,
                             strcmp(fields[3], "remember") == 0, message,
                             error, error_size);
    }
    if (strcmp(fields[0], "UNMOUNT") == 0 && count == 3 &&
        ss_valid_name(fields[1]) && ss_valid_name(fields[2]))
        return perform_unmount(daemon, uid, gid, fields[1], fields[2], message,
                               error, error_size);
    if (strcmp(fields[0], "STATUS") == 0 && count == 1) {
        int changed = 0;

        if (!refresh_local_shares(daemon, &changed)) {
            daemon_error(error, error_size, "cannot refresh local shares");
            return 0;
        }
        if (changed &&
            (!sync_exports(daemon, error, error_size) ||
             !start_publisher(daemon, error, error_size)))
            return 0;
        if (!format_status(daemon, uid, message)) {
            daemon_error(error, error_size, "out of memory");
            return 0;
        }
        return 1;
    }
    daemon_error(error, error_size, "unknown or invalid control command");
    return 0;
}

static void handle_control_client(SSDaemon *daemon, int descriptor)
{
    uid_t uid;
    gid_t gid;
    char *request = NULL;
    size_t request_length = 0;
    char error[1024] = "request failed";
    SSBuffer message;
    SSBuffer response;
    int ok = 0;

    ss_buffer_init(&message);
    ss_buffer_init(&response);
    if (!peer_credentials(descriptor, &uid, &gid, error, sizeof(error)) ||
        !ss_receive_frame(descriptor, &request, &request_length,
                          error, sizeof(error)))
        goto respond;
    if (strlen(request) != request_length) {
        daemon_error(error, sizeof(error),
                     "control request contains an embedded null byte");
        goto respond;
    }
    ok = process_request(daemon, uid, gid, request, &message, error,
                         sizeof(error));

respond:
    if (ok) {
        (void)ss_buffer_append(&response, "OK\n");
        if (message.data)
            (void)ss_buffer_append_n(&response, message.data, message.length);
    } else {
        (void)ss_buffer_append(&response, "ERR\n");
        (void)ss_buffer_append(&response, error[0] ? error : "request failed");
        (void)ss_buffer_append(&response, "\n");
    }
    if (response.data)
        (void)ss_send_frame(descriptor, response.data, response.length,
                            error, sizeof(error));
    free(request);
    ss_buffer_free(&message);
    ss_buffer_free(&response);
}

static void reconcile_mounts(SSDaemon *daemon)
{
    char error[512];

    if (daemon->mounts.mount_count == 0 ||
        !discover_servers(daemon, error, sizeof(error)))
        return;
    for (size_t index = 0; index < daemon->mounts.mount_count;) {
        SSClientMount *mount = &daemon->mounts.mounts[index];
        SSRemoteServer *server = find_remote_server(daemon, mount->server);
        SSRemoteShare *share = find_remote_share(server, mount->share);
        char target[PATH_MAX];
        SSMountInfo mounted;
        int is_mounted = 0;

        if (prepare_mount_target(mount->uid, mount->gid, mount->server,
                                 mount->share, target, sizeof(target), error,
                                 sizeof(error))) {
            ss_copy_string(mount->target, sizeof(mount->target), target);
            is_mounted = mounted_nfs_at(daemon, target, &mounted);
        }
        mount->mounted = is_mounted;
        if (share && strcmp(share->filesystem_id, mount->filesystem_id) == 0) {
            SSBuffer ignored;
            char expected_source[PATH_MAX];

            ss_copy_string(mount->address, sizeof(mount->address),
                           server->address);
            ss_copy_string(mount->export_path, sizeof(mount->export_path),
                           share->export_path);
            mount->access = share->access;
            if (is_mounted &&
                (snprintf(expected_source, sizeof(expected_source), "%s:%s",
                          server->address, share->export_path) >=
                     (int)sizeof(expected_source) ||
                 strcmp(mounted.source, expected_source) != 0)) {
                mount->available = 0;
                if (mount->misses < UINT_MAX)
                    mount->misses++;
                fprintf(stderr,
                        "simpleserved: unexpected NFS source at %s; leaving it untouched\n",
                        target);
                index++;
                continue;
            }

            mount->available = 1;
            mount->misses = 0;
            if (!is_mounted && mount->remembered) {
                ss_buffer_init(&ignored);
                if (!perform_mount(daemon, mount->uid, mount->gid, server, share,
                                   1, &ignored, error, sizeof(error)))
                    fprintf(stderr, "simpleserved: reconnect %s:%s failed: %s\n",
                            mount->server, mount->share, error);
                ss_buffer_free(&ignored);
            }
            index++;
            continue;
        }
        mount->available = 0;
        if (mount->misses < UINT_MAX)
            mount->misses++;
        if (is_mounted && mount->misses >= 3) {
            SSCommand command;

            if (ss_build_unmount_command(daemon->platform, target, 0, &command,
                                         error, sizeof(error)) &&
                run_command(daemon, &command, 5000, error, sizeof(error))) {
                mount->mounted = 0;
                if (!mount->remembered) {
                    remove_mount_record(daemon, index);
                    continue;
                }
            }
        }
        index++;
    }
    (void)save_remembered_mounts(daemon, error, sizeof(error));
    daemon->last_remote_refresh = time(NULL);
}

static void cleanup_daemon(SSDaemon *daemon)
{
    stop_publisher(daemon);
    if (daemon->control_fd >= 0)
        close(daemon->control_fd);
    if (daemon->manifest_fd >= 0)
        close(daemon->manifest_fd);
    if (daemon->socket_path[0])
        (void)unlink(daemon->socket_path);
}

static int initialize_daemon(SSDaemon *daemon, char *error, size_t error_size)
{
    const char *no_network;
    int changed;

    memset(daemon, 0, sizeof(*daemon));
    daemon->control_fd = -1;
    daemon->manifest_fd = -1;
    daemon->platform = ss_platform_detect();
    daemon->test_mode = getenv("SIMPLESERVE_TEST_MODE") != NULL;
    no_network = getenv("SIMPLESERVE_TEST_NO_NETWORK");
    daemon->no_network = no_network && strcmp(no_network, "0") != 0;
    if (daemon->platform == SS_PLATFORM_UNSUPPORTED) {
        daemon_error(error, error_size, "SimpleServe supports FreeBSD and Linux");
        return 0;
    }
    if (!daemon->test_mode && geteuid() != 0) {
        daemon_error(error, error_size, "simpleserved must run as root");
        return 0;
    }
    if (daemon->test_mode &&
        (!getenv("SIMPLESERVE_SOCKET") || !getenv("SIMPLESERVE_CONFIG") ||
         !getenv("SIMPLESERVE_STATE") || !getenv("SIMPLESERVE_EXPORTS"))) {
        daemon_error(error, error_size,
                     "test mode requires socket, config, state, and exports overrides");
        return 0;
    }
    if (!ss_copy_string(daemon->socket_path, sizeof(daemon->socket_path),
                        ss_default_socket_path(daemon->platform)) ||
        !ss_copy_string(daemon->config_path, sizeof(daemon->config_path),
                        ss_default_config_path()) ||
        !ss_copy_string(daemon->state_path, sizeof(daemon->state_path),
                        ss_default_state_path(daemon->platform)) ||
        !ss_copy_string(daemon->exports_path, sizeof(daemon->exports_path),
                        ss_default_exports_path(daemon->platform))) {
        daemon_error(error, error_size, "SimpleServe system path is too long");
        return 0;
    }
    if (!ss_load_server_config(daemon->config_path, &daemon->config,
                               error, error_size) ||
        !ss_load_mount_config(daemon->state_path, &daemon->mounts,
                              error, error_size) ||
        !refresh_local_shares(daemon, &changed) ||
        !sync_exports(daemon, error, error_size) ||
        !open_manifest_socket(daemon, error, error_size) ||
        !open_control_socket(daemon, error, error_size) ||
        !start_publisher(daemon, error, error_size))
        return 0;
    return 1;
}

static void daemon_loop(SSDaemon *daemon)
{
    while (!stop_requested) {
        struct pollfd descriptors[2];
        nfds_t count = 0;
        time_t now;

        descriptors[count].fd = daemon->control_fd;
        descriptors[count].events = POLLIN;
        descriptors[count].revents = 0;
        count++;
        if (daemon->manifest_fd >= 0) {
            descriptors[count].fd = daemon->manifest_fd;
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            count++;
        }
        if (poll(descriptors, count, 1000) < 0 && errno != EINTR) {
            fprintf(stderr, "simpleserved: poll failed: %s\n", strerror(errno));
            break;
        }
        if (descriptors[0].revents & POLLIN) {
            int client = accept(daemon->control_fd, NULL, NULL);

            if (client >= 0) {
                (void)set_close_on_exec(client);
                handle_control_client(daemon, client);
                close(client);
            }
        }
        if (count > 1 && descriptors[1].revents & POLLIN) {
            int client = accept(daemon->manifest_fd, NULL, NULL);

            if (client >= 0) {
                (void)set_close_on_exec(client);
                serve_manifest(daemon, client);
                close(client);
            }
        }
        now = time(NULL);
        if (now - daemon->last_local_refresh >= 2) {
            int changed = 0;
            char error[512];

            (void)refresh_local_shares(daemon, &changed);
            if (changed) {
                if (!sync_exports(daemon, error, sizeof(error)) ||
                    !start_publisher(daemon, error, sizeof(error))) {
                    fprintf(stderr, "simpleserved: share refresh failed: %s\n",
                            error);
                    stop_publisher(daemon);
                }
            } else if (active_share_count(daemon) > 0 &&
                       !start_publisher(daemon, error, sizeof(error))) {
                fprintf(stderr, "simpleserved: mDNS restart failed: %s\n", error);
            }
        }
        if (daemon->mounts.mount_count > 0 &&
            now - daemon->last_remote_refresh >= 15)
            reconcile_mounts(daemon);
    }
}

int main(int argc, char **argv)
{
    SSDaemon daemon;
    char error[1024];
    struct sigaction action;

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "-h") == 0)) {
        printf("Usage: simpleserved\n"
               "Privileged SimpleServe export, discovery, and mount daemon.\n");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "simpleserved: no command-line arguments are supported\n");
        return 2;
    }
    umask(022);
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    if (!initialize_daemon(&daemon, error, sizeof(error))) {
        fprintf(stderr, "simpleserved: %s\n", error);
        cleanup_daemon(&daemon);
        return 1;
    }
    fprintf(stderr, "simpleserved: ready on %s (%s)\n", daemon.socket_path,
            ss_platform_name(daemon.platform));
    daemon_loop(&daemon);
    cleanup_daemon(&daemon);
    return 0;
}
