#include <assert.h>
#include <stdint.h>
#include "../simpleui.h"

int main(void)
{
    SuiLoop loop;
    int64_t now = sui_monotonic_ms();
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
