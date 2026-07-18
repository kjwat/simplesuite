#define main simplepod_program_main
#include "../simplepod.c"
#undef main

#include <assert.h>

int main(void)
{
    char directory[] = "/tmp/simplepod-ipc-test.XXXXXX";
    int listener;
    struct sockaddr_un address;
    pid_t server;
    double position, duration;

    assert(mkdtemp(directory));
    snprintf(mpv_socket, sizeof mpv_socket, "%s/mpv.sock", directory);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", mpv_socket);
    assert(bind(listener, (struct sockaddr *)&address, sizeof address) == 0);
    assert(listen(listener, 1) == 0);

    server = fork();
    assert(server >= 0);
    if (server == 0) {
        char commands[512];
        int client = accept(listener, NULL, NULL);
        if (client < 0) _exit(1);
        if (read(client, commands, sizeof commands) <= 0) _exit(2);
        static const char replies[] =
            "{\"data\":12.5,\"request_id\":1,\"error\":\"success\"}\n"
            "{\"data\":3600.0,\"request_id\":2,\"error\":\"success\"}\n";
        if (write(client, replies, sizeof replies - 1) !=
            (ssize_t)(sizeof replies - 1)) _exit(3);
        close(client);
        _exit(0);
    }

    mpv_get_progress(&position, &duration);
    assert(position == 12.5);
    assert(duration == 3600.0);
    int status = 0;
    assert(waitpid(server, &status, 0) == server);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(listener);
    unlink(mpv_socket);

    int64_t started = sui_monotonic_ms();
    mpv_get_progress(&position, &duration);
    assert(position < 0 && duration < 0);
    assert(sui_monotonic_ms() - started < 100);

    char first_socket[sizeof(mpv_socket)];
    char second_socket[sizeof(mpv_socket)];
    snprintf(mpv_socket_prefix, sizeof(mpv_socket_prefix),
             "%s/player", directory);
    assert(format_mpv_socket_for_pid(first_socket, sizeof(first_socket),
                                     getpid()));
    assert(format_mpv_socket_for_pid(second_socket, sizeof(second_socket),
                                     getpid() + 1));
    assert(strcmp(first_socket, second_socket) != 0);

    assert(rmdir(directory) == 0);
    return 0;
}
