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
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PATH_BUF 4096
#define ID_LEN 160
#define TITLE_LEN 256
#define LOCATION_LEN 256
#define NOTES_LEN 1024
#define STATUS_LEN 512

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
    char due[17];
    char status[16];
    char fired_at[20];
    char fired_on[128];
} Reminder;

typedef struct {
    Reminder *items;
    size_t len;
    size_t cap;
} ReminderList;

typedef enum {
    VIEW_MONTH,
    VIEW_YEAR,
    VIEW_SEARCH
} ViewMode;

typedef enum {
    FOCUS_DAY,
    FOCUS_EVENT
} MonthFocus;

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
    char status[STATUS_LEN];
} App;

static char active_data_dir[PATH_BUF] = "";

static void make_event_id_base(const Event *event, char *out, size_t size);
static void clean_field(char *s);
static void app_set_status(App *app, const char *message);
static int init_data_dir(int prompt_if_missing);
static int set_data_dir(const char *input, int save_config);
static int run_setup_prompt(void);
static int write_data_config(const char *dir);
static int reminders_auto_install_attempted(void);

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
    return config_dir(out, size);
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
    } else if (cleaned[0] == '/') {
        written = snprintf(out, size, "%s", cleaned);
    } else {
        char cwd[PATH_BUF];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        written = snprintf(out, size, "%s/%s", cwd, cleaned);
    }

    if (!snprintf_ok(written, size)) return 0;
    strip_trailing_slashes(out);
    return out[0] == '/';
}

static int read_data_config(char *out, size_t size) {
    char path[PATH_BUF];
    FILE *file;
    char line[PATH_BUF + 32];

    if (!config_file_path(path, sizeof path)) return 0;
    file = fopen(path, "r");
    if (!file) return 0;

    while (fgets(line, sizeof line, file)) {
        char *value;

        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "DATA_DIR=", 9) != 0) continue;
        value = line + 9;
        clean_field(value);
        if (value[0] == '/' && snprintf_ok(snprintf(out, size, "%s", value), size)) {
            strip_trailing_slashes(out);
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static int reminders_auto_install_attempted(void) {
    char path[PATH_BUF];
    FILE *file;
    char line[256];

    if (!config_file_path(path, sizeof path)) return 0;
    file = fopen(path, "r");
    if (!file) return 0;

    while (fgets(line, sizeof line, file)) {
        char *value;

        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "REMINDERS_AUTO_INSTALL_ATTEMPTED=", 33) != 0) continue;
        value = line + 33;
        clean_field(value);
        fclose(file);
        return !strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "yes");
    }

    fclose(file);
    return 0;
}

static int config_exists(void) {
    char path[PATH_BUF];

    return config_file_path(path, sizeof path) && access(path, F_OK) == 0;
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

static int write_full_config(const char *dir, int auto_install_attempted) {
    char path[PATH_BUF];
    char tmp[PATH_BUF + 64];
    FILE *file;

    if (!ensure_config_root_dir()) return 0;
    if (!config_file_path(path, sizeof path)) return 0;
    if (!atomic_open_temp(path, tmp, sizeof tmp, &file)) return 0;

    if (fprintf(file, "DATA_DIR=%s\n", dir) < 0) {
        fclose(file);
        unlink(tmp);
        return 0;
    }
    if (auto_install_attempted &&
        fprintf(file, "REMINDERS_AUTO_INSTALL_ATTEMPTED=1\n") < 0) {
        fclose(file);
        unlink(tmp);
        return 0;
    }

    return finish_atomic_write(file, tmp, path);
}

static int write_data_config(const char *dir) {
    return write_full_config(dir, reminders_auto_install_attempted());
}

static int mark_reminders_auto_install_attempted(void) {
    char dir[PATH_BUF];

    if (active_data_dir[0]) {
        snprintf(dir, sizeof dir, "%s", active_data_dir);
    } else if (!read_data_config(dir, sizeof dir)) {
        if (!default_data_dir(dir, sizeof dir)) return 0;
    }

    return write_full_config(dir, 1);
}

static int set_data_dir(const char *input, int save_config) {
    char absolute[PATH_BUF];
    char events[PATH_BUF];
    int written;

    if (!path_to_absolute(input, absolute, sizeof absolute)) return 0;
    if (!mkdirs(absolute)) return 0;
    written = snprintf(events, sizeof events, "%s/events", absolute);
    if (!snprintf_ok(written, sizeof events) || !mkdirs(events)) return 0;
    if (save_config && !write_data_config(absolute)) return 0;
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
        fprintf(stderr, "simplecal: could not set DATA_DIR.\n");
        return 1;
    }

    printf("simplecal: DATA_DIR=%s\n", active_data_dir);
    return 0;
}

static int init_data_dir(int prompt_if_missing) {
    char dir[PATH_BUF];

    if (active_data_dir[0]) return 1;
    if (read_data_config(dir, sizeof dir)) return set_data_dir(dir, 0);

    if (prompt_if_missing && !config_exists()) {
        return run_setup_prompt() == 0;
    }

    if (!default_data_dir(dir, sizeof dir)) return 0;
    return set_data_dir(dir, 1);
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
           (!strcmp(r->status, "pending") || !strcmp(r->status, "fired"));
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
    strftime(due, due_size, "%Y-%m-%dT%H:%M", tmv);
    if (event_time) *event_time = start;
    return 1;
}

static time_t parse_due_time(const char *due) {
    struct tm tmv;
    int y, mo, d, h, mi;
    char tail;

    if (!due || sscanf(due, "%4d-%2d-%2dT%2d:%2d%c", &y, &mo, &d, &h, &mi, &tail) != 5)
        return (time_t)-1;
    if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59)
        return (time_t)-1;

    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

static int update_reminder_for_event(const Event *event) {
    ReminderList reminders = {0};
    Reminder r;
    char due[17];
    int index;
    int ok;
    int has_reminder;

    has_reminder = event_reminder_due(event, due, sizeof due, NULL);
    if (!load_reminders(&reminders)) return 0;

    index = find_reminder_index(&reminders, event->id);
    if (!has_reminder) {
        if (index >= 0 && !strcmp(reminders.items[index].status, "pending")) {
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

        if (!strcmp(existing->status, "fired")) {
            ok = write_reminders(&reminders);
            reminderlist_free(&reminders);
            return ok;
        }

        snprintf(existing->due, sizeof existing->due, "%s", due);
        snprintf(existing->status, sizeof existing->status, "pending");
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
    if (index >= 0 && !strcmp(reminders.items[index].status, "pending")) {
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
        char due[17];
        time_t event_time = (time_t)-1;

        if (!event_has_id(&events, r.event_id, &event) ||
            !event_reminder_due(&event, due, sizeof due, &event_time)) {
            if (!strcmp(r.status, "fired")) {
                ok = reminderlist_push(&out, r) && ok;
            }
            continue;
        }

        if (!strcmp(r.status, "fired")) {
            ok = reminderlist_push(&out, r) && ok;
            continue;
        }

        snprintf(r.due, sizeof r.due, "%s", due);
        snprintf(r.status, sizeof r.status, "pending");
        r.fired_at[0] = '\0';
        r.fired_on[0] = '\0';
        ok = reminderlist_push(&out, r) && ok;
        (void)event_time;
    }

    for (size_t i = 0; i < events.len; i++) {
        Event *event = &events.items[i];
        char due[17];
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

static int play_alarm(void) {
    char path[PATH_BUF];
    pid_t pid;
    int status = 0;

    if (!command_exists("mpv")) {
        fprintf(stderr, "simplecal: mpv is not installed; cannot play reminder alarm.\n");
        return 0;
    }
    if (!alarm_path(path, sizeof path)) {
        fprintf(stderr, "simplecal: HOME is not set; cannot find alarm MP3.\n");
        return 0;
    }
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "simplecal: alarm MP3 missing at %s\n", path);
        return 0;
    }

    pid = fork();
    if (pid == 0) {
        execlp("mpv", "mpv", "--really-quiet", path, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return 0;

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int check_reminders(void) {
    ReminderList reminders = {0};
    time_t now = time(NULL);
    int fired_count = 0;
    int changed = 0;
    int ok = 1;

    if (!ensure_config_dirs()) return 1;
    if (!load_reminders(&reminders)) return 1;

    for (size_t i = 0; i < reminders.len; i++) {
        Reminder *r = &reminders.items[i];
        time_t due;

        if (strcmp(r->status, "pending")) continue;
        due = parse_due_time(r->due);
        if (due == (time_t)-1 || due > now) continue;

        printf("simplecal: reminder due for %s at %s\n", r->event_id, r->due);
        if (!play_alarm()) {
            ok = 0;
            break;
        }

        snprintf(r->status, sizeof r->status, "fired");
        {
            struct tm *tmv = localtime(&now);
            char host[96] = "unknown";
            if (tmv) strftime(r->fired_at, sizeof r->fired_at, "%Y-%m-%dT%H:%M", tmv);
            gethostname(host, sizeof host);
            host[sizeof(host) - 1] = '\0';
            snprintf(r->fired_on, sizeof r->fired_on, "%s", host);
        }
        fired_count++;
        changed = 1;
    }

    if (changed && !write_reminders(&reminders)) ok = 0;
    reminderlist_free(&reminders);

    if (fired_count > 0) printf("simplecal: fired %d reminder(s)\n", fired_count);
    return ok ? 0 : 1;
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
        "Type=oneshot\n"
        "ExecStart=%h/.local/bin/simplecal --check-reminders\n";
    const char *timer_text =
        "[Unit]\n"
        "Description=Run SimpleCal reminder check every minute\n"
        "\n"
        "[Timer]\n"
        "OnCalendar=*-*-* *:*:00\n"
        "AccuracySec=15s\n"
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

    snprintf(tmp, sizeof tmp, "/tmp/simplecal-cron.%ld", (long)getpid());
    fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600);
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
    char path[PATH_BUF];

    if (!home_path(path, sizeof path, ".config/systemd/user/simplecal-reminders.timer")) return 0;
    return access(path, R_OK) == 0;
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
    char due[17];
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

static int prompt_event(App *app, Event *event, int editing) {
    char buf[NOTES_LEN];
    Date d;
    int start_min = 0;
    int end_min = 0;
    int remind = -1;

    if (!prompt_line_prefill("Title:", event->title, buf, sizeof buf)) return 0;
    if (!buf[0]) {
        app_set_status(app, "Title is required.");
        return 0;
    }
    copy_field(event->title, sizeof event->title, buf);

    if (!prompt_line_prefill("Date YYYY-MM-DD:", event->date, buf, sizeof buf)) return 0;
    if (!parse_date(buf, &d)) {
        app_set_status(app, "Invalid date.");
        return 0;
    }
    format_date(d, event->date, sizeof event->date);

    if (!prompt_line_prefill("Start HH:MM (blank ok):", event->start, buf, sizeof buf)) return 0;
    if (!parse_time_hhmm(buf, &start_min)) {
        app_set_status(app, "Invalid start time.");
        return 0;
    }
    if (buf[0]) snprintf(event->start, sizeof event->start, "%02d:%02d", start_min / 60, start_min % 60);
    else event->start[0] = '\0';

    if (!prompt_line_prefill("End HH:MM (blank ok):", event->end, buf, sizeof buf)) return 0;
    if (!parse_time_hhmm(buf, &end_min)) {
        app_set_status(app, "Invalid end time.");
        return 0;
    }
    if (event->start[0] && buf[0] && end_min < start_min) {
        app_set_status(app, "End time must not be before start time.");
        return 0;
    }
    if (buf[0]) snprintf(event->end, sizeof event->end, "%02d:%02d", end_min / 60, end_min % 60);
    else event->end[0] = '\0';

    if (!prompt_line_prefill("Location:", event->location, buf, sizeof buf)) return 0;
    copy_field(event->location, sizeof event->location, buf);

    if (!prompt_line_prefill("Notes:", event->notes, buf, sizeof buf)) return 0;
    copy_field(event->notes, sizeof event->notes, buf);

    if (!prompt_line_prefill("Remind minutes before (blank none):", event->remind_minutes, buf, sizeof buf))
        return 0;
    if (!parse_remind_minutes(buf, &remind)) {
        app_set_status(app, "Invalid reminder minutes.");
        return 0;
    }
    if (remind >= 0) snprintf(event->remind_minutes, sizeof event->remind_minutes, "%d", remind);
    else event->remind_minutes[0] = '\0';

    if (!editing) generate_event_id(event, event->id, sizeof event->id);
    return event_valid_for_storage(event);
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
    EventList list = {0};
    Date d;
    char datebuf[11];

    memset(&event, 0, sizeof event);
    format_date(app->selected, datebuf, sizeof datebuf);
    snprintf(event.date, sizeof event.date, "%s", datebuf);

    if (!prompt_event(app, &event, 0)) return;
    if (!parse_date(event.date, &d)) {
        app_set_status(app, "Invalid event date.");
        return;
    }
    if (!load_events_for_date(d, &list)) {
        app_set_status(app, "Could not read event file.");
        return;
    }
    if (!eventlist_push(&list, event) || !write_events_for_date(d, &list)) {
        eventlist_free(&list);
        app_set_status(app, "Could not save event.");
        return;
    }
    eventlist_free(&list);
    update_reminder_for_event(&event);
    set_selected_date(app, d);
    app_set_status(app, "Event added.");
    maybe_warn_background_reminders(app);
}

static void edit_event(App *app) {
    Event *current = selected_event(app);
    Event edited;
    Date old_date;
    Date new_date;
    EventList list = {0};
    int idx;

    if (!current) {
        app_set_status(app, "No event selected.");
        return;
    }

    edited = *current;
    if (!parse_date(current->date, &old_date)) {
        app_set_status(app, "Event has an invalid stored date.");
        return;
    }
    if (!prompt_event(app, &edited, 1)) return;
    if (!parse_date(edited.date, &new_date)) {
        app_set_status(app, "Invalid event date.");
        return;
    }

    if (date_cmp(old_date, new_date) != 0) {
        if (!remove_event_from_day(old_date, current->id, NULL)) {
            app_set_status(app, "Could not update old event file.");
            return;
        }
        if (!load_events_for_date(new_date, &list) ||
            !eventlist_push(&list, edited) ||
            !write_events_for_date(new_date, &list)) {
            eventlist_free(&list);
            app_set_status(app, "Could not write updated event.");
            return;
        }
        eventlist_free(&list);
    } else {
        if (!load_events_for_date(old_date, &list)) {
            app_set_status(app, "Could not read event file.");
            return;
        }
        idx = find_event_index(&list, current->id);
        if (idx < 0) {
            eventlist_free(&list);
            app_set_status(app, "Event was not found.");
            return;
        }
        list.items[idx] = edited;
        if (!write_events_for_date(old_date, &list)) {
            eventlist_free(&list);
            app_set_status(app, "Could not save event.");
            return;
        }
        eventlist_free(&list);
    }

    update_reminder_for_event(&edited);
    set_selected_date(app, new_date);
    if (app->view == VIEW_SEARCH) app->view = VIEW_MONTH;
    app_set_status(app, "Event updated.");
    maybe_warn_background_reminders(app);
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

static void set_event_reminder(App *app) {
    Event *event = selected_event(app);
    Event edited;
    Date d;
    EventList list = {0};
    char buf[64];
    int remind = -1;
    int idx;

    if (!event) {
        app_set_status(app, "No event selected.");
        return;
    }

    edited = *event;
    if (!prompt_line_prefill("Remind minutes before (blank none):", event->remind_minutes, buf, sizeof buf))
        return;
    if (!parse_remind_minutes(buf, &remind)) {
        app_set_status(app, "Invalid reminder minutes.");
        return;
    }
    if (remind >= 0) snprintf(edited.remind_minutes, sizeof edited.remind_minutes, "%d", remind);
    else edited.remind_minutes[0] = '\0';

    if (!parse_date(edited.date, &d) || !load_events_for_date(d, &list)) {
        app_set_status(app, "Could not read event file.");
        return;
    }
    idx = find_event_index(&list, edited.id);
    if (idx < 0) {
        eventlist_free(&list);
        app_set_status(app, "Event was not found.");
        return;
    }
    list.items[idx] = edited;
    if (!write_events_for_date(d, &list)) {
        eventlist_free(&list);
        app_set_status(app, "Could not save reminder.");
        return;
    }
    eventlist_free(&list);
    update_reminder_for_event(&edited);
    set_selected_date(app, d);
    if (app->view == VIEW_SEARCH) app->view = VIEW_MONTH;
    app_set_status(app, edited.remind_minutes[0] ? "Reminder updated." : "Reminder cleared.");
    maybe_warn_background_reminders(app);
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

static void draw_footer(App *app, const char *keys) {
    int h, w;

    getmaxyx(stdscr, h, w);
    mvhline(h - 2, 0, ACS_HLINE, w);
    mvaddnstr(h - 1, 1, keys, w - 2);
    if (app->status[0]) mvaddnstr(h - 2, 2, app->status, w - 4);
}

static void draw_event_line(int y, int x, int width, const Event *e, int selected) {
    char line[1024];
    char timebuf[32];

    if (e->start[0] && e->end[0])
        snprintf(timebuf, sizeof timebuf, "%s-%s", e->start, e->end);
    else if (e->start[0])
        snprintf(timebuf, sizeof timebuf, "%s", e->start);
    else
        snprintf(timebuf, sizeof timebuf, "all day");

    snprintf(line, sizeof line, "%c %-11s %s%s%s",
             selected ? '>' : ' ', timebuf, e->title,
             e->remind_minutes[0] ? " [r]" : "",
             e->location[0] ? " @" : "");
    if (selected) attron(A_REVERSE);
    mvaddnstr(y, x, line, width);
    if (selected) attroff(A_REVERSE);
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

    erase();
    getmaxyx(stdscr, h, w);
    mvprintw(0, 2, "simplecal  %s %04d", month_names[app->selected.month - 1], app->selected.year);
    mvprintw(1, 2, "q quit  ? help  arrows move  PgUp/PgDn month  t today  y year  a add  e edit  d delete  r remind  / search");

    mvprintw(grid_y, grid_x, "Su  Mo  Tu  We  Th  Fr  Sa");
    first = app->selected;
    first.day = 1;
    start_wday = weekday_of(first);
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
        if (today) attron(A_BOLD);
        mvaddnstr(y, x, cell, 3);
        if (today) attroff(A_BOLD);
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

    draw_footer(app, "Enter: focus/open event  m month  Home/t today");
    refresh();
}

static void draw_year_month(App *app, int month, int y, int x, int width) {
    static const char *names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    Date first = { app->selected.year, month, 1 };
    int start = weekday_of(first);
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
            if (today) attron(A_BOLD);
            mvaddnstr(cy, cx, buf, 2);
            if (today) attroff(A_BOLD);
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
    int start_y = 2;

    erase();
    getmaxyx(stdscr, h, w);
    if (w < 76) {
        cols = 3;
        block_w = 20;
    }
    start_x = (w - cols * block_w) / 2;
    if (start_x < 1) start_x = 1;

    mvprintw(0, 2, "simplecal  %04d yearly planner", app->selected.year);
    for (int m = 1; m <= 12; m++) {
        int idx = m - 1;
        int col = idx % cols;
        int row = idx / cols;
        int y = start_y + row * block_h;
        int x = start_x + col * block_w;
        if (y + 6 >= h - 2) continue;
        draw_year_month(app, m, y, x, block_w - 1);
    }

    draw_footer(app, "Enter/m month  arrows move  PgUp/PgDn month  Home/t today  a add  / search  q quit");
    refresh();
}

static void draw_search(App *app) {
    int h, w;
    int rows;

    erase();
    getmaxyx(stdscr, h, w);
    mvprintw(0, 2, "simplecal search: %s", app->search_term);
    mvprintw(1, 2, "Enter open  e edit  d delete  r remind  / search again  m month  y year  q quit");

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
            snprintf(line, sizeof line, "%s  %-5s  %s%s%s",
                     e->date, e->start[0] ? e->start : "--:--", e->title,
                     e->location[0] ? "  @ " : "",
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
    mvprintw(3, 2, "Arrows move");
    mvprintw(4, 2, "PgUp/PgDn month");
    mvprintw(5, 2, "Home/t today");
    mvprintw(6, 2, "y year");
    mvprintw(7, 2, "m month");
    mvprintw(8, 2, "a add");
    mvprintw(9, 2, "e edit");
    mvprintw(10, 2, "d delete");
    mvprintw(11, 2, "r reminder");
    mvprintw(12, 2, "/ search");
    mvprintw(13, 2, "? help");
    mvprintw(14, 2, "q quit");
    mvprintw(16, 2, "Press any key.");
    refresh();
    timeout(-1);
    getch();
    timeout(-1);
}

static void draw(App *app) {
    if (app->view == VIEW_YEAR) draw_year(app);
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

    if (ch == 'q' || ch == 'Q') {
        *running = 0;
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
        edit_event(app);
        return;
    }
    if (ch == 'd') {
        delete_event(app);
        return;
    }
    if (ch == 'r') {
        set_event_reminder(app);
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

    if (ch == '\n' || ch == KEY_ENTER) {
        if (app->focus == FOCUS_DAY && app->day_events.len > 0) app->focus = FOCUS_EVENT;
        else if (app->focus == FOCUS_EVENT) edit_event(app);
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
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(-1);

    while (running) {
        draw(&app);
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
                fprintf(stderr, "simplecal: could not set DATA_DIR.\n");
                return 1;
            }
            printf("simplecal: DATA_DIR=%s\n", active_data_dir);
            return 0;
        }
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage();
            return 0;
        }
        if (!init_data_dir(0)) {
            fprintf(stderr, "simplecal: could not initialize DATA_DIR.\n");
            return 1;
        }
        if (!strcmp(argv[1], "--check-reminders")) return check_reminders();
        if (!strcmp(argv[1], "--install-reminders")) return install_reminders(0);
        if (!strcmp(argv[1], "--reconcile-reminders")) return reconcile_reminders();
        usage();
        return 1;
    }

    return run_ui();
}
