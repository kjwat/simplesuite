#ifndef SIMPLEUI_H
#define SIMPLEUI_H

#include <limits.h>
#include <stdint.h>
#include <time.h>

#define SUI_ESCAPE_DELAY_MS 25
#define SUI_NETWORK_POLL_MS 50
#define SUI_PLAYBACK_POLL_MS 500

typedef struct {
    int dirty;
    int period_ms;
    int64_t next_tick_ms;
} SuiLoop;

/* Small, ncurses-independent event-loop helpers shared by SimpleSuite apps. */
static inline int64_t sui_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline int sui_ms_until(int64_t deadline_ms, int maximum_ms)
{
    int64_t remaining = deadline_ms - sui_monotonic_ms();
    if (remaining <= 0) return 0;
    if (maximum_ms >= 0 && remaining > maximum_ms) return maximum_ms;
    if (remaining > INT_MAX) return INT_MAX;
    return (int)remaining;
}

static inline int64_t sui_next_period(int64_t previous_ms, int period_ms)
{
    int64_t now = sui_monotonic_ms();
    int64_t next = previous_ms + period_ms;
    return next > now ? next : now + period_ms;
}

static inline void sui_sleep_ms(int milliseconds)
{
    struct timespec delay;
    if (milliseconds <= 0) return;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0) {}
}

static inline void sui_loop_init(SuiLoop *loop, int period_ms)
{
    loop->dirty = 1;
    loop->period_ms = period_ms;
    loop->next_tick_ms = sui_monotonic_ms();
}

static inline void sui_loop_mark_dirty(SuiLoop *loop) { loop->dirty = 1; }

static inline int sui_loop_take_dirty(SuiLoop *loop)
{
    int dirty = loop->dirty;
    loop->dirty = 0;
    return dirty;
}

static inline int sui_loop_tick_due(SuiLoop *loop)
{
    if (sui_monotonic_ms() < loop->next_tick_ms) return 0;
    loop->next_tick_ms = sui_next_period(loop->next_tick_ms, loop->period_ms);
    return 1;
}

static inline int sui_loop_timeout(const SuiLoop *loop, int maximum_ms)
{
    return loop->dirty ? 0 : sui_ms_until(loop->next_tick_ms, maximum_ms);
}

#endif
