#ifndef SIMPLEPROC_H
#define SIMPLEPROC_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Check PATH in-process. Spawning `sh -c "command -v ..."` made small UI
 * actions such as copy, preview, and alarm startup pay for an avoidable
 * process launch. */
static inline int ssp_command_available(const char *name)
{
    const char *path;
    const char *entry;
    char candidate[4096];

    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;

    path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

    entry = path;
    for (;;) {
        const char *end = strchr(entry, ':');
        size_t length = end ? (size_t)(end - entry) : strlen(entry);
        int written;

        if (length == 0)
            written = snprintf(candidate, sizeof candidate, "./%s", name);
        else
            written = snprintf(candidate, sizeof candidate, "%.*s/%s",
                               (int)length, entry, name);
        if (written > 0 && (size_t)written < sizeof candidate &&
            access(candidate, X_OK) == 0)
            return 1;

        if (!end)
            break;
        entry = end + 1;
    }
    return 0;
}

static inline int64_t ssp_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static inline void ssp_manage_deferred_child(pid_t add_pid)
{
    static pid_t *pids;
    static size_t count;
    static size_t capacity;
    size_t out = 0;

    for (size_t i = 0; i < count; i++) {
        pid_t result;

        do {
            result = waitpid(pids[i], NULL, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == 0)
            pids[out++] = pids[i];
    }
    count = out;
    if (add_pid <= 0)
        return;
    if (count == capacity) {
        size_t next = capacity ? capacity * 2 : 4;
        pid_t *grown = realloc(pids, next * sizeof(*grown));

        if (!grown)
            return;
        pids = grown;
        capacity = next;
    }
    pids[count++] = add_pid;
}

/* Clipboard owners may intentionally outlive their launcher. Feed the input
 * from an already-open file, double-fork, and return once ownership startup
 * has been handed off rather than waiting for the desktop process to exit. */
static inline int ssp_detach_argv_with_input_file(const char *path,
                                                  char *const argv[])
{
    pid_t launcher = fork();
    int status = 0;

    if (launcher == 0) {
        int input = open(path, O_RDONLY);
        pid_t worker;

        if (input < 0)
            _exit(127);
        unlink(path);
        worker = fork();
        if (worker < 0)
            _exit(127);
        if (worker > 0)
            _exit(0);

        setsid();
        if (dup2(input, STDIN_FILENO) < 0)
            _exit(127);
        if (input > STDERR_FILENO)
            close(input);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (launcher < 0)
        return 0;
    while (waitpid(launcher, &status, 0) < 0) {
        if (errno != EINTR)
            return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Capture a small helper's stdout without giving it control of UI latency.
 * The limit protects editors from accidentally importing an enormous binary
 * clipboard and the deadline handles stalled selection owners. */
static inline int ssp_capture_argv(char *const argv[], char **output,
                                   size_t limit, int timeout_ms)
{
    int pipe_fds[2];
    pid_t pid;
    char *buffer = NULL;
    size_t used = 0;
    size_t capacity = 0;
    int status = 0;
    int timed_out = 0;

    if (!output || !argv || !argv[0] || limit == 0 || timeout_ms < 0)
        return 0;
    ssp_manage_deferred_child(0);
    *output = NULL;
    if (pipe(pipe_fds) != 0)
        return 0;

    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (pipe_fds[1] > STDERR_FILENO)
            close(pipe_fds[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipe_fds[1]);
    if (pid < 0) {
        close(pipe_fds[0]);
        return 0;
    }
    (void)setpgid(pid, pid);
    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0)
        timed_out = 1;

    int64_t deadline = ssp_monotonic_ms() + timeout_ms;
    while (!timed_out && used < limit) {
        if (capacity - used < 4096 && capacity < limit + 1) {
            size_t next = capacity ? capacity * 2 : 4096;
            if (next > limit + 1)
                next = limit + 1;
            char *grown = realloc(buffer, next);
            if (!grown) {
                timed_out = 1;
                break;
            }
            buffer = grown;
            capacity = next;
        }

        size_t room = capacity - used;
        if (room > limit - used)
            room = limit - used;
        ssize_t count = read(pipe_fds[0], buffer + used, room);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            timed_out = 1;
            break;
        }

        int64_t remaining = deadline - ssp_monotonic_ms();
        if (remaining <= 0) {
            timed_out = 1;
            break;
        }
        struct pollfd poll_fd = {.fd = pipe_fds[0], .events = POLLIN};
        int ready;
        do {
            ready = poll(&poll_fd, 1,
                         remaining > INT32_MAX ? INT32_MAX : (int)remaining);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0)
            timed_out = 1;
    }
    if (used == limit)
        timed_out = 1;
    close(pipe_fds[0]);

    pid_t waited;
    do {
        waited = waitpid(pid, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    while (waited == 0) {
        int64_t remaining = deadline - ssp_monotonic_ms();
        if (remaining <= 0)
            break;
        int pause_ms = remaining > 5 ? 5 : (int)remaining;
        (void)poll(NULL, 0, pause_ms);
        do {
            waited = waitpid(pid, &status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
    }
    if (waited == 0) {
        if (kill(-pid, SIGKILL) != 0)
            kill(pid, SIGKILL);
        int64_t kill_deadline = ssp_monotonic_ms() + 20;
        do {
            do {
                waited = waitpid(pid, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited != 0 || ssp_monotonic_ms() >= kill_deadline)
                break;
            (void)poll(NULL, 0, 1);
        } while (1);
        if (waited == 0)
            ssp_manage_deferred_child(pid);
    }

    if (timed_out || waited != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || used == 0) {
        free(buffer);
        return 0;
    }
    buffer[used] = '\0';
    *output = buffer;
    return 1;
}

#endif
