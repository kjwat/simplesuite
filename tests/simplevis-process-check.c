#define main simplevis_program_main
#include "../simplevis.c"
#undef main

#include <assert.h>

int main(void)
{
    pid_t pid = -1;
    double started = now_seconds();
    FILE *capture = start_capture_process(
        "trap '' TERM; while :; do sleep 1; done", &pid);

    assert(capture);
    assert(pid > 0);
    assert(stop_capture_process(capture, pid) != -1);
    assert(now_seconds() - started < 1.0);
    errno = 0;
    assert(waitpid(pid, NULL, WNOHANG) < 0 && errno == ECHILD);

    return 0;
}
