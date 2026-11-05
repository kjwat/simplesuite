/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <ncurses.h>
#include <gio/gio.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "simplebright-backend.h"
#include "simpleui.h"

typedef struct {
    SbDevice devices[SB_DEVICES];
    int count;
    SbNight night;
    uint64_t sequence;
    char message[SB_TEXT];
    int error;
} Snapshot;
typedef struct { int kind, value, enabled; char device[160]; uint64_t sequence; } Work;
enum {SCAN, READ, BRIGHTNESS, NIGHT, TEMPERATURE, QUIT};
static GAsyncQueue *commands, *replies;
static Snapshot state;
static int selected, focus, help, demo, busy, bar_y, bar_height, bar_x[2];
static uint64_t sequence;
static const char *preferred_device;
static volatile sig_atomic_t stopping;
static void stop_signal(int number) { (void)number; stopping = 1; }

static void demo_scan(Snapshot *snapshot)
{
    snapshot->count = 2;
    snapshot->devices[0] = (SbDevice){.id="demo-panel", .name="Laptop display", .backend="Demo", .percent=70, .current=70, .maximum=100, .bus=-1};
    snapshot->devices[1] = (SbDevice){.id="demo-monitor", .name="External monitor", .backend="Demo", .percent=50, .current=50, .maximum=100, .bus=1};
    snapshot->night = (SbNight){.kelvin=4000, .can_toggle=1, .can_temperature=1, .backend="Demo (no screen changes)"};
}
static gpointer worker(gpointer unused)
{
    (void)unused;
    Snapshot current = {0};
    Work *next = NULL;
    for (;;) {
        Work *work = next ? next : g_async_queue_pop(commands); next = NULL;
        /* Coalesce key repeat for one control. Each request contains the full
         * target state, so the last request includes every intervening key. */
        if (work->kind == BRIGHTNESS || work->kind == NIGHT || work->kind == TEMPERATURE) {
            while ((next = g_async_queue_try_pop(commands))) {
                if (next->kind != work->kind || strcmp(next->device, work->device)) break;
                g_free(work); work = next; next = NULL;
            }
        }
        if (work->kind == QUIT) { g_free(work); break; }
        current.error = 0; current.message[0] = 0;
        if (work->kind == SCAN) {
            if (demo) demo_scan(&current);
            else {
                current.count = sb_scan(current.devices, current.message, sizeof(current.message));
                sb_night_request(3, sb_saved_temperature(), &current.night);
            }
        } else if (work->kind == NIGHT || work->kind == TEMPERATURE) {
            if (demo) { current.night.enabled = work->enabled; current.night.kelvin = work->value; }
            else if (sb_night_request(work->kind == TEMPERATURE ? 4 : work->enabled ? 1 : 2, work->value, &current.night)) {
                current.error = 1; g_strlcpy(current.message, current.night.error, sizeof(current.message));
            }
        } else {
            for (int i = 0; i < current.count; i++) if (!strcmp(current.devices[i].id, work->device)) {
                SbDevice *d = &current.devices[i];
                if (demo) { if (work->kind == BRIGHTNESS) d->percent = work->value; }
                else current.error = (work->kind == BRIGHTNESS ? sb_set(d, work->value, current.message, sizeof(current.message)) :
                                                                sb_read(d, current.message, sizeof(current.message))) != 0;
            }
            if (!demo && work->kind == READ) sb_night_request(0, 0, &current.night);
        }
        current.sequence = work->sequence; g_free(work);
        Snapshot *reply = g_new(Snapshot, 1); *reply = current; g_async_queue_push(replies, reply);
    }
    return NULL;
}

static void queue(int kind, int value)
{
    Work *work = g_new0(Work, 1);
    work->kind = kind; work->value = value; work->enabled = state.night.enabled;
    if (selected < state.count) g_strlcpy(work->device, state.devices[selected].id, sizeof(work->device));
    work->sequence = ++sequence; busy = 1; g_async_queue_push(commands, work);
}
static void centered(int y, const char *text, int color)
{
    int width = (int)strlen(text); if (width > COLS - 4) width = COLS - 4;
    attron(COLOR_PAIR(color)); mvaddnstr(y, (COLS - width) / 2, text, width); attroff(COLOR_PAIR(color));
}
static void label(int y, int x, const char *text, int active)
{
    if (active) attron(A_BOLD | COLOR_PAIR(5)); else attron(COLOR_PAIR(1));
    mvaddnstr(y, x - (int)strlen(text) / 2, text, COLS - x - 1);
    if (active) attroff(A_BOLD | COLOR_PAIR(5)); else attroff(COLOR_PAIR(1));
}
static void draw_bar(int index, int level, const char *title, const char *value, int enabled)
{
    int x = bar_x[index], filled = (level * bar_height + 50) / 100;
    attron(COLOR_PAIR(enabled ? 1 : 6));
    mvaddch(bar_y - 1, x - 2, ACS_ULCORNER); mvhline(bar_y - 1, x - 1, ACS_HLINE, 3); mvaddch(bar_y - 1, x + 2, ACS_URCORNER);
    for (int i = 0; i < bar_height; i++) {
        mvaddch(bar_y + i, x - 2, ACS_VLINE); mvaddch(bar_y + i, x + 2, ACS_VLINE);
        int color = !enabled ? 6 : index ? 4 : i < bar_height / 5 ? 4 : i < bar_height / 2 ? 3 : 2;
        attron(COLOR_PAIR(color));
        mvhline(bar_y + i, x - 1, i >= bar_height - filled && enabled ? (chtype)' ' | A_REVERSE : (chtype)' ', 3);
        attroff(COLOR_PAIR(color)); attron(COLOR_PAIR(enabled ? 1 : 6));
    }
    mvaddch(bar_y + bar_height, x - 2, ACS_LLCORNER); mvhline(bar_y + bar_height, x - 1, ACS_HLINE, 3); mvaddch(bar_y + bar_height, x + 2, ACS_LRCORNER);
    attroff(COLOR_PAIR(enabled ? 1 : 6));
    label(bar_y + bar_height + 2, x, value, focus == index);
    label(bar_y + bar_height + 3, x, title, focus == index);
}
static void draw(void)
{
    erase();
    if (LINES < 18 || COLS < 48) {
        mvaddnstr(0, 0, "Simplebright needs at least 48 columns and 18 rows.", COLS - 1);
        if (LINES > 2) mvaddnstr(2, 0, "Resize the terminal, or press Q to quit.", COLS - 1);
        refresh(); return;
    }
    attron(COLOR_PAIR(1)); box(stdscr, 0, 0); attroff(COLOR_PAIR(1));
    centered(0, " Simplebright 1.0 ", 3);
    char line[SB_TEXT];
    if (state.count) {
        SbDevice *d = &state.devices[selected];
        snprintf(line, sizeof(line), "Display: %s (%d/%d)", d->name, selected + 1, state.count);
        mvaddnstr(2, 3, line, COLS - 6);
        snprintf(line, sizeof(line), "Brightness: %s    Night: %s", d->backend, state.night.backend);
    } else snprintf(line, sizeof(line), "Display: %s", busy ? "Detecting..." : "No brightness device");
    attron(COLOR_PAIR(1)); mvaddnstr(3, 3, line, COLS - 6); attroff(COLOR_PAIR(1));
    mvaddnstr(5, 3, "N: Night mode   Tab/Left/Right: Select   F6: Display", COLS - 6);
    bar_height = (LINES - 13) * 2 / 3;
    if (bar_height < 3) bar_height = 3;
    bar_y = 7 + (LINES - 13 - bar_height) / 2;
    if (bar_y + bar_height + 3 >= LINES - 4) bar_y = LINES - bar_height - 8;
    bar_x[0] = COLS / 2 - 10; bar_x[1] = COLS / 2 + 10;
    int brightness = state.count ? state.devices[selected].percent : 0;
    snprintf(line, sizeof(line), "%d%%", brightness);
    draw_bar(0, brightness, "Brightness", state.count ? line : "--", state.count > 0);
    int warmth = (6500 - state.night.kelvin) * 100 / 5500;
    snprintf(line, sizeof(line), "%d K", state.night.kelvin);
    draw_bar(1, warmth, "Night warmth", state.night.can_temperature ? line : "Unavailable", state.night.enabled && state.night.can_temperature);
    snprintf(line, sizeof(line), "Night mode: %s%s", state.night.enabled ? "ON" : "OFF", demo ? "   [DEMO]" : "");
    centered(LINES - 4, line, state.night.enabled ? 4 : 1);
    centered(LINES - 3, *state.message ? state.message : busy ? "Applying..." : "Up/Down: Adjust   PgUp/PgDn: 10%   Home/End: Max/Min", state.error ? 7 : 1);
    centered(LINES - 2, "F6: Next display   R: Rescan   ?: Help   Q/Esc: Quit", 1);
    if (help) {
        const char *lines[] = {
            "Simplebright controls", "", "Up/Down or K/J: adjust the selected bar",
            "Tab / Left / Right: move between the two bars", "N: toggle night mode; warmth is remembered when off",
            "Warmer = taller bar = fewer kelvin (1000-6500 K)", "PgUp/PgDn: larger steps   Home/End: maximum/minimum",
            "Mouse: click/drag a bar or use the wheel", "F6 or [ / ]: previous/next brightness display",
            "R: discover displays and night-light capabilities", "", "Brightness uses hardware backlight or monitor DDC/CI.",
            "Night: own X11/Wayland gamma, then desktop controls.", "Unavailable controls never disable brightness.",
            "Night filtering covers the desktop; brightness is per display.", "Settings stay in effect after quitting.",
            "", "Press ? or Esc to close help"
        };
        int height = MIN((int)G_N_ELEMENTS(lines), LINES - 4), width = MIN(66, COLS - 4);
        int y = (LINES - height) / 2, x = (COLS - width) / 2;
        attron(COLOR_PAIR(3));
        for (int i = 0; i < height; i++) { mvhline(y + i, x, ' ', width); mvaddnstr(y + i, x + 1, lines[i], width - 2); }
        attroff(COLOR_PAIR(3));
    }
    refresh();
}

static void adjust(int level, int absolute)
{
    state.message[0] = 0; state.error = 0;
    if (!focus && state.count) {
        SbDevice *d = &state.devices[selected];
        d->percent = CLAMP(absolute ? level : d->percent + level, 1, 100);
        queue(BRIGHTNESS, d->percent);
    } else if (focus && state.night.can_temperature) {
        int warmth = 6500 - state.night.kelvin;
        warmth = absolute ? level * 55 : warmth + level * 55;
        state.night.kelvin = 6500 - CLAMP(warmth, 0, 5500);
        queue(TEMPERATURE, state.night.kelvin);
    }
}

static int tui(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("simplebright: use a terminal, or --help for command-line controls.\n", stderr); return 1;
    }
    commands = g_async_queue_new(); replies = g_async_queue_new();
    GThread *thread = g_thread_new("simplebright-devices", worker, NULL);
    signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal); signal(SIGHUP, stop_signal);
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    set_escdelay(SUI_ESCAPE_DELAY_MS); timeout(50);
    if (has_colors()) {
        start_color(); use_default_colors();
        init_pair(1, COLOR_CYAN, -1); init_pair(2, COLOR_GREEN, -1); init_pair(3, COLOR_WHITE, -1);
        init_pair(4, COLOR_YELLOW, -1); init_pair(5, COLOR_BLACK, COLOR_CYAN); init_pair(6, COLOR_BLUE, -1); init_pair(7, COLOR_RED, -1);
    }
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL); mouseinterval(0);
    state.night.kelvin = 4000;
    queue(SCAN, 0); draw();
    int64_t next_read = sui_monotonic_ms() + 2000;
    while (!stopping) {
        Snapshot *reply; int dirty = 0;
        while ((reply = g_async_queue_try_pop(replies))) {
            if (reply->sequence >= sequence) {
                char id[160] = ""; if (selected < state.count) g_strlcpy(id, state.devices[selected].id, sizeof(id));
                if (!*id && preferred_device) g_strlcpy(id, preferred_device, sizeof(id));
                state = *reply; selected = 0;
                for (int i = 0; i < state.count; i++) if (!strcmp(id, state.devices[i].id)) selected = i;
                busy = 0; dirty = 1;
            }
            g_free(reply);
        }
        if (!busy && sui_monotonic_ms() >= next_read) {
            queue(READ, 0); next_read = sui_monotonic_ms() + 2000;
        }
        int key = getch();
        if (key != ERR) dirty = 1;
        if (help) { if (key == '?' || key == 27 || key == KEY_F(1)) help = 0; }
        else switch (key) {
            case 'q': case 'Q': case 27: stopping = 1; break;
            case '?': case KEY_F(1): help = 1; break;
            case '\t': case KEY_BTAB: focus = !focus; break;
            case KEY_LEFT: case 'h': focus = 0; break;
            case KEY_RIGHT: case 'l': focus = 1; break;
            case KEY_UP: case 'k': case '+': case '=': adjust(1, 0); break;
            case KEY_DOWN: case 'j': case '-': adjust(-1, 0); break;
            case KEY_PPAGE: adjust(10, 0); break;
            case KEY_NPAGE: adjust(-10, 0); break;
            case KEY_HOME: adjust(100, 1); break;
            case KEY_END: adjust(focus ? 0 : 1, 1); break;
            case 'n': case 'N':
                if (state.night.can_toggle) {
                    state.night.enabled = !state.night.enabled;
                    if (state.night.enabled && state.night.can_temperature) focus = 1;
                    queue(NIGHT, state.night.kelvin);
                } else {
                    g_strlcpy(state.message, "This session does not expose a night-mode toggle.", sizeof(state.message)); state.error = 1;
                }
                break;
            case KEY_F(6): case ']': if (state.count) selected = (selected + 1) % state.count; break;
            case '[': if (state.count) selected = (selected + state.count - 1) % state.count; break;
            case 'r': case 'R': queue(SCAN, 0); break;
            case KEY_MOUSE: {
                MEVENT event;
                if (getmouse(&event) == OK) {
                    if (event.x >= bar_x[0] - 5 && event.x <= bar_x[0] + 5) focus = 0;
                    if (event.x >= bar_x[1] - 5 && event.x <= bar_x[1] + 5) focus = 1;
                    if (event.bstate & BUTTON4_PRESSED) adjust(1, 0);
#ifdef BUTTON5_PRESSED
                    else if (event.bstate & BUTTON5_PRESSED) adjust(-1, 0);
#endif
                    else if ((event.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | REPORT_MOUSE_POSITION)) &&
                             abs(event.x - bar_x[focus]) <= 3 && event.y >= bar_y && event.y < bar_y + bar_height)
                        adjust((bar_y + bar_height - 1 - event.y) * 100 / MAX(1, bar_height - 1), 1);
                }
                break;
            }
            default: if (key >= '0' && key <= '9') adjust(key == '0' ? 100 : (key - '0') * 10, 1); break;
        }
        if (dirty) draw();
    }
    queue(QUIT, 0);
    endwin(); g_thread_join(thread);
    Snapshot *reply; while ((reply = g_async_queue_try_pop(replies))) g_free(reply);
    g_async_queue_unref(commands); g_async_queue_unref(replies);
    return 0;
}

static void usage(void)
{
    puts("Simplebright 1.0 - brightness and night warmth\n"
         "Usage: simplebright [--demo]\n"
         "       simplebright --list | --get | --set PERCENT | --change DELTA\n"
         "       simplebright --night on|off|toggle | --temperature KELVIN | --status\n"
         "Options: --device ID selects a brightness display; --setup-help explains support.\n"
         "Brightness range: 1-100%. Temperature: 1000-6500 K (lower is warmer).\n"
         "N toggles night mode. Tab selects a bar. Up/Down adjusts it. Q quits.");
}
static int integer(const char *text, int low, int high, int *out)
{
    if (!text || !*text) return -1;
    char *end; errno = 0; long value = strtol(text, &end, 10);
    if (errno || *end || value < low || value > high) return -1;
    *out = (int)value; return 0;
}
int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    if (argc == 2 && !strcmp(argv[1], "--night-daemon")) return sb_night_daemon();
    if (argc == 2 && !strcmp(argv[1], "--stop")) { SbNight night = {0}; return sb_night_request(5, 0, &night) ? 1 : 0; }
    const char *device = NULL, *action = NULL, *argument = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        if (!strcmp(argv[i], "--version")) { puts("simplebright 1.0.0"); return 0; }
        if (!strcmp(argv[i], "--setup-help")) {
            puts("Laptop brightness: Linux backlight, using direct access, logind/elogind, or brightnessctl.\n"
                 "External monitors: install ddcutil, enable DDC/CI, and allow access to /dev/i2c-*.\n"
                 "Night: Simplebright's own X11 gamma or wlr-gamma-control on compatible Wayland.\n"
                 "Fallbacks: GNOME/Cinnamon GSettings and public KDE KWin night-light interfaces.\n"
                 "Capabilities are probed at runtime; toggle and temperature are independent.\n"
                 "No extensions, settings windows, redshift, gammastep, or config-file edits.\n"
                 "Night warmth affects the desktop; hardware brightness affects the selected display.\n"
                 "Desktop schedules are saved before manual activation and restored when switched off.\n"
                 "Unsupported night controls never disable brightness. R rescans capabilities."); return 0;
        }
        if (!strcmp(argv[i], "--demo")) { demo = 1; continue; }
        if (!strcmp(argv[i], "--device") && i + 1 < argc) { device = argv[++i]; continue; }
        if (!strcmp(argv[i], "--list") || !strcmp(argv[i], "--get") || !strcmp(argv[i], "--status")) {
            if (action) goto bad;
            action = argv[i]; continue;
        }
        if ((!strcmp(argv[i], "--set") || !strcmp(argv[i], "--change") || !strcmp(argv[i], "--night") || !strcmp(argv[i], "--temperature")) && i + 1 < argc) {
            if (action) goto bad;
            action = argv[i]; argument = argv[++i]; continue;
        }
        goto bad;
    }
    if (demo && action) goto bad;
    if (!action) { preferred_device = device; return tui(); }
    if (!strcmp(action, "--night") || !strcmp(action, "--temperature") || !strcmp(action, "--status")) {
        SbNight night = {.kelvin = sb_saved_temperature()};
        sb_night_request(0, 0, &night);
        int enabled = night.enabled, kelvin = night.kelvin, command = 0;
        if (!strcmp(action, "--night")) {
            if (!strcmp(argument, "on")) enabled = 1;
            else if (!strcmp(argument, "off")) enabled = 0;
            else if (!strcmp(argument, "toggle")) enabled = !enabled;
            else goto bad;
            if (!night.can_toggle) { fprintf(stderr, "simplebright: night toggle unavailable (%s).\n", night.backend); return 1; }
            command = enabled ? 1 : 2;
        } else if (!strcmp(action, "--temperature")) {
            if (integer(argument, 1000, 6500, &kelvin)) goto bad;
            if (!night.can_temperature) { fprintf(stderr, "simplebright: temperature control unavailable (%s).\n", night.backend); return 1; }
            command = 4;
        }
        if (sb_night_request(command, kelvin, &night)) { fprintf(stderr, "simplebright: %s\n", night.error); return 1; }
        printf("night=%s temperature=%d toggle=%s temperature-control=%s backend=%s\n", night.enabled ? "on" : "off", night.kelvin,
               night.can_toggle ? "yes" : "no", night.can_temperature ? "yes" : "no", night.backend);
        return 0;
    }
    SbDevice devices[SB_DEVICES]; char error[SB_TEXT] = "";
    int count = sb_scan(devices, error, sizeof(error)), index = 0;
    if (!strcmp(action, "--list")) {
        for (int i = 0; i < count; i++) printf("%s\t%d%%\t%s\t%s\n", devices[i].id, devices[i].percent, devices[i].backend, devices[i].name);
        if (!count) { fprintf(stderr, "simplebright: %s\n", error); return 1; } return 0;
    }
    if (!count) { fprintf(stderr, "simplebright: %s\n", error); return 1; }
    if (device) {
        for (index = 0; index < count; index++) if (!strcmp(device, devices[index].id)) break;
        if (index == count) { fprintf(stderr, "simplebright: unknown display: %s\n", device); return 1; }
    }
    if (strcmp(action, "--get")) {
        int value; if (integer(argument, !strcmp(action, "--change") ? -100 : 1, 100, &value)) goto bad;
        if (!strcmp(action, "--change")) value = CLAMP(devices[index].percent + value, 1, 100);
        if (sb_set(&devices[index], value, error, sizeof(error))) { fprintf(stderr, "simplebright: %s\n", error); return 1; }
    }
    printf("%d\n", devices[index].percent); return 0;
bad:
    fputs("simplebright: invalid arguments. Use --help.\n", stderr); return 2;
}
