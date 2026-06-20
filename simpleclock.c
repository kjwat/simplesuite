#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ALARM_FILE ".simpleclock-alarm"
#define WORKER_FILE ".simpleclock-alarm-worker"

static bool alarm_path(char *path, size_t size) {
    const char *home = getenv("HOME");
    int written;

    if (!home || !*home) {
        return false;
    }

    written = snprintf(path, size, "%s/%s", home, ALARM_FILE);
    return written > 0 && (size_t)written < size;
}

static bool load_alarm(long *alarm_time) {
    char path[4096];
    FILE *file;
    long saved_time;

    if (!alarm_path(path, sizeof path)) {
        return false;
    }

    file = fopen(path, "r");
    if (!file) {
        return false;
    }

    if (fscanf(file, "%ld", &saved_time) != 1 || saved_time <= 0) {
        fclose(file);
        return false;
    }

    fclose(file);
    *alarm_time = saved_time;
    return true;
}

static void save_alarm(long alarm_time, bool alarm_on) {
    char path[4096];
    char temp_path[4100];
    FILE *file;
    int written;
    bool write_failed;

    if (!alarm_path(path, sizeof path)) {
        return;
    }

    if (!alarm_on) {
        unlink(path);
        return;
    }

    written = snprintf(temp_path, sizeof temp_path, "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof temp_path) {
        return;
    }

    file = fopen(temp_path, "w");
    if (!file) {
        return;
    }

    write_failed = fprintf(file, "%ld\n", alarm_time) < 0;
    if (fclose(file) != 0) {
        write_failed = true;
    }
    if (write_failed) {
        unlink(temp_path);
        return;
    }

    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
    }
}

static bool alarm_is_current(long alarm_time) {
    long saved_time = 0;

    return load_alarm(&saved_time) && saved_time == alarm_time;
}

static bool worker_path(char *path, size_t size) {
    const char *home = getenv("HOME");
    int written;

    if (!home || !*home) {
        return false;
    }

    written = snprintf(path, size, "%s/%s", home, WORKER_FILE);
    return written > 0 && (size_t)written < size;
}

static bool worker_is_running(long alarm_time) {
    char path[4096];
    FILE *file;
    long saved_time;
    long saved_pid;

    if (!worker_path(path, sizeof path)) {
        return false;
    }

    file = fopen(path, "r");
    if (!file) {
        return false;
    }

    if (fscanf(file, "%ld %ld", &saved_pid, &saved_time) != 2) {
        fclose(file);
        return false;
    }
    fclose(file);

    return saved_time == alarm_time && saved_pid > 1 &&
           (kill((pid_t)saved_pid, 0) == 0 || errno == EPERM);
}

static void record_worker(long alarm_time) {
    char path[4096];
    FILE *file;

    if (!worker_path(path, sizeof path)) {
        return;
    }

    file = fopen(path, "w");
    if (!file) {
        return;
    }
    fprintf(file, "%ld %ld\n", (long)getpid(), alarm_time);
    fclose(file);
}

static void forget_worker(long alarm_time) {
    char path[4096];
    FILE *file;
    long saved_time;
    long saved_pid;

    if (!worker_path(path, sizeof path)) {
        return;
    }

    file = fopen(path, "r");
    if (!file) {
        return;
    }
    if (fscanf(file, "%ld %ld", &saved_pid, &saved_time) != 2) {
        fclose(file);
        return;
    }
    fclose(file);

    if (saved_pid == (long)getpid() && saved_time == alarm_time) {
        unlink(path);
    }
}

static void show_alarm_notification(void) {
    pid_t pid = fork();

    if (pid == 0) {
        execlp("notify-send", "notify-send", "--urgency=critical",
               "--expire-time=0", "SimpleClock", "Alarm ringing", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static void sleep_milliseconds(long milliseconds) {
    struct timespec duration;

    duration.tv_sec = milliseconds / 1000;
    duration.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}

static void play_alarm_tone(void) {
    pid_t pid = fork();

    if (pid == 0) {
        execlp("speaker-test", "speaker-test", "-t", "sine",
               "-f", "523", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        sleep_milliseconds(180);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

static void run_alarm_worker(long alarm_time) {
    long delay;

    record_worker(alarm_time);

    while (alarm_is_current(alarm_time)) {
        delay = alarm_time - time(NULL);
        if (delay <= 0) {
            break;
        }
        sleep((unsigned int)(delay > 1 ? 1 : delay));
    }

    if (!alarm_is_current(alarm_time)) {
        forget_worker(alarm_time);
        _exit(0);
    }

    show_alarm_notification();
    for (int i = 0; i < 40 && alarm_is_current(alarm_time); i++) {
        play_alarm_tone();
        sleep_milliseconds(1320);
    }

    if (alarm_is_current(alarm_time)) {
        save_alarm(alarm_time, false);
    }
    forget_worker(alarm_time);
    _exit(0);
}

static void start_alarm_worker(long alarm_time) {
    pid_t first;

    if (worker_is_running(alarm_time)) {
        return;
    }

    first = fork();
    if (first < 0) {
        return;
    }
    if (first == 0) {
        pid_t second;

        if (setsid() < 0) {
            _exit(1);
        }
        second = fork();
        if (second < 0) {
            _exit(1);
        }
        if (second > 0) {
            _exit(0);
        }

        if (!freopen("/dev/null", "r", stdin))
            _exit(127);
        if (!freopen("/dev/null", "w", stdout))
            _exit(127);
        if (!freopen("/dev/null", "w", stderr))
            _exit(127);
        run_alarm_worker(alarm_time);
    }

    waitpid(first, NULL, 0);
}

long nowsec(void) {
    return time(NULL);
}

long parse_duration(const char *s) {
    long value = 0;
    char unit = 's';

    if (sscanf(s, "%ld%c", &value, &unit) < 1) {
        return -1;
    }

    switch (unit) {
        case 'd':
        case 'D':
            return value * 86400L;

        case 'h':
        case 'H':
            return value * 3600L;

        case 'm':
        case 'M':
            return value * 60L;

        case 's':
        case 'S':
        default:
            return value;
    }
}

void ask(char *buf, int n, const char *prompt) {
    timeout(-1);
    echo();
    curs_set(1);

    mvprintw(LINES - 2, 2, "%s", prompt);
    clrtoeol();
    move(LINES - 1, 2);
    clrtoeol();

    getnstr(buf, n - 1);

    noecho();
    curs_set(0);
    timeout(1000);
}

int main(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    leaveok(stdscr, TRUE);
    timeout(1000);

    long timer_end = 0;
    bool timer_on = false;
    bool timer_paused = false;
    bool timer_ringing = false;
    long timer_left_paused = 0;

    long alarm_time = 0;
    bool alarm_on = load_alarm(&alarm_time);
    bool alarm_ringing = false;

    bool sw_on = false;
    long sw_start = 0;
    long sw_elapsed = 0;

    if (alarm_on) {
        start_alarm_worker(alarm_time);
    }

    while (1) {
        long t = nowsec();

        if (timer_on && !timer_paused && t >= timer_end) {
            timer_on = false;
            timer_paused = false;
            timer_left_paused = 0;
            timer_ringing = true;
        }

        if (alarm_on && t >= alarm_time) {
            alarm_on = false;
            alarm_ringing = true;
        }

        if (timer_ringing || alarm_ringing) {
            beep();
        }

        struct tm *lt = localtime(&t);
        char dt[128];
        strftime(dt, sizeof dt, "%A, %B %d, %Y   %I:%M:%S %p", lt);

        erase();

        mvprintw(1, 2, "SIMPLECLOCK");
        mvprintw(3, 2, "%s", dt);

        long sw = sw_elapsed;
        if (sw_on) {
            sw += t - sw_start;
        }

        mvprintw(6, 2, "Stopwatch: %02ld:%02ld:%02ld  [%s]",
                 sw / 3600, (sw / 60) % 60, sw % 60,
                 sw_on ? "running" : "stopped");

        if (timer_ringing) {
            mvprintw(8, 2, "Timer: RINGING - press x to stop");
        } else if (timer_on && timer_paused) {
            long left = timer_left_paused;
            mvprintw(8, 2, "Timer: %02ld:%02ld:%02ld  [paused]",
                     left / 3600, (left / 60) % 60, left % 60);
        } else if (timer_on) {
            long left = timer_end - t;
            mvprintw(8, 2, "Timer: %02ld:%02ld:%02ld  [running]",
                     left / 3600, (left / 60) % 60, left % 60);
        } else {
            mvprintw(8, 2, "Timer: off");
        }

        if (alarm_ringing) {
            mvprintw(10, 2, "Alarm: RINGING - press x to stop");
        } else if (alarm_on) {
            struct tm *at = localtime(&alarm_time);
            char abuf[64];
            strftime(abuf, sizeof abuf, "%I:%M %p", at);
            mvprintw(10, 2, "Alarm: %s", abuf);
        } else {
            mvprintw(10, 2, "Alarm: off");
        }

        mvprintw(14, 2, "[s] start/stop stopwatch");
        mvprintw(15, 2, "[r] reset stopwatch");
        mvprintw(16, 2, "[t] set timer (30s, 5m, 2h, 1d)");
        mvprintw(17, 2, "[space] pause/resume timer");
        mvprintw(18, 2, "[a] set alarm HH:MM 24h");
        mvprintw(19, 2, "[x] stop ringing");
        mvprintw(20, 2, "[c] clear timer/alarm");
        mvprintw(21, 2, "[q] quit");

        wnoutrefresh(stdscr);
        doupdate();

        int ch = getch();

        if (ch == 'q') {
            break;
        }

        if (ch == 'x') {
            timer_ringing = false;
            if (alarm_ringing) {
                save_alarm(alarm_time, false);
            }
            alarm_ringing = false;
        }

        if (ch == ' ') {
            if (timer_on && !timer_paused) {
                timer_left_paused = timer_end - t;
                if (timer_left_paused < 0) {
                    timer_left_paused = 0;
                }
                timer_paused = true;
            } else if (timer_on && timer_paused) {
                timer_end = nowsec() + timer_left_paused;
                timer_paused = false;
            }
        }

        if (ch == 's') {
            if (sw_on) {
                sw_elapsed += t - sw_start;
                sw_on = false;
            } else {
                sw_start = t;
                sw_on = true;
            }
        }

        if (ch == 'r') {
            sw_on = false;
            sw_elapsed = 0;
        }

        if (ch == 't') {
            char buf[32];

            ask(buf, sizeof buf, "Timer (examples: 30s 5m 2h 1d): ");

            long seconds = parse_duration(buf);

            if (seconds > 0) {
                timer_end = nowsec() + seconds;
                timer_on = true;
                timer_paused = false;
                timer_left_paused = 0;
                timer_ringing = false;
            }
        }

        if (ch == 'a') {
            char buf[32];
            ask(buf, sizeof buf, "Alarm time HH:MM 24h: ");

            int h, m;
            if (sscanf(buf, "%d:%d", &h, &m) == 2 &&
                h >= 0 && h < 24 &&
                m >= 0 && m < 60) {

                time_t raw = time(NULL);
                struct tm tm_alarm = *localtime(&raw);

                tm_alarm.tm_hour = h;
                tm_alarm.tm_min = m;
                tm_alarm.tm_sec = 0;

                alarm_time = mktime(&tm_alarm);

                if (alarm_time <= nowsec()) {
                    alarm_time += 24 * 60 * 60;
                }

                alarm_on = true;
                alarm_ringing = false;
                save_alarm(alarm_time, alarm_on);
                start_alarm_worker(alarm_time);
            }
        }

        if (ch == 'c') {
            timer_on = false;
            timer_paused = false;
            timer_left_paused = 0;
            alarm_on = false;
            timer_ringing = false;
            alarm_ringing = false;
            save_alarm(alarm_time, alarm_on);
        }
    }

    endwin();
    return 0;
}
