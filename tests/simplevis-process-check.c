#define main simplevis_program_main
#include "../simplevis.c"
#undef main

#include <assert.h>

static void assert_capture_command_selection(void)
{
    char *cmd;

    unsetenv("SIMPLEVIS_CMD");
    unsetenv("SIMPLEVIS_SOURCE");

    cmd = capture_command(NULL, "cat /tmp/audio.raw");
    assert(strcmp(cmd, "cat /tmp/audio.raw") == 0);
    free(cmd);

    setenv("SIMPLEVIS_CMD", "cat /tmp/env.raw", 1);
    cmd = capture_command(NULL, NULL);
    assert(strcmp(cmd, "cat /tmp/env.raw") == 0);
    free(cmd);
    unsetenv("SIMPLEVIS_CMD");

    cmd = capture_command("named-source", NULL);
    assert(strcmp(cmd,
                  "parec --raw --format=s16le --rate=44100 "
                  "--channels=1 --latency-msec=25 -d 'named-source' "
                  "2>/dev/null") == 0);
    free(cmd);

    setenv("SIMPLEVIS_SOURCE", "env-source", 1);
    cmd = capture_command(NULL, NULL);
    assert(strcmp(cmd,
                  "parec --raw --format=s16le --rate=44100 "
                  "--channels=1 --latency-msec=25 -d 'env-source' "
                  "2>/dev/null") == 0);
    free(cmd);
    unsetenv("SIMPLEVIS_SOURCE");

    cmd = capture_command(NULL, NULL);
#ifdef __FreeBSD__
    pid_t parser;
    int parser_status;

    assert(strstr(cmd, "pgrep") != NULL);
    assert(strstr(cmd, "procstat pargs") != NULL);
    assert(strstr(cmd, "ffmpeg -nostdin") != NULL);
    assert(strstr(cmd, "live_flags='-fflags nobuffer'") != NULL);
    assert(strstr(cmd, "http://*|https://*|icy://*|rtmp://*|rtsp://*") !=
           NULL);
    assert(strstr(cmd, "-readrate 1") != NULL);
    assert(strstr(cmd, "-flush_packets 1") != NULL);
    assert(strstr(cmd, "pactl list sinks") != NULL);
    assert(strstr(cmd, "Monitor") != NULL);
    assert(strstr(cmd, "source=\"$sink.monitor\"") != NULL);
    assert(strstr(cmd, "sleep 1") != NULL);

    parser = fork();
    assert(parser >= 0);
    if (parser == 0) {
        execl("/bin/sh", "sh", "-n", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    assert(waitpid(parser, &parser_status, 0) == parser);
    assert(WIFEXITED(parser_status));
    assert(WEXITSTATUS(parser_status) == 0);
#else
    assert(strcmp(cmd,
                  "parec --raw --format=s16le --rate=44100 "
                  "--channels=1 --latency-msec=25 "
                  "-d \"$(pactl get-default-sink).monitor\" "
                  "2>/dev/null") == 0);
#endif
    free(cmd);
}

static void assert_drain_freshness(void)
{
    int pipe_fds[2];
    int16_t window[FRAME_SAMPLES] = {0};
    int16_t raw[FRAME_SAMPLES + 512];
    unsigned char carry = 0;
    int has_carry = 0;

    assert(pipe(pipe_fds) == 0);
    assert(fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) == 0);
    for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); i++)
        raw[i] = (int16_t)(i + 1);
    assert(write(pipe_fds[1], raw, sizeof(raw)) == (ssize_t)sizeof(raw));
    close(pipe_fds[1]);

    assert(drain_audio(pipe_fds[0], window, &carry, &has_carry) == 0);
    assert(window[0] == 513);
    assert(window[FRAME_SAMPLES - 1] == FRAME_SAMPLES + 512);

    close(pipe_fds[0]);

    assert(pipe(pipe_fds) == 0);
    assert(fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) == 0);
    for (int i = 0; i < FRAME_SAMPLES; i++)
        window[i] = 123;

    assert(drain_audio(pipe_fds[0], window, &carry, &has_carry) == 0);
    for (int i = 0; i < FRAME_SAMPLES; i++)
        assert(window[i] == 123);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

int main(void)
{
    pid_t pid = -1;
    double started = now_seconds();
    FILE *capture;

    assert_capture_command_selection();
    assert_drain_freshness();

    capture = start_capture_process(
        "trap '' TERM; while :; do sleep 1; done", &pid);
    assert(capture);
    assert(pid > 0);
    assert(stop_capture_process(capture, pid) != -1);
    assert(now_seconds() - started < 1.0);
    errno = 0;
    assert(waitpid(pid, NULL, WNOHANG) < 0 && errno == ECHILD);

    return 0;
}
