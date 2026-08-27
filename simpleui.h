#ifndef SIMPLEUI_H
#define SIMPLEUI_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SUI_ESCAPE_DELAY_MS 25
#define SUI_NETWORK_POLL_MS 50
#define SUI_PLAYBACK_POLL_MS 500

#define SUI_SYNC_UPDATE_QUERY "\033[?2026$p"
#define SUI_SYNC_UPDATE_BEGIN "\033[?2026h"
#define SUI_SYNC_UPDATE_END "\033[?2026l"
#define SUI_CURSOR_DEFAULT "\033[0 q"
#define SUI_CURSOR_STEADY_BAR "\033[6 q"

typedef struct {
    int dirty;
    int period_ms;
    int64_t next_tick_ms;
} SuiLoop;

/*
 * Physical terminal presentation shared by the text-facing SimpleSuite apps.
 *
 * ncurses keeps a desired and physical screen, but a doupdate() can still be
 * observed by the terminal while it is being emitted. DEC private mode 2026
 * lets a compatible terminal hold those cell changes and reveal the completed
 * frame at once. Frame depth is tracked so an app can stage chrome, a prose
 * window, and a prompt independently without exposing intermediate states.
 */
typedef struct {
    int output_fd;
    int synchronized_updates_mode;
    int synchronized_updates_supported;
    unsigned int frame_depth;
    int frame_active;
    int cursor_style_changed;
} SuiTerminal;

enum {
    SUI_SYNC_DISABLED = -1,
    SUI_SYNC_AUTO = 0,
    SUI_SYNC_FORCED = 1
};

static inline int sui_env_enabled(const char *name)
{
    const char *value;

    if (!name || !*name)
        return 0;
    value = getenv(name);
    return value && strcmp(value, "1") == 0;
}

static inline int sui_terminal_write_control(int output_fd,
                                             const char *sequence)
{
    size_t length;
    size_t offset = 0;

    if (output_fd < 0 || !sequence || !isatty(output_fd))
        return 0;

    if (output_fd == STDOUT_FILENO)
        (void)fflush(stdout);
    length = strlen(sequence);
    while (offset < length) {
        ssize_t written = write(output_fd, sequence + offset,
                                length - offset);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return 0;
        offset += (size_t)written;
    }
    return 1;
}

static inline void sui_terminal_init(SuiTerminal *terminal, int output_fd)
{
    if (!terminal)
        return;
    memset(terminal, 0, sizeof(*terminal));
    terminal->output_fd = output_fd;
    terminal->synchronized_updates_mode = SUI_SYNC_AUTO;
}

static inline void sui_terminal_finish_frame(SuiTerminal *terminal)
{
    if (!terminal)
        return;
    terminal->frame_depth = 0;
    if (!terminal->frame_active)
        return;

    (void)sui_terminal_write_control(terminal->output_fd,
                                     SUI_SYNC_UPDATE_END);
    terminal->frame_active = 0;
}

static inline void sui_terminal_use_steady_bar_cursor(SuiTerminal *terminal)
{
    if (!terminal || terminal->cursor_style_changed)
        return;
    terminal->cursor_style_changed =
        sui_terminal_write_control(terminal->output_fd,
                                   SUI_CURSOR_STEADY_BAR);
}

static inline void sui_terminal_restore(SuiTerminal *terminal)
{
    if (!terminal)
        return;
    sui_terminal_finish_frame(terminal);
    if (!terminal->cursor_style_changed)
        return;

    (void)sui_terminal_write_control(terminal->output_fd,
                                     SUI_CURSOR_DEFAULT);
    terminal->cursor_style_changed = 0;
}

/*
 * SIMPLESUITE_{NO_,}SYNC controls every participating application. Callers
 * may additionally supply old or app-specific environment names.
 */
static inline void sui_terminal_probe_synchronized_updates(
    SuiTerminal *terminal, const char *disable_env, const char *force_env)
{
    int disabled;
    int forced;

    if (!terminal)
        return;

    sui_terminal_finish_frame(terminal);
    disabled = sui_env_enabled("SIMPLESUITE_NO_SYNC") ||
               sui_env_enabled(disable_env);
    forced = sui_env_enabled("SIMPLESUITE_SYNC") ||
             sui_env_enabled(force_env);

    if (disabled) {
        terminal->synchronized_updates_mode = SUI_SYNC_DISABLED;
        terminal->synchronized_updates_supported = 0;
        return;
    }
    if (forced) {
        terminal->synchronized_updates_mode = SUI_SYNC_FORCED;
        terminal->synchronized_updates_supported = 1;
        return;
    }

    terminal->synchronized_updates_mode = SUI_SYNC_AUTO;
    terminal->synchronized_updates_supported = 0;
    (void)sui_terminal_write_control(terminal->output_fd,
                                     SUI_SYNC_UPDATE_QUERY);
}

/* sequence excludes the leading escape byte, as returned by app CSI readers. */
static inline int sui_terminal_handle_mode_response(SuiTerminal *terminal,
                                                    const char *sequence)
{
    int mode;
    int status;
    int consumed = 0;

    if (!terminal || !sequence)
        return 0;
    if ((unsigned char)sequence[0] == 27)
        sequence++;
    if (sscanf(sequence, "[?%d;%d$y%n", &mode, &status, &consumed) != 2 ||
        sequence[consumed] != '\0' || mode != 2026)
        return 0;

    if (terminal->synchronized_updates_mode == SUI_SYNC_AUTO)
        terminal->synchronized_updates_supported =
            status == 1 || status == 2;
    return 1;
}

static inline void sui_terminal_begin_frame(SuiTerminal *terminal)
{
    if (!terminal || terminal->frame_depth == UINT_MAX)
        return;
    if (terminal->frame_depth++ != 0)
        return;

    terminal->frame_active =
        terminal->synchronized_updates_supported &&
        sui_terminal_write_control(terminal->output_fd,
                                   SUI_SYNC_UPDATE_BEGIN);
}

static inline void sui_terminal_end_frame(SuiTerminal *terminal)
{
    if (!terminal || terminal->frame_depth == 0)
        return;
    terminal->frame_depth--;
    if (terminal->frame_depth != 0 || !terminal->frame_active)
        return;

    (void)sui_terminal_write_control(terminal->output_fd,
                                     SUI_SYNC_UPDATE_END);
    terminal->frame_active = 0;
}

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
