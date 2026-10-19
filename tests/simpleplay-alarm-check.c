/* Exercise the actual reminder launcher and engine, including stop/reuse. */
#define main alarm_program_main
#ifdef TEST_SIMPLECLOCK
#include "../simpleclock.c"
#else
#include "../simplecal.c"
#endif
#undef main
#include <assert.h>

int main(void)
{
    pid_t first = -1, second = -1;
#ifdef TEST_SIMPLECLOCK
    char directory[4096];
    assert(state_dir(directory, sizeof directory));
    assert(ensure_alarm_player_running(&first, 0));
    assert(ensure_alarm_player_running(&second, 0));
#else
    assert(ensure_state_dir());
    assert(ensure_alarm_player_running(&first));
    assert(ensure_alarm_player_running(&second));
#endif
    assert(first > 0 && first == second);
#ifdef __linux__
    char executable[128];
    struct stat actual, expected;
    snprintf(executable, sizeof executable, "/proc/%ld/exe", (long)first);
    assert(stat(executable, &actual) == 0);
    assert(stat(simplesuite_audio_player(), &expected) == 0);
    assert(actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino);
#endif
    stop_alarm_player(1);
    int status;
    assert(waitpid(first, &status, 0) == first);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(!alarm_player_running(NULL, 1));
    return 0;
}
