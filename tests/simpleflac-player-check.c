#define main simpleflac_program_main
#include "../simpleflac.c"
#undef main

#include <assert.h>

int main(void)
{
    char directory[] = "/tmp/simpleflac-player-test.XXXXXX";
    char first[sizeof(mpv_socket_path)];
    char second[sizeof(mpv_socket_path)];
    struct sockaddr_un address;
    pid_t player;
    pid_t server;
    int ready_pipe[2];
    int listener;
    double position, duration;

    assert(parse_escape_sequence("[1;2D") == KEY_SLEFT);
    assert(parse_escape_sequence("[1;2C") == KEY_SRIGHT);
    assert(parse_escape_sequence("[57350;2u") == KEY_SLEFT);
    assert(parse_escape_sequence("[57351;2u") == KEY_SRIGHT);
    assert(seek_seconds_for_key(KEY_SLEFT) == -15);
    assert(seek_seconds_for_key(KEY_SRIGHT) == 15);

    CueTrack cue = {.start = 30, .end = 90, .has_end = true};
    adjust_progress_for_cue(45, 200, &cue, &position, &duration);
    assert(position == 15);
    assert(duration == 60);

    assert(mkdtemp(directory));
    snprintf(mpv_socket_prefix, sizeof(mpv_socket_prefix),
             "%s/player", directory);
    assert(format_mpv_socket_for_pid(first, sizeof(first), getpid()));
    assert(format_mpv_socket_for_pid(second, sizeof(second), getpid() + 1));
    assert(strcmp(first, second) != 0);

    int socket_file = open(first, O_CREAT | O_WRONLY, 0600);
    assert(socket_file >= 0);
    assert(close(socket_file) == 0);

    assert(pipe(ready_pipe) == 0);
    player = fork();
    assert(player >= 0);
    if (player == 0) {
        close(ready_pipe[0]);
        signal(SIGTERM, SIG_IGN);
        assert(write(ready_pipe[1], "x", 1) == 1);
        close(ready_pipe[1]);
        for (;;) pause();
    }
    close(ready_pipe[1]);
    char ready;
    assert(read(ready_pipe[0], &ready, 1) == 1);
    close(ready_pipe[0]);

    current_player = player;
    snprintf(mpv_socket_path, sizeof(mpv_socket_path), "%s", first);
    long long started = monotonic_millis();
    retire_current_player();
    assert(monotonic_millis() - started < 100);
    assert(current_player == -1);
    assert(retired_player_count == 1);
    assert(strcmp(retired_players[0].socket_path, first) == 0);

    for (int i = 0; i < 100 && retired_player_count; i++) {
        reap_retired_players(false);
        if (retired_player_count) usleep(10000);
    }
    assert(retired_player_count == 0);
    assert(access(first, F_OK) != 0);
    free(retired_players);
    retired_players = NULL;
    retired_player_capacity = 0;

    snprintf(mpv_socket_path, sizeof(mpv_socket_path), "%s", first);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", first);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 3) == 0);

    server = fork();
    assert(server >= 0);
    if (server == 0) {
        char command[512];
        int client = accept(listener, NULL, NULL);
        if (client < 0 || read(client, command, sizeof(command)) <= 0) _exit(1);
        static const char replies[] =
            "{\"data\":12.5,\"request_id\":1,\"error\":\"success\"}\n"
            "{\"data\":240.0,\"request_id\":2,\"error\":\"success\"}\n";
        if (write(client, replies, sizeof(replies) - 1) !=
            (ssize_t)(sizeof(replies) - 1)) _exit(2);
        close(client);

        client = accept(listener, NULL, NULL);
        if (client < 0) _exit(3);
        ssize_t count = read(client, command, sizeof(command) - 1);
        if (count <= 0) _exit(4);
        command[count] = '\0';
        if (!strstr(command, "[\"seek\",-15,\"relative\"]")) _exit(5);
        close(client);

        client = accept(listener, NULL, NULL);
        if (client < 0) _exit(6);
        count = read(client, command, sizeof(command) - 1);
        if (count <= 0) _exit(7);
        command[count] = '\0';
        if (!strstr(command, "[\"seek\",15,\"relative\"]")) _exit(8);
        close(client);
        _exit(0);
    }

    mpv_get_progress(&position, &duration);
    assert(position == 12.5);
    assert(duration == 240.0);
    seek_relative(seek_seconds_for_key(KEY_SLEFT));
    seek_relative(seek_seconds_for_key(KEY_SRIGHT));

    int child_status = 0;
    assert(waitpid(server, &child_status, 0) == server);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    close(listener);
    unlink(first);
    mpv_socket_path[0] = '\0';
    assert(rmdir(directory) == 0);
    return 0;
}
