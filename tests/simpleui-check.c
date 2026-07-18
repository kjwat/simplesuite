#include <assert.h>
#include <stdint.h>
#include "../simpleproc.h"
#include "../simpleui.h"

int main(void)
{
    SuiLoop loop;
    char *captured = NULL;
    char *capture_argv[] = {"sh", "-c", "printf ready", NULL};
    char *timeout_argv[] = {"sh", "-c", "sleep 5", NULL};
    int64_t now = sui_monotonic_ms();
    assert(SUI_ESCAPE_DELAY_MS <= 25);
    assert(ssp_command_available("sh"));
    assert(!ssp_command_available("simple-suite-command-that-does-not-exist"));
    assert(ssp_capture_argv(capture_argv, &captured, 1024, 500));
    assert(!strcmp(captured, "ready"));
    free(captured);
    captured = NULL;
    int64_t capture_started = sui_monotonic_ms();
    assert(!ssp_capture_argv(timeout_argv, &captured, 1024, 30));
    assert(!captured);
    assert(sui_monotonic_ms() - capture_started < 500);
    assert(now > 0);
    assert(sui_ms_until(now - 1, 1000) == 0);
    assert(sui_ms_until(now + 10000, 25) <= 25);
    assert(sui_next_period(0, 100) >= now);
    sui_loop_init(&loop, 100);
    assert(sui_loop_take_dirty(&loop));
    assert(!sui_loop_take_dirty(&loop));
    assert(sui_loop_tick_due(&loop));
    sui_loop_mark_dirty(&loop);
    assert(sui_loop_timeout(&loop, 100) == 0);
    sui_sleep_ms(2);
    assert(sui_monotonic_ms() >= now);
    return 0;
}
