#define main simpleflac_program_main
#include "../simpleflac.c"
#undef main

#include <assert.h>

int main(void)
{
    char directory[] = "/tmp/simpleflac-player-test.XXXXXX";
    char first[sizeof(mpv_socket_path)];
    char second[sizeof(mpv_socket_path)];
    pid_t player;
    int ready_pipe[2];

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
    assert(rmdir(directory) == 0);
    return 0;
}
