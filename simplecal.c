#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdint.h>
#endif

#define PATH_BUF 4096
#define ID_LEN 160
#define TITLE_LEN 256
#define LOCATION_LEN 256
#define NOTES_LEN 1024
#define STATUS_LEN 512
#define COLOR_PAIR_TODAY 1

typedef struct {
    int year;
    int month;
    int day;
} Date;

typedef struct {
    char id[ID_LEN];
    char title[TITLE_LEN];
    char date[11];
    char start[6];
    char end[6];
    char location[LOCATION_LEN];
    char notes[NOTES_LEN];
    char remind_minutes[16];
} Event;

typedef struct {
    Event *items;
    size_t len;
    size_t cap;
} EventList;

typedef struct {
    char event_id[ID_LEN];
    char due[20];
    char status[16];
    char fired_at[20];
    char fired_on[128];
} Reminder;

typedef enum {
    ALARM_START_FAILED = 0,
    ALARM_START_RUNNING = 1,
    ALARM_START_EXITED_OK = 2
} AlarmStartResult;

typedef struct {
    Reminder *items;
    size_t len;
    size_t cap;
} ReminderList;

typedef enum {
    VIEW_MONTH,
    VIEW_YEAR,
    VIEW_SEARCH,
    VIEW_EVENT_DETAIL
} ViewMode;

typedef enum {
    FOCUS_DAY,
    FOCUS_EVENT
} MonthFocus;

typedef enum {
    DETAIL_FIELD_TITLE,
    DETAIL_FIELD_DATE,
    DETAIL_FIELD_START,
    DETAIL_FIELD_END,
    DETAIL_FIELD_LOCATION,
    DETAIL_FIELD_NOTES,
    DETAIL_FIELD_REMIND,
    DETAIL_FIELD_COUNT
} DetailField;

typedef struct {
    ViewMode view;
    MonthFocus focus;
    Date selected;
    Date today;
    EventList day_events;
    EventList search_results;
    size_t event_cursor;
    size_t search_cursor;
    int search_top;
    char search_term[128];
    char detail_event_id[ID_LEN];
    char auto_opened_ringing_event_id[ID_LEN];
    int detail_editing;
    DetailField detail_field;
    int detail_cursor;
    int detail_dirty;
    int detail_new_event;
    Event detail_edit_event;
    char status[STATUS_LEN];
} App;

typedef struct {
    char data_dir[PATH_BUF];
    char default_reminder_lead_times[128];
    char theme[32];
    char today_color[32];
    int first_day_of_week;
    int clock_24h;
    int reminders_auto_install_attempted;
    int legacy_migration_warned;
} SimpleCalConfig;

static char active_data_dir[PATH_BUF] = "";
static int color_today_enabled = 0;
static SimpleCalConfig simplecal_config;
static int simplecal_config_loaded = 0;

static void make_event_id_base(const Event *event, char *out, size_t size);
static void trim(char *s);
static void clean_field(char *s);
static void app_set_status(App *app, const char *message);
static int init_data_dir(int prompt_if_missing);
static int set_data_dir(const char *input, int save_config);
static int run_setup_prompt(void);
static int write_data_config(const char *dir);
static int load_simplecal_config(void);
static int update_config_key(const char *key, const char *value);
static int migrate_legacy_data(void);
static int reminders_auto_install_attempted(void);
static int clear_reminders(const char *event_id, int *cleared_count, int quiet);
static int find_event_by_id(const char *event_id, Event *out);
static int parse_time_hhmm(const char *s, int *minutes);

static int snprintf_ok(int written, size_t size) {
    return written > 0 && (size_t)written < size;
}

static int mkdir_one(const char *path) {
    if (mkdir(path, 0700) == 0) return 1;
    return errno == EEXIST;
}

static int mkdirs(const char *path) {
    char tmp[PATH_BUF];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof tmp) return 0;
    snprintf(tmp, sizeof tmp, "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!mkdir_one(tmp)) return 0;
            *p = '/';
        }
    }

    return mkdir_one(tmp);
}

static int home_path(char *out, size_t size, const char *suffix) {
    const char *home = getenv("HOME");
    int written;

    if (!home || !*home) return 0;
    written = snprintf(out, size, "%s/%s", home, suffix);
    return snprintf_ok(written, size);
}

static int config_dir(char *out, size_t size) {
    return home_path(out, size, ".config/simplecal");
}

static int config_file_path(char *out, size_t size) {
    return home_path(out, size, ".config/simplecal/config");
}

static int default_data_dir(char *out, size_t size) {
    return home_path(out, size, ".local/share/simplecal");
}

static int ensure_config_root_dir(void) {
    char path[PATH_BUF];

    return config_dir(path, sizeof path) && mkdirs(path);
}

static void strip_trailing_slashes(char *path) {
    size_t len;

    if (!path) return;
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/') path[--len] = '\0';
}

static void lowercase_ascii(char *s) {
    if (!s) return;
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void config_defaults(SimpleCalConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    default_data_dir(cfg->data_dir, sizeof cfg->data_dir);
    snprintf(cfg->default_reminder_lead_times,
             sizeof cfg->default_reminder_lead_times, "10,30,60");
    snprintf(cfg->theme, sizeof cfg->theme, "default");
    snprintf(cfg->today_color, sizeof cfg->today_color, "yellow");
    cfg->first_day_of_week = 0;
    cfg->clock_24h = 1;
}

static int path_to_absolute(const char *input, char *out, size_t size) {
    char cleaned[PATH_BUF];
    const char *home;
    int written;

    if (!input || !*input || !out || size == 0) return 0;
    snprintf(cleaned, sizeof cleaned, "%s", input);
    clean_field(cleaned);
    if (!cleaned[0]) return 0;

    home = getenv("HOME");
    if (cleaned[0] == '~' && cleaned[1] == '/') {
        if (!home || !*home) return 0;
        written = snprintf(out, size, "%s/%s", home, cleaned + 2);
    } else if (!strncmp(cleaned, "$HOME", 5) &&
               (cleaned[5] == '\0' || cleaned[5] == '/')) {
        if (!home || !*home) return 0;
        written = snprintf(out, size, "%s%s", home, cleaned + 5);
    } else if (cleaned[0] == '/') {
        written = snprintf(out, size, "%s", cleaned);
    } else {
        char base[PATH_BUF];
        if (!config_dir(base, sizeof base)) return 0;
        written = snprintf(out, size, "%s/%s", base, cleaned);
    }

    if (!snprintf_ok(written, size)) return 0;
    strip_trailing_slashes(out);
    return out[0] == '/';
}

static int config_value_bool(const char *value) {
    char buf[32];

    snprintf(buf, sizeof buf, "%s", value ? value : "");
    clean_field(buf);
    lowercase_ascii(buf);
    return !strcmp(buf, "1") || !strcmp(buf, "true") ||
           !strcmp(buf, "yes") || !strcmp(buf, "on");
}

static int parse_first_day_value(const char *value, int *out) {
    char buf[32];
    char *end = NULL;
    long numeric;

    snprintf(buf, sizeof buf, "%s", value ? value : "");
    clean_field(buf);
    lowercase_ascii(buf);

    if (!strcmp(buf, "sunday") || !strcmp(buf, "sun")) {
        *out = 0;
        return 1;
    }
    if (!strcmp(buf, "monday") || !strcmp(buf, "mon")) {
        *out = 1;
        return 1;
    }
    if (!strcmp(buf, "tuesday") || !strcmp(buf, "tue")) {
        *out = 2;
        return 1;
    }
    if (!strcmp(buf, "wednesday") || !strcmp(buf, "wed")) {
        *out = 3;
        return 1;
    }
    if (!strcmp(buf, "thursday") || !strcmp(buf, "thu")) {
        *out = 4;
        return 1;
    }
    if (!strcmp(buf, "friday") || !strcmp(buf, "fri")) {
        *out = 5;
        return 1;
    }
    if (!strcmp(buf, "saturday") || !strcmp(buf, "sat")) {
        *out = 6;
        return 1;
    }

    errno = 0;
    numeric = strtol(buf, &end, 10);
    if (errno || !end || *end || numeric < 0 || numeric > 6) return 0;
    *out = (int)numeric;
    return 1;
}

static int parse_clock_value(const char *value, int *clock_24h) {
    char buf[32];

    snprintf(buf, sizeof buf, "%s", value ? value : "");
    clean_field(buf);
    lowercase_ascii(buf);
    if (!strcmp(buf, "24") || !strcmp(buf, "24h") ||
        !strcmp(buf, "true") || !strcmp(buf, "yes") || !strcmp(buf, "1")) {
        *clock_24h = 1;
        return 1;
    }
    if (!strcmp(buf, "12") || !strcmp(buf, "12h") ||
        !strcmp(buf, "false") || !strcmp(buf, "no") || !strcmp(buf, "0")) {
        *clock_24h = 0;
        return 1;
    }
    return 0;
}

static int split_config_line(char *line, char **key, char **value) {
    char *eq;

    line[strcspn(line, "\r\n")] = '\0';
    trim(line);
    if (!line[0] || line[0] == '#') return 0;
    eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = '\0';
    *key = line;
    *value = eq + 1;
    trim(*key);
    trim(*value);
    return (*key)[0] != '\0';
}

static void apply_config_value(SimpleCalConfig *cfg, const char *key, const char *value) {
    if (!strcmp(key, "data_dir") || !strcmp(key, "DATA_DIR")) {
        char absolute[PATH_BUF];

        if (path_to_absolute(value, absolute, sizeof absolute))
            snprintf(cfg->data_dir, sizeof cfg->data_dir, "%s", absolute);
    } else if (!strcmp(key, "default_reminder_lead_times")) {
        snprintf(cfg->default_reminder_lead_times,
                 sizeof cfg->default_reminder_lead_times, "%s", value);
        clean_field(cfg->default_reminder_lead_times);
    } else if (!strcmp(key, "theme")) {
        snprintf(cfg->theme, sizeof cfg->theme, "%s", value);
        clean_field(cfg->theme);
    } else if (!strcmp(key, "today_color")) {
        snprintf(cfg->today_color, sizeof cfg->today_color, "%s", value);
        clean_field(cfg->today_color);
        lowercase_ascii(cfg->today_color);
    } else if (!strcmp(key, "first_day_of_week")) {
        int first_day;

        if (parse_first_day_value(value, &first_day)) cfg->first_day_of_week = first_day;
    } else if (!strcmp(key, "clock") || !strcmp(key, "clock_24h")) {
        int clock_24h;

        if (parse_clock_value(value, &clock_24h)) cfg->clock_24h = clock_24h;
    } else if (!strcmp(key, "reminders_auto_install_attempted") ||
               !strcmp(key, "REMINDERS_AUTO_INSTALL_ATTEMPTED")) {
        cfg->reminders_auto_install_attempted = config_value_bool(value);
    } else if (!strcmp(key, "legacy_migration_warned")) {
        cfg->legacy_migration_warned = config_value_bool(value);
    }
}

static int read_data_config(char *out, size_t size) {
    if (!load_simplecal_config()) return 0;
    return snprintf_ok(snprintf(out, size, "%s", simplecal_config.data_dir), size);
}

static void format_display_time(const char *hhmm, char *out, size_t size) {
    int minutes;
    int hour;
    int minute;
    int display_hour;
    const char *suffix;

    if (!out || size == 0) return;
    if (!hhmm || !*hhmm || !parse_time_hhmm(hhmm, &minutes)) {
        snprintf(out, size, "%s", hhmm && *hhmm ? hhmm : "");
        return;
    }
    if (simplecal_config.clock_24h) {
        snprintf(out, size, "%s", hhmm);
        return;
    }

    hour = minutes / 60;
    minute = minutes % 60;
    suffix = hour < 12 ? "AM" : "PM";
    display_hour = hour % 12;
    if (display_hour == 0) display_hour = 12;
    snprintf(out, size, "%d:%02d %s", display_hour, minute, suffix);
}

static void format_event_time_range(const Event *event, char *out, size_t size) {
    char start[32];
    char end[32];

    if (!out || size == 0) return;
    if (event && event->start[0] && event->end[0]) {
        format_display_time(event->start, start, sizeof start);
        format_display_time(event->end, end, sizeof end);
        snprintf(out, size, "%s-%s", start, end);
    } else if (event && event->start[0]) {
        format_display_time(event->start, out, size);
    } else {
        snprintf(out, size, "all day");
    }
}

static int reminders_auto_install_attempted(void) {
    if (!load_simplecal_config()) return 0;
    return simplecal_config.reminders_auto_install_attempted;
}

static int events_dir(char *out, size_t size) {
    int written;

    if (!active_data_dir[0] && !init_data_dir(0)) return 0;
    written = snprintf(out, size, "%s/events", active_data_dir);
    return snprintf_ok(written, size);
}

static int reminders_path(char *out, size_t size) {
    int written;

    if (!active_data_dir[0] && !init_data_dir(0)) return 0;
    written = snprintf(out, size, "%s/reminders.db", active_data_dir);
    return snprintf_ok(written, size);
}

static int alarm_path(char *out, size_t size) {
    return home_path(out, size, ".local/share/simplesuite/simplecal-alarm.mp3");
}

static int state_dir(char *out, size_t size) {
    return home_path(out, size, ".local/state/simplecal");
}

static int check_lock_path(char *out, size_t size) {
    return home_path(out, size, ".local/state/simplecal/check.lock");
}

static int alarm_pid_path(char *out, size_t size) {
    return home_path(out, size, ".local/state/simplecal/alarm.pid");
}

static int ensure_state_dir(void) {
    char path[PATH_BUF];

    return state_dir(path, sizeof path) && mkdirs(path);
}

static int ensure_config_dirs(void) {
    char path[PATH_BUF];

    if (!active_data_dir[0] && !init_data_dir(0)) return 0;
    if (!ensure_config_root_dir()) return 0;
    if (!mkdirs(active_data_dir)) return 0;
    if (!events_dir(path, sizeof path) || !mkdirs(path)) return 0;
    return 1;
}

static int parse_date(const char *s, Date *out) {
    int y, m, d;
    char tail;
    struct tm tmv;
    time_t t;
    struct tm *check;

    if (!s || sscanf(s, "%4d-%2d-%2d%c", &y, &m, &d, &tail) != 3) return 0;
    if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return 0;

    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;
    tmv.tm_isdst = -1;
    t = mktime(&tmv);
    if (t == (time_t)-1) return 0;

    check = localtime(&t);
    if (!check) return 0;
    if (check->tm_year != y - 1900 || check->tm_mon != m - 1 ||
        check->tm_mday != d) return 0;

    if (out) {
        out->year = y;
        out->month = m;
        out->day = d;
    }
    return 1;
}

static void format_date(Date d, char *out, size_t size) {
    snprintf(out, size, "%04d-%02d-%02d", d.year, d.month, d.day);
}

static int parse_time_hhmm(const char *s, int *minutes) {
    int h, m;
    char tail;

    if (!s || !*s) {
        if (minutes) *minutes = 0;
        return 1;
    }

    if (sscanf(s, "%2d:%2d%c", &h, &m, &tail) != 2) return 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) return 0;
    if (minutes) *minutes = h * 60 + m;
    return 1;
}

static int parse_remind_minutes(const char *s, int *out) {
    long value = 0;
    char *end = NULL;

    if (!s || !*s) {
        if (out) *out = -1;
        return 1;
    }

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno || !end || *end || value < 0 || value > 525600) return 0;

    if (out) *out = (int)value;
    return 1;
}

static Date today_date(void) {
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    Date d = { 1970, 1, 1 };

    if (tmv) {
        d.year = tmv->tm_year + 1900;
        d.month = tmv->tm_mon + 1;
        d.day = tmv->tm_mday;
    }

    return d;
}

static int date_cmp(Date a, Date b) {
    if (a.year != b.year) return a.year < b.year ? -1 : 1;
    if (a.month != b.month) return a.month < b.month ? -1 : 1;
    if (a.day != b.day) return a.day < b.day ? -1 : 1;
    return 0;
}

static time_t date_time_value(Date d, const char *hhmm) {
    struct tm tmv;
    int minutes = 0;

    parse_time_hhmm(hhmm, &minutes);
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = d.year - 1900;
    tmv.tm_mon = d.month - 1;
    tmv.tm_mday = d.day;
    tmv.tm_hour = minutes / 60;
    tmv.tm_min = minutes % 60;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

static Date date_from_time(time_t t) {
    struct tm *tmv = localtime(&t);
    Date d = { 1970, 1, 1 };

    if (tmv) {
        d.year = tmv->tm_year + 1900;
        d.month = tmv->tm_mon + 1;
        d.day = tmv->tm_mday;
    }

    return d;
}

static Date add_days(Date d, int days) {
    time_t t = date_time_value(d, "12:00");
    struct tm tmv;

    if (t == (time_t)-1) return d;
    tmv = *localtime(&t);
    tmv.tm_mday += days;
    tmv.tm_isdst = -1;
    t = mktime(&tmv);
    return date_from_time(t);
}

static int is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30,
                                31, 31, 30, 31, 30, 31 };
    if (month == 2) return is_leap(year) ? 29 : 28;
    if (month < 1 || month > 12) return 30;
    return days[month - 1];
}

static Date add_months(Date d, int months) {
    int total = d.year * 12 + (d.month - 1) + months;
    int maxday;

    d.year = total / 12;
    d.month = total % 12 + 1;
    if (d.month <= 0) {
        d.month += 12;
        d.year--;
    }

    maxday = days_in_month(d.year, d.month);
    if (d.day > maxday) d.day = maxday;
    return d;
}

static int weekday_of(Date d) {
    time_t t = date_time_value(d, "12:00");
    struct tm *tmv;

    if (t == (time_t)-1) return 0;
    tmv = localtime(&t);
    return tmv ? tmv->tm_wday : 0;
}

static int configured_weekday_position(int tm_wday) {
    int first = simplecal_config.first_day_of_week;

    if (first < 0 || first > 6) first = 0;
    return (tm_wday - first + 7) % 7;
}

static void weekday_header(char *out, size_t size) {
    static const char *names[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    int first = simplecal_config.first_day_of_week;
    size_t used = 0;

    if (!out || size == 0) return;
    out[0] = '\0';
    if (first < 0 || first > 6) first = 0;
    for (int i = 0; i < 7; i++) {
        int day = (first + i) % 7;
        int written;

        if (used >= size) break;
        written = snprintf(out + used, size - used, "%s%s", i ? "  " : "", names[day]);
        if (written < 0) break;
        if ((size_t)written >= size - used) {
            used = size - 1;
            break;
        }
        used += (size_t)written;
    }
}

static void trim(char *s) {
    size_t len;

    if (!s) return;
    while (isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static void clean_field(char *s) {
    if (!s) return;
    for (char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n' || (unsigned char)*p < 32) *p = ' ';
    }
    trim(s);
}

static void copy_field(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
    clean_field(dst);
}

static int eventlist_push(EventList *list, Event event) {
    Event *items;
    size_t newcap;

    if (list->len == list->cap) {
        newcap = list->cap ? list->cap * 2 : 8;
        items = realloc(list->items, newcap * sizeof *items);
        if (!items) return 0;
        list->items = items;
        list->cap = newcap;
    }
    list->items[list->len++] = event;
    return 1;
}

static void eventlist_free(EventList *list) {
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static int reminderlist_push(ReminderList *list, Reminder reminder) {
    Reminder *items;
    size_t newcap;

    if (list->len == list->cap) {
        newcap = list->cap ? list->cap * 2 : 8;
        items = realloc(list->items, newcap * sizeof *items);
        if (!items) return 0;
        list->items = items;
        list->cap = newcap;
    }
    list->items[list->len++] = reminder;
    return 1;
}

static void reminderlist_free(ReminderList *list) {
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static int event_compare(const void *ap, const void *bp) {
    const Event *a = ap;
    const Event *b = bp;
    int c = strcmp(a->date, b->date);

    if (c) return c;
    c = strcmp(a->start, b->start);
    if (c) return c;
    c = strcmp(a->title, b->title);
    if (c) return c;
    return strcmp(a->id, b->id);
}

static int event_file_path_for_date(Date d, char *out, size_t size) {
    char base[PATH_BUF];
    int written;

    if (!events_dir(base, sizeof base)) return 0;
    written = snprintf(out, size, "%s/%04d/%04d-%02d-%02d.cal",
                       base, d.year, d.year, d.month, d.day);
    return snprintf_ok(written, size);
}

static int ensure_event_year_dir(Date d) {
    char base[PATH_BUF];
    char path[PATH_BUF];
    int written;

    if (!events_dir(base, sizeof base)) return 0;
    written = snprintf(path, sizeof path, "%s/%04d", base, d.year);
    return snprintf_ok(written, sizeof path) && mkdirs(path);
}

static int atomic_open_temp(const char *path, char *tmp, size_t tmp_size, FILE **out) {
    int written = snprintf(tmp, tmp_size, "%s.tmp.%ld", path, (long)getpid());
    if (!snprintf_ok(written, tmp_size)) return 0;
    *out = fopen(tmp, "w");
    return *out != NULL;
}

static int finish_atomic_write(FILE *file, const char *tmp, const char *path) {
    int ok = 1;

    if (fflush(file) != 0) ok = 0;
    if (fclose(file) != 0) ok = 0;
    if (ok && rename(tmp, path) != 0) ok = 0;
    if (!ok) unlink(tmp);
    return ok;
}

static int write_default_config_file(void) {
    char path[PATH_BUF];
    char tmp[PATH_BUF + 64];
    FILE *file;

    if (!ensure_config_root_dir()) return 0;
    if (!config_file_path(path, sizeof path)) return 0;
    if (!atomic_open_temp(path, tmp, sizeof tmp, &file)) return 0;

    if (fprintf(file,
                "# SimpleCal configuration\n"
                "# Calendar data lives under data_dir. Relative paths are resolved under this config directory.\n"
                "data_dir=$HOME/.local/share/simplecal\n"
                "default_reminder_lead_times=10,30,60\n"
                "theme=default\n"
                "today_color=yellow\n"
                "first_day_of_week=sunday\n"
                "clock=24h\n"
                "reminders_auto_install_attempted=0\n"
                "legacy_migration_warned=0\n") < 0) {
        fclose(file);
        unlink(tmp);
        return 0;
    }

    return finish_atomic_write(file, tmp, path);
}

static int config_keys_match(const char *line_key, const char *target_key) {
    if (!strcmp(target_key, "data_dir"))
        return !strcmp(line_key, "data_dir") || !strcmp(line_key, "DATA_DIR");
    if (!strcmp(target_key, "reminders_auto_install_attempted"))
        return !strcmp(line_key, "reminders_auto_install_attempted") ||
               !strcmp(line_key, "REMINDERS_AUTO_INSTALL_ATTEMPTED");
    return !strcmp(line_key, target_key);
}

static int config_line_matches_key(const char *line, const char *key) {
    char copy[PATH_BUF + 256];
    char *line_key;
    char *value;

    snprintf(copy, sizeof copy, "%s", line ? line : "");
    if (!split_config_line(copy, &line_key, &value)) return 0;
    return config_keys_match(line_key, key);
}

static int config_has_key(const char *key) {
    char path[PATH_BUF];
    char line[PATH_BUF + 256];
    FILE *file;

    if (!config_file_path(path, sizeof path)) return 0;
    file = fopen(path, "r");
    if (!file) return 0;

    while (fgets(line, sizeof line, file)) {
        if (config_line_matches_key(line, key)) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static int update_config_key(const char *key, const char *value) {
    char path[PATH_BUF];
    char tmp[PATH_BUF + 64];
    char line[PATH_BUF + 256];
    FILE *in;
    FILE *out;
    int updated = 0;
    int ok = 1;

    if (!ensure_config_root_dir()) return 0;
    if (!config_file_path(path, sizeof path)) return 0;
    if (access(path, F_OK) != 0 && !write_default_config_file()) return 0;

    in = fopen(path, "r");
    if (!in) return 0;
    if (!atomic_open_temp(path, tmp, sizeof tmp, &out)) {
        fclose(in);
        return 0;
    }

    while (fgets(line, sizeof line, in)) {
        if (config_line_matches_key(line, key)) {
            if (!updated) {
                if (fprintf(out, "%s=%s\n", key, value) < 0) ok = 0;
                updated = 1;
            }
            continue;
        }
        if (fputs(line, out) == EOF) ok = 0;
    }

    if (!updated && fprintf(out, "%s=%s\n", key, value) < 0) ok = 0;
    if (fclose(in) != 0) ok = 0;
    if (!finish_atomic_write(out, tmp, path)) ok = 0;
    return ok;
}

static int ensure_config_key(const char *key, const char *value) {
    if (config_has_key(key)) return 1;
    return update_config_key(key, value);
}

static int ensure_config_defaults_present(void) {
    int ok = 1;

    ok = ensure_config_key("data_dir", "$HOME/.local/share/simplecal") && ok;
    ok = ensure_config_key("default_reminder_lead_times", "10,30,60") && ok;
    ok = ensure_config_key("theme", "default") && ok;
    ok = ensure_config_key("today_color", "yellow") && ok;
    ok = ensure_config_key("first_day_of_week", "sunday") && ok;
    ok = ensure_config_key("clock", "24h") && ok;
    ok = ensure_config_key("reminders_auto_install_attempted", "0") && ok;
    ok = ensure_config_key("legacy_migration_warned", "0") && ok;
    return ok;
}

static int load_simplecal_config(void) {
    char path[PATH_BUF];
    char line[PATH_BUF + 256];
    FILE *file;
    SimpleCalConfig cfg;

    if (simplecal_config_loaded) return 1;
    config_defaults(&cfg);

    if (!ensure_config_root_dir()) return 0;
    if (!config_file_path(path, sizeof path)) return 0;
    if (access(path, F_OK) != 0 && !write_default_config_file()) return 0;

    file = fopen(path, "r");
    if (!file) return 0;

    while (fgets(line, sizeof line, file)) {
        char copy[PATH_BUF + 256];
        char *key;
        char *value;

        snprintf(copy, sizeof copy, "%s", line);
        if (!split_config_line(copy, &key, &value)) continue;
        apply_config_value(&cfg, key, value);
    }

    fclose(file);
    simplecal_config = cfg;
    simplecal_config_loaded = 1;
    return ensure_config_defaults_present();
}

static int write_data_config(const char *dir) {
    char absolute[PATH_BUF];

    if (!path_to_absolute(dir, absolute, sizeof absolute)) return 0;
    if (!update_config_key("data_dir", dir)) return 0;
    snprintf(simplecal_config.data_dir, sizeof simplecal_config.data_dir, "%s", absolute);
    simplecal_config_loaded = 1;
    return 1;
}

static int mark_reminders_auto_install_attempted(void) {
    if (!load_simplecal_config()) return 0;
    if (!update_config_key("reminders_auto_install_attempted", "1")) return 0;
    simplecal_config.reminders_auto_install_attempted = 1;
    return 1;
}

static int path_is_within(const char *base, const char *path) {
    char b[PATH_BUF];
    char p[PATH_BUF];
    size_t len;

    if (!base || !path || !*base || !*path) return 0;
    snprintf(b, sizeof b, "%s", base);
    snprintf(p, sizeof p, "%s", path);
    strip_trailing_slashes(b);
    strip_trailing_slashes(p);
    if (!strcmp(b, p)) return 1;
    len = strlen(b);
    return len > 1 && !strncmp(p, b, len) && p[len] == '/';
}

static int executable_dir(char *out, size_t size) {
    char *slash;

    if (!out || size == 0) return 0;
#ifdef __APPLE__
    uint32_t len = (uint32_t)size;
    if (_NSGetExecutablePath(out, &len) != 0 || out[0] != '/') return 0;
#else
    ssize_t len;
    len = readlink("/proc/self/exe", out, size - 1);
    if (len <= 0 || (size_t)len >= size) return 0;
    out[len] = '\0';
#endif
    slash = strrchr(out, '/');
    if (!slash) return 0;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    strip_trailing_slashes(out);
    return out[0] == '/';
}

static int source_tree_marker_exists(const char *dir) {
    char path[PATH_BUF];

    if (!dir || !*dir) return 0;
    if (!snprintf_ok(snprintf(path, sizeof path, "%s/simplecal.c", dir), sizeof path))
        return 0;
    return access(path, R_OK) == 0;
}

static int data_dir_is_unsafe(const char *path, char *reason, size_t reason_size) {
    char source_tree[PATH_BUF];
    char cwd[PATH_BUF];
    char exe[PATH_BUF];
    char exe_parent[PATH_BUF];
    char *slash;

    if (home_path(source_tree, sizeof source_tree, "simplesuite") &&
        path_is_within(source_tree, path)) {
        snprintf(reason, reason_size, "it is inside the SimpleSuite source tree");
        return 1;
    }
    if (getcwd(cwd, sizeof cwd)) {
        strip_trailing_slashes(cwd);
        if (!strcmp(cwd, path)) {
            snprintf(reason, reason_size, "it is the current working directory");
            return 1;
        }
        if (source_tree_marker_exists(cwd) && path_is_within(cwd, path)) {
            snprintf(reason, reason_size, "it is inside the SimpleSuite source tree");
            return 1;
        }
    }
    if (executable_dir(exe, sizeof exe)) {
        if (path_is_within(exe, path)) {
            snprintf(reason, reason_size, "it is beside the running executable");
            return 1;
        }
        snprintf(exe_parent, sizeof exe_parent, "%s", exe);
        slash = strrchr(exe_parent, '/');
        if (slash && slash != exe_parent) {
            *slash = '\0';
            if (source_tree_marker_exists(exe_parent) && path_is_within(exe_parent, path)) {
                snprintf(reason, reason_size, "it is inside the SimpleSuite source tree");
                return 1;
            }
        }
    }
    return 0;
}

static int copy_file_path(const char *src, const char *dst) {
    FILE *in;
    FILE *out;
    char buf[8192];
    size_t n;
    int ok = 1;

    in = fopen(src, "rb");
    if (!in) return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }

    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) ok = 0;
    if (fclose(in) != 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    if (!ok) unlink(dst);
    return ok;
}

static int move_file_safely(const char *src, const char *dst, int *migrated, int *blocked) {
    if (access(dst, F_OK) == 0) {
        if (blocked) (*blocked)++;
        return 1;
    }
    if (rename(src, dst) == 0) {
        if (migrated) (*migrated)++;
        return 1;
    }
    if (copy_file_path(src, dst)) {
        if (unlink(src) == 0) {
            if (migrated) (*migrated)++;
            return 1;
        }
        unlink(dst);
    }
    if (blocked) (*blocked)++;
    return 0;
}

static int migrate_dir_contents(const char *src, const char *dst, int *migrated, int *blocked) {
    DIR *dir;
    struct dirent *entry;

    dir = opendir(src);
    if (!dir) return errno == ENOENT ? 1 : 0;
    if (!mkdirs(dst)) {
        closedir(dir);
        return 0;
    }

    while ((entry = readdir(dir))) {
        char src_path[PATH_BUF];
        char dst_path[PATH_BUF];
        struct stat st;

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!snprintf_ok(snprintf(src_path, sizeof src_path, "%s/%s", src, entry->d_name),
                         sizeof src_path) ||
            !snprintf_ok(snprintf(dst_path, sizeof dst_path, "%s/%s", dst, entry->d_name),
                         sizeof dst_path)) {
            if (blocked) (*blocked)++;
            continue;
        }
        if (lstat(src_path, &st) != 0) {
            if (blocked) (*blocked)++;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            migrate_dir_contents(src_path, dst_path, migrated, blocked);
            rmdir(src_path);
        } else if (S_ISREG(st.st_mode)) {
            move_file_safely(src_path, dst_path, migrated, blocked);
        } else if (blocked) {
            (*blocked)++;
        }
    }

    closedir(dir);
    rmdir(src);
    return 1;
}

static int migrate_legacy_base(const char *base, int *found, int *migrated, int *blocked) {
    char events_src[PATH_BUF];
    char events_dst[PATH_BUF];
    char reminders_src[PATH_BUF];
    char reminders_dst[PATH_BUF];
    struct stat st;

    if (!base || !*base || path_is_within(active_data_dir, base)) return 1;
    if (!snprintf_ok(snprintf(events_src, sizeof events_src, "%s/events", base),
                     sizeof events_src) ||
        !events_dir(events_dst, sizeof events_dst) ||
        !snprintf_ok(snprintf(reminders_src, sizeof reminders_src, "%s/reminders.db", base),
                     sizeof reminders_src) ||
        !reminders_path(reminders_dst, sizeof reminders_dst)) {
        return 0;
    }

    if (stat(events_src, &st) == 0 && S_ISDIR(st.st_mode)) {
        *found = 1;
        migrate_dir_contents(events_src, events_dst, migrated, blocked);
    }
    if (stat(reminders_src, &st) == 0 && S_ISREG(st.st_mode)) {
        *found = 1;
        move_file_safely(reminders_src, reminders_dst, migrated, blocked);
    }
    return 1;
}

static int migrate_legacy_data(void) {
    char config_root[PATH_BUF];
    char source_tree[PATH_BUF];
    char exe[PATH_BUF];
    char exe_parent[PATH_BUF];
    int found = 0;
    int migrated = 0;
    int blocked = 0;

    if (!active_data_dir[0]) return 0;
    if (config_dir(config_root, sizeof config_root))
        migrate_legacy_base(config_root, &found, &migrated, &blocked);
    if (home_path(source_tree, sizeof source_tree, "simplesuite"))
        migrate_legacy_base(source_tree, &found, &migrated, &blocked);
    if (executable_dir(exe, sizeof exe)) {
        migrate_legacy_base(exe, &found, &migrated, &blocked);
        snprintf(exe_parent, sizeof exe_parent, "%s", exe);
        char *slash = strrchr(exe_parent, '/');
        if (slash && slash != exe_parent) {
            *slash = '\0';
            migrate_legacy_base(exe_parent, &found, &migrated, &blocked);
        }
    }

    if (found && !simplecal_config.legacy_migration_warned) {
        fprintf(stderr,
                "simplecal: legacy calendar data was found and migrated to %s. "
                "%d item(s) moved, %d left in place due to conflicts or errors.\n",
                active_data_dir, migrated, blocked);
        update_config_key("legacy_migration_warned", "1");
        simplecal_config.legacy_migration_warned = 1;
    }
    return 1;
}

static int set_data_dir(const char *input, int save_config) {
    char absolute[PATH_BUF];
    char events[PATH_BUF];
    char reason[160];
    int written;

    if (!path_to_absolute(input, absolute, sizeof absolute)) return 0;
    if (data_dir_is_unsafe(absolute, reason, sizeof reason)) {
        fprintf(stderr, "simplecal: refusing data_dir=%s because %s.\n", absolute, reason);
        return 0;
    }
    if (!mkdirs(absolute)) return 0;
    written = snprintf(events, sizeof events, "%s/events", absolute);
    if (!snprintf_ok(written, sizeof events) || !mkdirs(events)) return 0;
    if (save_config && !write_data_config(input)) return 0;
    snprintf(active_data_dir, sizeof active_data_dir, "%s", absolute);
    return 1;
}

static int run_setup_prompt(void) {
    char def[PATH_BUF];
    char choice[32];
    char input[PATH_BUF];
    const char *selected;

    if (!default_data_dir(def, sizeof def)) {
        fprintf(stderr, "simplecal: HOME is not set; cannot create config.\n");
        return 1;
    }

    printf("SimpleCal setup\n");
    printf("1) Use default local calendar directory: %s\n", def);
    printf("2) Choose another directory\n");
    printf("Choice [1]: ");
    fflush(stdout);

    if (!fgets(choice, sizeof choice, stdin)) return 1;
    clean_field(choice);

    selected = def;
    if (choice[0] == '2') {
        printf("Calendar directory: ");
        fflush(stdout);
        if (!fgets(input, sizeof input, stdin)) return 1;
        clean_field(input);
        if (!input[0]) {
            fprintf(stderr, "simplecal: empty calendar directory.\n");
            return 1;
        }
        selected = input;
    }

    if (!set_data_dir(selected, 1)) {
        fprintf(stderr, "simplecal: could not set data_dir.\n");
        return 1;
    }

    printf("simplecal: data_dir=%s\n", active_data_dir);
    return 0;
}

static int init_data_dir(int prompt_if_missing) {
    char dir[PATH_BUF];
    char def[PATH_BUF];

    (void)prompt_if_missing;
    if (active_data_dir[0]) return 1;
    if (!load_simplecal_config()) return 0;
    if (!read_data_config(dir, sizeof dir)) return 0;
    if (!set_data_dir(dir, 0)) {
        if (!default_data_dir(def, sizeof def)) return 0;
        fprintf(stderr,
                "simplecal: configured data_dir is unsafe or unavailable; using %s.\n",
                def);
        if (!set_data_dir(def, 0)) return 0;
        write_data_config("$HOME/.local/share/simplecal");
    }
    migrate_legacy_data();
    return 1;
}

static int parse_event_line(Event *event, const char *key, const char *value) {
    if (!strcmp(key, "ID")) copy_field(event->id, sizeof event->id, value);
    else if (!strcmp(key, "TITLE")) copy_field(event->title, sizeof event->title, value);
    else if (!strcmp(key, "DATE")) copy_field(event->date, sizeof event->date, value);
    else if (!strcmp(key, "START")) copy_field(event->start, sizeof event->start, value);
    else if (!strcmp(key, "END")) copy_field(event->end, sizeof event->end, value);
    else if (!strcmp(key, "LOCATION")) copy_field(event->location, sizeof event->location, value);
    else if (!strcmp(key, "NOTES")) copy_field(event->notes, sizeof event->notes, value);
    else if (!strcmp(key, "REMIND_MINUTES")) copy_field(event->remind_minutes, sizeof event->remind_minutes, value);
    else return 0;
    return 1;
}

static int event_valid_basic(const Event *event, int require_id) {
    Date d;
    int start_min = 0;
    int end_min = 0;
    int remind = -1;

    if (require_id && !event->id[0]) return 0;
    if (!event->title[0] || !parse_date(event->date, &d)) return 0;
    if (!parse_time_hhmm(event->start, &start_min)) return 0;
    if (!parse_time_hhmm(event->end, &end_min)) return 0;
    if (event->start[0] && event->end[0] && end_min < start_min) return 0;
    if (!parse_remind_minutes(event->remind_minutes, &remind)) return 0;
    return 1;
}

static int event_valid_for_storage(const Event *event) {
    return event_valid_basic(event, 1);
}

static int event_id_exists_in_list(EventList *list, const char *id, size_t skip) {
    for (size_t i = 0; i < list->len; i++) {
        if (i != skip && !strcmp(list->items[i].id, id)) return 1;
    }
    return 0;
}

static void assign_missing_event_ids(EventList *list, int *changed) {
    for (size_t i = 0; i < list->len; i++) {
        Event *event = &list->items[i];
        char base[ID_LEN];
        char candidate[ID_LEN];
        int suffix = 1;

        if (event->id[0]) continue;

        make_event_id_base(event, base, sizeof base);
        snprintf(candidate, sizeof candidate, "%s", base);
        while (event_id_exists_in_list(list, candidate, i)) {
            suffix++;
            snprintf(candidate, sizeof candidate, "%s-%d", base, suffix);
        }
        snprintf(event->id, sizeof event->id, "%s", candidate);
        if (changed) *changed = 1;
    }
}

static int write_events_file_path(const char *path, EventList *list) {
    char tmp[PATH_BUF + 64];
    FILE *file;
    int ok = 1;

    if (list->len == 0) {
        unlink(path);
        return 1;
    }

    qsort(list->items, list->len, sizeof list->items[0], event_compare);
    if (!atomic_open_temp(path, tmp, sizeof tmp, &file)) return 0;

    for (size_t i = 0; i < list->len; i++) {
        Event *e = &list->items[i];
        if (!event_valid_for_storage(e)) {
            ok = 0;
            break;
        }
        if (fprintf(file,
                    "ID=%s\n"
                    "TITLE=%s\n"
                    "DATE=%s\n"
                    "START=%s\n"
                    "END=%s\n"
                    "LOCATION=%s\n"
                    "NOTES=%s\n"
                    "REMIND_MINUTES=%s\n\n",
                    e->id, e->title, e->date, e->start, e->end,
                    e->location, e->notes, e->remind_minutes) < 0) {
            ok = 0;
            break;
        }
    }

    if (!ok) {
        fclose(file);
        unlink(tmp);
        return 0;
    }

    return finish_atomic_write(file, tmp, path);
}

static int load_events_file(const char *path, EventList *list) {
    FILE *file = fopen(path, "r");
    char line[2048];
    Event current;
    EventList file_events = {0};
    int have = 0;
    int changed = 0;

    if (!file) return errno == ENOENT;

    memset(&current, 0, sizeof current);
    while (fgets(line, sizeof line, file)) {
        char *eq;
        char *key;
        char *value;

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            if (have && event_valid_basic(&current, 0)) {
                if (!eventlist_push(&file_events, current)) {
                    fclose(file);
                    eventlist_free(&file_events);
                    return 0;
                }
            }
            memset(&current, 0, sizeof current);
            have = 0;
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        value = eq + 1;
        trim(key);
        parse_event_line(&current, key, value);
        have = 1;
    }

    if (have && event_valid_basic(&current, 0)) {
        if (!eventlist_push(&file_events, current)) {
            fclose(file);
            eventlist_free(&file_events);
            return 0;
        }
    }

    fclose(file);
    assign_missing_event_ids(&file_events, &changed);
    if (changed && !write_events_file_path(path, &file_events)) {
        eventlist_free(&file_events);
        return 0;
    }
    for (size_t i = 0; i < file_events.len; i++) {
        if (!eventlist_push(list, file_events.items[i])) {
            eventlist_free(&file_events);
            return 0;
        }
    }
    eventlist_free(&file_events);
    return 1;
}

static int load_events_for_date(Date d, EventList *list) {
    char path[PATH_BUF];

    if (!event_file_path_for_date(d, path, sizeof path)) return 0;
    if (!load_events_file(path, list)) return 0;
    if (list->len > 1) qsort(list->items, list->len, sizeof list->items[0], event_compare);
    return 1;
}

static int write_events_for_date(Date d, EventList *list) {
    char path[PATH_BUF];

    if (!ensure_config_dirs() || !ensure_event_year_dir(d)) return 0;
    if (!event_file_path_for_date(d, path, sizeof path)) return 0;
    return write_events_file_path(path, list);
}

static int load_all_events(EventList *list) {
    char base[PATH_BUF];
    DIR *years;
    struct dirent *year_ent;

    if (!events_dir(base, sizeof base)) return 0;
    years = opendir(base);
    if (!years) return errno == ENOENT;

    while ((year_ent = readdir(years))) {
        char year_path[PATH_BUF];
        DIR *days;
        struct dirent *day_ent;
        int written;

        if (year_ent->d_name[0] == '.') continue;
        written = snprintf(year_path, sizeof year_path, "%s/%s", base, year_ent->d_name);
        if (!snprintf_ok(written, sizeof year_path)) continue;

        days = opendir(year_path);
        if (!days) continue;
        while ((day_ent = readdir(days))) {
            char file_path[PATH_BUF];
            size_t n = strlen(day_ent->d_name);

            if (day_ent->d_name[0] == '.') continue;
            if (n < 5 || strcmp(day_ent->d_name + n - 4, ".cal") != 0) continue;

            written = snprintf(file_path, sizeof file_path, "%s/%s", year_path, day_ent->d_name);
            if (!snprintf_ok(written, sizeof file_path)) continue;
            load_events_file(file_path, list);
        }
        closedir(days);
    }

    closedir(years);
    if (list->len > 1) qsort(list->items, list->len, sizeof list->items[0], event_compare);
    return 1;
}

static int event_id_exists(const char *id) {
    EventList all = {0};
    int found = 0;

    if (!load_all_events(&all)) return 0;
    for (size_t i = 0; i < all.len; i++) {
        if (!strcmp(all.items[i].id, id)) {
            found = 1;
            break;
        }
    }
    eventlist_free(&all);
    return found;
}

static int day_has_events(Date d) {
    EventList list = {0};
    int result = 0;

    if (load_events_for_date(d, &list)) result = list.len > 0;
    eventlist_free(&list);
    return result;
}

static int find_event_index(EventList *list, const char *id) {
    for (size_t i = 0; i < list->len; i++) {
        if (!strcmp(list->items[i].id, id)) return (int)i;
    }
    return -1;
}

static int remove_event_from_day(Date d, const char *event_id, Event *removed) {
    EventList list = {0};
    int index;
    int ok;

    if (!load_events_for_date(d, &list)) return 0;
    index = find_event_index(&list, event_id);
    if (index < 0) {
        eventlist_free(&list);
        return 0;
    }

    if (removed) *removed = list.items[index];
    memmove(&list.items[index], &list.items[index + 1],
            (list.len - (size_t)index - 1) * sizeof list.items[0]);
    list.len--;
    ok = write_events_for_date(d, &list);
    eventlist_free(&list);
    return ok;
}

static void parse_reminder_line(Reminder *reminder, const char *key, const char *value) {
    if (!strcmp(key, "EVENT_ID")) copy_field(reminder->event_id, sizeof reminder->event_id, value);
    else if (!strcmp(key, "DUE")) copy_field(reminder->due, sizeof reminder->due, value);
    else if (!strcmp(key, "STATUS")) copy_field(reminder->status, sizeof reminder->status, value);
    else if (!strcmp(key, "FIRED_AT")) copy_field(reminder->fired_at, sizeof reminder->fired_at, value);
    else if (!strcmp(key, "FIRED_ON")) copy_field(reminder->fired_on, sizeof reminder->fired_on, value);
}

static int reminder_valid(const Reminder *r) {
    return r->event_id[0] && r->due[0] &&
           (!strcmp(r->status, "pending") ||
            !strcmp(r->status, "ringing") ||
            !strcmp(r->status, "fired") ||
            !strcmp(r->status, "missed") ||
            !strcmp(r->status, "error"));
}

static int load_reminders(ReminderList *list) {
    char path[PATH_BUF];
    FILE *file;
    char line[2048];
    Reminder current;
    int have = 0;

    if (!reminders_path(path, sizeof path)) return 0;
    file = fopen(path, "r");
    if (!file) return errno == ENOENT;

    memset(&current, 0, sizeof current);
    while (fgets(line, sizeof line, file)) {
        char *eq;
        char *key;
        char *value;

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            if (have) {
                if (!current.status[0]) snprintf(current.status, sizeof current.status, "pending");
                if (reminder_valid(&current) && !reminderlist_push(list, current)) {
                    fclose(file);
                    return 0;
                }
            }
            memset(&current, 0, sizeof current);
            have = 0;
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = line;
        value = eq + 1;
        trim(key);
        parse_reminder_line(&current, key, value);
        have = 1;
    }

    if (have) {
        if (!current.status[0]) snprintf(current.status, sizeof current.status, "pending");
        if (reminder_valid(&current) && !reminderlist_push(list, current)) {
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return 1;
}

static int write_reminders(ReminderList *list) {
    char path[PATH_BUF];
    char tmp[PATH_BUF + 64];
    FILE *file;

    if (!ensure_config_dirs() || !reminders_path(path, sizeof path)) return 0;
    if (!atomic_open_temp(path, tmp, sizeof tmp, &file)) return 0;

    for (size_t i = 0; i < list->len; i++) {
        Reminder *r = &list->items[i];
        if (!reminder_valid(r)) continue;
        if (fprintf(file,
                    "EVENT_ID=%s\n"
                    "DUE=%s\n"
                    "STATUS=%s\n"
                    "FIRED_AT=%s\n"
                    "FIRED_ON=%s\n\n",
                    r->event_id, r->due, r->status, r->fired_at, r->fired_on) < 0) {
            fclose(file);
            unlink(tmp);
            return 0;
        }
    }

    return finish_atomic_write(file, tmp, path);
}

static int find_reminder_index(ReminderList *list, const char *event_id) {
    for (size_t i = 0; i < list->len; i++) {
        if (!strcmp(list->items[i].event_id, event_id)) return (int)i;
    }
    return -1;
}

static int event_reminder_due(const Event *event, char *due, size_t due_size, time_t *event_time) {
    Date d;
    time_t start;
    time_t due_time;
    struct tm *tmv;
    int remind = -1;

    if (!parse_date(event->date, &d)) return 0;
    if (!parse_remind_minutes(event->remind_minutes, &remind) || remind < 0) return 0;
    if (!parse_time_hhmm(event->start, NULL)) return 0;

    start = date_time_value(d, event->start);
    if (start == (time_t)-1) return 0;
    due_time = start - (time_t)remind * 60;
    tmv = localtime(&due_time);
    if (!tmv) return 0;
    strftime(due, due_size, "%Y-%m-%dT%H:%M:%S", tmv);
    if (event_time) *event_time = start;
    return 1;
}

static time_t parse_due_time(const char *due) {
    struct tm tmv;
    int y, mo, d, h, mi;
    char tail;

    int sec = 0;

    if (!due) return (time_t)-1;
    if (sscanf(due, "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &sec, &tail) != 6) {
        if (sscanf(due, "%4d-%2d-%2dT%2d:%2d%c", &y, &mo, &d, &h, &mi, &tail) != 5)
            return (time_t)-1;
        sec = 0;
    }
    if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59)
        return (time_t)-1;

    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

static int update_reminder_for_event(const Event *event) {
    ReminderList reminders = {0};
    Reminder r;
    char due[20];
    int index;
    int ok;
    int has_reminder;

    has_reminder = event_reminder_due(event, due, sizeof due, NULL);
    if (!load_reminders(&reminders)) return 0;

    index = find_reminder_index(&reminders, event->id);
    if (!has_reminder) {
        if (index >= 0 &&
            (!strcmp(reminders.items[index].status, "pending") ||
             !strcmp(reminders.items[index].status, "ringing"))) {
            memmove(&reminders.items[index], &reminders.items[index + 1],
                    (reminders.len - (size_t)index - 1) * sizeof reminders.items[0]);
            reminders.len--;
        }
        ok = write_reminders(&reminders);
        reminderlist_free(&reminders);
        return ok;
    }

    if (index >= 0) {
        Reminder *existing = &reminders.items[index];
        time_t due_time;

        if (!strcmp(existing->status, "fired")) {
            ok = write_reminders(&reminders);
            reminderlist_free(&reminders);
            return ok;
        }

        snprintf(existing->due, sizeof existing->due, "%s", due);
        due_time = parse_due_time(due);
        if (!strcmp(existing->status, "ringing") &&
            due_time != (time_t)-1 && due_time <= time(NULL)) {
            snprintf(existing->status, sizeof existing->status, "ringing");
        } else {
            snprintf(existing->status, sizeof existing->status, "pending");
        }
        existing->fired_at[0] = '\0';
        existing->fired_on[0] = '\0';
    } else {
        memset(&r, 0, sizeof r);
        snprintf(r.event_id, sizeof r.event_id, "%s", event->id);
        snprintf(r.due, sizeof r.due, "%s", due);
        snprintf(r.status, sizeof r.status, "pending");
        if (!reminderlist_push(&reminders, r)) {
            reminderlist_free(&reminders);
            return 0;
        }
    }

    ok = write_reminders(&reminders);
    reminderlist_free(&reminders);
    return ok;
}

static int remove_pending_reminder(const char *event_id) {
    ReminderList reminders = {0};
    int index;
    int ok;

    if (!load_reminders(&reminders)) return 0;
    index = find_reminder_index(&reminders, event_id);
    if (index >= 0 &&
        (!strcmp(reminders.items[index].status, "pending") ||
         !strcmp(reminders.items[index].status, "ringing"))) {
        memmove(&reminders.items[index], &reminders.items[index + 1],
                (reminders.len - (size_t)index - 1) * sizeof reminders.items[0]);
        reminders.len--;
    }
    ok = write_reminders(&reminders);
    reminderlist_free(&reminders);
    return ok;
}

static int event_has_id(EventList *events, const char *event_id, Event *out) {
    for (size_t i = 0; i < events->len; i++) {
        if (!strcmp(events->items[i].id, event_id)) {
            if (out) *out = events->items[i];
            return 1;
        }
    }
    return 0;
}

static int reconcile_reminders(void) {
    EventList events = {0};
    ReminderList reminders = {0};
    ReminderList out = {0};
    time_t now = time(NULL);
    int ok = 1;

    if (!ensure_config_dirs()) return 1;
    if (!load_all_events(&events) || !load_reminders(&reminders)) {
        eventlist_free(&events);
        reminderlist_free(&reminders);
        return 1;
    }

    for (size_t i = 0; i < reminders.len; i++) {
        Reminder r = reminders.items[i];
        Event event;
        char due[20];
        time_t event_time = (time_t)-1;

        if (!event_has_id(&events, r.event_id, &event) ||
            !event_reminder_due(&event, due, sizeof due, &event_time)) {
            if (!strcmp(r.status, "fired")) {
                ok = reminderlist_push(&out, r) && ok;
            }
            continue;
        }

        if (!strcmp(r.status, "fired") || !strcmp(r.status, "error") ||
            !strcmp(r.status, "missed")) {
            ok = reminderlist_push(&out, r) && ok;
            continue;
        }

        snprintf(r.due, sizeof r.due, "%s", due);
        {
            time_t due_time = parse_due_time(due);
            if (!strcmp(r.status, "ringing") &&
                due_time != (time_t)-1 && due_time <= now) {
                snprintf(r.status, sizeof r.status, "ringing");
            } else {
                snprintf(r.status, sizeof r.status, "pending");
            }
        }
        r.fired_at[0] = '\0';
        r.fired_on[0] = '\0';
        ok = reminderlist_push(&out, r) && ok;
        (void)event_time;
    }

    for (size_t i = 0; i < events.len; i++) {
        Event *event = &events.items[i];
        char due[20];
        time_t event_time = (time_t)-1;

        if (!event_reminder_due(event, due, sizeof due, &event_time)) continue;
        if (event_time != (time_t)-1 && event_time < now) continue;
        if (find_reminder_index(&out, event->id) >= 0) continue;

        Reminder r;
        memset(&r, 0, sizeof r);
        snprintf(r.event_id, sizeof r.event_id, "%s", event->id);
        snprintf(r.due, sizeof r.due, "%s", due);
        snprintf(r.status, sizeof r.status, "pending");
        ok = reminderlist_push(&out, r) && ok;
    }

    if (ok) ok = write_reminders(&out);

    eventlist_free(&events);
    reminderlist_free(&reminders);
    reminderlist_free(&out);
    return ok ? 0 : 1;
}

static int command_exists(const char *cmd) {
    char command[256];
    int rc;

    snprintf(command, sizeof command, "command -v %s >/dev/null 2>&1", cmd);
    rc = system(command);
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static int file_contains_text(const char *path, const char *needle) {
    FILE *file = fopen(path, "r");
    char line[1024];
    int found = 0;

    if (!file) return 0;
    while (fgets(line, sizeof line, file)) {
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

static void shell_quote(const char *src, char *dst, size_t size) {
    size_t j = 0;

    if (size == 0) return;
    dst[j++] = '\'';
    for (size_t i = 0; src && src[i] && j + 5 < size; i++) {
        if (src[i] == '\'') {
            memcpy(dst + j, "'\\''", 4);
            j += 4;
        } else {
            dst[j++] = src[i];
        }
    }
    if (j + 1 < size) dst[j++] = '\'';
    dst[j] = '\0';
}

static void format_timestamp(time_t value, char *out, size_t size) {
    struct tm *tmv = localtime(&value);

    if (!out || size == 0) return;
    if (tmv) strftime(out, size, "%Y-%m-%dT%H:%M:%S", tmv);
    else snprintf(out, size, "unknown");
}

static void set_fired_fields(Reminder *r, time_t now) {
    char host[96] = "unknown";

    format_timestamp(now, r->fired_at, sizeof r->fired_at);
    gethostname(host, sizeof host);
    host[sizeof(host) - 1] = '\0';
    snprintf(r->fired_on, sizeof r->fired_on, "%s", host);
}

static int reminder_is_ringing(const Reminder *r) {
    return !strcmp(r->status, "ringing");
}

static int reminder_is_clearable(const Reminder *r, time_t now) {
    time_t due;

    if (!strcmp(r->status, "ringing") || !strcmp(r->status, "error") ||
        !strcmp(r->status, "missed"))
        return 1;

    due = parse_due_time(r->due);
    return !strcmp(r->status, "pending") && due != (time_t)-1 && due <= now;
}

static int count_ringing_reminders_list(const ReminderList *list) {
    int count = 0;

    for (size_t i = 0; i < list->len; i++) {
        if (reminder_is_ringing(&list->items[i])) count++;
    }
    return count;
}

static int ringing_reminder_count(void) {
    ReminderList reminders = {0};
    int count = 0;

    if (!load_reminders(&reminders)) return 0;
    count = count_ringing_reminders_list(&reminders);
    reminderlist_free(&reminders);
    return count;
}

static int write_alarm_pid(pid_t pid) {
    char path[PATH_BUF];
    FILE *file;

    if (!ensure_state_dir() || !alarm_pid_path(path, sizeof path)) return 0;
    file = fopen(path, "w");
    if (!file) return 0;
    fprintf(file, "PID=%ld\n", (long)pid);
    return fclose(file) == 0;
}

static int read_alarm_pid(pid_t *pid) {
    char path[PATH_BUF];
    FILE *file;
    char line[128];
    long value = -1;

    if (pid) *pid = -1;
    if (!alarm_pid_path(path, sizeof path)) return 0;
    file = fopen(path, "r");
    if (!file) return 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "PID=%ld", &value) == 1 && value > 1) {
            fclose(file);
            if (pid) *pid = (pid_t)value;
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static void remove_alarm_pid(void) {
    char path[PATH_BUF];

    if (alarm_pid_path(path, sizeof path)) unlink(path);
}

static int process_alive(pid_t pid) {
    if (pid <= 1) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

static int poll_alarm_exit(pid_t pid, const char *label, int *exit_ok, int quiet) {
    int status = 0;
    pid_t rc;

    if (exit_ok) *exit_ok = 0;
    do {
        rc = waitpid(pid, &status, WNOHANG);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) return 0;
    if (rc < 0) {
        if (errno == ECHILD && process_alive(pid)) return 0;
        if (!quiet)
            fprintf(stderr, "simplecal: %s PID %ld is no longer running (%s)\n",
                    label, (long)pid, strerror(errno));
        return 1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (!quiet)
            fprintf(stderr, "simplecal: %s PID %ld exit status: %d\n",
                    label, (long)pid, code);
        if (exit_ok) *exit_ok = code == 0;
    } else if (WIFSIGNALED(status)) {
        if (!quiet)
            fprintf(stderr, "simplecal: %s PID %ld terminated by signal: %d\n",
                    label, (long)pid, WTERMSIG(status));
    } else {
        if (!quiet)
            fprintf(stderr, "simplecal: %s PID %ld ended without a normal exit status\n",
                    label, (long)pid);
    }
    return 1;
}

static int alarm_player_running(pid_t *pid_out, int quiet) {
    pid_t pid;
    int exit_ok = 0;

    if (pid_out) *pid_out = -1;
    if (!read_alarm_pid(&pid)) return 0;
    if (poll_alarm_exit(pid, "alarm player", &exit_ok, quiet)) {
        (void)exit_ok;
        remove_alarm_pid();
        return 0;
    }
    if (!process_alive(pid)) {
        if (!quiet)
            fprintf(stderr, "simplecal: alarm player PID %ld is stale\n", (long)pid);
        remove_alarm_pid();
        return 0;
    }
    if (pid_out) *pid_out = pid;
    return 1;
}

static void stop_alarm_player(int quiet) {
    pid_t pid;

    if (!read_alarm_pid(&pid)) return;
    if (process_alive(pid)) {
        if (!quiet) fprintf(stderr, "simplecal: stopping alarm player PID=%ld\n", (long)pid);
        if (kill(-pid, SIGTERM) != 0 && errno == ESRCH) kill(pid, SIGTERM);
    }
    remove_alarm_pid();
}

static void redirect_child_stdio_to_devnull(void) {
    int fd = open("/dev/null", O_RDWR);

    if (fd < 0) return;
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
}

static AlarmStartResult start_alarm_argv(const char *label, char *const argv[], pid_t *pid_out) {
    pid_t pid;
    int errpipe[2];
    int flags;
    int child_errno = 0;
    ssize_t nread;
    struct timespec delay = { 0, 20000000L };

    if (pid_out) *pid_out = -1;
    if (pipe(errpipe) != 0) {
        fprintf(stderr, "simplecal: could not create alarm exec pipe for %s: %s\n",
                label, strerror(errno));
        return ALARM_START_FAILED;
    }
    flags = fcntl(errpipe[1], F_GETFD);
    if (flags >= 0) fcntl(errpipe[1], F_SETFD, flags | FD_CLOEXEC);

    fprintf(stderr, "simplecal: trying alarm player: %s\n", label);
    pid = fork();
    if (pid == 0) {
        close(errpipe[0]);
        setpgid(0, 0);
        redirect_child_stdio_to_devnull();
        execvp(argv[0], argv);
        child_errno = errno;
        write(errpipe[1], &child_errno, sizeof child_errno);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "simplecal: could not fork for %s: %s\n", label, strerror(errno));
        close(errpipe[0]);
        close(errpipe[1]);
        return ALARM_START_FAILED;
    }

    close(errpipe[1]);
    nread = read(errpipe[0], &child_errno, sizeof child_errno);
    close(errpipe[0]);
    if (nread > 0) {
        int status = 0;
        fprintf(stderr, "simplecal: exec failed for %s: %s\n", argv[0], strerror(child_errno));
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        fprintf(stderr, "simplecal: %s exit status: 127\n", label);
        return ALARM_START_FAILED;
    }

    if (!write_alarm_pid(pid)) {
        fprintf(stderr, "simplecal: could not write alarm PID file\n");
    }
    fprintf(stderr, "simplecal: started alarm player PID=%ld (%s)\n", (long)pid, label);

    for (int i = 0; i < 10; i++) {
        int exit_ok = 0;
        if (poll_alarm_exit(pid, label, &exit_ok, 0)) {
            remove_alarm_pid();
            return exit_ok ? ALARM_START_EXITED_OK : ALARM_START_FAILED;
        }
        nanosleep(&delay, NULL);
    }

    if (pid_out) *pid_out = pid;
    return ALARM_START_RUNNING;
}

static AlarmStartResult start_alarm_shell_player(const char *player, const char *path, pid_t *pid_out) {
    char quoted_path[PATH_BUF + 16];
    char command[PATH_BUF * 2];
    char label[512];
    char *argv[] = { "sh", "-c", command, NULL };

    shell_quote(path, quoted_path, sizeof quoted_path);
    if (!snprintf_ok(snprintf(command, sizeof command, "exec %s %s", player, quoted_path),
                     sizeof command)) {
        fprintf(stderr, "simplecal: SIMPLECAL_ALARM_PLAYER command is too long\n");
        return ALARM_START_FAILED;
    }
    snprintf(label, sizeof label, "SIMPLECAL_ALARM_PLAYER=%s", player);
    return start_alarm_argv(label, argv, pid_out);
}

static void ensure_alarm_audio_environment(void) {
    char runtime_dir[128];
    char bus[192];
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");

    if (!xdg_runtime || !*xdg_runtime) {
        snprintf(runtime_dir, sizeof runtime_dir, "/run/user/%ld", (long)getuid());
        if (setenv("XDG_RUNTIME_DIR", runtime_dir, 0) == 0) {
            fprintf(stderr, "simplecal: set XDG_RUNTIME_DIR=%s\n", runtime_dir);
            xdg_runtime = getenv("XDG_RUNTIME_DIR");
        }
    }

    if ((!getenv("DBUS_SESSION_BUS_ADDRESS") || !*getenv("DBUS_SESSION_BUS_ADDRESS")) &&
        xdg_runtime && *xdg_runtime) {
        snprintf(bus, sizeof bus, "unix:path=%s/bus", xdg_runtime);
        if (setenv("DBUS_SESSION_BUS_ADDRESS", bus, 0) == 0) {
            fprintf(stderr, "simplecal: set DBUS_SESSION_BUS_ADDRESS=%s\n", bus);
        }
    }
}

static void log_alarm_audio_environment(void) {
    const char *names[] = {
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
        "PULSE_SERVER",
        "PIPEWIRE_REMOTE",
        "PIPEWIRE_RUNTIME_DIR",
        "WAYLAND_DISPLAY",
        "DISPLAY"
    };

    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *value = getenv(names[i]);
        fprintf(stderr, "simplecal: env %s=%s\n", names[i], value && *value ? value : "(unset)");
    }
}

static int ensure_alarm_player_running(pid_t *pid_out) {
    char path[PATH_BUF];
    const char *custom_player = getenv("SIMPLECAL_ALARM_PLAYER");
    int tried = 0;
    pid_t pid = -1;
    AlarmStartResult result;

    if (alarm_player_running(&pid, 0)) {
        if (pid_out) *pid_out = pid;
        return 1;
    }
    if (!alarm_path(path, sizeof path)) {
        fprintf(stderr, "simplecal: HOME is not set; cannot find alarm MP3.\n");
        return 0;
    }
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "simplecal: alarm MP3 missing at %s\n", path);
        return 0;
    }

    fprintf(stderr, "simplecal: alarm path: %s\n", path);
    ensure_alarm_audio_environment();
    log_alarm_audio_environment();

    if (custom_player && *custom_player) {
        result = start_alarm_shell_player(custom_player, path, pid_out);
        return result != ALARM_START_FAILED;
    }

    if (command_exists("mpv")) {
        char *argv[] = {
            "mpv", "--no-config", "--no-video", "--audio-display=no",
            "--audio-device=pipewire", "--msg-level=ao=debug,cplayer=info",
            "--volume=100", path, NULL
        };
        tried = 1;
        result = start_alarm_argv("mpv pipewire", argv, pid_out);
        if (result != ALARM_START_FAILED) return 1;

        {
            char *pulse_argv[] = {
                "mpv", "--no-config", "--no-video", "--audio-display=no",
                "--audio-device=pulse", "--msg-level=ao=debug,cplayer=info",
                "--volume=100", path, NULL
            };
            result = start_alarm_argv("mpv pulse", pulse_argv, pid_out);
            if (result != ALARM_START_FAILED) return 1;
        }

        {
            char *auto_argv[] = {
                "mpv", "--no-config", "--no-video", "--audio-display=no",
                "--msg-level=ao=debug,cplayer=info", "--volume=100", path, NULL
            };
            result = start_alarm_argv("mpv auto", auto_argv, pid_out);
            if (result != ALARM_START_FAILED) return 1;
        }
    } else {
        fprintf(stderr, "simplecal: mpv not found; trying fallback players\n");
    }

    if (command_exists("pw-play")) {
        char *argv[] = { "pw-play", path, NULL };
        tried = 1;
        result = start_alarm_argv("pw-play", argv, pid_out);
        if (result != ALARM_START_FAILED) return 1;
    }

    if (command_exists("paplay")) {
        char *argv[] = { "paplay", path, NULL };
        tried = 1;
        result = start_alarm_argv("paplay", argv, pid_out);
        if (result != ALARM_START_FAILED) return 1;
    }

    if (command_exists("ffplay")) {
        char *argv[] = { "ffplay", "-nodisp", "-autoexit", path, NULL };
        tried = 1;
        result = start_alarm_argv("ffplay", argv, pid_out);
        if (result != ALARM_START_FAILED) return 1;
    }

    if (!tried) {
        fprintf(stderr, "simplecal: no alarm player found (tried mpv, paplay, pw-play, ffplay)\n");
    } else {
        fprintf(stderr, "simplecal: all alarm players failed; reminder remains ringing until retries are exhausted\n");
    }
    return 0;
}

static int acquire_check_lock(int *fd_out) {
    char path[PATH_BUF];
    int fd;

    if (fd_out) *fd_out = -1;
    if (!ensure_state_dir() || !check_lock_path(path, sizeof path)) return -1;
    fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        fprintf(stderr, "simplecal: could not open reminder lock: %s\n", strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            fprintf(stderr, "simplecal: reminder checker already running\n");
            close(fd);
            return 0;
        }
        fprintf(stderr, "simplecal: could not lock reminder checker: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (fd_out) *fd_out = fd;
    return 1;
}

static void mark_ringing_reminders_error(ReminderList *reminders) {
    for (size_t i = 0; i < reminders->len; i++) {
        Reminder *r = &reminders->items[i];
        if (reminder_is_ringing(r)) snprintf(r->status, sizeof r->status, "error");
    }
}

static int check_reminders(void) {
    int lock_fd = -1;
    int lock_state;
    int consecutive_player_failures = 0;
    int exit_code = 0;

    if (!ensure_config_dirs()) return 1;
    lock_state = acquire_check_lock(&lock_fd);
    if (lock_state == 0) return 0;
    if (lock_state < 0) return 1;

    for (;;) {
        ReminderList reminders = {0};
        time_t now = time(NULL);
        char nowbuf[20];
        int changed = 0;
        int ringing_count = 0;

        if (!load_reminders(&reminders)) {
            exit_code = 1;
            break;
        }

        format_timestamp(now, nowbuf, sizeof nowbuf);
        for (size_t i = 0; i < reminders.len; i++) {
            Reminder *r = &reminders.items[i];
            time_t due;

            if (strcmp(r->status, "pending")) continue;
            due = parse_due_time(r->due);
            if (due == (time_t)-1 || due > now) continue;

            snprintf(r->status, sizeof r->status, "ringing");
            r->fired_at[0] = '\0';
            r->fired_on[0] = '\0';
            changed = 1;
            fprintf(stderr,
                    "simplecal: reminder due EVENT_ID=%s DUE=%s now=%s drift_seconds=%ld\n",
                    r->event_id, r->due, nowbuf, (long)(now - due));
        }

        ringing_count = count_ringing_reminders_list(&reminders);
        if (changed && !write_reminders(&reminders)) {
            reminderlist_free(&reminders);
            exit_code = 1;
            break;
        }

        if (ringing_count <= 0) {
            stop_alarm_player(0);
            reminderlist_free(&reminders);
            break;
        }

        {
            pid_t player_pid = -1;
            if (ensure_alarm_player_running(&player_pid)) {
                consecutive_player_failures = 0;
                fprintf(stderr, "simplecal: ringing %d reminder(s); player PID=%ld\n",
                        ringing_count, (long)player_pid);
            } else {
                consecutive_player_failures++;
                fprintf(stderr,
                        "simplecal: alarm playback failed while %d reminder(s) ringing; failure %d\n",
                        ringing_count, consecutive_player_failures);
                if (consecutive_player_failures >= 3) {
                    mark_ringing_reminders_error(&reminders);
                    write_reminders(&reminders);
                    stop_alarm_player(0);
                    reminderlist_free(&reminders);
                    exit_code = 1;
                    break;
                }
            }
        }

        reminderlist_free(&reminders);
        sleep(1);
    }

    if (lock_fd >= 0) close(lock_fd);
    return exit_code;
}

static int clear_reminders(const char *event_id, int *cleared_count, int quiet) {
    ReminderList reminders = {0};
    time_t now = time(NULL);
    int cleared = 0;
    int ok;

    if (cleared_count) *cleared_count = 0;
    if (!ensure_config_dirs()) return 1;
    if (!load_reminders(&reminders)) return 1;

    for (size_t i = 0; i < reminders.len; i++) {
        Reminder *r = &reminders.items[i];

        if (event_id && strcmp(r->event_id, event_id)) continue;
        if (!reminder_is_clearable(r, now)) continue;

        snprintf(r->status, sizeof r->status, "fired");
        set_fired_fields(r, now);
        if (!quiet)
            fprintf(stderr, "simplecal: cleared reminder EVENT_ID=%s at %s\n",
                    r->event_id, r->fired_at);
        cleared++;
    }

    ok = write_reminders(&reminders);
    if (cleared_count) *cleared_count = cleared;
    if (cleared > 0 && count_ringing_reminders_list(&reminders) == 0) stop_alarm_player(quiet);
    reminderlist_free(&reminders);

    if (!ok) return 1;
    if (!quiet && event_id && cleared == 0) {
        fprintf(stderr, "simplecal: no ringing reminder found for EVENT_ID=%s\n", event_id);
    } else if (!quiet && !event_id && cleared == 0) {
        fprintf(stderr, "simplecal: no ringing reminders to clear\n");
    }
    return 0;
}

static int run_command_ok(const char *cmd) {
    int rc = system(cmd);
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static int run_command_ok_maybe_quiet(const char *cmd, int quiet) {
    char quiet_cmd[1024];

    if (!quiet) return run_command_ok(cmd);
    if (!snprintf_ok(snprintf(quiet_cmd, sizeof quiet_cmd, "%s >/dev/null 2>&1", cmd),
                     sizeof quiet_cmd))
        return 0;
    return run_command_ok(quiet_cmd);
}

static int write_text_atomic(const char *path, const char *text) {
    char tmp[PATH_BUF + 64];
    FILE *file;

    if (!atomic_open_temp(path, tmp, sizeof tmp, &file)) return 0;
    if (fputs(text, file) == EOF) {
        fclose(file);
        unlink(tmp);
        return 0;
    }
    return finish_atomic_write(file, tmp, path);
}

static int install_systemd_reminders(int quiet) {
    char dir[PATH_BUF];
    char service_path[PATH_BUF];
    char timer_path[PATH_BUF];
    const char *service_text =
        "[Unit]\n"
        "Description=SimpleCal reminder check\n"
        "\n"
        "[Service]\n"
        "# SIMPLECAL_REMINDER_SERVICE_VERSION=3\n"
        "Type=oneshot\n"
        "Environment=XDG_RUNTIME_DIR=%t\n"
        "Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=%t/bus\n"
        "PassEnvironment=PULSE_SERVER PIPEWIRE_REMOTE WAYLAND_DISPLAY DISPLAY XAUTHORITY SIMPLECAL_ALARM_PLAYER\n"
        "ExecStart=%h/.local/bin/simplecal --check-reminders\n";
    const char *timer_text =
        "[Unit]\n"
        "Description=Run SimpleCal reminder alarms\n"
        "\n"
        "[Timer]\n"
        "OnBootSec=10s\n"
        "OnUnitActiveSec=1s\n"
        "AccuracySec=1s\n"
        "Persistent=true\n"
        "Unit=simplecal-reminders.service\n"
        "\n"
        "[Install]\n"
        "WantedBy=timers.target\n";
    int written;

    if (!home_path(dir, sizeof dir, ".config/systemd/user")) return 0;
    if (!mkdirs(dir)) return 0;

    written = snprintf(service_path, sizeof service_path, "%s/simplecal-reminders.service", dir);
    if (!snprintf_ok(written, sizeof service_path)) return 0;
    written = snprintf(timer_path, sizeof timer_path, "%s/simplecal-reminders.timer", dir);
    if (!snprintf_ok(written, sizeof timer_path)) return 0;

    if (!write_text_atomic(service_path, service_text)) return 0;
    if (!write_text_atomic(timer_path, timer_text)) return 0;
    if (!run_command_ok_maybe_quiet("systemctl --user daemon-reload", quiet)) return 0;
    if (!run_command_ok_maybe_quiet("systemctl --user enable --now simplecal-reminders.timer", quiet)) return 0;

    if (!quiet) printf("simplecal: installed systemd user timer backend\n");
    return 1;
}

static int install_cron_reminders(int quiet) {
    char tmp[PATH_BUF];
    FILE *in;
    FILE *out;
    char line[2048];
    const char *cron_line = "* * * * * ~/.local/bin/simplecal --check-reminders >/dev/null 2>&1\n";
    int fd;
    int rc;
    int ok = 1;

    snprintf(tmp, sizeof tmp, "/tmp/simplecal-cron.XXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0) return 0;
    out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(tmp);
        return 0;
    }

    in = popen("crontab -l 2>/dev/null", "r");
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (strstr(line, "simplecal --check-reminders")) continue;
            fputs(line, out);
        }
        pclose(in);
    }

    fputs(cron_line, out);
    if (fclose(out) != 0) ok = 0;

    if (ok) {
        pid_t pid = fork();
        int status = 0;
        if (pid == 0) {
            if (quiet) {
                int nullfd = open("/dev/null", O_WRONLY);
                if (nullfd >= 0) {
                    dup2(nullfd, STDOUT_FILENO);
                    dup2(nullfd, STDERR_FILENO);
                    close(nullfd);
                }
            }
            execlp("crontab", "crontab", tmp, (char *)NULL);
            _exit(127);
        }
        if (pid < 0) ok = 0;
        while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (pid > 0) ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    rc = unlink(tmp);
    (void)rc;
    if (ok && !quiet) printf("simplecal: installed cron reminder backend\n");
    return ok;
}

static int install_reminders(int quiet) {
    if (!ensure_config_dirs()) {
        if (!quiet) fprintf(stderr, "simplecal: could not create ~/.config/simplecal\n");
        return 1;
    }

    if (run_command_ok("systemctl --user show-environment >/dev/null 2>&1")) {
        if (install_systemd_reminders(quiet)) return 0;
        if (!quiet) fprintf(stderr, "simplecal: systemd user timer install failed; trying cron fallback\n");
    }

    if (!command_exists("crontab")) {
        if (!quiet) fprintf(stderr, "simplecal: systemd user is unavailable and crontab was not found\n");
        return 1;
    }
    if (!install_cron_reminders(quiet)) {
        if (!quiet) fprintf(stderr, "simplecal: cron reminder install failed\n");
        return 1;
    }
    return 0;
}

static int systemd_reminder_installed(void) {
    char service_path[PATH_BUF];
    char timer_path[PATH_BUF];
    char dir[PATH_BUF];
    int written;

    if (!home_path(dir, sizeof dir, ".config/systemd/user")) return 0;
    written = snprintf(service_path, sizeof service_path, "%s/simplecal-reminders.service", dir);
    if (!snprintf_ok(written, sizeof service_path)) return 0;
    written = snprintf(timer_path, sizeof timer_path, "%s/simplecal-reminders.timer", dir);
    if (!snprintf_ok(written, sizeof timer_path)) return 0;

    return access(timer_path, R_OK) == 0 &&
           access(service_path, R_OK) == 0 &&
           file_contains_text(service_path, "SIMPLECAL_REMINDER_SERVICE_VERSION=3");
}

static int cron_reminder_installed(void) {
    FILE *in = popen("crontab -l 2>/dev/null", "r");
    char line[2048];
    int found = 0;

    if (!in) return 0;
    while (fgets(line, sizeof line, in)) {
        if (strstr(line, "simplecal --check-reminders")) {
            found = 1;
            break;
        }
    }
    pclose(in);
    return found;
}

static int background_reminders_installed(void) {
    return systemd_reminder_installed() || cron_reminder_installed();
}

static int event_has_future_reminder(const Event *event) {
    char due[20];
    time_t due_time;

    if (!event_reminder_due(event, due, sizeof due, NULL)) return 0;
    due_time = parse_due_time(due);
    return due_time != (time_t)-1 && due_time > time(NULL);
}

static int any_future_reminders(void) {
    EventList events = {0};
    int found = 0;

    if (!load_all_events(&events)) return 0;
    for (size_t i = 0; i < events.len; i++) {
        if (event_has_future_reminder(&events.items[i])) {
            found = 1;
            break;
        }
    }
    eventlist_free(&events);
    return found;
}

static void maybe_warn_background_reminders(App *app) {
    if (!background_reminders_installed() && any_future_reminders()) {
        app_set_status(app, "Background reminders not installed. Run simplecal --install-reminders.");
    }
}

static int auto_install_background_reminders(App *app) {
    if (background_reminders_installed()) return 0;
    if (reminders_auto_install_attempted()) return 0;

    mark_reminders_auto_install_attempted();
    if (install_reminders(1) == 0) return 0;

    app_set_status(app, "Background reminders could not be installed automatically. Run simplecal --install-reminders.");
    return 1;
}

static void make_slug(const char *title, char *out, size_t size) {
    size_t j = 0;
    int dash = 0;

    for (size_t i = 0; title && title[i] && j + 1 < size; i++) {
        unsigned char c = (unsigned char)title[i];
        if (isalnum(c)) {
            out[j++] = (char)tolower(c);
            dash = 0;
        } else if (!dash && j > 0) {
            out[j++] = '-';
            dash = 1;
        }
        if (j >= 48) break;
    }
    while (j > 0 && out[j - 1] == '-') j--;
    if (j == 0) {
        snprintf(out, size, "event");
    } else {
        out[j] = '\0';
    }
}

static void make_event_id_base(const Event *event, char *out, size_t size) {
    char slug[64];
    char hhmm[5] = "0000";

    make_slug(event->title, slug, sizeof slug);
    if (event->start[0]) snprintf(hhmm, sizeof hhmm, "%.2s%.2s", event->start, event->start + 3);
    snprintf(out, size, "%s-%s-%s", event->date, hhmm, slug);
}

static void generate_event_id(const Event *event, char *out, size_t size) {
    char base[ID_LEN];
    int suffix = 1;

    make_event_id_base(event, base, sizeof base);
    snprintf(out, size, "%s", base);

    while (event_id_exists(out)) {
        suffix++;
        snprintf(out, size, "%s-%d", base, suffix);
    }
}

static void app_set_status(App *app, const char *message) {
    snprintf(app->status, sizeof app->status, "%s", message ? message : "");
}

static void reload_day_events(App *app) {
    char selected_id[ID_LEN] = "";

    if (app->event_cursor < app->day_events.len) {
        snprintf(selected_id, sizeof selected_id, "%s", app->day_events.items[app->event_cursor].id);
    }

    eventlist_free(&app->day_events);
    load_events_for_date(app->selected, &app->day_events);

    app->event_cursor = 0;
    if (selected_id[0]) {
        int idx = find_event_index(&app->day_events, selected_id);
        if (idx >= 0) app->event_cursor = (size_t)idx;
    }
    if (app->event_cursor >= app->day_events.len && app->day_events.len > 0)
        app->event_cursor = app->day_events.len - 1;
    if (app->day_events.len == 0) app->focus = FOCUS_DAY;
}

static void set_selected_date(App *app, Date d) {
    app->selected = d;
    reload_day_events(app);
}

static int prompt_line_prefill(const char *label, const char *prefill, char *out, size_t outsz) {
    int h, w;
    int pos;

    if (!out || outsz == 0) return 0;
    snprintf(out, outsz, "%s", prefill ? prefill : "");
    pos = (int)strlen(out);

    echo();
    curs_set(1);
    timeout(-1);

    while (1) {
        int ch;
        int label_len;
        int input_x;
        int max_chars;

        getmaxyx(stdscr, h, w);
        label_len = (int)strlen(label);
        input_x = label_len + 3;
        max_chars = w - input_x - 1;
        if (max_chars < 8) max_chars = 8;

        move(h - 3, 0);
        clrtoeol();
        mvprintw(h - 3, 1, "%s", label);
        mvaddnstr(h - 3, input_x, out, max_chars);
        move(h - 3, input_x + pos);
        refresh();

        ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) break;
        if (ch == 27) {
            out[0] = '\0';
            noecho();
            curs_set(0);
            timeout(-1);
            return 0;
        }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && pos > 0) {
            memmove(out + pos - 1, out + pos, strlen(out + pos) + 1);
            pos--;
            continue;
        }
        if (ch == KEY_LEFT && pos > 0) {
            pos--;
            continue;
        }
        if (ch == KEY_RIGHT && pos < (int)strlen(out)) {
            pos++;
            continue;
        }
        if (ch >= 0 && ch <= UCHAR_MAX && isprint((unsigned char)ch) && strlen(out) + 1 < outsz) {
            memmove(out + pos + 1, out + pos, strlen(out + pos) + 1);
            out[pos++] = (char)ch;
        }
    }

    noecho();
    curs_set(0);
    timeout(-1);
    clean_field(out);
    return 1;
}

static int prompt_yes_no(const char *message) {
    int h, w;
    int ch;

    getmaxyx(stdscr, h, w);
    move(h - 3, 0);
    clrtoeol();
    mvprintw(h - 3, 1, "%s [y/N]", message);
    refresh();
    timeout(-1);
    ch = getch();
    timeout(-1);
    return ch == 'y' || ch == 'Y';
}

static Event *selected_event(App *app) {
    if (app->view == VIEW_SEARCH) {
        if (app->search_cursor < app->search_results.len)
            return &app->search_results.items[app->search_cursor];
        return NULL;
    }
    if (app->event_cursor < app->day_events.len) return &app->day_events.items[app->event_cursor];
    return NULL;
}

static void add_event(App *app) {
    Event event;
    char datebuf[11];

    memset(&event, 0, sizeof event);
    format_date(app->selected, datebuf, sizeof datebuf);
    snprintf(event.date, sizeof event.date, "%s", datebuf);

    app->detail_edit_event = event;
    app->detail_event_id[0] = '\0';
    app->detail_new_event = 1;
    app->detail_editing = 1;
    app->detail_field = DETAIL_FIELD_TITLE;
    app->detail_cursor = 0;
    app->detail_dirty = 0;
    app->view = VIEW_EVENT_DETAIL;
    app->focus = FOCUS_DAY;
    app_set_status(app, "Editing event.");
    curs_set(1);
}

static int normalize_event_for_storage(Event *event, int require_id, char *error, size_t error_size) {
    Date d;
    int start_min = 0;
    int end_min = 0;
    int remind = -1;

    clean_field(event->title);
    clean_field(event->date);
    clean_field(event->start);
    clean_field(event->end);
    clean_field(event->location);
    clean_field(event->notes);
    clean_field(event->remind_minutes);

    if (!event->title[0]) {
        snprintf(error, error_size, "Title is required.");
        return 0;
    }
    if (!parse_date(event->date, &d)) {
        snprintf(error, error_size, "Invalid date.");
        return 0;
    }
    format_date(d, event->date, sizeof event->date);

    if (!parse_time_hhmm(event->start, &start_min)) {
        snprintf(error, error_size, "Invalid start time.");
        return 0;
    }
    if (event->start[0])
        snprintf(event->start, sizeof event->start, "%02d:%02d", start_min / 60, start_min % 60);

    if (!parse_time_hhmm(event->end, &end_min)) {
        snprintf(error, error_size, "Invalid end time.");
        return 0;
    }
    if (event->end[0])
        snprintf(event->end, sizeof event->end, "%02d:%02d", end_min / 60, end_min % 60);

    if (event->start[0] && event->end[0] && end_min < start_min) {
        snprintf(error, error_size, "End time must not be before start time.");
        return 0;
    }

    if (!parse_remind_minutes(event->remind_minutes, &remind)) {
        snprintf(error, error_size, "Invalid reminder minutes.");
        return 0;
    }
    if (remind >= 0) snprintf(event->remind_minutes, sizeof event->remind_minutes, "%d", remind);
    else event->remind_minutes[0] = '\0';

    if (!event_valid_basic(event, require_id)) {
        snprintf(error, error_size, "Event is invalid.");
        return 0;
    }
    return 1;
}

static int save_new_event(App *app, Event *event) {
    EventList list = {0};
    Date d;

    if (!event || !event->title[0]) {
        app_set_status(app, "Title is required.");
        return 0;
    }
    if (!parse_date(event->date, &d)) {
        app_set_status(app, "Invalid date.");
        return 0;
    }

    generate_event_id(event, event->id, sizeof event->id);
    if (!load_events_for_date(d, &list)) {
        app_set_status(app, "Could not read event file.");
        return 0;
    }
    if (!eventlist_push(&list, *event) || !write_events_for_date(d, &list)) {
        eventlist_free(&list);
        app_set_status(app, "Could not save event.");
        return 0;
    }
    eventlist_free(&list);
    update_reminder_for_event(event);
    set_selected_date(app, d);
    maybe_warn_background_reminders(app);
    return 1;
}

static int save_event_changes(App *app, const Event *edited) {
    Event stored;
    Date old_date;
    Date new_date;
    EventList list = {0};
    int idx;

    if (!edited || !edited->id[0]) {
        app_set_status(app, "No event selected.");
        return 0;
    }
    if (!find_event_by_id(edited->id, &stored)) {
        app_set_status(app, "Event was not found.");
        return 0;
    }
    if (!parse_date(stored.date, &old_date) || !parse_date(edited->date, &new_date)) {
        app_set_status(app, "Event has an invalid stored date.");
        return 0;
    }

    if (date_cmp(old_date, new_date) != 0) {
        if (!remove_event_from_day(old_date, edited->id, NULL)) {
            app_set_status(app, "Could not update old event file.");
            return 0;
        }
        if (!load_events_for_date(new_date, &list) ||
            !eventlist_push(&list, *edited) ||
            !write_events_for_date(new_date, &list)) {
            eventlist_free(&list);
            app_set_status(app, "Could not write updated event.");
            return 0;
        }
        eventlist_free(&list);
    } else {
        if (!load_events_for_date(old_date, &list)) {
            app_set_status(app, "Could not read event file.");
            return 0;
        }
        idx = find_event_index(&list, edited->id);
        if (idx < 0) {
            eventlist_free(&list);
            app_set_status(app, "Event was not found.");
            return 0;
        }
        list.items[idx] = *edited;
        if (!write_events_for_date(old_date, &list)) {
            eventlist_free(&list);
            app_set_status(app, "Could not save event.");
            return 0;
        }
        eventlist_free(&list);
    }

    update_reminder_for_event(edited);
    set_selected_date(app, new_date);
    return 1;
}

static void delete_event(App *app) {
    Event *event = selected_event(app);
    Date d;
    char id[ID_LEN];

    if (!event) {
        app_set_status(app, "No event selected.");
        return;
    }
    if (!prompt_yes_no("Delete selected event?")) {
        app_set_status(app, "Delete canceled.");
        return;
    }
    if (!parse_date(event->date, &d)) {
        app_set_status(app, "Event has an invalid stored date.");
        return;
    }

    snprintf(id, sizeof id, "%s", event->id);
    if (!remove_event_from_day(d, id, NULL)) {
        app_set_status(app, "Could not delete event.");
        return;
    }
    remove_pending_reminder(id);
    set_selected_date(app, d);
    if (app->view == VIEW_SEARCH) app->view = VIEW_MONTH;
    app_set_status(app, "Event deleted.");
}

static int event_matches(const Event *e, const char *term) {
    char haystack[TITLE_LEN + LOCATION_LEN + NOTES_LEN + 4];
    char lower_term[128];

    snprintf(haystack, sizeof haystack, "%s %s %s", e->title, e->location, e->notes);
    for (char *p = haystack; *p; p++) *p = (char)tolower((unsigned char)*p);
    snprintf(lower_term, sizeof lower_term, "%s", term);
    for (char *p = lower_term; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(haystack, lower_term) != NULL;
}

static void run_search(App *app) {
    EventList all = {0};
    char term[sizeof app->search_term];

    if (!prompt_line_prefill("Search:", app->search_term, term, sizeof term)) return;
    if (!term[0]) {
        app_set_status(app, "Search canceled.");
        return;
    }

    eventlist_free(&app->search_results);
    snprintf(app->search_term, sizeof app->search_term, "%s", term);

    if (!load_all_events(&all)) {
        app_set_status(app, "Could not read events.");
        return;
    }
    for (size_t i = 0; i < all.len; i++) {
        if (event_matches(&all.items[i], term)) {
            eventlist_push(&app->search_results, all.items[i]);
        }
    }
    eventlist_free(&all);
    app->search_cursor = 0;
    app->search_top = 0;
    app->view = VIEW_SEARCH;
    if (app->search_results.len == 0) app_set_status(app, "No search results.");
    else app_set_status(app, "Search results.");
}

static void open_search_result(App *app) {
    Event *event = selected_event(app);
    Date d;

    if (!event) return;
    if (!parse_date(event->date, &d)) return;
    app->view = VIEW_MONTH;
    app->focus = FOCUS_EVENT;
    set_selected_date(app, d);
    for (size_t i = 0; i < app->day_events.len; i++) {
        if (!strcmp(app->day_events.items[i].id, event->id)) {
            app->event_cursor = i;
            break;
        }
    }
}

static int is_back_key(int ch) {
    return ch == KEY_BACKSPACE || ch == 127 || ch == 8;
}

static int find_event_by_id(const char *event_id, Event *out) {
    EventList all = {0};
    int found = 0;

    if (!event_id || !event_id[0]) return 0;
    if (!load_all_events(&all)) return 0;
    found = event_has_id(&all, event_id, out);
    eventlist_free(&all);
    return found;
}

static int open_event_detail(App *app, const Event *event) {
    Event copy;
    Date d;
    int idx;

    if (!event || !event->id[0]) return 0;
    copy = *event;
    if (!parse_date(copy.date, &d)) return 0;

    set_selected_date(app, d);
    idx = find_event_index(&app->day_events, copy.id);
    if (idx >= 0) app->event_cursor = (size_t)idx;
    app->focus = FOCUS_EVENT;
    snprintf(app->detail_event_id, sizeof app->detail_event_id, "%s", copy.id);
    app->view = VIEW_EVENT_DETAIL;
    app->detail_editing = 0;
    app->detail_field = DETAIL_FIELD_TITLE;
    app->detail_cursor = 0;
    app->detail_dirty = 0;
    app->detail_new_event = 0;
    return 1;
}

static int open_event_detail_by_id(App *app, const char *event_id) {
    Event event;

    if (!find_event_by_id(event_id, &event)) return 0;
    return open_event_detail(app, &event);
}

static int open_selected_event_detail(App *app) {
    Event *event = selected_event(app);
    Event copy;

    if (!event) return 0;
    copy = *event;
    return open_event_detail(app, &copy);
}

static void close_event_detail(App *app) {
    app->view = VIEW_MONTH;
    app->focus = FOCUS_EVENT;
    app->detail_event_id[0] = '\0';
    app->detail_editing = 0;
    app->detail_cursor = 0;
    app->detail_dirty = 0;
    app->detail_new_event = 0;
    curs_set(0);
    reload_day_events(app);
}

static int load_detail_event(App *app, Event *out) {
    Event *event;

    if (app->detail_new_event) {
        if (out) *out = app->detail_edit_event;
        return 1;
    }
    if (!app->detail_event_id[0]) return 0;

    event = selected_event(app);
    if (event && !strcmp(event->id, app->detail_event_id)) {
        if (out) *out = *event;
        return 1;
    }

    return find_event_by_id(app->detail_event_id, out);
}

static char *detail_field_value(Event *event, DetailField field, size_t *size) {
    if (size) *size = 0;
    switch (field) {
    case DETAIL_FIELD_TITLE:
        if (size) *size = sizeof event->title;
        return event->title;
    case DETAIL_FIELD_DATE:
        if (size) *size = sizeof event->date;
        return event->date;
    case DETAIL_FIELD_START:
        if (size) *size = sizeof event->start;
        return event->start;
    case DETAIL_FIELD_END:
        if (size) *size = sizeof event->end;
        return event->end;
    case DETAIL_FIELD_LOCATION:
        if (size) *size = sizeof event->location;
        return event->location;
    case DETAIL_FIELD_NOTES:
        if (size) *size = sizeof event->notes;
        return event->notes;
    case DETAIL_FIELD_REMIND:
        if (size) *size = sizeof event->remind_minutes;
        return event->remind_minutes;
    case DETAIL_FIELD_COUNT:
        break;
    }
    return NULL;
}

static const char *detail_field_label(DetailField field) {
    switch (field) {
    case DETAIL_FIELD_TITLE: return "Title:";
    case DETAIL_FIELD_DATE: return "Date:";
    case DETAIL_FIELD_START: return "Start:";
    case DETAIL_FIELD_END: return "End:";
    case DETAIL_FIELD_LOCATION: return "Location:";
    case DETAIL_FIELD_NOTES: return "Notes:";
    case DETAIL_FIELD_REMIND: return "Remind minutes before:";
    case DETAIL_FIELD_COUNT: break;
    }
    return "";
}

static void clamp_detail_cursor(App *app) {
    char *value = detail_field_value(&app->detail_edit_event, app->detail_field, NULL);
    int len = value ? (int)strlen(value) : 0;

    if (app->detail_cursor < 0) app->detail_cursor = 0;
    if (app->detail_cursor > len) app->detail_cursor = len;
}

static int event_content_equal(const Event *a, const Event *b) {
    return !strcmp(a->id, b->id) &&
           !strcmp(a->title, b->title) &&
           !strcmp(a->date, b->date) &&
           !strcmp(a->start, b->start) &&
           !strcmp(a->end, b->end) &&
           !strcmp(a->location, b->location) &&
           !strcmp(a->notes, b->notes) &&
           !strcmp(a->remind_minutes, b->remind_minutes);
}

static int begin_detail_edit(App *app) {
    Event event;
    char *value;

    if (!load_detail_event(app, &event)) {
        app_set_status(app, "Event was not found.");
        return 0;
    }
    app->detail_edit_event = event;
    app->detail_editing = 1;
    app->detail_field = DETAIL_FIELD_TITLE;
    app->detail_dirty = 0;
    app->detail_new_event = 0;
    value = detail_field_value(&app->detail_edit_event, app->detail_field, NULL);
    app->detail_cursor = value ? (int)strlen(value) : 0;
    app_set_status(app, "Editing event.");
    curs_set(1);
    return 1;
}

static void cancel_new_event_detail(App *app) {
    app->view = VIEW_MONTH;
    app->focus = FOCUS_DAY;
    app->detail_event_id[0] = '\0';
    app->detail_editing = 0;
    app->detail_cursor = 0;
    app->detail_dirty = 0;
    app->detail_new_event = 0;
    curs_set(0);
    reload_day_events(app);
    app_set_status(app, "Event canceled.");
}


static void cancel_detail_edit_without_validation(App *app) {
    if (app->detail_new_event || !app->detail_event_id[0]) {
        cancel_new_event_detail(app);
        return;
    }

    app->detail_editing = 0;
    app->detail_cursor = 0;
    app->detail_dirty = 0;
    curs_set(0);
    close_event_detail(app);
    app_set_status(app, "Edit canceled.");
}

static int detail_backspace_deletes_text(DetailField field) {
    return field == DETAIL_FIELD_TITLE ||
           field == DETAIL_FIELD_LOCATION ||
           field == DETAIL_FIELD_NOTES;
}




static int commit_detail_edit(App *app, int leave_edit_mode, int explicit_save) {
    Event edited = app->detail_edit_event;
    Event stored;
    char error[STATUS_LEN];

    if (app->detail_new_event) {
        clean_field(edited.title);
        if (!app->detail_dirty || !edited.title[0]) {
            if (leave_edit_mode) {
                cancel_new_event_detail(app);
                return 1;
            }
            app_set_status(app, "Title is required.");
            return 0;
        }

        error[0] = '\0';
        if (!normalize_event_for_storage(&edited, 0, error, sizeof error)) {
            app_set_status(app, error);
            return 0;
        }
        if (!save_new_event(app, &edited)) return 0;

        app->detail_edit_event = edited;
        snprintf(app->detail_event_id, sizeof app->detail_event_id, "%s", edited.id);
        app->detail_dirty = 0;
        app->detail_new_event = 0;
        app->focus = FOCUS_EVENT;
        clamp_detail_cursor(app);
        if (leave_edit_mode) {
            close_event_detail(app);
            app_set_status(app, "Event added.");
        } else {
            app_set_status(app, explicit_save ? "Event saved." : "Editing event.");
        }
        return 1;
    }

    if (!app->detail_dirty) {
        if (leave_edit_mode) {
            app->detail_editing = 0;
            curs_set(0);
        }
        app_set_status(app, "No changes.");
        return 1;
    }

    error[0] = '\0';
    if (!normalize_event_for_storage(&edited, 1, error, sizeof error)) {
        app_set_status(app, error);
        return 0;
    }
    if (find_event_by_id(edited.id, &stored) && event_content_equal(&edited, &stored)) {
        app->detail_edit_event = edited;
        app->detail_dirty = 0;
        clamp_detail_cursor(app);
        if (leave_edit_mode) {
            app->detail_editing = 0;
            curs_set(0);
        }
        app_set_status(app, "No changes.");
        return 1;
    }
    if (!save_event_changes(app, &edited)) return 0;

    app->detail_edit_event = edited;
    snprintf(app->detail_event_id, sizeof app->detail_event_id, "%s", edited.id);
    app->detail_dirty = 0;
    clamp_detail_cursor(app);
    if (leave_edit_mode) {
        app->detail_editing = 0;
        curs_set(0);
    }
    app_set_status(app, leave_edit_mode ? "Event updated." :
                   (explicit_save ? "Event saved." : "Editing event."));
    maybe_warn_background_reminders(app);
    return 1;
}

static void move_detail_field(App *app, int delta) {
    int field;
    char *value;

    field = (int)app->detail_field + delta;
    if (field < 0) field = 0;
    if (field >= DETAIL_FIELD_COUNT) field = DETAIL_FIELD_COUNT - 1;
    app->detail_field = (DetailField)field;
    value = detail_field_value(&app->detail_edit_event, app->detail_field, NULL);
    app->detail_cursor = value ? (int)strlen(value) : 0;
}

static int edit_detail_text(App *app, int ch) {
    char *value;
    size_t size;
    size_t len;

    value = detail_field_value(&app->detail_edit_event, app->detail_field, &size);
    if (!value || size == 0) return 0;
    len = strlen(value);

    if (is_back_key(ch)) {
        if (detail_backspace_deletes_text(app->detail_field) &&
            app->detail_cursor > 0) {
            memmove(value + app->detail_cursor - 1,
                    value + app->detail_cursor,
                    strlen(value + app->detail_cursor) + 1);
            app->detail_cursor--;
            app->detail_dirty = 1;
            return 1;
        }

        cancel_detail_edit_without_validation(app);
        return 1;
    }

    if (ch == KEY_LEFT) {
        if (app->detail_cursor > 0) app->detail_cursor--;
        return 1;
    }
    if (ch == KEY_RIGHT) {
        if (app->detail_cursor < (int)len) app->detail_cursor++;
        return 1;
    }
    if (ch == KEY_UP) {
        move_detail_field(app, -1);
        return 1;
    }
    if (ch == KEY_DOWN) {
        move_detail_field(app, 1);
        return 1;
    }
    if (ch == 10 || ch == KEY_ENTER) {
        commit_detail_edit(app, 0, 1);
        return 1;
    }
    if (ch == 27) {
        cancel_detail_edit_without_validation(app);
        return 1;
    }
    if (ch >= 0 && ch <= UCHAR_MAX && isprint((unsigned char)ch) && len + 1 < size) {
        memmove(value + app->detail_cursor + 1,
                value + app->detail_cursor,
                strlen(value + app->detail_cursor) + 1);
        value[app->detail_cursor++] = (char)ch;
        app->detail_dirty = 1;
        return 1;
    }

    return 0;
}

static int open_selected_event_detail_edit(App *app) {
    return open_selected_event_detail(app) && begin_detail_edit(app);
}

static int first_ringing_reminder_event_id(char *out, size_t size) {
    ReminderList reminders = {0};
    int found = 0;

    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!load_reminders(&reminders)) return 0;

    for (size_t i = 0; i < reminders.len; i++) {
        Reminder *r = &reminders.items[i];
        if (!reminder_is_ringing(r)) continue;
        snprintf(out, size, "%s", r->event_id);
        found = 1;
        break;
    }

    reminderlist_free(&reminders);
    return found;
}

static int open_first_ringing_reminder_detail(App *app) {
    char event_id[ID_LEN];

    if (!first_ringing_reminder_event_id(event_id, sizeof event_id)) {
        app->auto_opened_ringing_event_id[0] = '\0';
        return 0;
    }

    if (!strcmp(app->auto_opened_ringing_event_id, event_id)) return 0;
    if (!open_event_detail_by_id(app, event_id)) return 0;

    snprintf(app->auto_opened_ringing_event_id,
             sizeof app->auto_opened_ringing_event_id, "%s", event_id);
    app_set_status(app, "Reminder ringing.");
    return 1;
}

static void draw_footer(App *app, const char *keys) {
    int h, w;

    getmaxyx(stdscr, h, w);
    mvhline(h - 2, 0, ACS_HLINE, w);
    mvaddnstr(h - 1, 1, keys, w - 2);
    if (app->status[0]) mvaddnstr(h - 2, 2, app->status, w - 4);
}

static void draw_ringing_banner(int y) {
    int h, w;
    int count = ringing_reminder_count();
    char line[160];

    if (count <= 0) return;
    getmaxyx(stdscr, h, w);
    if (y < 0 || y >= h) return;
    snprintf(line, sizeof line, "RINGING: %d reminder(s). Press c to clear.", count);
    attron(A_REVERSE | A_BOLD);
    mvhline(y, 0, ' ', w);
    mvaddnstr(y, 2, line, w - 4);
    attroff(A_REVERSE | A_BOLD);
}

static void draw_event_line(int y, int x, int width, const Event *e, int selected) {
    char line[1024];
    char timebuf[32];

    if (width <= 0) return;
    format_event_time_range(e, timebuf, sizeof timebuf);

    snprintf(line, sizeof line, "%c %-11s %s%s",
             selected ? '>' : ' ', timebuf, e->title,
             e->remind_minutes[0] ? " [r]" : "");
    if (selected) attron(A_REVERSE);
    mvhline(y, x, ' ', width);
    mvaddnstr(y, x, line, width);
    if (selected) attroff(A_REVERSE);
}

static int detail_value_offset(int cursor, int value_w) {
    if (value_w <= 1) return cursor > 0 ? cursor - 1 : 0;
    if (cursor >= value_w) return cursor - value_w + 1;
    return 0;
}

static int draw_detail_form_line(App *app, Event *event, DetailField field,
                                 int y, int x, int width, int label_w,
                                 int *cursor_y, int *cursor_x) {
    const char *label = detail_field_label(field);
    char *value = detail_field_value(event, field, NULL);
    int label_len = (int)strlen(label);
    int value_x = x + label_w;
    int value_w;
    int selected = app->detail_editing && app->detail_field == field;
    int offset = 0;

    if (width <= 0) return y + 1;
    if (value_x <= x + label_len) value_x = x + label_len + 1;
    if (value_x >= x + width) value_x = x + width - 1;
    value_w = x + width - value_x;
    if (value_w < 1) value_w = 1;

    mvhline(y, x, ' ', width);
    attron(A_BOLD);
    mvaddnstr(y, x, label, width);
    attroff(A_BOLD);

    if (selected) {
        clamp_detail_cursor(app);
        offset = detail_value_offset(app->detail_cursor, value_w);
        attron(A_REVERSE);
        mvhline(y, value_x, ' ', value_w);
        if (value && value[offset]) mvaddnstr(y, value_x, value + offset, value_w);
        attroff(A_REVERSE);
        if (cursor_y) *cursor_y = y;
        if (cursor_x) {
            int xoff = app->detail_cursor - offset;
            if (xoff < 0) xoff = 0;
            if (xoff >= value_w) xoff = value_w - 1;
            *cursor_x = value_x + xoff;
        }
    } else if (value) {
        mvaddnstr(y, value_x, value, value_w);
    }

    return y + 1;
}

static void draw_event_detail(App *app) {
    Event stored;
    Event event;
    int h, w;
    int width;
    int y;
    int bottom;
    int label_w = 24;
    int cursor_y = -1;
    int cursor_x = -1;
    const char *footer;
    int ringing;

    erase();
    getmaxyx(stdscr, h, w);
    width = w - 4;
    if (width < 1) width = 1;
    bottom = h - 3;
    ringing = ringing_reminder_count() > 0;

    if (!load_detail_event(app, &stored)) {
        curs_set(0);
        mvaddnstr(1, 2, "Event not found.", width);
        draw_footer(app, "Backspace: back  c clear ringing  q quit");
        refresh();
        return;
    }

    if (ringing) draw_ringing_banner(0);
    event = app->detail_editing ? app->detail_edit_event : stored;

    if (width < label_w + 8) label_w = 7;
    y = ringing ? 2 : 1;
    for (DetailField field = DETAIL_FIELD_TITLE; field < DETAIL_FIELD_COUNT && y < bottom; field++) {
        y = draw_detail_form_line(app, &event, field, y, 2, width, label_w,
                                  &cursor_y, &cursor_x);
    }

    if (app->detail_editing) {
        if (app->detail_new_event)
            footer = "New event: Up/Down field  Left/Right cursor  Enter save  Backspace delete/back  Esc cancel";
        else
            footer = "Editing: Up/Down field  Left/Right cursor  Enter save  Backspace delete/back  Esc cancel";
        curs_set(1);
        if (cursor_y >= 0 && cursor_x >= 0) move(cursor_y, cursor_x);
    } else {
        footer = "e edit  Backspace: agenda  c clear ringing  q quit";
        curs_set(0);
    }
    draw_footer(app, footer);
    refresh();
}

static void draw_month(App *app) {
    static const char *month_names[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    int h, w;
    int grid_x = 2;
    int grid_y = 4;
    int agenda_x;
    int agenda_y;
    int agenda_w;
    Date first;
    int start_wday;
    int dim;
    char datebuf[11];
    char weekdays[32];

    erase();
    getmaxyx(stdscr, h, w);
    mvprintw(0, 2, "simplecal  %s %04d", month_names[app->selected.month - 1], app->selected.year);
    mvprintw(1, 2, "q quit  ? help  arrows move  PgUp/PgDn month  t today  y year  a add  e edit  dD delete  c clear  / search");
    draw_ringing_banner(2);

    weekday_header(weekdays, sizeof weekdays);
    mvaddnstr(grid_y, grid_x, weekdays, (int)strlen(weekdays));
    first = app->selected;
    first.day = 1;
    start_wday = configured_weekday_position(weekday_of(first));
    dim = days_in_month(app->selected.year, app->selected.month);

    for (int day = 1; day <= dim; day++) {
        Date d = { app->selected.year, app->selected.month, day };
        int pos = start_wday + day - 1;
        int row = pos / 7;
        int col = pos % 7;
        int y = grid_y + 2 + row;
        int x = grid_x + col * 4;
        int selected = day == app->selected.day;
        int today = date_cmp(d, app->today) == 0;
        int has_events = day_has_events(d);
        char cell[5];

        snprintf(cell, sizeof cell, "%2d%c", day, has_events ? '*' : ' ');
        if (selected) attron(A_REVERSE);
        if (today && color_today_enabled) attron(COLOR_PAIR(COLOR_PAIR_TODAY));
        else if (today) attron(A_BOLD);
        mvaddnstr(y, x, cell, 3);
        if (today && color_today_enabled) attroff(COLOR_PAIR(COLOR_PAIR_TODAY));
        else if (today) attroff(A_BOLD);
        if (selected) attroff(A_REVERSE);
    }

    if (w >= 82) {
        agenda_x = 36;
        agenda_y = 4;
        agenda_w = w - agenda_x - 2;
    } else {
        agenda_x = 2;
        agenda_y = 13;
        agenda_w = w - 4;
    }
    if (agenda_w < 20) agenda_w = 20;

    format_date(app->selected, datebuf, sizeof datebuf);
    mvprintw(agenda_y, agenda_x, "Agenda %s", datebuf);
    if (app->focus == FOCUS_EVENT) mvprintw(agenda_y, agenda_x + 19, "[event]");
    else mvprintw(agenda_y, agenda_x + 19, "[day]");

    if (app->day_events.len == 0) {
        mvaddnstr(agenda_y + 2, agenda_x, "No events.", agenda_w);
    } else {
        int max_rows = h - agenda_y - 5;
        if (max_rows < 1) max_rows = 1;
        for (int i = 0; i < max_rows && (size_t)i < app->day_events.len; i++) {
            draw_event_line(agenda_y + 2 + i, agenda_x, agenda_w,
                            &app->day_events.items[i],
                            app->focus == FOCUS_EVENT && (size_t)i == app->event_cursor);
        }
        if (app->event_cursor < app->day_events.len) {
            Event *e = &app->day_events.items[app->event_cursor];
            int y = agenda_y + 3 + max_rows;
            if (y < h - 3 && e->location[0]) mvprintw(y++, agenda_x, "Location: %.*s", agenda_w - 10, e->location);
            if (y < h - 3 && e->notes[0]) mvprintw(y, agenda_x, "Notes: %.*s", agenda_w - 7, e->notes);
        }
    }

    draw_footer(app, "Enter: focus/detail  Backspace: back  m month  Home/t today  c clear ringing");
    refresh();
}

static void draw_year_month(App *app, int month, int y, int x, int width) {
    static const char *names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    Date first = { app->selected.year, month, 1 };
    int start = configured_weekday_position(weekday_of(first));
    int dim = days_in_month(app->selected.year, month);
    char title[32];

    snprintf(title, sizeof title, "%s %04d", names[month - 1], app->selected.year);
    mvaddnstr(y, x, title, width);

    for (int week = 0; week < 6; week++) {
        for (int dow = 0; dow < 7; dow++) {
            int day = week * 7 + dow - start + 1;
            int cx = x + dow * 2;
            int cy = y + 1 + week;
            if (day < 1 || day > dim) {
                mvaddnstr(cy, cx, "  ", 2);
                continue;
            }

            Date d = { app->selected.year, month, day };
            int selected = date_cmp(d, app->selected) == 0;
            int today = date_cmp(d, app->today) == 0;
            char buf[3];
            snprintf(buf, sizeof buf, "%2d", day);
            if (selected) attron(A_REVERSE);
            if (today && color_today_enabled) attron(COLOR_PAIR(COLOR_PAIR_TODAY));
            else if (today) attron(A_BOLD);
            mvaddnstr(cy, cx, buf, 2);
            if (today && color_today_enabled) attroff(COLOR_PAIR(COLOR_PAIR_TODAY));
            else if (today) attroff(A_BOLD);
            if (selected) attroff(A_REVERSE);
        }
    }
}

static void draw_year(App *app) {
    int h, w;
    int block_w = 18;
    int block_h = 7;
    int cols = 4;
    int start_x;
    int start_y = 3;

    erase();
    getmaxyx(stdscr, h, w);
    if (w < 76) {
        cols = 3;
        block_w = 20;
    }
    start_x = (w - cols * block_w) / 2;
    if (start_x < 1) start_x = 1;

    mvprintw(0, 2, "simplecal  %04d yearly planner", app->selected.year);
    draw_ringing_banner(1);
    for (int m = 1; m <= 12; m++) {
        int idx = m - 1;
        int col = idx % cols;
        int row = idx / cols;
        int y = start_y + row * block_h;
        int x = start_x + col * block_w;
        if (y + 6 >= h - 2) continue;
        draw_year_month(app, m, y, x, block_w - 1);
    }

    draw_footer(app, "Enter/m month  arrows move  PgUp/PgDn month  Home/t today  a add  c clear  / search  q quit");
    refresh();
}

static void draw_search(App *app) {
    int h, w;
    int rows;

    erase();
    getmaxyx(stdscr, h, w);
    mvprintw(0, 2, "simplecal search: %s", app->search_term);
    mvprintw(1, 2, "Enter open  e edit  dD delete  c clear  / search again  m month  y year  q quit");
    draw_ringing_banner(2);

    rows = h - 5;
    if (rows < 1) rows = 1;
    if ((int)app->search_cursor < app->search_top) app->search_top = (int)app->search_cursor;
    if ((int)app->search_cursor >= app->search_top + rows)
        app->search_top = (int)app->search_cursor - rows + 1;
    if (app->search_top < 0) app->search_top = 0;

    if (app->search_results.len == 0) {
        mvaddnstr(3, 2, "No events found.", w - 4);
    } else {
        for (int r = 0; r < rows && (size_t)(app->search_top + r) < app->search_results.len; r++) {
            size_t i = (size_t)(app->search_top + r);
            Event *e = &app->search_results.items[i];
            char line[1024];
            char timebuf[32];

            format_event_time_range(e, timebuf, sizeof timebuf);
            snprintf(line, sizeof line, "%s  %-7s  %s%s%s",
                     e->date, timebuf, e->title,
                     e->location[0] ? "  " : "",
                     e->location[0] ? e->location : "");
            if (i == app->search_cursor) attron(A_REVERSE);
            mvaddnstr(3 + r, 2, line, w - 4);
            if (i == app->search_cursor) attroff(A_REVERSE);
        }
    }

    draw_footer(app, "Up/Down result  PgUp/PgDn page");
    refresh();
}

static void show_help(void) {
    erase();
    mvprintw(1, 2, "simplecal help");
    mvprintw(3, 2, "Arrows move, PgUp/PgDn month, Home/t today");
    mvprintw(4, 2, "y year, m month, a add, e edit, dD delete");
    mvprintw(5, 2, "Enter focuses events or opens detail, Backspace goes back");
    mvprintw(6, 2, "c clear ringing, / search");
    mvprintw(7, 2, "? help, q quit");
    mvprintw(8, 2, "Press any key.");
    refresh();
    timeout(-1);
    getch();
    timeout(-1);
}

static void draw(App *app) {
    if (app->view == VIEW_EVENT_DETAIL) draw_event_detail(app);
    else if (app->view == VIEW_YEAR) draw_year(app);
    else if (app->view == VIEW_SEARCH) draw_search(app);
    else draw_month(app);
}

static void move_day(App *app, int days) {
    set_selected_date(app, add_days(app->selected, days));
}

static void move_month(App *app, int months) {
    set_selected_date(app, add_months(app->selected, months));
}

static void handle_key(App *app, int ch, int *running) {
    if (ch == ERR) return;

    if (app->view == VIEW_EVENT_DETAIL && app->detail_editing) {
        edit_detail_text(app, ch);
        return;
    }
    if (ch == 'q' || ch == 'Q') {
        *running = 0;
        return;
    }
    if (ch == 'c') {
        int cleared = 0;
        if (clear_reminders(NULL, &cleared, 1) == 0) {
            if (cleared > 0) app_set_status(app, "Ringing reminders cleared.");
            else app_set_status(app, "No ringing reminders.");
        } else {
            app_set_status(app, "Could not clear reminders.");
        }
        if (cleared > 0) app->auto_opened_ringing_event_id[0] = '\0';
        clearok(stdscr, TRUE);
        erase();
        refresh();
        return;
    }
    if (app->view == VIEW_EVENT_DETAIL) {
        if (is_back_key(ch)) close_event_detail(app);
        else if (ch == 'e') begin_detail_edit(app);
        return;
    }
    if (ch == '?') {
        show_help();
        return;
    }
    if (ch == 'a') {
        add_event(app);
        return;
    }
    if (ch == 'e') {
        if (app->view == VIEW_SEARCH) {
            if (!open_selected_event_detail_edit(app)) app_set_status(app, "No event selected.");
            return;
        }
        if (app->view == VIEW_MONTH && app->focus == FOCUS_EVENT) {
            if (!open_selected_event_detail_edit(app)) app_set_status(app, "No event selected.");
            return;
        }
        if (app->view == VIEW_MONTH && app->focus == FOCUS_DAY) {
            app_set_status(app, "Press Enter to focus events.");
            return;
        }
        if (!open_selected_event_detail_edit(app)) app_set_status(app, "No event selected.");
        return;
    }
    if (ch == 'd') {
        int ch2;
        int h, w;

        getmaxyx(stdscr, h, w);
        move(h - 3, 0);
        clrtoeol();
        mvprintw(h - 3, 1, "Delete event: press D to arm, any other key cancels.");
        refresh();

        timeout(-1);
        ch2 = getch();
        timeout(-1);

        if (ch2 == 'D') delete_event(app);
        else app_set_status(app, "Delete canceled.");
        return;
    }
    if (ch == '/') {
        run_search(app);
        return;
    }
    if (ch == 'y') {
        app->view = VIEW_YEAR;
        app->focus = FOCUS_DAY;
        return;
    }
    if (ch == 'm') {
        app->view = VIEW_MONTH;
        app->focus = FOCUS_DAY;
        reload_day_events(app);
        return;
    }
    if (ch == 't' || ch == KEY_HOME) {
        app->view = app->view == VIEW_SEARCH ? VIEW_MONTH : app->view;
        set_selected_date(app, app->today);
        return;
    }
    if (app->view == VIEW_SEARCH) {
        int page = LINES - 5;
        if (page < 1) page = 1;

        if (is_back_key(ch)) {
            app->view = VIEW_MONTH;
            app->focus = FOCUS_DAY;
            reload_day_events(app);
            app_set_status(app, "");
            return;
        }
        if (ch == KEY_UP && app->search_cursor > 0) app->search_cursor--;
        else if (ch == KEY_DOWN && app->search_cursor + 1 < app->search_results.len) app->search_cursor++;
        else if (ch == KEY_PPAGE) {
            if (app->search_cursor > (size_t)page) app->search_cursor -= (size_t)page;
            else app->search_cursor = 0;
        } else if (ch == KEY_NPAGE) {
            app->search_cursor += (size_t)page;
            if (app->search_cursor >= app->search_results.len && app->search_results.len > 0)
                app->search_cursor = app->search_results.len - 1;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            open_search_result(app);
        }
        return;
    }

    if (ch == KEY_PPAGE) {
        move_month(app, -1);
        return;
    }
    if (ch == KEY_NPAGE) {
        move_month(app, 1);
        return;
    }

    if (app->view == VIEW_YEAR) {
        if (ch == KEY_LEFT) move_day(app, -1);
        else if (ch == KEY_RIGHT) move_day(app, 1);
        else if (ch == KEY_UP) move_day(app, -7);
        else if (ch == KEY_DOWN) move_day(app, 7);
        else if (ch == '\n' || ch == KEY_ENTER) app->view = VIEW_MONTH;
        return;
    }

    if (is_back_key(ch)) {
        if (app->focus == FOCUS_EVENT) app->focus = FOCUS_DAY;
        return;
    }

    if (ch == '\n' || ch == KEY_ENTER) {
        if (app->focus == FOCUS_DAY && app->day_events.len > 0) app->focus = FOCUS_EVENT;
        else if (app->focus == FOCUS_EVENT && !open_selected_event_detail(app))
            app_set_status(app, "No event selected.");
        return;
    }

    if (app->focus == FOCUS_EVENT) {
        if (ch == KEY_UP && app->event_cursor > 0) app->event_cursor--;
        else if (ch == KEY_DOWN && app->event_cursor + 1 < app->day_events.len) app->event_cursor++;
        else if (ch == KEY_LEFT) move_day(app, -1);
        else if (ch == KEY_RIGHT) move_day(app, 1);
        return;
    }

    if (ch == KEY_LEFT) move_day(app, -1);
    else if (ch == KEY_RIGHT) move_day(app, 1);
    else if (ch == KEY_UP) move_day(app, -7);
    else if (ch == KEY_DOWN) move_day(app, 7);
}

static int configured_today_color(void) {
    char color[32];
    char theme[32];

    snprintf(theme, sizeof theme, "%s", simplecal_config.theme);
    clean_field(theme);
    lowercase_ascii(theme);
    if (!strcmp(theme, "mono") || !strcmp(theme, "none") || !strcmp(theme, "off"))
        return -1;
    snprintf(color, sizeof color, "%s", simplecal_config.today_color);
    clean_field(color);
    lowercase_ascii(color);
    if (!strcmp(color, "none") || !strcmp(color, "off") || !strcmp(color, "false"))
        return -1;
    if (!strcmp(color, "black")) return COLOR_BLACK;
    if (!strcmp(color, "red")) return COLOR_RED;
    if (!strcmp(color, "green")) return COLOR_GREEN;
    if (!strcmp(color, "blue")) return COLOR_BLUE;
    if (!strcmp(color, "magenta")) return COLOR_MAGENTA;
    if (!strcmp(color, "cyan")) return COLOR_CYAN;
    if (!strcmp(color, "white")) return COLOR_WHITE;
    return COLOR_YELLOW;
}

static int run_ui(void) {
    App app;
    int running = 1;

    if (!init_data_dir(1)) {
        fprintf(stderr, "simplecal: setup failed.\n");
        return 1;
    }

    memset(&app, 0, sizeof app);
    app.today = today_date();
    app.selected = app.today;
    app.view = VIEW_MONTH;
    app.focus = FOCUS_DAY;
    app_set_status(&app, "Ready.");
    reload_day_events(&app);
    if (!auto_install_background_reminders(&app)) {
        maybe_warn_background_reminders(&app);
    }

    initscr();
    if (has_colors() && start_color() != ERR) {
        int bg = -1;
        int today_color = configured_today_color();
        if (use_default_colors() == ERR) bg = COLOR_BLACK;
        if (today_color >= 0 && init_pair(COLOR_PAIR_TODAY, (short)today_color, (short)bg) != ERR)
            color_today_enabled = 1;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(-1);

    while (running) {
        app.today = today_date();
        open_first_ringing_reminder_detail(&app);
        draw(&app);
        timeout(1000);
        handle_key(&app, getch(), &running);
    }

    endwin();
    eventlist_free(&app.day_events);
    eventlist_free(&app.search_results);
    return 0;
}

static void usage(void) {
    printf("simplecal\n");
    printf("  simplecal                 open calendar\n");
    printf("  simplecal --setup         choose calendar data directory\n");
    printf("  simplecal --data-dir DIR  set calendar data directory\n");
    printf("  simplecal --check-reminders\n");
    printf("  simplecal --clear-reminder EVENT_ID\n");
    printf("  simplecal --clear-reminders\n");
    printf("  simplecal --clear-all-reminders\n");
    printf("  simplecal --install-reminders\n");
    printf("  simplecal --reconcile-reminders\n");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (!strcmp(argv[1], "--setup")) return run_setup_prompt();
        if (!strcmp(argv[1], "--data-dir")) {
            if (argc < 3) {
                usage();
                return 1;
            }
            if (!set_data_dir(argv[2], 1)) {
                fprintf(stderr, "simplecal: could not set data_dir.\n");
                return 1;
            }
            printf("simplecal: data_dir=%s\n", active_data_dir);
            return 0;
        }
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage();
            return 0;
        }
        if (!init_data_dir(0)) {
            fprintf(stderr, "simplecal: could not initialize data_dir.\n");
            return 1;
        }
        if (!strcmp(argv[1], "--check-reminders")) return check_reminders();
        if (!strcmp(argv[1], "--clear-reminder")) {
            int cleared = 0;
            if (argc < 3) {
                usage();
                return 1;
            }
            if (clear_reminders(argv[2], &cleared, 0) != 0) return 1;
            printf("simplecal: cleared %d reminder(s)\n", cleared);
            return 0;
        }
        if (!strcmp(argv[1], "--clear-all-reminders") ||
            !strcmp(argv[1], "--clear-reminders")) {
            int cleared = 0;
            if (clear_reminders(NULL, &cleared, 0) != 0) return 1;
            printf("simplecal: cleared %d reminder(s)\n", cleared);
            return 0;
        }
        if (!strcmp(argv[1], "--install-reminders")) return install_reminders(0);
        if (!strcmp(argv[1], "--reconcile-reminders")) return reconcile_reminders();
        usage();
        return 1;
    }

    return run_ui();
}
