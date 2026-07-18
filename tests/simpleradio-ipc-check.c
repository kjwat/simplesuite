#define main simpleradio_program_main
#include "../simpleradio.c"
#undef main

#include <assert.h>

int main(void)
{
    int sockets[2];
    char response[256];
    static const char json[] =
        "{\"data\":\"Station title\",\"request_id\":99}\n";

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int flags = fcntl(sockets[0], F_GETFL, 0);
    assert(flags >= 0);
    assert(fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
    assert(write(sockets[1], json, sizeof json - 1) ==
           (ssize_t)(sizeof json - 1));
    assert(mpv_read_response(sockets[0], response, sizeof response) > 0);
    assert(strstr(response, "Station title"));

    long long started = monotonic_millis();
    assert(mpv_read_response(sockets[0], response, sizeof response) == 0);
    assert(monotonic_millis() - started < 100);

    static const char snapshot[] =
        "{\"data\":\"Combined title\",\"request_id\":201}\n"
        "{\"data\":42.5,\"request_id\":203}\n"
        "{\"data\":false,\"request_id\":205}\n";
    char line[256];
    double position = 0.0;
    bool paused_for_cache = true;
    assert(copy_mpv_response_line(snapshot, 201, line, sizeof(line)));
    char *title = json_data_string(line);
    assert(title && strcmp(title, "Combined title") == 0);
    free(title);
    assert(copy_mpv_response_line(snapshot, 203, line, sizeof(line)));
    assert(json_data_double(line, &position) && position == 42.5);
    assert(copy_mpv_response_line(snapshot, 205, line, sizeof(line)));
    assert(json_data_bool(line, &paused_for_cache) && !paused_for_cache);

    close(sockets[0]);
    close(sockets[1]);
    return 0;
}
