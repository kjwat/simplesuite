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
    assert(strstr(cmd, "pgrep") != NULL);
    assert(strstr(cmd, "procstat pargs") != NULL);
    assert(strstr(cmd, "ffmpeg -nostdin") != NULL);
    assert(strstr(cmd, "pactl list sinks") != NULL);
    assert(strstr(cmd, "Monitor") != NULL);
    assert(strstr(cmd, "source=\"$sink.monitor\"") != NULL);
    assert(strstr(cmd, "sleep 1") != NULL);
#else
    assert(strcmp(cmd,
                  "parec --raw --format=s16le --rate=44100 "
                  "--channels=1 --latency-msec=25 "
                  "-d \"$(pactl get-default-sink).monitor\" "
                  "2>/dev/null") == 0);
#endif
    free(cmd);
}

static void assert_drain_limit(void)
{
#ifdef __FreeBSD__
    int pipe_fds[2];
    int16_t window[FRAME_SAMPLES] = {0};
    unsigned char carry = 0;
    int has_carry = 0;
    unsigned char raw[64];

    assert(pipe(pipe_fds) == 0);
    assert(fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) == 0);
    for (size_t i = 0; i < sizeof(raw); i += 2) {
        raw[i] = (unsigned char)(i / 2 + 1);
        raw[i + 1] = 0;
    }
    assert(write(pipe_fds[1], raw, sizeof(raw)) == (ssize_t)sizeof(raw));
    close(pipe_fds[1]);

    assert(drain_audio(pipe_fds[0], window, &carry, &has_carry, 4) == 0);
    assert(window[FRAME_SAMPLES - 4] == 1);
    assert(window[FRAME_SAMPLES - 1] == 4);
    assert(drain_audio(pipe_fds[0], window, &carry, &has_carry, 4) == 0);
    assert(window[FRAME_SAMPLES - 4] == 5);
    assert(window[FRAME_SAMPLES - 1] == 8);

    close(pipe_fds[0]);
#endif
}

int main(void)
{
    pid_t pid = -1;
    double started = now_seconds();
    FILE *capture;

    assert_capture_command_selection();
    assert_drain_limit();

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
