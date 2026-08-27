#define _XOPEN_SOURCE 700

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include "../simpleproc.h"
#include "../simpleui.h"

enum TerminalCaptureAction {
    CAPTURE_NESTED_FRAME,
    CAPTURE_FINISHED_FRAME,
    CAPTURE_PROBE,
    CAPTURE_CURSOR_RESTORE
};

static size_t capture_terminal_output(enum TerminalCaptureAction action,
                                      char *output, size_t output_size,
                                      SuiTerminal *terminal)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    int slave;
    char *slave_name;
    size_t used = 0;

    assert(master >= 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    slave_name = ptsname(master);
    assert(slave_name);
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    assert(slave >= 0);

    sui_terminal_init(terminal, slave);
    if (action == CAPTURE_PROBE) {
        sui_terminal_probe_synchronized_updates(terminal, NULL, NULL);
    } else if (action == CAPTURE_CURSOR_RESTORE) {
        sui_terminal_use_steady_block_cursor(terminal);
        sui_terminal_restore(terminal);
    } else {
        terminal->synchronized_updates_supported = 1;
        sui_terminal_begin_frame(terminal);
        sui_terminal_begin_frame(terminal);
        if (action == CAPTURE_NESTED_FRAME) {
            sui_terminal_end_frame(terminal);
            sui_terminal_end_frame(terminal);
        } else {
            sui_terminal_finish_frame(terminal);
        }
    }
    close(slave);

    while (used < output_size) {
        ssize_t count = read(master, output + used, output_size - used);

        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0)
            break;
        assert(errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO);
        break;
    }
    close(master);
    return used;
}

static void check_terminal_response_detection(void)
{
    SuiTerminal terminal;

    sui_terminal_init(&terminal, -1);
    assert(sui_terminal_handle_mode_response(&terminal, "[?2026;2$y"));
    assert(terminal.synchronized_updates_supported);
    assert(sui_terminal_handle_mode_response(&terminal,
                                             "\033[?2026;1$y"));
    assert(terminal.synchronized_updates_supported);
    assert(sui_terminal_handle_mode_response(&terminal, "[?2026;0$y"));
    assert(!terminal.synchronized_updates_supported);

    terminal.synchronized_updates_supported = 1;
    assert(!sui_terminal_handle_mode_response(&terminal, "[?25;2$y"));
    assert(terminal.synchronized_updates_supported);
    assert(!sui_terminal_handle_mode_response(&terminal,
                                              "[?2026;2$ytrailing"));
    assert(terminal.synchronized_updates_supported);

    terminal.synchronized_updates_mode = SUI_SYNC_DISABLED;
    terminal.synchronized_updates_supported = 0;
    assert(sui_terminal_handle_mode_response(&terminal, "[?2026;2$y"));
    assert(!terminal.synchronized_updates_supported);

    terminal.synchronized_updates_mode = SUI_SYNC_FORCED;
    terminal.synchronized_updates_supported = 1;
    assert(sui_terminal_handle_mode_response(&terminal, "[?2026;0$y"));
    assert(terminal.synchronized_updates_supported);
}

static void check_terminal_frame_sequences(void)
{
    char output[128];
    const char expected[] = SUI_SYNC_UPDATE_BEGIN SUI_SYNC_UPDATE_END;
    SuiTerminal terminal;
    size_t used;

    used = capture_terminal_output(CAPTURE_NESTED_FRAME, output,
                                   sizeof(output), &terminal);
    assert(used == sizeof(expected) - 1);
    assert(memcmp(output, expected, used) == 0);
    assert(terminal.frame_depth == 0);
    assert(!terminal.frame_active);

    used = capture_terminal_output(CAPTURE_FINISHED_FRAME, output,
                                   sizeof(output), &terminal);
    assert(used == sizeof(expected) - 1);
    assert(memcmp(output, expected, used) == 0);
    assert(terminal.frame_depth == 0);
    assert(!terminal.frame_active);
}

static void check_terminal_capability_probe(void)
{
    char output[128];
    SuiTerminal terminal;
    size_t used;

    assert(unsetenv("SIMPLESUITE_NO_SYNC") == 0);
    assert(unsetenv("SIMPLESUITE_SYNC") == 0);
    used = capture_terminal_output(CAPTURE_PROBE, output,
                                   sizeof(output), &terminal);
    assert(used == strlen(SUI_SYNC_UPDATE_QUERY));
    assert(memcmp(output, SUI_SYNC_UPDATE_QUERY, used) == 0);
    assert(terminal.synchronized_updates_mode == SUI_SYNC_AUTO);
    assert(!terminal.synchronized_updates_supported);

    assert(setenv("SIMPLESUITE_NO_SYNC", "1", 1) == 0);
    sui_terminal_probe_synchronized_updates(&terminal, NULL, NULL);
    assert(terminal.synchronized_updates_mode == SUI_SYNC_DISABLED);
    assert(!terminal.synchronized_updates_supported);

    assert(unsetenv("SIMPLESUITE_NO_SYNC") == 0);
    assert(setenv("SIMPLESUITE_SYNC", "1", 1) == 0);
    sui_terminal_probe_synchronized_updates(&terminal, NULL, NULL);
    assert(terminal.synchronized_updates_mode == SUI_SYNC_FORCED);
    assert(terminal.synchronized_updates_supported);
}

static void check_terminal_cursor_restore(void)
{
    char output[128];
    const char expected[] = SUI_CURSOR_STEADY_BLOCK SUI_CURSOR_DEFAULT;
    SuiTerminal terminal;
    size_t used;

    used = capture_terminal_output(CAPTURE_CURSOR_RESTORE, output,
                                   sizeof(output), &terminal);
    assert(used == sizeof(expected) - 1);
    assert(memcmp(output, expected, used) == 0);
    assert(!terminal.cursor_style_changed);
}

static void check_presentation_policy(void)
{
    assert(sui_presentation_style(SUI_FACE_PROSE) == 0);
    assert(sui_presentation_style(SUI_FACE_PASSIVE_CHROME) == SUI_STYLE_DIM);
    assert(!(sui_presentation_style(SUI_FACE_PASSIVE_CHROME) &
             SUI_STYLE_REVERSE));
    assert(sui_presentation_style(SUI_FACE_ACTIVE_CONTROL) ==
           SUI_STYLE_REVERSE);
}

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
    check_terminal_response_detection();
    check_terminal_frame_sequences();
    check_terminal_capability_probe();
    check_terminal_cursor_restore();
    check_presentation_policy();
    return 0;
}
