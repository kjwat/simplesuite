#define _GNU_SOURCE

#include "simpleserve.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
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
#define SS_AVAHI_RETRY_MS 2000
#define SS_MANIFEST_RETRY_MS 5000
#define SS_TAILSCALE_REFRESH_SECONDS 30
#define SS_TAILSCALE_PEER_REFRESH_SECONDS 60
#define SS_TAILSCALE_IDENTITY_REFRESH_SECONDS 300
#define SS_ROUTE_PROBE_MS 750

typedef enum {
    SS_TAILSCALE_MISSING = 0,
    SS_TAILSCALE_DAEMON_STOPPED,
    SS_TAILSCALE_NEEDS_LOGIN,
    SS_TAILSCALE_INACTIVE,
    SS_TAILSCALE_ACTIVE
} SSTailscaleState;

typedef struct {
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char name[256];
    char type[128];
    char domain[256];
    char server_name[SS_MAX_NAME + 1];
    uint64_t generation;
    long long retry_at_ms;
    int resolve_requested;
    int resolving;
    int manifest_pending;
} SSDiscoveredService;

typedef struct {
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char name[256];
    char type[128];
    char domain[256];
    uint64_t generation;
    char hostname[256];
    char address[64];
    char advertised_name[SS_MAX_NAME + 1];
    unsigned int port;
} SSManifestJob;

typedef struct SSDaemon SSDaemon;

typedef struct {
    SSDaemon *daemon;
    AvahiIfIndex interface;
    AvahiProtocol protocol;
    char name[256];
    char type[128];
    char domain[256];
    uint64_t generation;
    int in_use;
} SSResolverContext;

struct SSDaemon {
    SSPlatform platform;
    char socket_path[PATH_MAX];
    char config_path[PATH_MAX];
    char state_path[PATH_MAX];
    char exports_path[PATH_MAX];
    char fstab_path[PATH_MAX];
    char smb_conf_path[PATH_MAX];
    char samba_path[PATH_MAX];
    SSServerConfig config;
    SSMountConfig mounts;
    SSRemoteServer remotes[SS_MAX_SERVERS];
    uint64_t remote_sources[SS_MAX_SERVERS];
    size_t remote_count;
    SSDiscoveredService services[SS_MAX_SERVERS];
    size_t service_count;
    SSResolverContext resolver_contexts[SS_MAX_SERVERS];
    SSManifestJob manifest_jobs[SS_MAX_SERVERS];
    size_t manifest_job_count;
    uint64_t next_service_generation;
    unsigned long remote_revision;
    unsigned long reconciled_remote_revision;
    pthread_mutex_t remote_mutex;
    pthread_cond_t manifest_condition;
    pthread_t avahi_thread;
    pthread_t manifest_thread;
    AvahiSimplePoll *avahi_poll;
    AvahiClient *avahi_client;
    AvahiServiceBrowser *avahi_browser;
    int remote_sync_initialized;
    int avahi_thread_started;
    int manifest_thread_started;
    int discovery_stopping;
    int avahi_client_restart_requested;
    int avahi_browser_restart_requested;
    int avahi_all_for_now;
    long long avahi_restart_at_ms;
    int control_fd;
    int manifest_fd;
    pid_t publisher_pid;
    int test_mode;
    int no_network;
    int tailscale_installed;
    int tailscale_active;
    SSTailscaleState tailscale_state;
    char tailscale_address[64];
    char tailscale_name[256];
    time_t last_local_refresh;
    time_t last_mount_reconcile;
    time_t last_tailscale_refresh;
    time_t last_tailscale_peer_refresh;
    time_t last_tailscale_identity_refresh;
};

static volatile sig_atomic_t stop_requested;

static int save_remembered_mounts(SSDaemon *daemon, char *error,
                                  size_t error_size);

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

        if (failure && *failure) {
            for (size_t index = 0; index < command->argc; index++) {
                if (strstr(command->argv[index], failure)) {
                    daemon_error(error, error_size,
                                 "test command failure: %s",
                                 command->argv[index]);
                    return 0;
                }
            }
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

static const char *tailscale_program(const SSDaemon *daemon)
{
    static const char *const paths[] = {
        "/usr/bin/tailscale", "/usr/local/bin/tailscale", "/bin/tailscale",
        "/usr/sbin/tailscale", "/usr/local/sbin/tailscale",
        "/sbin/tailscale", "/snap/bin/tailscale", NULL
    };

    return first_command(daemon, paths);
}

static int tailscale_address_from_output(const char *output, char *address,
                                         size_t address_size)
{
    char copy[4096];
    char *save = NULL;
    char *word;

    if (!output || !ss_copy_string(copy, sizeof(copy), output))
        return 0;
    for (word = strtok_r(copy, " \t\r\n", &save); word;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        if (ss_tailscale_ipv4_address(word))
            return ss_copy_string(address, address_size, word);
    }
    return 0;
}

static int tailscale_peer_name_valid(const char *name)
{
    size_t length;

    if (!name || !(length = strlen(name)) || length >= 256)
        return 0;
    for (size_t index = 0; index < length; index++) {
        unsigned char value = (unsigned char)name[index];
        int alphanumeric = (value >= 'a' && value <= 'z') ||
                           (value >= 'A' && value <= 'Z') ||
                           (value >= '0' && value <= '9');

        if (!alphanumeric && value != '-' && value != '.')
            return 0;
        if (index == 0 && !alphanumeric)
            return 0;
    }
    return 1;
}

static int tailscale_name_from_status(const char *status, char *name,
                                      size_t name_size)
{
    const char *cursor;
    const char *end;
    size_t length;

    if (!status || !name || name_size == 0 ||
        !(cursor = strstr(status, "\"DNSName\"")))
        return 0;
    cursor += strlen("\"DNSName\"");
    while (*cursor == ':' || isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor++ != '"' || !(end = strchr(cursor, '"')))
        return 0;
    length = (size_t)(end - cursor);
    if (length == 0 || length >= name_size)
        return 0;
    memcpy(name, cursor, length);
    name[length] = '\0';
    if (!tailscale_peer_name_valid(name)) {
        name[0] = '\0';
        return 0;
    }
    return 1;
}

static int test_tailscale_address(char *address, size_t address_size)
{
    const char *path = getenv("SIMPLESERVE_TEST_TAILSCALE_IP_FILE");
    const char *value = getenv("SIMPLESERVE_TEST_TAILSCALE_IP");
    char *contents = NULL;
    size_t length = 0;
    char error[256];
    int found = 0;

    if (path && *path &&
        ss_read_file(path, 4096, &contents, &length, error, sizeof(error))) {
        (void)length;
        found = tailscale_address_from_output(contents, address, address_size);
    } else if (value && *value) {
        found = tailscale_address_from_output(value, address, address_size);
    }
    free(contents);
    return found;
}

static void refresh_tailscale_state(SSDaemon *daemon, int force, int *changed)
{
    const char *program = NULL;
    char address[64] = "";
    char name[256] = "";
    int installed = 0;
    int active = 0;
    SSTailscaleState state = SS_TAILSCALE_MISSING;
    time_t now = time(NULL);

    *changed = 0;
    if (!force && daemon->last_tailscale_refresh != 0 &&
        now - daemon->last_tailscale_refresh < SS_TAILSCALE_REFRESH_SECONDS)
        return;
    if (daemon->test_mode) {
        const char *test_installed =
            getenv("SIMPLESERVE_TEST_TAILSCALE_INSTALLED");
        const char *test_state =
            getenv("SIMPLESERVE_TEST_TAILSCALE_STATE");

        active = test_tailscale_address(address, sizeof(address));
        installed = active ||
                    (test_installed && strcmp(test_installed, "0") != 0);
        if (active)
            state = SS_TAILSCALE_ACTIVE;
        else if (test_state && strcmp(test_state, "stopped") == 0)
            state = SS_TAILSCALE_DAEMON_STOPPED;
        else if (test_state && strcmp(test_state, "logged-out") == 0)
            state = SS_TAILSCALE_NEEDS_LOGIN;
        else if (installed)
            state = SS_TAILSCALE_INACTIVE;
        if (active) {
            const char *test_name =
                getenv("SIMPLESERVE_TEST_TAILSCALE_NAME");

            if (tailscale_peer_name_valid(test_name))
                ss_copy_string(name, sizeof(name), test_name);
        }
    } else {
        program = tailscale_program(daemon);
        installed = program != NULL;
        if (program) {
            const char *arguments[] = {"ip", "-4", NULL};
            SSCommand command;
            char output[4096] = "";
            char command_error[512] = "";
            int command_ok;

            state = SS_TAILSCALE_INACTIVE;
            command_ok = command_from(&command, program, arguments) &&
                         run_command_capture(
                             daemon, &command, 2500, output, sizeof(output),
                             command_error, sizeof(command_error));
            if (command_ok)
                active = tailscale_address_from_output(
                    output, address, sizeof(address));
            if (active) {
                state = SS_TAILSCALE_ACTIVE;
            } else if (strcasestr(output, "needslogin") ||
                       strcasestr(output, "not logged") ||
                       strcasestr(command_error, "needslogin") ||
                       strcasestr(command_error, "not logged")) {
                state = SS_TAILSCALE_NEEDS_LOGIN;
            } else if (!command_ok) {
                const char *status_arguments[] = {"status", "--json", NULL};
                char status_output[4096] = "";

                if (command_from(&command, program, status_arguments) &&
                    run_command_capture(
                        daemon, &command, 2500, status_output,
                        sizeof(status_output), command_error,
                        sizeof(command_error))) {
                    if (strstr(status_output,
                               "\"BackendState\": \"NeedsLogin\"") ||
                        strstr(status_output,
                               "\"BackendState\":\"NeedsLogin\""))
                        state = SS_TAILSCALE_NEEDS_LOGIN;
                } else {
                    state = SS_TAILSCALE_DAEMON_STOPPED;
                }
            }
            if (active) {
                int identity_due = !daemon->tailscale_name[0] ||
                    daemon->last_tailscale_identity_refresh == 0 ||
                    now - daemon->last_tailscale_identity_refresh >=
                        SS_TAILSCALE_IDENTITY_REFRESH_SECONDS;

                if (daemon->tailscale_name[0])
                    ss_copy_string(name, sizeof(name),
                                   daemon->tailscale_name);
                if (identity_due) {
                    const char *status_arguments[] = {
                        "status", "--json", "--peers=false", NULL
                    };
                    SSCommand identity_command;
                    char status_output[8192] = "";
                    char ignored[512] = "";

                    if (command_from(&identity_command, program,
                                     status_arguments)) {
                        (void)run_command_capture(
                            daemon, &identity_command, 2500, status_output,
                            sizeof(status_output), ignored, sizeof(ignored));
                        if (!tailscale_name_from_status(
                                status_output, name, sizeof(name)) &&
                            daemon->tailscale_name[0])
                            ss_copy_string(name, sizeof(name),
                                           daemon->tailscale_name);
                    }
                    daemon->last_tailscale_identity_refresh = now;
                }
            }
        }
    }
    if (installed != daemon->tailscale_installed ||
        active != daemon->tailscale_active ||
        state != daemon->tailscale_state ||
        strcmp(address, daemon->tailscale_address) != 0 ||
        strcmp(name, daemon->tailscale_name) != 0)
        *changed = 1;
    daemon->tailscale_installed = installed;
    daemon->tailscale_active = active;
    daemon->tailscale_state = state;
    ss_copy_string(daemon->tailscale_address,
                   sizeof(daemon->tailscale_address), address);
    ss_copy_string(daemon->tailscale_name,
                   sizeof(daemon->tailscale_name), name);
    daemon->last_tailscale_refresh = now;
}

static int peer_name_candidate(const char *source, int short_name,
                               char *candidate, size_t candidate_size)
{
    char *dot;
    size_t length;

    if (!source || !*source ||
        !ss_copy_string(candidate, candidate_size, source))
        return 0;
    length = strlen(candidate);
    while (length > 0 && candidate[length - 1] == '.')
        candidate[--length] = '\0';
    if (length > 6 && strcasecmp(candidate + length - 6, ".local") == 0)
        candidate[length - 6] = '\0';
    if (short_name && (dot = strchr(candidate, '.')) != NULL)
        *dot = '\0';
    return candidate[0] != '\0';
}

static int resolve_peer_tailscale_address(SSDaemon *daemon,
                                          const SSRemoteServer *server,
                                          char *address,
                                          size_t address_size,
                                          char *resolved_name,
                                          size_t resolved_name_size)
{
    char candidates[4][256];
    size_t candidate_count = 0;
    const char *program;

    address[0] = '\0';
    if (resolved_name && resolved_name_size)
        resolved_name[0] = '\0';
    if (!daemon->tailscale_active || !server)
        return 0;
    if (daemon->test_mode) {
        const char *test =
            getenv("SIMPLESERVE_TEST_REMOTE_TAILSCALE_ADDRESS");
        const char *test_name =
            getenv("SIMPLESERVE_TEST_REMOTE_TAILSCALE_NAME");

        if (!test || !ss_tailscale_ipv4_address(test) ||
            !ss_copy_string(address, address_size, test))
            return 0;
        if (resolved_name && resolved_name_size)
            ss_copy_string(resolved_name, resolved_name_size,
                           test_name && *test_name ? test_name :
                           (server->tailscale_name[0] ?
                                server->tailscale_name : server->name));
        return 1;
    }
    program = tailscale_program(daemon);
    if (!program)
        return 0;
    if (server->tailscale_name[0] &&
        ss_copy_string(candidates[candidate_count], sizeof(candidates[0]),
                       server->tailscale_name))
        candidate_count++;
    if (peer_name_candidate(server->hostname, 1, candidates[candidate_count],
                            sizeof(candidates[0])))
        if (candidate_count == 0 ||
            strcasecmp(candidates[candidate_count], candidates[0]) != 0)
            candidate_count++;
    if (server->name[0] &&
        (candidate_count == 0 ||
         strcasecmp(candidates[0], server->name) != 0) &&
        ss_copy_string(candidates[candidate_count], sizeof(candidates[0]),
                       server->name))
        candidate_count++;
    if (candidate_count < 4 &&
        peer_name_candidate(server->hostname, 0,
                            candidates[candidate_count],
                            sizeof(candidates[0])) &&
        (candidate_count == 0 ||
         strcasecmp(candidates[candidate_count], candidates[0]) != 0) &&
        (candidate_count < 2 ||
         strcasecmp(candidates[candidate_count], candidates[1]) != 0))
        candidate_count++;
    for (size_t index = 0; index < candidate_count; index++) {
        const char *arguments[] = {
            "ip", "-4", "--", candidates[index], NULL
        };
        SSCommand command;
        char output[4096];
        char ignored[512];

        if (command_from(&command, program, arguments) &&
            run_command_capture(daemon, &command, 2000, output,
                                sizeof(output), ignored, sizeof(ignored)) &&
            tailscale_address_from_output(output, address, address_size)) {
            if (resolved_name && resolved_name_size)
                ss_copy_string(resolved_name, resolved_name_size,
                               candidates[index]);
            return 1;
        }
    }
    return 0;
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
        int use_openrc = daemon->test_mode ?
            (test_init && strcmp(test_init, "openrc") == 0) :
            rc_service != NULL;

        if (use_openrc && rc_service) {
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
        !ss_render_exports(daemon->platform, &daemon->config,
                           daemon->tailscale_active, &generated,
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

typedef struct {
    char *contents;
    size_t length;
    mode_t mode;
    int existed;
} SSFileSnapshot;

static int snapshot_file(const char *path, SSFileSnapshot *snapshot,
                         char *error, size_t error_size)
{
    struct stat status;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->mode = 0644;
    if (lstat(path, &status) != 0) {
        if (errno != ENOENT) {
            daemon_error(error, error_size, "cannot inspect %s: %s", path,
                         strerror(errno));
            return 0;
        }
        snapshot->contents = strdup("");
        if (!snapshot->contents) {
            daemon_error(error, error_size, "out of memory");
            return 0;
        }
        return 1;
    }
    if (!S_ISREG(status.st_mode)) {
        daemon_error(error, error_size,
                     "refusing to replace non-regular configuration file %s",
                     path);
        return 0;
    }
    snapshot->existed = 1;
    snapshot->mode = status.st_mode & 07777;
    return ss_read_file(path, 4U * 1024U * 1024U, &snapshot->contents,
                        &snapshot->length, error, error_size);
}

static void free_file_snapshot(SSFileSnapshot *snapshot)
{
    free(snapshot->contents);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int restore_snapshot(const char *path,
                            const SSFileSnapshot *snapshot,
                            char *error, size_t error_size)
{
    if (snapshot->existed)
        return ss_atomic_write(path, snapshot->contents, snapshot->length,
                               snapshot->mode, error, error_size);
    if (unlink(path) != 0 && errno != ENOENT) {
        daemon_error(error, error_size, "cannot remove %s: %s", path,
                     strerror(errno));
        return 0;
    }
    return 1;
}

static int ensure_linux_smb(SSDaemon *daemon, char *error,
                            size_t error_size)
{
    static const char *const systemctl_paths[] = {
        "/bin/systemctl", "/usr/bin/systemctl", NULL
    };
    static const char *const rc_service_paths[] = {
        "/sbin/rc-service", "/usr/sbin/rc-service",
        "/bin/rc-service", "/usr/bin/rc-service", NULL
    };
    static const char *const rc_update_paths[] = {
        "/sbin/rc-update", "/usr/sbin/rc-update",
        "/bin/rc-update", "/usr/bin/rc-update", NULL
    };
    static const char *const service_paths[] = {
        "/usr/sbin/service", "/sbin/service", NULL
    };
    static const char *const update_rc_paths[] = {
        "/usr/sbin/update-rc.d", "/sbin/update-rc.d", NULL
    };
    static const char *const chkconfig_paths[] = {
        "/usr/sbin/chkconfig", "/sbin/chkconfig", NULL
    };
    static const char *const service_names[] = {
        "smbd", "smb", "samba", NULL
    };
    static const char *const systemd_names[] = {
        "smbd.service", "smb.service", "samba.service", NULL
    };
    static const char *const openrc_names[] = {
        "samba", "smbd", "smb", NULL
    };
    const char *test_init = getenv("SIMPLESERVE_TEST_INIT");
    const char *systemctl = first_command(daemon, systemctl_paths);
    SSCommand command;

    if ((daemon->test_mode &&
         (!test_init || strcmp(test_init, "systemd") == 0)) ||
        (!daemon->test_mode && systemctl &&
         access("/run/systemd/system", F_OK) == 0)) {
        for (size_t index = 0; systemd_names[index]; index++) {
            const char *arguments[] = {
                "enable", "--now", systemd_names[index], NULL
            };

            if (!command_from(&command,
                              systemctl ? systemctl : "/bin/systemctl",
                              arguments))
                return 0;
            if (run_command(daemon, &command, 30000, error, error_size))
                return 1;
        }
        return 0;
    }
    {
        const char *rc_service = first_command(daemon, rc_service_paths);
        const char *rc_update = first_command(daemon, rc_update_paths);
        int use_openrc = daemon->test_mode ?
            (test_init && strcmp(test_init, "openrc") == 0) :
            (rc_service && rc_update);

        if (use_openrc) {
            if (!rc_service || !rc_update) {
                daemon_error(error, error_size,
                             "OpenRC commands are missing for Samba");
                return 0;
            }
            for (size_t index = 0; openrc_names[index]; index++) {
                const char *enable_args[] = {
                    "add", openrc_names[index], "default", NULL
                };
                const char *start_args[] = {
                    openrc_names[index], "start", NULL
                };

                if (!command_from(&command, rc_update, enable_args))
                    return 0;
                if (!run_command(daemon, &command, 10000, error, error_size))
                    continue;
                if (!command_from(&command, rc_service, start_args))
                    return 0;
                if (run_command(daemon, &command, 30000, error, error_size))
                    return 1;
            }
            return 0;
        }
    }
    {
        const char *service = first_command(daemon, service_paths);
        const char *update_rc = first_command(daemon, update_rc_paths);
        const char *chkconfig = first_command(daemon, chkconfig_paths);

        if (!service) {
            daemon_error(error, error_size,
                         "no supported Linux service manager found for Samba");
            return 0;
        }
        for (size_t index = 0; service_names[index]; index++) {
            const char *start_args[] = {
                service_names[index], "start", NULL
            };

            if (!command_from(&command, service, start_args))
                return 0;
            if (!run_command(daemon, &command, 30000, error, error_size))
                continue;
            if (update_rc) {
                const char *enable_args[] = {
                    service_names[index], "enable", NULL
                };

                if (command_from(&command, update_rc, enable_args))
                    (void)run_command(daemon, &command, 10000, error,
                                      error_size);
            } else if (chkconfig) {
                const char *enable_args[] = {
                    service_names[index], "on", NULL
                };

                if (command_from(&command, chkconfig, enable_args))
                    (void)run_command(daemon, &command, 10000, error,
                                      error_size);
            }
            return 1;
        }
        return 0;
    }
}

static int reload_linux_smb(SSDaemon *daemon, char *error,
                            size_t error_size)
{
    static const char *const systemctl_paths[] = {
        "/bin/systemctl", "/usr/bin/systemctl", NULL
    };
    static const char *const rc_service_paths[] = {
        "/sbin/rc-service", "/usr/sbin/rc-service",
        "/bin/rc-service", "/usr/bin/rc-service", NULL
    };
    static const char *const service_paths[] = {
        "/usr/sbin/service", "/sbin/service", NULL
    };
    static const char *const smbcontrol_paths[] = {
        "/usr/bin/smbcontrol", "/usr/local/bin/smbcontrol",
        "/bin/smbcontrol", NULL
    };
    static const char *const service_names[] = {
        "smbd", "smb", "samba", NULL
    };
    static const char *const systemd_names[] = {
        "smbd.service", "smb.service", "samba.service", NULL
    };
    static const char *const openrc_names[] = {
        "samba", "smbd", "smb", NULL
    };
    const char *test_init = getenv("SIMPLESERVE_TEST_INIT");
    const char *systemctl = first_command(daemon, systemctl_paths);
    SSCommand command;

    if ((daemon->test_mode &&
         (!test_init || strcmp(test_init, "systemd") == 0)) ||
        (!daemon->test_mode && systemctl &&
         access("/run/systemd/system", F_OK) == 0)) {
        for (size_t index = 0; systemd_names[index]; index++) {
            const char *arguments[] = {
                "reload", systemd_names[index], NULL
            };

            if (!command_from(&command,
                              systemctl ? systemctl : "/bin/systemctl",
                              arguments))
                return 0;
            if (run_command(daemon, &command, 15000, error, error_size))
                return 1;
        }
    } else {
        const char *rc_service = first_command(daemon, rc_service_paths);
        int use_openrc = daemon->test_mode ?
            (test_init && strcmp(test_init, "openrc") == 0) :
            rc_service != NULL;

        if (use_openrc) {
            for (size_t index = 0; rc_service && openrc_names[index];
                 index++) {
                const char *arguments[] = {
                    openrc_names[index], "reload", NULL
                };

                if (!command_from(&command, rc_service, arguments))
                    return 0;
                if (run_command(daemon, &command, 15000, error, error_size))
                    return 1;
            }
        } else {
            const char *service = first_command(daemon, service_paths);

            for (size_t index = 0; service && service_names[index]; index++) {
                const char *arguments[] = {
                    service_names[index], "reload", NULL
                };

                if (!command_from(&command, service, arguments))
                    return 0;
                if (run_command(daemon, &command, 15000, error, error_size))
                    return 1;
            }
        }
    }
    {
        const char *smbcontrol = first_command(daemon, smbcontrol_paths);
        const char *arguments[] = {"all", "reload-config", NULL};

        if (!smbcontrol) {
            daemon_error(error, error_size,
                         "cannot reload the Linux Samba service");
            return 0;
        }
        if (!command_from(&command, smbcontrol, arguments))
            return 0;
        return run_command(daemon, &command, 15000, error, error_size);
    }
}

static int validate_linux_smb(SSDaemon *daemon, const char *config_path,
                              char *error, size_t error_size)
{
    static const char *const testparm_paths[] = {
        "/usr/bin/testparm", "/usr/local/bin/testparm",
        "/bin/testparm", NULL
    };
    const char *testparm = first_command(daemon, testparm_paths);
    const char *arguments[] = {"-s", config_path, NULL};
    SSCommand command;

    if (!testparm) {
        daemon_error(error, error_size,
                     "testparm is missing; install the Samba server package");
        return 0;
    }
    if (!command_from(&command, testparm, arguments))
        return 0;
    return run_command(daemon, &command, 15000, error, error_size);
}

static int render_samba_config(const SSServerConfig *config,
                               SSBuffer *output, char *error,
                               size_t error_size)
{
    return ss_render_samba_config(config, output, error, error_size);
}

static int restore_samba_configuration(
    SSDaemon *daemon, const SSFileSnapshot *old_main,
    const SSFileSnapshot *old_managed, char *error, size_t error_size)
{
    /* Restore a referenced include before its parent configuration. If the
     * include did not exist, withdraw the parent reference before removing
     * the newly created file. */
    if (old_managed->existed) {
        if (!restore_snapshot(daemon->samba_path, old_managed,
                              error, error_size) ||
            !restore_snapshot(daemon->smb_conf_path, old_main,
                              error, error_size))
            return 0;
    } else {
        if (!restore_snapshot(daemon->smb_conf_path, old_main,
                              error, error_size) ||
            !restore_snapshot(daemon->samba_path, old_managed,
                              error, error_size))
            return 0;
    }
    return 1;
}

static int sync_samba(SSDaemon *daemon, char *error, size_t error_size)
{
    static const char registration_marker[] =
        "# BEGIN SimpleServe managed Samba include";
    SSFileSnapshot old_main;
    SSFileSnapshot old_managed;
    SSBuffer generated;
    SSBuffer final_main;
    SSBuffer candidate_main;
    char candidate_managed_path[PATH_MAX];
    char candidate_main_path[PATH_MAX];
    char original_error[512];
    int have_shares;
    int registered;
    int managed_changed;
    int main_changed;
    int changed;
    int installed = 0;
    int ok = 0;

    if (daemon->platform != SS_PLATFORM_LINUX)
        return 1;
    memset(&old_main, 0, sizeof(old_main));
    memset(&old_managed, 0, sizeof(old_managed));
    ss_buffer_init(&generated);
    ss_buffer_init(&final_main);
    ss_buffer_init(&candidate_main);
    candidate_managed_path[0] = '\0';
    candidate_main_path[0] = '\0';
    have_shares = active_share_count(daemon) > 0;
    if (!snapshot_file(daemon->smb_conf_path, &old_main,
                       error, error_size) ||
        !snapshot_file(daemon->samba_path, &old_managed,
                       error, error_size))
        goto done;
    registered = have_shares || old_managed.existed ||
                 strstr(old_main.contents, registration_marker) != NULL;
    if (!registered) {
        ok = 1;
        goto done;
    }
    if (!render_samba_config(&daemon->config, &generated,
                             error, error_size) ||
        !ss_replace_managed_samba_include(
            old_main.contents, daemon->samba_path, &final_main,
            error, error_size))
        goto done;
    managed_changed = old_managed.length != generated.length ||
        memcmp(old_managed.contents, generated.data, generated.length) != 0;
    main_changed = old_main.length != final_main.length ||
        memcmp(old_main.contents, final_main.data, final_main.length) != 0;
    changed = managed_changed || main_changed;

    if (changed) {
        if (snprintf(candidate_managed_path,
                     sizeof(candidate_managed_path), "%s.candidate.%ld",
                     daemon->samba_path, (long)getpid()) >=
                (int)sizeof(candidate_managed_path) ||
            snprintf(candidate_main_path, sizeof(candidate_main_path),
                     "%s.candidate.%ld", daemon->smb_conf_path,
                     (long)getpid()) >= (int)sizeof(candidate_main_path)) {
            daemon_error(error, error_size,
                         "Samba candidate path is too long");
            goto done;
        }
        (void)unlink(candidate_managed_path);
        (void)unlink(candidate_main_path);
        if (!ss_replace_managed_samba_include(
                old_main.contents, candidate_managed_path, &candidate_main,
                error, error_size) ||
            !ss_atomic_write(candidate_managed_path, generated.data,
                             generated.length, 0644, error, error_size) ||
            !ss_atomic_write(candidate_main_path, candidate_main.data,
                             candidate_main.length, old_main.mode,
                             error, error_size) ||
            !validate_linux_smb(daemon, candidate_main_path,
                                error, error_size))
            goto done;
        (void)unlink(candidate_main_path);
        candidate_main_path[0] = '\0';
        (void)unlink(candidate_managed_path);
        candidate_managed_path[0] = '\0';

        installed = 1;
        if (managed_changed &&
            !ss_atomic_write(daemon->samba_path, generated.data,
                             generated.length, old_managed.mode,
                             error, error_size))
            goto rollback;
        if (main_changed &&
            !ss_atomic_write(daemon->smb_conf_path, final_main.data,
                             final_main.length, old_main.mode,
                             error, error_size))
            goto rollback;
    }
    if (!validate_linux_smb(daemon, daemon->smb_conf_path,
                            error, error_size)) {
        if (installed)
            goto rollback;
        goto done;
    }
    if ((have_shares || changed) &&
        !ensure_linux_smb(daemon, error, error_size)) {
        if (installed)
            goto rollback;
        goto done;
    }
    if ((have_shares || changed) &&
        !reload_linux_smb(daemon, error, error_size)) {
        if (installed)
            goto rollback;
        goto done;
    }
    ok = 1;
    goto done;

rollback:
    ss_copy_string(original_error, sizeof(original_error), error);
    {
        char rollback_error[512];

        if (!restore_samba_configuration(
                daemon, &old_main, &old_managed, rollback_error,
                sizeof(rollback_error))) {
            fprintf(stderr,
                    "simpleserved: Samba configuration rollback failed: %s\n",
                    rollback_error);
        } else {
            if (old_main.existed &&
                !validate_linux_smb(daemon, daemon->smb_conf_path,
                                    rollback_error,
                                    sizeof(rollback_error)))
                fprintf(stderr,
                        "simpleserved: restored Samba validation failed: %s\n",
                        rollback_error);
            if (!reload_linux_smb(daemon, rollback_error,
                                  sizeof(rollback_error)))
                fprintf(stderr,
                        "simpleserved: restored Samba reload failed: %s\n",
                        rollback_error);
        }
    }
    ss_copy_string(error, error_size, original_error);

done:
    if (candidate_main_path[0])
        (void)unlink(candidate_main_path);
    if (candidate_managed_path[0])
        (void)unlink(candidate_managed_path);
    free_file_snapshot(&old_main);
    free_file_snapshot(&old_managed);
    ss_buffer_free(&generated);
    ss_buffer_free(&final_main);
    ss_buffer_free(&candidate_main);
    return ok;
}

static int sync_mount_persistence(SSDaemon *daemon, char *error,
                                  size_t error_size)
{
    SSBuffer generated;
    SSBuffer final;
    char *old_contents = NULL;
    size_t old_length = 0;
    int ok = 0;

    if (daemon->platform != SS_PLATFORM_LINUX)
        return 1;
    ss_buffer_init(&generated);
    ss_buffer_init(&final);
    if (!ss_render_fstab(&daemon->config, &generated, error, error_size) ||
        !file_contents_or_empty(daemon->fstab_path, &old_contents, &old_length,
                                error, error_size) ||
        !ss_replace_managed_fstab(old_contents,
                                  generated.data ? generated.data : "", &final,
                                  error, error_size))
        goto done;
    if (old_length != final.length ||
        memcmp(old_contents, final.data ? final.data : "", final.length) != 0) {
        if (!ss_atomic_write(daemon->fstab_path,
                             final.data ? final.data : "", final.length,
                             0644, error, error_size))
            goto done;
    }
    ok = 1;

done:
    free(old_contents);
    ss_buffer_free(&generated);
    ss_buffer_free(&final);
    return ok;
}

static int start_avahi_daemon_once(SSDaemon *daemon, char *error,
                                   size_t error_size)
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
                               "Content-Length: %zu\r\n",
                               body.length);
        if (daemon->tailscale_active && daemon->tailscale_name[0])
            (void)ss_buffer_appendf(
                &response, "X-SimpleServe-Tailscale-Name: %s\r\n",
                daemon->tailscale_name);
        if (daemon->tailscale_active && daemon->tailscale_address[0])
            (void)ss_buffer_appendf(
                &response, "X-SimpleServe-Tailscale-IPv4: %s\r\n",
                daemon->tailscale_address);
        (void)ss_buffer_append(&response,
                               "Connection: close\r\n\r\n");
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

static int route_reachable(SSDaemon *daemon, SSRoute route,
                           const char *address)
{
    char ignored[256];
    int descriptor;

    if (route == SS_ROUTE_TAILSCALE && !daemon->tailscale_active)
        return 0;
    if (daemon->test_mode) {
        const char *setting = getenv(
            route == SS_ROUTE_TAILSCALE ?
                "SIMPLESERVE_TEST_TAILSCALE_REACHABLE" :
                "SIMPLESERVE_TEST_LAN_REACHABLE");
        const char *setting_file = getenv(
            route == SS_ROUTE_TAILSCALE ?
                "SIMPLESERVE_TEST_TAILSCALE_REACHABLE_FILE" :
                "SIMPLESERVE_TEST_LAN_REACHABLE_FILE");
        char *contents = NULL;
        size_t length = 0;

        if (setting_file && *setting_file &&
            ss_read_file(setting_file, 64, &contents, &length, ignored,
                         sizeof(ignored))) {
            int reachable = length > 0 && contents[0] != '0';

            free(contents);
            return reachable;
        }
        free(contents);

        return !setting || strcmp(setting, "0") != 0;
    }
    descriptor = connect_ipv4_timeout(address, 111, SS_ROUTE_PROBE_MS,
                                      ignored, sizeof(ignored));
    if (descriptor < 0)
        return 0;
    close(descriptor);
    return 1;
}

static int http_header_value(const char *response, const char *headers_end,
                             const char *wanted, char *value,
                             size_t value_size)
{
    const char *cursor = response;
    size_t wanted_length;

    if (!response || !headers_end || headers_end < response || !wanted ||
        !value || value_size == 0)
        return 0;
    value[0] = '\0';
    wanted_length = strlen(wanted);
    while (cursor < headers_end) {
        const char *line_end = memchr(cursor, '\n',
                                      (size_t)(headers_end - cursor));
        const char *content_end = line_end ? line_end : headers_end;
        const char *colon;
        const char *start;
        size_t length;

        if (content_end > cursor && content_end[-1] == '\r')
            content_end--;
        colon = memchr(cursor, ':', (size_t)(content_end - cursor));
        if (colon && (size_t)(colon - cursor) == wanted_length &&
            strncasecmp(cursor, wanted, wanted_length) == 0) {
            start = colon + 1;
            while (start < content_end &&
                   (*start == ' ' || *start == '\t'))
                start++;
            while (content_end > start &&
                   (content_end[-1] == ' ' || content_end[-1] == '\t'))
                content_end--;
            length = (size_t)(content_end - start);
            if (length == 0 || length >= value_size)
                return 0;
            for (size_t index = 0; index < length; index++) {
                unsigned char byte = (unsigned char)start[index];

                if (byte < 33 || byte > 126)
                    return 0;
            }
            memcpy(value, start, length);
            value[length] = '\0';
            return 1;
        }
        if (!line_end)
            break;
        cursor = line_end + 1;
    }
    return 0;
}

static int fetch_manifest(const char *address, unsigned int port,
                          SSRemoteServer *server, char *error,
                          size_t error_size)
{
    char request[256];
    SSBuffer response;
    int descriptor;
    long long deadline;
    char *headers_end;
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
        int poll_result = poll(&poll_descriptor, 1,
                               remaining > 500 ? 500 : (int)remaining);

        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            daemon_error(error, error_size,
                         "cannot read manifest response from %s: %s",
                         address, strerror(errno));
            goto done;
        }
        if (poll_result == 0)
            continue;
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
        (!(headers_end = strstr(response.data, "\r\n\r\n")))) {
        daemon_error(error, error_size, "invalid manifest HTTP response from %s",
                     address);
        goto done;
    }
    body = headers_end + 4;
    ok = ss_parse_manifest(body, address, port, server, error, error_size);
    if (ok) {
        char metadata[256];

        if (http_header_value(response.data, headers_end,
                              "X-SimpleServe-Tailscale-Name", metadata,
                              sizeof(metadata)) &&
            tailscale_peer_name_valid(metadata))
            ss_copy_string(server->tailscale_name,
                           sizeof(server->tailscale_name), metadata);
        if (http_header_value(response.data, headers_end,
                              "X-SimpleServe-Tailscale-IPv4", metadata,
                              sizeof(metadata)) &&
            ss_tailscale_ipv4_address(metadata))
            ss_copy_string(server->tailscale_address,
                           sizeof(server->tailscale_address), metadata);
    }

done:
    close(descriptor);
    ss_buffer_free(&response);
    return ok;
}

static int service_identity_matches(const SSDiscoveredService *service,
                                    AvahiIfIndex interface,
                                    AvahiProtocol protocol, const char *name,
                                    const char *type, const char *domain)
{
    return service->interface == interface && service->protocol == protocol &&
           strcmp(service->name, name) == 0 &&
           strcmp(service->type, type) == 0 &&
           strcmp(service->domain, domain) == 0;
}

static size_t find_service_index_locked(const SSDaemon *daemon,
                                        AvahiIfIndex interface,
                                        AvahiProtocol protocol,
                                        const char *name, const char *type,
                                        const char *domain)
{
    for (size_t index = 0; index < daemon->service_count; index++) {
        if (service_identity_matches(&daemon->services[index], interface,
                                     protocol, name, type, domain))
            return index;
    }
    return SIZE_MAX;
}

static size_t find_remote_index_locked(const SSDaemon *daemon,
                                       const char *name)
{
    for (size_t index = 0; index < daemon->remote_count; index++) {
        if (strcmp(daemon->remotes[index].name, name) == 0)
            return index;
    }
    return SIZE_MAX;
}

static void remove_remote_index_locked(SSDaemon *daemon, size_t index)
{
    if (index >= daemon->remote_count)
        return;
    if (index + 1 < daemon->remote_count) {
        memmove(&daemon->remotes[index], &daemon->remotes[index + 1],
                (daemon->remote_count - index - 1) *
                    sizeof(daemon->remotes[0]));
        memmove(&daemon->remote_sources[index],
                &daemon->remote_sources[index + 1],
                (daemon->remote_count - index - 1) *
                    sizeof(daemon->remote_sources[0]));
    }
    daemon->remote_count--;
    daemon->remote_revision++;
}

static void remove_remote_source_locked(SSDaemon *daemon, uint64_t generation)
{
    for (size_t index = 0; index < daemon->remote_count; index++) {
        if (daemon->remote_sources[index] == generation) {
            remove_remote_index_locked(daemon, index);
            return;
        }
    }
}

static void clear_remote_discovery_locked(SSDaemon *daemon)
{
    daemon->service_count = 0;
    daemon->manifest_job_count = 0;
    if (daemon->remote_count > 0) {
        daemon->remote_count = 0;
        daemon->remote_revision++;
    }
    daemon->avahi_all_for_now = 0;
}

static void remove_service_index_locked(SSDaemon *daemon, size_t index)
{
    uint64_t generation;
    char server_name[SS_MAX_NAME + 1];

    if (index >= daemon->service_count)
        return;
    generation = daemon->services[index].generation;
    ss_copy_string(server_name, sizeof(server_name),
                   daemon->services[index].server_name);
    remove_remote_source_locked(daemon, generation);
    if (index + 1 < daemon->service_count) {
        memmove(&daemon->services[index], &daemon->services[index + 1],
                (daemon->service_count - index - 1) *
                    sizeof(daemon->services[0]));
    }
    daemon->service_count--;
    if (server_name[0]) {
        for (size_t candidate = 0; candidate < daemon->service_count;
             candidate++) {
            SSDiscoveredService *service = &daemon->services[candidate];

            if (strcmp(service->server_name, server_name) == 0) {
                service->resolve_requested = 1;
                service->retry_at_ms = 0;
                break;
            }
        }
    }
}

static int avahi_txt_value(AvahiStringList *txt, const char *wanted,
                           char *value, size_t value_size)
{
    AvahiStringList *item;
    char *key = NULL;
    char *found = NULL;
    size_t found_size = 0;
    int ok = 0;

    if (!txt)
        return 0;
    item = avahi_string_list_find(txt, wanted);
    if (item && avahi_string_list_get_pair(item, &key, &found,
                                           &found_size) == 0 && found &&
        found_size > 0 && found_size < value_size &&
        memchr(found, '\0', found_size) == NULL) {
        memcpy(value, found, found_size);
        value[found_size] = '\0';
        ok = 1;
    }
    avahi_free(key);
    avahi_free(found);
    return ok;
}

static int advertised_server_name(const char *service_name,
                                  AvahiStringList *txt, char *server_name,
                                  size_t server_name_size)
{
    char candidate[SS_MAX_NAME + 1];
    const char suffix[] = " SimpleServe";
    size_t length;

    if (avahi_txt_value(txt, "server", candidate, sizeof(candidate)) &&
        ss_valid_name(candidate))
        return ss_copy_string(server_name, server_name_size, candidate);
    if (!ss_copy_string(candidate, sizeof(candidate), service_name))
        return 0;
    length = strlen(candidate);
    if (length > sizeof(suffix) - 1 &&
        strcmp(candidate + length - (sizeof(suffix) - 1), suffix) == 0)
        candidate[length - (sizeof(suffix) - 1)] = '\0';
    return ss_valid_name(candidate) &&
           ss_copy_string(server_name, server_name_size, candidate);
}

static int queue_manifest_locked(SSDaemon *daemon,
                                 const SSResolverContext *context,
                                 const char *hostname, const char *address,
                                 unsigned int port,
                                 const char *advertised_name)
{
    size_t service_index = find_service_index_locked(
        daemon, context->interface, context->protocol, context->name,
        context->type, context->domain);
    SSManifestJob *job;
    SSDiscoveredService *service;

    if (service_index == SIZE_MAX ||
        daemon->services[service_index].generation != context->generation)
        return 0;
    service = &daemon->services[service_index];
    if (service->manifest_pending)
        return 1;
    if (daemon->manifest_job_count >= SS_MAX_SERVERS)
        return 0;
    job = &daemon->manifest_jobs[daemon->manifest_job_count++];
    memset(job, 0, sizeof(*job));
    job->interface = context->interface;
    job->protocol = context->protocol;
    job->generation = context->generation;
    job->port = port;
    if (!ss_copy_string(job->name, sizeof(job->name), context->name) ||
        !ss_copy_string(job->type, sizeof(job->type), context->type) ||
        !ss_copy_string(job->domain, sizeof(job->domain), context->domain) ||
        !ss_copy_string(job->hostname, sizeof(job->hostname), hostname) ||
        !ss_copy_string(job->address, sizeof(job->address), address) ||
        !ss_copy_string(job->advertised_name,
                        sizeof(job->advertised_name), advertised_name)) {
        daemon->manifest_job_count--;
        return 0;
    }
    ss_copy_string(service->server_name, sizeof(service->server_name),
                   advertised_name);
    service->manifest_pending = 1;
    pthread_cond_signal(&daemon->manifest_condition);
    return 1;
}

static void service_resolver_callback(
    AvahiServiceResolver *resolver, AvahiIfIndex interface,
    AvahiProtocol protocol, AvahiResolverEvent event, const char *name,
    const char *type, const char *domain, const char *hostname,
    const AvahiAddress *address, uint16_t port, AvahiStringList *txt,
    AvahiLookupResultFlags flags, void *userdata)
{
    SSResolverContext *context = userdata;
    SSDaemon *daemon = context->daemon;
    char address_text[AVAHI_ADDRESS_STR_MAX] = "";
    char server_name[SS_MAX_NAME + 1] = "";
    size_t service_index;
    int queued = 0;

    (void)interface;
    (void)protocol;
    (void)name;
    (void)type;
    (void)domain;
    (void)flags;
    if (event == AVAHI_RESOLVER_FOUND && hostname && address && port > 0 &&
        avahi_address_snprint(address_text, sizeof(address_text), address) &&
        ss_private_ipv4_address(address_text) &&
        advertised_server_name(context->name, txt, server_name,
                               sizeof(server_name))) {
        pthread_mutex_lock(&daemon->remote_mutex);
        queued = queue_manifest_locked(daemon, context, hostname, address_text,
                                       port, server_name);
        pthread_mutex_unlock(&daemon->remote_mutex);
    }
    pthread_mutex_lock(&daemon->remote_mutex);
    service_index = find_service_index_locked(
        daemon, context->interface, context->protocol, context->name,
        context->type, context->domain);
    if (service_index != SIZE_MAX &&
        daemon->services[service_index].generation == context->generation) {
        SSDiscoveredService *service = &daemon->services[service_index];

        service->resolving = 0;
        if (!queued) {
            service->resolve_requested = 1;
            service->retry_at_ms = monotonic_ms() + SS_AVAHI_RETRY_MS;
        }
    }
    pthread_mutex_unlock(&daemon->remote_mutex);
    avahi_service_resolver_free(resolver);
    memset(context, 0, sizeof(*context));
}

static void service_browser_callback(
    AvahiServiceBrowser *browser, AvahiIfIndex interface,
    AvahiProtocol protocol, AvahiBrowserEvent event, const char *name,
    const char *type, const char *domain, AvahiLookupResultFlags flags,
    void *userdata)
{
    SSDaemon *daemon = userdata;
    size_t index;

    (void)browser;
    (void)flags;
    switch (event) {
    case AVAHI_BROWSER_NEW:
        pthread_mutex_lock(&daemon->remote_mutex);
        index = find_service_index_locked(daemon, interface, protocol, name,
                                          type, domain);
        if (index == SIZE_MAX && daemon->service_count < SS_MAX_SERVERS) {
            SSDiscoveredService *service =
                &daemon->services[daemon->service_count];

            memset(service, 0, sizeof(*service));
            service->interface = interface;
            service->protocol = protocol;
            if (ss_copy_string(service->name, sizeof(service->name), name) &&
                ss_copy_string(service->type, sizeof(service->type), type) &&
                ss_copy_string(service->domain, sizeof(service->domain),
                               domain)) {
                service->generation = ++daemon->next_service_generation;
                service->resolve_requested = 1;
                daemon->service_count++;
            }
        }
        pthread_mutex_unlock(&daemon->remote_mutex);
        break;
    case AVAHI_BROWSER_REMOVE:
        pthread_mutex_lock(&daemon->remote_mutex);
        index = find_service_index_locked(daemon, interface, protocol, name,
                                          type, domain);
        if (index != SIZE_MAX)
            remove_service_index_locked(daemon, index);
        pthread_mutex_unlock(&daemon->remote_mutex);
        break;
    case AVAHI_BROWSER_FAILURE:
        fprintf(stderr, "simpleserved: Avahi browser failed: %s\n",
                avahi_strerror(avahi_client_errno(daemon->avahi_client)));
        pthread_mutex_lock(&daemon->remote_mutex);
        clear_remote_discovery_locked(daemon);
        pthread_mutex_unlock(&daemon->remote_mutex);
        daemon->avahi_browser_restart_requested = 1;
        daemon->avahi_restart_at_ms = monotonic_ms() + SS_AVAHI_RETRY_MS;
        break;
    case AVAHI_BROWSER_ALL_FOR_NOW:
        pthread_mutex_lock(&daemon->remote_mutex);
        daemon->avahi_all_for_now = 1;
        pthread_mutex_unlock(&daemon->remote_mutex);
        break;
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        break;
    }
}

static int create_avahi_browser(SSDaemon *daemon, AvahiClient *client)
{
    if (daemon->avahi_browser)
        return 1;
    daemon->avahi_browser = avahi_service_browser_new(
        client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, SS_SERVICE_TYPE, NULL, 0,
        service_browser_callback, daemon);
    if (!daemon->avahi_browser) {
        fprintf(stderr, "simpleserved: cannot create Avahi browser: %s\n",
                avahi_strerror(avahi_client_errno(client)));
        return 0;
    }
    daemon->avahi_browser_restart_requested = 0;
    return 1;
}

static void avahi_client_callback(AvahiClient *client, AvahiClientState state,
                                  void *userdata)
{
    SSDaemon *daemon = userdata;

    daemon->avahi_client = client;
    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        if (!create_avahi_browser(daemon, client)) {
            daemon->avahi_browser_restart_requested = 1;
            daemon->avahi_restart_at_ms = monotonic_ms() + SS_AVAHI_RETRY_MS;
        }
        break;
    case AVAHI_CLIENT_FAILURE:
        fprintf(stderr, "simpleserved: Avahi client failed: %s\n",
                avahi_strerror(avahi_client_errno(client)));
        pthread_mutex_lock(&daemon->remote_mutex);
        clear_remote_discovery_locked(daemon);
        pthread_mutex_unlock(&daemon->remote_mutex);
        daemon->avahi_client_restart_requested = 1;
        daemon->avahi_restart_at_ms = monotonic_ms() + SS_AVAHI_RETRY_MS;
        break;
    case AVAHI_CLIENT_S_REGISTERING:
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_CONNECTING:
        break;
    }
}

static int create_avahi_client(SSDaemon *daemon)
{
    int avahi_error = 0;
    AvahiClient *client;

    client = avahi_client_new(avahi_simple_poll_get(daemon->avahi_poll),
                              AVAHI_CLIENT_NO_FAIL, avahi_client_callback,
                              daemon, &avahi_error);
    if (!client) {
        fprintf(stderr, "simpleserved: cannot create Avahi client: %s\n",
                avahi_strerror(avahi_error));
        return 0;
    }
    daemon->avahi_client = client;
    daemon->avahi_client_restart_requested = 0;
    return 1;
}

static void process_resolve_requests(SSDaemon *daemon)
{
    for (;;) {
        SSResolverContext *context = NULL;
        long long now = monotonic_ms();

        pthread_mutex_lock(&daemon->remote_mutex);
        for (size_t index = 0; index < daemon->service_count; index++) {
            SSDiscoveredService *service = &daemon->services[index];

            if (!service->resolve_requested || service->resolving ||
                service->retry_at_ms > now)
                continue;
            for (size_t slot = 0; slot < SS_MAX_SERVERS; slot++) {
                if (!daemon->resolver_contexts[slot].in_use) {
                    context = &daemon->resolver_contexts[slot];
                    memset(context, 0, sizeof(*context));
                    context->in_use = 1;
                    break;
                }
            }
            if (!context)
                break;
            context->daemon = daemon;
            context->interface = service->interface;
            context->protocol = service->protocol;
            context->generation = service->generation;
            if (!ss_copy_string(context->name, sizeof(context->name),
                                service->name) ||
                !ss_copy_string(context->type, sizeof(context->type),
                                service->type) ||
                !ss_copy_string(context->domain, sizeof(context->domain),
                                service->domain)) {
                memset(context, 0, sizeof(*context));
                context = NULL;
                break;
            }
            service->resolve_requested = 0;
            service->resolving = 1;
            break;
        }
        pthread_mutex_unlock(&daemon->remote_mutex);
        if (!context)
            return;
        if (!avahi_service_resolver_new(
                daemon->avahi_client, context->interface, context->protocol,
                context->name, context->type, context->domain,
                AVAHI_PROTO_INET, 0, service_resolver_callback, context)) {
            size_t index;

            pthread_mutex_lock(&daemon->remote_mutex);
            index = find_service_index_locked(
                daemon, context->interface, context->protocol, context->name,
                context->type, context->domain);
            if (index != SIZE_MAX &&
                daemon->services[index].generation == context->generation) {
                daemon->services[index].resolving = 0;
                daemon->services[index].resolve_requested = 1;
                daemon->services[index].retry_at_ms =
                    monotonic_ms() + SS_AVAHI_RETRY_MS;
            }
            pthread_mutex_unlock(&daemon->remote_mutex);
            memset(context, 0, sizeof(*context));
            return;
        }
    }
}

static void cache_manifest_result_locked(SSDaemon *daemon,
                                         const SSManifestJob *job,
                                         const SSRemoteServer *remote)
{
    size_t service_index = find_service_index_locked(
        daemon, job->interface, job->protocol, job->name, job->type,
        job->domain);
    size_t remote_index;

    if (service_index == SIZE_MAX ||
        daemon->services[service_index].generation != job->generation)
        return;
    daemon->services[service_index].manifest_pending = 0;
    if (!remote || strcmp(remote->name, job->advertised_name) != 0) {
        daemon->services[service_index].resolve_requested = 1;
        daemon->services[service_index].retry_at_ms =
            monotonic_ms() + SS_MANIFEST_RETRY_MS;
        return;
    }
    remote_index = find_remote_index_locked(daemon, remote->name);
    if (remote_index == SIZE_MAX) {
        if (daemon->remote_count >= SS_MAX_SERVERS)
            return;
        remote_index = daemon->remote_count++;
    }
    daemon->remotes[remote_index] = *remote;
    daemon->remote_sources[remote_index] = job->generation;
    daemon->remote_revision++;
    daemon->services[service_index].retry_at_ms = 0;
}

static void *manifest_worker_main(void *userdata)
{
    SSDaemon *daemon = userdata;

    for (;;) {
        SSManifestJob job;
        SSRemoteServer *remote;
        char error[512];
        int fetched;

        pthread_mutex_lock(&daemon->remote_mutex);
        while (!daemon->discovery_stopping &&
               daemon->manifest_job_count == 0)
            pthread_cond_wait(&daemon->manifest_condition,
                              &daemon->remote_mutex);
        if (daemon->discovery_stopping) {
            pthread_mutex_unlock(&daemon->remote_mutex);
            break;
        }
        job = daemon->manifest_jobs[0];
        if (daemon->manifest_job_count > 1) {
            memmove(&daemon->manifest_jobs[0], &daemon->manifest_jobs[1],
                    (daemon->manifest_job_count - 1) *
                        sizeof(daemon->manifest_jobs[0]));
        }
        daemon->manifest_job_count--;
        pthread_mutex_unlock(&daemon->remote_mutex);

        remote = malloc(sizeof(*remote));
        fetched = remote && fetch_manifest(job.address, job.port, remote,
                                           error, sizeof(error));
        if (fetched)
            ss_copy_string(remote->hostname, sizeof(remote->hostname),
                           job.hostname);
        pthread_mutex_lock(&daemon->remote_mutex);
        cache_manifest_result_locked(daemon, &job, fetched ? remote : NULL);
        pthread_mutex_unlock(&daemon->remote_mutex);
        if (!fetched)
            fprintf(stderr, "simpleserved: manifest refresh for %s failed: %s\n",
                    job.advertised_name,
                    remote ? error : "out of memory");
        free(remote);
        if (daemon->avahi_poll)
            avahi_simple_poll_wakeup(daemon->avahi_poll);
    }
    return NULL;
}

static void restart_avahi_client_if_needed(SSDaemon *daemon)
{
    if (!daemon->avahi_client_restart_requested ||
        monotonic_ms() < daemon->avahi_restart_at_ms)
        return;
    if (daemon->avahi_browser) {
        avahi_service_browser_free(daemon->avahi_browser);
        daemon->avahi_browser = NULL;
    }
    if (daemon->avahi_client) {
        avahi_client_free(daemon->avahi_client);
        daemon->avahi_client = NULL;
        memset(daemon->resolver_contexts, 0,
               sizeof(daemon->resolver_contexts));
    }
    if (!create_avahi_client(daemon)) {
        daemon->avahi_client_restart_requested = 1;
        daemon->avahi_restart_at_ms = monotonic_ms() + SS_AVAHI_RETRY_MS;
    }
}

static void restart_avahi_browser_if_needed(SSDaemon *daemon)
{
    if (!daemon->avahi_browser_restart_requested ||
        daemon->avahi_client_restart_requested ||
        monotonic_ms() < daemon->avahi_restart_at_ms ||
        !daemon->avahi_client ||
        avahi_client_get_state(daemon->avahi_client) != AVAHI_CLIENT_S_RUNNING)
        return;
    if (daemon->avahi_browser) {
        avahi_service_browser_free(daemon->avahi_browser);
        daemon->avahi_browser = NULL;
    }
    if (!create_avahi_browser(daemon, daemon->avahi_client)) {
        daemon->avahi_browser_restart_requested = 1;
        daemon->avahi_restart_at_ms = monotonic_ms() + SS_AVAHI_RETRY_MS;
    }
}

static void *avahi_worker_main(void *userdata)
{
    SSDaemon *daemon = userdata;

    for (;;) {
        int stopping;
        int result;

        pthread_mutex_lock(&daemon->remote_mutex);
        stopping = daemon->discovery_stopping;
        pthread_mutex_unlock(&daemon->remote_mutex);
        if (stopping)
            break;
        restart_avahi_client_if_needed(daemon);
        restart_avahi_browser_if_needed(daemon);
        if (daemon->avahi_client && daemon->avahi_browser)
            process_resolve_requests(daemon);
        result = avahi_simple_poll_iterate(daemon->avahi_poll, 250);
        if (result < 0) {
            pthread_mutex_lock(&daemon->remote_mutex);
            clear_remote_discovery_locked(daemon);
            pthread_mutex_unlock(&daemon->remote_mutex);
            daemon->avahi_client_restart_requested = 1;
            daemon->avahi_restart_at_ms = monotonic_ms() + SS_AVAHI_RETRY_MS;
        }
    }
    return NULL;
}

static int seed_test_remote_cache(SSDaemon *daemon, char *error,
                                  size_t error_size)
{
    const char *manifest_path = getenv("SIMPLESERVE_TEST_MANIFEST");
    const char *address = getenv("SIMPLESERVE_TEST_REMOTE_ADDRESS");
    char *contents = NULL;
    size_t length = 0;
    SSRemoteServer *remote;
    int ok;

    if (!manifest_path || !*manifest_path)
        return 1;
    if (!address || !*address)
        address = "127.0.0.2";
    if (!ss_read_file(manifest_path, SS_DAEMON_CONFIG_MAX, &contents, &length,
                      error, error_size))
        return 0;
    (void)length;
    remote = malloc(sizeof(*remote));
    if (!remote) {
        free(contents);
        daemon_error(error, error_size, "out of memory");
        return 0;
    }
    ok = ss_parse_manifest(contents, address, daemon->config.port, remote,
                           error, error_size);
    free(contents);
    if (ok) {
        pthread_mutex_lock(&daemon->remote_mutex);
        daemon->remotes[0] = *remote;
        daemon->remote_sources[0] = 0;
        daemon->remote_count = 1;
        daemon->remote_revision++;
        pthread_mutex_unlock(&daemon->remote_mutex);
    }
    free(remote);
    return ok;
}

static int initialize_remote_discovery(SSDaemon *daemon, char *error,
                                       size_t error_size)
{
    int thread_error;

    if (pthread_mutex_init(&daemon->remote_mutex, NULL) != 0) {
        daemon_error(error, error_size, "cannot initialize discovery mutex");
        return 0;
    }
    if (pthread_cond_init(&daemon->manifest_condition, NULL) != 0) {
        pthread_mutex_destroy(&daemon->remote_mutex);
        daemon_error(error, error_size,
                     "cannot initialize discovery condition variable");
        return 0;
    }
    daemon->remote_sync_initialized = 1;
    if (daemon->no_network)
        return seed_test_remote_cache(daemon, error, error_size);
    daemon->avahi_poll = avahi_simple_poll_new();
    if (!daemon->avahi_poll) {
        daemon_error(error, error_size, "cannot create Avahi poll loop");
        return 0;
    }
    if (!create_avahi_client(daemon)) {
        daemon_error(error, error_size, "cannot connect native Avahi client");
        return 0;
    }
    thread_error = pthread_create(&daemon->manifest_thread, NULL,
                                  manifest_worker_main, daemon);
    if (thread_error != 0) {
        daemon_error(error, error_size, "cannot start manifest worker: %s",
                     strerror(thread_error));
        return 0;
    }
    daemon->manifest_thread_started = 1;
    thread_error = pthread_create(&daemon->avahi_thread, NULL,
                                  avahi_worker_main, daemon);
    if (thread_error != 0) {
        daemon_error(error, error_size, "cannot start Avahi worker: %s",
                     strerror(thread_error));
        return 0;
    }
    daemon->avahi_thread_started = 1;
    return 1;
}

static void stop_remote_discovery(SSDaemon *daemon)
{
    if (!daemon->remote_sync_initialized)
        return;
    pthread_mutex_lock(&daemon->remote_mutex);
    daemon->discovery_stopping = 1;
    pthread_cond_broadcast(&daemon->manifest_condition);
    pthread_mutex_unlock(&daemon->remote_mutex);
    if (daemon->avahi_poll)
        avahi_simple_poll_wakeup(daemon->avahi_poll);
    if (daemon->avahi_thread_started) {
        pthread_join(daemon->avahi_thread, NULL);
        daemon->avahi_thread_started = 0;
    }
    if (daemon->manifest_thread_started) {
        pthread_join(daemon->manifest_thread, NULL);
        daemon->manifest_thread_started = 0;
    }
    if (daemon->avahi_browser) {
        avahi_service_browser_free(daemon->avahi_browser);
        daemon->avahi_browser = NULL;
    }
    if (daemon->avahi_client) {
        avahi_client_free(daemon->avahi_client);
        daemon->avahi_client = NULL;
        memset(daemon->resolver_contexts, 0,
               sizeof(daemon->resolver_contexts));
    }
    if (daemon->avahi_poll) {
        avahi_simple_poll_free(daemon->avahi_poll);
        daemon->avahi_poll = NULL;
    }
    pthread_cond_destroy(&daemon->manifest_condition);
    pthread_mutex_destroy(&daemon->remote_mutex);
    daemon->remote_sync_initialized = 0;
}

static SSRemoteServer *copy_cached_remote(SSDaemon *daemon, const char *name)
{
    SSRemoteServer *copy = malloc(sizeof(*copy));
    size_t index;

    if (!copy)
        return NULL;
    pthread_mutex_lock(&daemon->remote_mutex);
    index = find_remote_index_locked(daemon, name);
    if (index != SIZE_MAX)
        *copy = daemon->remotes[index];
    pthread_mutex_unlock(&daemon->remote_mutex);
    if (index == SIZE_MAX) {
        free(copy);
        return NULL;
    }
    for (size_t mount_index = 0;
         mount_index < daemon->mounts.mount_count; mount_index++) {
        const SSClientMount *mount = &daemon->mounts.mounts[mount_index];

        if (strcmp(mount->server, copy->name) != 0)
            continue;
        if (!copy->hostname[0] && mount->hostname[0])
            ss_copy_string(copy->hostname, sizeof(copy->hostname),
                           mount->hostname);
        if (!copy->address[0] && mount->lan_address[0])
            ss_copy_string(copy->address, sizeof(copy->address),
                           mount->lan_address);
        if (!copy->tailscale_name[0] && mount->tailscale_name[0])
            ss_copy_string(copy->tailscale_name,
                           sizeof(copy->tailscale_name),
                           mount->tailscale_name);
        if (!copy->tailscale_address[0] && mount->tailscale_address[0])
            ss_copy_string(copy->tailscale_address,
                           sizeof(copy->tailscale_address),
                           mount->tailscale_address);
        if (copy->port == 0 && mount->port > 0)
            copy->port = mount->port;
    }
    return copy;
}

static void request_remote_refresh(SSDaemon *daemon, const char *name)
{
    int wake = 0;

    if (!daemon->remote_sync_initialized || daemon->no_network)
        return;
    pthread_mutex_lock(&daemon->remote_mutex);
    for (size_t index = 0; index < daemon->service_count; index++) {
        SSDiscoveredService *service = &daemon->services[index];

        if (strcmp(service->server_name, name) == 0) {
            service->resolve_requested = 1;
            service->retry_at_ms = 0;
            wake = 1;
        }
    }
    if (!wake) {
        for (size_t index = 0; index < daemon->service_count; index++) {
            daemon->services[index].resolve_requested = 1;
            daemon->services[index].retry_at_ms = 0;
        }
        wake = daemon->service_count > 0;
    }
    pthread_mutex_unlock(&daemon->remote_mutex);
    if (wake && daemon->avahi_poll)
        avahi_simple_poll_wakeup(daemon->avahi_poll);
}

static void invalidate_remote_and_refresh(SSDaemon *daemon, const char *name)
{
    int wake = 0;
    size_t remote_index;

    if (!daemon->remote_sync_initialized)
        return;
    pthread_mutex_lock(&daemon->remote_mutex);
    remote_index = find_remote_index_locked(daemon, name);
    if (remote_index != SIZE_MAX)
        remove_remote_index_locked(daemon, remote_index);
    for (size_t index = 0; index < daemon->service_count; index++) {
        SSDiscoveredService *service = &daemon->services[index];

        if (strcmp(service->server_name, name) == 0) {
            service->generation = ++daemon->next_service_generation;
            service->resolve_requested = 1;
            service->resolving = 0;
            service->manifest_pending = 0;
            service->retry_at_ms = 0;
            wake = 1;
        }
    }
    pthread_mutex_unlock(&daemon->remote_mutex);
    if (wake && daemon->avahi_poll)
        avahi_simple_poll_wakeup(daemon->avahi_poll);
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

static SSRemoteServer *copy_remembered_remote(SSDaemon *daemon, uid_t uid,
                                               const char *server_name,
                                               const char *share_name)
{
    SSClientMount *mount =
        find_client_mount(daemon, uid, server_name, share_name);
    SSRemoteServer *server;
    SSRemoteShare *share;

    if (!mount || !mount->export_path[0] ||
        (!mount->lan_address[0] && !mount->tailscale_address[0]))
        return NULL;
    server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    if (!ss_copy_string(server->name, sizeof(server->name), mount->server) ||
        !ss_copy_string(server->hostname, sizeof(server->hostname),
                        mount->hostname) ||
        !ss_copy_string(server->address, sizeof(server->address),
                        mount->lan_address) ||
        !ss_copy_string(server->tailscale_name,
                        sizeof(server->tailscale_name),
                        mount->tailscale_name) ||
        !ss_copy_string(server->tailscale_address,
                        sizeof(server->tailscale_address),
                        mount->tailscale_address)) {
        free(server);
        return NULL;
    }
    server->port = mount->port ? mount->port : SS_DEFAULT_PORT;
    server->reachable = 1;
    server->share_count = 1;
    share = &server->shares[0];
    ss_copy_string(share->name, sizeof(share->name), mount->share);
    ss_copy_string(share->export_path, sizeof(share->export_path),
                   mount->export_path);
    ss_copy_string(share->filesystem_id, sizeof(share->filesystem_id),
                   mount->filesystem_id);
    share->access = mount->access;
    return server;
}

static void refresh_remote_tailscale_address(SSDaemon *daemon,
                                              SSRemoteServer *server)
{
    char address[64];
    char name[256];

    if (resolve_peer_tailscale_address(daemon, server, address,
                                       sizeof(address), name,
                                       sizeof(name))) {
        ss_copy_string(server->tailscale_address,
                       sizeof(server->tailscale_address), address);
        if (name[0])
            ss_copy_string(server->tailscale_name,
                           sizeof(server->tailscale_name), name);
    }
}

static void update_mount_metadata(SSClientMount *mount,
                                  const SSRemoteServer *server,
                                  const SSRemoteShare *share)
{
    if (server->hostname[0])
        ss_copy_string(mount->hostname, sizeof(mount->hostname),
                       server->hostname);
    if (server->address[0])
        ss_copy_string(mount->lan_address, sizeof(mount->lan_address),
                       server->address);
    if (server->tailscale_name[0])
        ss_copy_string(mount->tailscale_name,
                       sizeof(mount->tailscale_name),
                       server->tailscale_name);
    if (server->tailscale_address[0])
        ss_copy_string(mount->tailscale_address,
                       sizeof(mount->tailscale_address),
                       server->tailscale_address);
    if (server->port > 0)
        mount->port = server->port;
    if (share) {
        ss_copy_string(mount->export_path, sizeof(mount->export_path),
                       share->export_path);
        mount->access = share->access;
    }
}

static void refresh_remembered_peer_metadata(SSDaemon *daemon, int force)
{
    time_t now = time(NULL);
    char error[512];
    int changed = 0;

    if (!force && daemon->last_tailscale_peer_refresh != 0 &&
        now - daemon->last_tailscale_peer_refresh <
            SS_TAILSCALE_PEER_REFRESH_SECONDS)
        return;
    for (size_t index = 0; index < daemon->mounts.mount_count; index++) {
        SSClientMount *mount = &daemon->mounts.mounts[index];
        SSRemoteServer *server = copy_cached_remote(daemon, mount->server);
        SSRemoteShare *share = find_remote_share(server, mount->share);
        SSClientMount before = *mount;
        int first_for_server = 1;

        if (server && share &&
            strcmp(share->filesystem_id, mount->filesystem_id) == 0)
            update_mount_metadata(mount, server, share);
        for (size_t previous = 0; previous < index; previous++) {
            if (strcmp(daemon->mounts.mounts[previous].server,
                       mount->server) == 0) {
                first_for_server = 0;
                break;
            }
        }
        if (daemon->tailscale_active && first_for_server) {
            SSRemoteServer candidate;
            char tailscale_address[64];
            char tailscale_name[256];

            if (server) {
                candidate = *server;
            } else {
                memset(&candidate, 0, sizeof(candidate));
                ss_copy_string(candidate.name, sizeof(candidate.name),
                               mount->server);
                ss_copy_string(candidate.hostname,
                               sizeof(candidate.hostname), mount->hostname);
                ss_copy_string(candidate.tailscale_name,
                               sizeof(candidate.tailscale_name),
                               mount->tailscale_name);
            }
            if (resolve_peer_tailscale_address(
                    daemon, &candidate, tailscale_address,
                    sizeof(tailscale_address), tailscale_name,
                    sizeof(tailscale_name))) {
                for (size_t peer = index;
                     peer < daemon->mounts.mount_count; peer++) {
                    if (strcmp(daemon->mounts.mounts[peer].server,
                               mount->server) == 0 &&
                        strcmp(daemon->mounts.mounts[peer]
                                   .tailscale_address,
                               tailscale_address) != 0) {
                        ss_copy_string(
                            daemon->mounts.mounts[peer].tailscale_address,
                            sizeof(daemon->mounts.mounts[peer]
                                       .tailscale_address),
                            tailscale_address);
                        changed = 1;
                    }
                    if (strcmp(daemon->mounts.mounts[peer].server,
                               mount->server) == 0 &&
                        tailscale_name[0] &&
                        strcmp(daemon->mounts.mounts[peer].tailscale_name,
                               tailscale_name) != 0) {
                        ss_copy_string(
                            daemon->mounts.mounts[peer].tailscale_name,
                            sizeof(daemon->mounts.mounts[peer]
                                       .tailscale_name),
                            tailscale_name);
                        changed = 1;
                    }
                }
            }
        }
        if (memcmp(&before, mount, sizeof(before)) != 0)
            changed = 1;
        free(server);
    }
    if (changed &&
        !save_remembered_mounts(daemon, error, sizeof(error)))
        fprintf(stderr,
                "simpleserved: cannot save refreshed peer metadata: %s\n",
                error);
    daemon->last_tailscale_peer_refresh = now;
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

    (void)daemon;
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
                              int remember, SSRoute route,
                              const char *address)
{
    record->uid = uid;
    record->gid = gid;
    ss_copy_string(record->server, sizeof(record->server), server->name);
    ss_copy_string(record->share, sizeof(record->share), share->name);
    update_mount_metadata(record, server, share);
    ss_copy_string(record->address, sizeof(record->address), address);
    ss_copy_string(record->filesystem_id, sizeof(record->filesystem_id),
                   share->filesystem_id);
    ss_copy_string(record->target, sizeof(record->target), target);
    record->access = share->access;
    record->remembered = remember || record->remembered;
    record->mounted = 1;
    record->available = 1;
    record->misses = 0;
    record->route = route;
}

static SSRoute mounted_source_route(const SSRemoteServer *server,
                                    const SSRemoteShare *share,
                                    const char *source, char *address,
                                    size_t address_size)
{
    char expected[PATH_MAX];

    if (server->address[0] &&
        snprintf(expected, sizeof(expected), "%s:%s", server->address,
                 share->export_path) < (int)sizeof(expected) &&
        strcmp(source, expected) == 0) {
        ss_copy_string(address, address_size, server->address);
        return SS_ROUTE_LAN;
    }
    if (server->tailscale_address[0] &&
        snprintf(expected, sizeof(expected), "%s:%s",
                 server->tailscale_address, share->export_path) <
            (int)sizeof(expected) &&
        strcmp(source, expected) == 0) {
        ss_copy_string(address, address_size, server->tailscale_address);
        return SS_ROUTE_TAILSCALE;
    }
    address[0] = '\0';
    return SS_ROUTE_NONE;
}

static SSRoute remembered_source_route(const SSRemoteShare *share,
                                       const char *source, char *address,
                                       size_t address_size)
{
    const char *colon;
    struct in_addr parsed;
    size_t length;

    if (!share || !source || !(colon = strchr(source, ':')) ||
        strcmp(colon + 1, share->export_path) != 0)
        return SS_ROUTE_NONE;
    length = (size_t)(colon - source);
    if (length == 0 || length >= address_size || length >= 64)
        return SS_ROUTE_NONE;
    memcpy(address, source, length);
    address[length] = '\0';
    if (inet_pton(AF_INET, address, &parsed) != 1) {
        address[0] = '\0';
        return SS_ROUTE_NONE;
    }
    return ss_tailscale_ipv4_address(address) ?
        SS_ROUTE_TAILSCALE : SS_ROUTE_LAN;
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
    char selected_address[64] = "";
    char original_error[512] = "";
    char rollback_error[512];
    SSMountInfo existing;
    SSCommand command;
    SSRoute selected_route = SS_ROUTE_NONE;
    int lan_usable;
    int tailscale_usable = 0;
    int tailscale_checked = 0;
    int attempted = 0;
    int mounted = 0;
    int had_record;

    if (!server || !share ||
        !prepare_mount_target(uid, gid, server->name, share->name, target,
                              sizeof(target), error, error_size))
        return 0;
    refresh_remote_tailscale_address(daemon, server);
    record = find_client_mount(daemon, uid, server->name, share->name);
    if (!record && daemon->mounts.mount_count >= SS_MAX_MOUNTS) {
        daemon_error(error, error_size, "too many managed mounts");
        return 0;
    }
    had_record = record != NULL;
    if (record) {
        previous_record = *record;
        update_mount_metadata(record, server, share);
        if (record->remembered &&
            memcmp(&previous_record, record, sizeof(previous_record)) != 0 &&
            !save_remembered_mounts(daemon, error, error_size)) {
            *record = previous_record;
            return 0;
        }
        previous_record = *record;
    }
    if (mounted_nfs_at(daemon, target, &existing)) {
        int prefer_lan;

        selected_route = mounted_source_route(
            server, share, existing.source, selected_address,
            sizeof(selected_address));
        if (selected_route == SS_ROUTE_NONE && record && record->remembered)
            selected_route = remembered_source_route(
                share, existing.source, selected_address,
                sizeof(selected_address));
        if (record && record->route != SS_ROUTE_NONE &&
            strcmp(record->address, selected_address) == 0)
            selected_route = record->route;
        if (selected_route == SS_ROUTE_NONE) {
            daemon_error(error, error_size,
                         "refusing to adopt unexpected NFS mount %s at %s",
                         existing.source, target);
            return 0;
        }
        prefer_lan = selected_route == SS_ROUTE_TAILSCALE &&
                     server->address[0] &&
                     route_reachable(daemon, SS_ROUTE_LAN, server->address);
        if (!prefer_lan &&
            route_reachable(daemon, selected_route, selected_address)) {
            if (!record) {
                record = &daemon->mounts.mounts[
                    daemon->mounts.mount_count++];
                memset(record, 0, sizeof(*record));
            }
            fill_mount_record(record, uid, gid, server, share, target,
                              remember, selected_route, selected_address);
            if (record->remembered &&
                !save_remembered_mounts(daemon, error, error_size)) {
                if (had_record)
                    *record = previous_record;
                else
                    daemon->mounts.mount_count--;
                return 0;
            }
            return ss_buffer_appendf(
                       message, "%s:%s is already mounted at %s\n",
                       server->name, share->name, target) &&
                   ss_buffer_appendf(message, "Route: %s (%s)\n",
                                     ss_route_name(selected_route),
                                     selected_address);
        }
        if (!ss_build_unmount_command(daemon->platform, target, 0, &command,
                                      error, error_size) ||
            !run_command(daemon, &command, 5000, error, error_size)) {
            ss_copy_string(original_error, sizeof(original_error), error);
            daemon_error(error, error_size,
                         "managed %s mount at %s could not be released safely for route selection: %s",
                         ss_route_name(selected_route), target,
                         original_error[0] ? original_error :
                                             "unmount failed");
            return 0;
        }
        if (record)
            record->mounted = 0;
    }
    if (!daemon->test_mode && !directory_empty(target)) {
        daemon_error(error, error_size,
                     "refusing to hide files in non-empty mountpoint %s", target);
        return 0;
    }
    if (!ensure_nfs_client(daemon, error, error_size))
        return 0;
    lan_usable = server->address[0] &&
                 route_reachable(daemon, SS_ROUTE_LAN, server->address);
    for (int route_attempt = 0; route_attempt < 2; route_attempt++) {
        if (!lan_usable && !tailscale_checked) {
            tailscale_checked = 1;
            tailscale_usable = daemon->tailscale_active &&
                               server->tailscale_address[0] &&
                               route_reachable(daemon, SS_ROUTE_TAILSCALE,
                                               server->tailscale_address);
        }
        if (!ss_choose_route(server->address, lan_usable,
                             server->tailscale_address, tailscale_usable,
                             &selected_route, selected_address,
                             sizeof(selected_address)))
            break;
        attempted = 1;
        if (!ss_build_mount_command(daemon->platform, selected_address,
                                    share->export_path, target, share->access,
                                    &command, error, error_size))
            return 0;
        if (run_command(daemon, &command, 30000, error, error_size)) {
            if (daemon->test_mode ||
                mounted_nfs_at(daemon, target, &existing)) {
                mounted = 1;
                break;
            }
            daemon_error(error, error_size,
                         "mount command returned success but %s is not an NFS mount",
                         target);
        }
        ss_copy_string(original_error, sizeof(original_error), error);
        if (!daemon->test_mode &&
            mounted_nfs_at(daemon, target, &existing) &&
            !rollback_mount(daemon, target, rollback_error,
                            sizeof(rollback_error))) {
            daemon_error(error, error_size,
                         "%s; partial mount rollback failed: %s",
                         original_error, rollback_error);
            return 0;
        }
        ss_copy_string(error, error_size, original_error);
        if (selected_route == SS_ROUTE_LAN)
            lan_usable = 0;
        else
            tailscale_usable = 0;
    }
    if (!mounted) {
        if (!attempted)
            daemon_error(error, error_size,
                         "%s:%s is unavailable over LAN and Tailscale",
                         server->name, share->name);
        else
            daemon_error(error, error_size,
                         "cannot mount %s:%s over any usable route: %s",
                         server->name, share->name,
                         original_error[0] ? original_error : "mount failed");
        return 0;
    }
    if (!record) {
        record = &daemon->mounts.mounts[daemon->mounts.mount_count++];
        memset(record, 0, sizeof(*record));
    }
    fill_mount_record(record, uid, gid, server, share, target, remember,
                      selected_route, selected_address);
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
                             record->remembered ? " (remembered)" : "") &&
           ss_buffer_appendf(message, "Route: %s (%s)\n",
                             ss_route_name(selected_route), selected_address);
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
        !sync_mount_persistence(daemon, error, error_size) ||
        !sync_exports(daemon, error, error_size) ||
        !sync_samba(daemon, error, error_size) ||
        !start_publisher(daemon, error, error_size)) {
        ss_copy_string(original_error, sizeof(original_error), error);
        daemon->config = *old_config;
        if (!ss_save_server_config(daemon->config_path, &daemon->config,
                                   rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: configuration rollback failed: %s\n",
                    rollback_error);
        (void)refresh_local_shares(daemon, &changed);
        if (!sync_mount_persistence(daemon, rollback_error,
                                    sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: mount persistence rollback failed: %s\n",
                    rollback_error);
        if (!sync_exports(daemon, rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: share rollback failed: %s\n",
                    rollback_error);
        if (!sync_samba(daemon, rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: Samba rollback failed: %s\n",
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
        !sync_mount_persistence(daemon, error, error_size) ||
        !sync_exports(daemon, error, error_size) ||
        !sync_samba(daemon, error, error_size) ||
        !start_publisher(daemon, error, error_size)) {
        ss_copy_string(original_error, sizeof(original_error), error);
        daemon->config = *old_config;
        if (!ss_save_server_config(daemon->config_path, &daemon->config,
                                   rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: configuration rollback failed: %s\n",
                    rollback_error);
        (void)refresh_local_shares(daemon, &changed);
        if (!sync_mount_persistence(daemon, rollback_error,
                                    sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: mount persistence rollback failed: %s\n",
                    rollback_error);
        if (!sync_exports(daemon, rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: unshare rollback failed: %s\n",
                    rollback_error);
        if (!sync_samba(daemon, rollback_error, sizeof(rollback_error)))
            fprintf(stderr, "simpleserved: Samba rollback failed: %s\n",
                    rollback_error);
        ss_copy_string(error, error_size, original_error);
        free(old_config);
        return 0;
    }
    free(old_config);
    return ss_buffer_appendf(message, "Stopped sharing %s\n", name);
}

static int has_remembered_peers(const SSDaemon *daemon)
{
    for (size_t index = 0; index < daemon->mounts.mount_count; index++) {
        if (daemon->mounts.mounts[index].remembered)
            return 1;
    }
    return 0;
}

static int append_role_summary(const SSDaemon *daemon, SSBuffer *message)
{
    int server = daemon->config.share_count > 0;
    int client = has_remembered_peers(daemon);

    if (server && client)
        return ss_buffer_append(message, "Roles: server + client\n");
    if (server)
        return ss_buffer_append(message, "Roles: server\n");
    if (client)
        return ss_buffer_append(message, "Roles: client\n");
    return ss_buffer_append(message, "Roles: idle\n");
}

static int append_tailscale_status(const SSDaemon *daemon,
                                   SSBuffer *message)
{
    switch (daemon->tailscale_state) {
    case SS_TAILSCALE_ACTIVE:
        return ss_buffer_appendf(message, "Tailscale: active (%s)\n",
                                 daemon->tailscale_address);
    case SS_TAILSCALE_DAEMON_STOPPED:
        return ss_buffer_append(message,
                                "Tailscale: installed, daemon unavailable\n");
    case SS_TAILSCALE_NEEDS_LOGIN:
        return ss_buffer_append(message,
                                "Tailscale: running, not authenticated\n");
    case SS_TAILSCALE_INACTIVE:
        return ss_buffer_append(message,
                                "Tailscale: installed, inactive\n");
    case SS_TAILSCALE_MISSING:
        return ss_buffer_append(message, "Tailscale: unavailable\n");
    }
    return 0;
}

static int format_configuration(SSDaemon *daemon, SSBuffer *message)
{
    if (!ss_buffer_append(message, "SimpleServe configured.\n") ||
        !append_role_summary(daemon, message) ||
        !ss_buffer_append(message, "LAN transport: enabled\n"))
        return 0;
    return append_tailscale_status(daemon, message);
}

static int format_discovery(SSDaemon *daemon, uid_t uid, SSBuffer *message)
{
    int ok = 0;

    pthread_mutex_lock(&daemon->remote_mutex);
    if (!ss_buffer_append(message, "AVAILABLE SHARES\n\n"))
        goto done;
    if (daemon->remote_count == 0) {
        if (!ss_buffer_append(
                message, "No SimpleServe shares found on this LAN.\n"))
            goto done;
    }
    for (size_t server_index = 0; server_index < daemon->remote_count;
         server_index++) {
        const SSRemoteServer *server = &daemon->remotes[server_index];

        if (!ss_buffer_appendf(message, "%s\n", server->name))
            goto done;
        if (server->share_count == 0) {
            if (!ss_buffer_append(message, "  (no active shares)\n"))
                goto done;
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
                goto done;
        }
        if (!ss_buffer_append(message, "\n"))
            goto done;
    }
    {
        size_t remembered = 0;

        for (size_t mount_index = 0;
             mount_index < daemon->mounts.mount_count; mount_index++) {
            const SSClientMount *mount =
                &daemon->mounts.mounts[mount_index];
            int discovered = 0;

            if (!mount->remembered || (uid != 0 && mount->uid != uid))
                continue;
            for (size_t server_index = 0;
                 server_index < daemon->remote_count && !discovered;
                 server_index++) {
                const SSRemoteServer *server =
                    &daemon->remotes[server_index];

                if (strcmp(server->name, mount->server) != 0)
                    continue;
                for (size_t share_index = 0;
                     share_index < server->share_count; share_index++) {
                    if (strcmp(server->shares[share_index].name,
                               mount->share) == 0) {
                        discovered = 1;
                        break;
                    }
                }
            }
            if (discovered)
                continue;
            if (remembered++ == 0 &&
                !ss_buffer_append(message, "\nREMEMBERED SHARES\n\n"))
                goto done;
            if (!ss_buffer_appendf(
                    message, "  %s:%s  %s%s%s\n", mount->server,
                    mount->share,
                    mount->lan_address[0] ? "LAN" : "",
                    mount->lan_address[0] && mount->tailscale_name[0] ?
                        " + " : "",
                    mount->tailscale_name[0] ? "Tailscale" :
                    (!mount->lan_address[0] ? "awaiting discovery" : "")))
                goto done;
        }
    }
    ok = 1;

done:
    pthread_mutex_unlock(&daemon->remote_mutex);
    return ok;
}

static int format_status(SSDaemon *daemon, uid_t uid, SSBuffer *message)
{
    if (!ss_buffer_appendf(message, "SIMPLESERVE STATUS\n\nServer: %s\n",
                           daemon->config.server_name))
        return 0;
    if (!append_role_summary(daemon, message))
        return 0;
    if (!append_tailscale_status(daemon, message))
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
            if (!ss_buffer_appendf(message, "  %s:%s -> %s  %s%s%s",
                                   mount->server, mount->share, target,
                                   mount->mounted ? "mounted" : "not mounted",
                                   mount->available ? "" :
                                                      ", server unavailable",
                                   mount->remembered ? ", remembered" : ""))
                return 0;
            if (mount->mounted && mount->route != SS_ROUTE_NONE &&
                mount->address[0]) {
                if (!ss_buffer_appendf(message, ", route: %s, address: %s",
                                       ss_route_name(mount->route),
                                       mount->address))
                    return 0;
            }
            if (!ss_buffer_append(message, "\n"))
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
    if ((strcmp(fields[0], "SHARE") == 0 ||
         strcmp(fields[0], "DISCOVER") == 0 ||
         strcmp(fields[0], "MOUNT") == 0 ||
         strcmp(fields[0], "STATUS") == 0) &&
        count > 0) {
        int tailscale_changed = 0;
        int force = strcmp(fields[0], "DISCOVER") == 0 ||
                    strcmp(fields[0], "MOUNT") == 0;

        refresh_tailscale_state(daemon, force, &tailscale_changed);
        if (tailscale_changed && active_share_count(daemon) > 0 &&
            !sync_exports(daemon, error, error_size))
            fprintf(stderr,
                    "simpleserved: optional transport export refresh failed: %s\n",
                    error);
        if (strcmp(fields[0], "DISCOVER") == 0)
            refresh_remembered_peer_metadata(daemon, 1);
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
        if (!format_discovery(daemon, uid, message)) {
            daemon_error(error, error_size, "out of memory");
            return 0;
        }
        return 1;
    }
    if (strcmp(fields[0], "CONFIGURE") == 0 && count == 1) {
        int tailscale_changed = 0;
        int shares_changed = 0;

        refresh_tailscale_state(daemon, 1, &tailscale_changed);
        (void)tailscale_changed;
        request_remote_refresh(daemon, "");
        refresh_remembered_peer_metadata(daemon, 1);
        if (!refresh_local_shares(daemon, &shares_changed) ||
            !sync_exports(daemon, error, error_size) ||
            (shares_changed &&
             (!sync_samba(daemon, error, error_size) ||
              !start_publisher(daemon, error, error_size))))
            return 0;
        if (!format_configuration(daemon, message)) {
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
        int mounted;

        server = copy_cached_remote(daemon, fields[1]);
        share = find_remote_share(server, fields[2]);
        if (!server || !share) {
            free(server);
            server = copy_remembered_remote(daemon, uid, fields[1], fields[2]);
            share = find_remote_share(server, fields[2]);
        }
        if (!server || !share) {
            request_remote_refresh(daemon, fields[1]);
            daemon_error(error, error_size, "share %s:%s is not available",
                         fields[1], fields[2]);
            free(server);
            return 0;
        }
        mounted = perform_mount(daemon, uid, gid, server, share,
                                strcmp(fields[3], "remember") == 0, message,
                                error, error_size);
        if (!mounted)
            invalidate_remote_and_refresh(daemon, fields[1]);
        free(server);
        return mounted;
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
             !sync_samba(daemon, error, error_size) ||
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

    if (daemon->mounts.mount_count == 0)
        return;
    for (size_t index = 0; index < daemon->mounts.mount_count;) {
        SSClientMount *mount = &daemon->mounts.mounts[index];
        SSRemoteServer *server = copy_cached_remote(daemon, mount->server);
        SSRemoteShare *share = find_remote_share(server, mount->share);
        char target[PATH_MAX];
        SSMountInfo mounted;
        int is_mounted = 0;
        int metadata_valid;

        if (prepare_mount_target(mount->uid, mount->gid, mount->server,
                                 mount->share, target, sizeof(target), error,
                                 sizeof(error))) {
            ss_copy_string(mount->target, sizeof(mount->target), target);
            is_mounted = mounted_nfs_at(daemon, target, &mounted);
        }
        mount->mounted = is_mounted;
        if (!server) {
            server = copy_remembered_remote(
                daemon, mount->uid, mount->server, mount->share);
            share = find_remote_share(server, mount->share);
        }
        metadata_valid = share &&
                         strcmp(share->filesystem_id,
                                mount->filesystem_id) == 0;
        if (metadata_valid)
            update_mount_metadata(mount, server, share);
        else if (server)
            request_remote_refresh(daemon, mount->server);
        if (is_mounted && metadata_valid) {
            char mounted_address[64];
            SSRoute route = mounted_source_route(
                server, share, mounted.source, mounted_address,
                sizeof(mounted_address));

            if (route == SS_ROUTE_NONE && mount->remembered)
                route = remembered_source_route(
                    share, mounted.source, mounted_address,
                    sizeof(mounted_address));
            if (mount->route != SS_ROUTE_NONE &&
                strcmp(mount->address, mounted_address) == 0)
                route = mount->route;
            if (route == SS_ROUTE_NONE) {
                mount->available = 0;
                if (mount->misses < UINT_MAX)
                    mount->misses++;
                fprintf(stderr,
                        "simpleserved: unexpected NFS source at %s; leaving it untouched\n",
                        target);
                free(server);
                index++;
                continue;
            }
            mount->route = route;
            ss_copy_string(mount->address, sizeof(mount->address),
                           mounted_address);
            if (route_reachable(daemon, route, mounted_address)) {
                mount->available = 1;
                mount->misses = 0;
                free(server);
                index++;
                continue;
            }
        }
        mount->available = 0;
        if (mount->misses < UINT_MAX)
            mount->misses++;
        if (is_mounted && mount->misses >= 3) {
            SSCommand command;

            if (ss_build_unmount_command(daemon->platform, target, 0,
                                         &command, error, sizeof(error)) &&
                run_command(daemon, &command, 5000, error, sizeof(error))) {
                mount->mounted = 0;
                is_mounted = 0;
                if (!mount->remembered) {
                    free(server);
                    remove_mount_record(daemon, index);
                    continue;
                }
            }
        }
        if (!is_mounted && metadata_valid && mount->remembered) {
            SSBuffer ignored;

            ss_buffer_init(&ignored);
            if (!perform_mount(daemon, mount->uid, mount->gid, server, share,
                               1, &ignored, error, sizeof(error))) {
                fprintf(stderr,
                        "simpleserved: reconnect %s:%s failed: %s\n",
                        mount->server, mount->share, error);
                invalidate_remote_and_refresh(daemon, mount->server);
            }
            ss_buffer_free(&ignored);
        }
        free(server);
        index++;
    }
    (void)save_remembered_mounts(daemon, error, sizeof(error));
    daemon->last_mount_reconcile = time(NULL);
    pthread_mutex_lock(&daemon->remote_mutex);
    daemon->reconciled_remote_revision = daemon->remote_revision;
    pthread_mutex_unlock(&daemon->remote_mutex);
}

static void cleanup_daemon(SSDaemon *daemon)
{
    stop_remote_discovery(daemon);
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
         !getenv("SIMPLESERVE_STATE") || !getenv("SIMPLESERVE_EXPORTS") ||
         !getenv("SIMPLESERVE_FSTAB") ||
         (daemon->platform == SS_PLATFORM_LINUX &&
          (!getenv("SIMPLESERVE_SMB_CONF") ||
           !getenv("SIMPLESERVE_SAMBA"))))) {
        daemon_error(error, error_size,
                     "test mode requires socket, config, state, exports, fstab, and Linux Samba overrides");
        return 0;
    }
    if (!ss_copy_string(daemon->socket_path, sizeof(daemon->socket_path),
                        ss_default_socket_path(daemon->platform)) ||
        !ss_copy_string(daemon->config_path, sizeof(daemon->config_path),
                        ss_default_config_path()) ||
        !ss_copy_string(daemon->state_path, sizeof(daemon->state_path),
                        ss_default_state_path(daemon->platform)) ||
        !ss_copy_string(daemon->exports_path, sizeof(daemon->exports_path),
                        ss_default_exports_path(daemon->platform)) ||
        !ss_copy_string(daemon->fstab_path, sizeof(daemon->fstab_path),
                        ss_default_fstab_path()) ||
        !ss_copy_string(daemon->smb_conf_path, sizeof(daemon->smb_conf_path),
                        ss_default_smb_conf_path()) ||
        !ss_copy_string(daemon->samba_path, sizeof(daemon->samba_path),
                        ss_default_samba_path())) {
        daemon_error(error, error_size, "SimpleServe system path is too long");
        return 0;
    }
    if (!ss_load_server_config(daemon->config_path, &daemon->config,
                               error, error_size) ||
        !ss_load_mount_config(daemon->state_path, &daemon->mounts,
                              error, error_size))
        return 0;
    {
        int tailscale_changed = 0;

        refresh_tailscale_state(daemon, 1, &tailscale_changed);
    }
    if (!refresh_local_shares(daemon, &changed) ||
        !sync_mount_persistence(daemon, error, error_size) ||
        !sync_exports(daemon, error, error_size) ||
        !sync_samba(daemon, error, error_size) ||
        !open_manifest_socket(daemon, error, error_size) ||
        !open_control_socket(daemon, error, error_size) ||
        (!daemon->no_network &&
         !start_avahi_daemon_once(daemon, error, error_size)) ||
        !initialize_remote_discovery(daemon, error, error_size) ||
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
                    !sync_samba(daemon, error, sizeof(error)) ||
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
        if (now - daemon->last_tailscale_refresh >=
            SS_TAILSCALE_REFRESH_SECONDS) {
            int tailscale_changed = 0;
            char error[512];

            refresh_tailscale_state(daemon, 0, &tailscale_changed);
            if (tailscale_changed && active_share_count(daemon) > 0 &&
                !sync_exports(daemon, error, sizeof(error)))
                fprintf(stderr,
                        "simpleserved: Tailscale export refresh failed: %s\n",
                        error);
        }
        if (daemon->mounts.mount_count > 0) {
            int cache_changed;

            refresh_remembered_peer_metadata(daemon, 0);

            pthread_mutex_lock(&daemon->remote_mutex);
            cache_changed = daemon->remote_revision !=
                            daemon->reconciled_remote_revision;
            pthread_mutex_unlock(&daemon->remote_mutex);
            if (cache_changed || now - daemon->last_mount_reconcile >= 15)
                reconcile_mounts(daemon);
        }
    }
}

int main(int argc, char **argv)
{
    SSDaemon *daemon;
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
    /* The fixed discovery tables are larger than typical service stacks. */
    daemon = malloc(sizeof(*daemon));
    if (!daemon) {
        fprintf(stderr, "simpleserved: out of memory\n");
        return 1;
    }
    umask(022);
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    if (!initialize_daemon(daemon, error, sizeof(error))) {
        fprintf(stderr, "simpleserved: %s\n", error);
        cleanup_daemon(daemon);
        free(daemon);
        return 1;
    }
    fprintf(stderr, "simpleserved: ready on %s (%s)\n", daemon->socket_path,
            ss_platform_name(daemon->platform));
    daemon_loop(daemon);
    cleanup_daemon(daemon);
    free(daemon);
    return 0;
}
