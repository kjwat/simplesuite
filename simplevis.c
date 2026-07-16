/* simplevis.c
   A small terminal music visualizer.

   Runtime dependency:
     parec and pactl, or SIMPLEVIS_CMD set to a raw PCM capture command.

   The capture command must write signed 16-bit little-endian mono audio at
   44100 Hz to stdout.
*/

#define _GNU_SOURCE

#include <ctype.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_RATE 44100
#define FRAME_SAMPLES 2048
#define TARGET_FRAME_RATE 60
#define MOTION_REFERENCE_RATE ((double)SAMPLE_RATE / 1024.0)
#define MIN_FREQUENCY 20.0
#define MAX_FREQUENCY 20000.0
#define DISPLAY_RANGE_DB 48.0
#define SPECTRAL_TILT_START_HZ 100.0
#define SPECTRAL_TILT_DB_PER_OCTAVE 1.25
#define MAX_SPECTRAL_TILT_DB 9.0
#define BAR_ATTACK_SECONDS 0.025
#define BAR_RELEASE_SECONDS 0.160
#define BAR_HEIGHT_HYSTERESIS 0.80
#define CEILING_ATTACK_SECONDS 0.075
#define CEILING_RELEASE_SECONDS 1.500
#define MIN_BARS 8
#define MAX_BARS 96
#define MIN_WIDTH 1
#define MAX_WIDTH 8
#define WHITE_BAR_COLOR 16
#define FIRST_BAR_COLOR 17
#define THEME_BAR_COLOR 18
#define HUE_SECTOR_COUNT 6
#define WHITE_BAR_PAIR 1
#define FIRST_BAR_PAIR 2
#define THEME_BAR_PAIR (FIRST_BAR_PAIR + HUE_SECTOR_COUNT)
#define COLOR_TRANSITION_SECONDS 5.0
#define COLOR_HOLD_SECONDS 10.0
#define MIN_COLOR_DISTANCE 0.42
#define THEME_REFRESH_SECONDS 2.0
#define THEME_TEXT_SIZE 32768
#define THEME_PATH_SIZE 1024

typedef struct {
    int first_bin;
    int last_bin;
    double target;
    double value;
} Band;

typedef struct {
    double real;
    double imag;
} Complex;

static volatile sig_atomic_t stop = 0;

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static void die(const char *msg) {
    fprintf(stderr, "simplevis: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p)
        die("strdup");
    return p;
}

static char *shell_quote(const char *s) {
    size_t len = 3;

    for (const char *p = s; *p; p++)
        len += (*p == '\'') ? 4 : 1;

    char *out = malloc(len);
    if (!out)
        die("malloc");

    char *w = out;
    *w++ = '\'';

    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            memcpy(w, "'\\''", 4);
            w += 4;
        } else {
            *w++ = *p;
        }
    }

    *w++ = '\'';
    *w = '\0';
    return out;
}

static char *capture_command(const char *source_arg, const char *cmd_arg) {
    const char *cmd = cmd_arg ? cmd_arg : getenv("SIMPLEVIS_CMD");
    const char *source = source_arg ? source_arg : getenv("SIMPLEVIS_SOURCE");

    if (cmd && *cmd)
        return xstrdup(cmd);

    if (source && *source) {
        char *quoted = shell_quote(source);
        int need = snprintf(NULL, 0,
                            "parec --raw --format=s16le --rate=%d "
                            "--channels=1 --latency-msec=25 -d %s "
                            "2>/dev/null",
                            SAMPLE_RATE, quoted);
        if (need < 0)
            die("snprintf");

        char *out = malloc((size_t)need + 1);
        if (!out)
            die("malloc");

        snprintf(out, (size_t)need + 1,
                 "parec --raw --format=s16le --rate=%d "
                 "--channels=1 --latency-msec=25 -d %s 2>/dev/null",
                 SAMPLE_RATE, quoted);
        free(quoted);
        return out;
    }

    return xstrdup("parec --raw --format=s16le --rate=44100 "
                   "--channels=1 --latency-msec=25 "
                   "-d \"$(pactl get-default-sink).monitor\" "
                   "2>/dev/null");
}

static void usage(FILE *f) {
    fprintf(f,
            "usage: simplevis [-b bars] [-g gain] [-s pulse-source] "
            "[-c command]\n"
            "\n"
            "  -b bars          target number of bars, 8..96\n"
            "  -g gain          visual gain, default 1.0\n"
            "  -s pulse-source  PulseAudio/PipeWire source to capture\n"
            "  -c command       command that writes s16le mono 44100 Hz PCM\n"
            "\n"
            "env:\n"
            "  SIMPLEVIS_SOURCE  same as -s\n"
            "  SIMPLEVIS_CMD     same as -c\n"
            "  SIMPLEVIS_COLOR   system-theme override as #RRGGBB\n"
            "  SIMPLEVIS_COLOR_FILE  file containing a #RRGGBB color\n"
            "  SIMPLEVIS_COLOR_CMD   command that prints a #RRGGBB color\n");
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static double clamp_double(double v, double lo, double hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static void mvaddstr_clipped(int y, int x, const char *s, int cols) {
    if (cols <= 0 || !s)
        return;

    if (x < 0) {
        int skip = -x;
        int len = (int)strlen(s);

        if (skip >= len)
            return;

        s += skip;
        x = 0;
    }

    if (x >= cols)
        return;

    mvaddnstr(y, x, s, cols - x);
}

static double now_seconds(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void sleep_until(double deadline) {
    struct timespec ts;
    int rc;

    ts.tv_sec = (time_t)deadline;
    ts.tv_nsec = (long)((deadline - (double)ts.tv_sec) * 1000000000.0);

    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (rc == EINTR && !stop);
}

static void append_samples(int16_t *window, const int16_t *incoming,
                           size_t count) {
    if (count >= FRAME_SAMPLES) {
        memcpy(window, incoming + count - FRAME_SAMPLES,
               FRAME_SAMPLES * sizeof(window[0]));
        return;
    }

    memmove(window, window + count,
            (FRAME_SAMPLES - count) * sizeof(window[0]));
    memcpy(window + FRAME_SAMPLES - count, incoming,
           count * sizeof(window[0]));
}

static int drain_audio(int fd, int16_t *window,
                       unsigned char *carry, int *has_carry) {
    unsigned char raw[8193];
    int16_t incoming[4096];
    int received = 0;

    for (;;) {
        size_t prefix = *has_carry ? 1U : 0U;
        ssize_t got;

        if (prefix)
            raw[0] = *carry;

        got = read(fd, raw + prefix, sizeof(raw) - prefix);
        if (got > 0) {
            size_t total = prefix + (size_t)got;
            size_t count = total / 2;

            for (size_t i = 0; i < count; i++) {
                uint16_t value = (uint16_t)raw[i * 2] |
                                 (uint16_t)raw[i * 2 + 1] << 8;
                incoming[i] = (int16_t)value;
            }
            append_samples(window, incoming, count);
            received = 1;

            *has_carry = (int)(total & 1U);
            if (*has_carry)
                *carry = raw[total - 1];
            continue;
        }

        if (got == 0)
            return received ? 0 : 1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

static void spectrum_rgb(double position, double *r, double *g, double *b) {
    double scaled, blend;
    int sector;

    position = fmod(position, 1.0);
    if (position < 0.0)
        position += 1.0;

    scaled = position * HUE_SECTOR_COUNT;
    sector = (int)scaled;
    blend = scaled - sector;

    /* Six equal HSV sectors make hue advance at a constant angular rate. */
    switch (sector) {
    case 0: *r = 1.0;         *g = blend;       *b = 0.0;         break;
    case 1: *r = 1.0 - blend; *g = 1.0;         *b = 0.0;         break;
    case 2: *r = 0.0;         *g = 1.0;         *b = blend;       break;
    case 3: *r = 0.0;         *g = 1.0 - blend; *b = 1.0;         break;
    case 4: *r = blend;       *g = 0.0;         *b = 1.0;         break;
    default:
        *r = 1.0;
        *g = 0.0;
        *b = 1.0 - blend;
        break;
    }
}

typedef struct {
    double r;
    double g;
    double b;
} RGBColor;

typedef enum {
    COLOR_MODE_WHITE,
    COLOR_MODE_CYCLE,
    COLOR_MODE_THEME
} ColorMode;

static ColorMode toggle_color_mode(ColorMode current, ColorMode requested) {
    return current == requested ? COLOR_MODE_WHITE : requested;
}

typedef struct {
    RGBColor color;
    int has_rgb;
    char source[32];
    char watch_path[THEME_PATH_SIZE];
    dev_t watch_device;
    ino_t watch_inode;
    off_t watch_size;
    time_t watch_mtime;
    int has_watch;
    int poll;
} ThemeAccent;

typedef struct {
    RGBColor from;
    RGBColor to;
    double segment_start;
    double segment_seconds;
    int initialized;
} ColorJourney;

static ColorJourney color_journey = {0};

static double random_unit(void) {
    return (double)rand() / (double)RAND_MAX;
}

static double color_distance(RGBColor a, RGBColor b) {
    double dr = a.r - b.r;
    double dg = a.g - b.g;
    double db = a.b - b.b;

    return sqrt(dr * dr + dg * dg + db * db);
}

static void hsv_to_rgb(double h, double s, double v,
                       double *r, double *g, double *b)
{
    double c = v * s;
    double hp = h * 6.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double m = v - c;
    double rr, gg, bb;

    if (hp < 1)      { rr=c; gg=x; bb=0; }
    else if (hp < 2) { rr=x; gg=c; bb=0; }
    else if (hp < 3) { rr=0; gg=c; bb=x; }
    else if (hp < 4) { rr=0; gg=x; bb=c; }
    else if (hp < 5) { rr=x; gg=0; bb=c; }
    else             { rr=c; gg=0; bb=x; }

    *r = rr + m;
    *g = gg + m;
    *b = bb + m;
}

static RGBColor random_visible_color(void)
{
    RGBColor c;
    double h,s,v;

    for (;;) {
        h = random_unit();
        s = 0.68 + random_unit() * 0.32;
        v = 0.88 + random_unit() * 0.12;

        /* Reject the muddy yellow/olive region. */
        if (h > 0.11 && h < 0.20 && s < 0.92)
            continue;

        hsv_to_rgb(h,s,v,&c.r,&c.g,&c.b);
        return c;
    }
}

static RGBColor random_distant_color(RGBColor from)
{
    RGBColor candidate;
    int attempts = 0;

    do {
        candidate = random_visible_color();
        attempts++;
    } while (color_distance(from, candidate) < MIN_COLOR_DISTANCE &&
             attempts < 1000);

    return candidate;
}

static void begin_color_journey(double now) {
    color_journey.from = random_visible_color();
    color_journey.to = random_distant_color(color_journey.from);
    color_journey.segment_start = now;
    color_journey.segment_seconds = COLOR_TRANSITION_SECONDS;
    color_journey.initialized = 1;
}

static void advance_color_journey(double now) {
    if (!color_journey.initialized) {
        begin_color_journey(now);
        return;
    }

    while (now >= color_journey.segment_start +
                  color_journey.segment_seconds + COLOR_HOLD_SECONDS) {
        color_journey.segment_start +=
            color_journey.segment_seconds + COLOR_HOLD_SECONDS;
        color_journey.from = color_journey.to;
        color_journey.to = random_distant_color(color_journey.from);
    }
}

static void color_journey_rgb(double now, double *r, double *g, double *b) {
    double progress;
    double eased;

    advance_color_journey(now);
    if (now >= color_journey.segment_start + color_journey.segment_seconds)
        progress = 1.0;
    else
        progress = (now - color_journey.segment_start) /
                   color_journey.segment_seconds;
    progress = clamp_double(progress, 0.0, 1.0);

    /* Smoothstep keeps every handoff continuous while still letting the
       middle of each journey move with a little more life. */
    eased = progress * progress * (3.0 - 2.0 * progress);

    *r = color_journey.from.r +
         (color_journey.to.r - color_journey.from.r) * eased;
    *g = color_journey.from.g +
         (color_journey.to.g - color_journey.from.g) * eased;
    *b = color_journey.from.b +
         (color_journey.to.b - color_journey.from.b) * eased;
}

static int hex_digit_value(int ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static int parse_hex_color(const char *text, RGBColor *color) {
    int value[6];

    if (!text || !color)
        return 0;
    while (isspace((unsigned char)*text))
        text++;
    if (*text == '#')
        text++;

    for (int i = 0; i < 6; i++) {
        value[i] = hex_digit_value((unsigned char)text[i]);
        if (value[i] < 0)
            return 0;
    }

    color->r = (value[0] * 16 + value[1]) / 255.0;
    color->g = (value[2] * 16 + value[3]) / 255.0;
    color->b = (value[4] * 16 + value[5]) / 255.0;
    return 1;
}

static int find_nth_hex_color(const char *text, int wanted,
                              RGBColor *color) {
    int found = 0;

    if (!text || wanted < 0)
        return 0;

    for (const char *p = text; *p; p++) {
        if (*p != '#' || !parse_hex_color(p, color))
            continue;
        if (found == wanted)
            return 1;
        found++;
    }
    return 0;
}

static int find_keyed_hex_color(const char *text, const char *key,
                                RGBColor *color) {
    size_t key_length;
    const char *match;

    if (!text || !key)
        return 0;
    key_length = strlen(key);
    match = text;

    while ((match = strstr(match, key)) != NULL) {
        const char *line_end = strchr(match, '\n');
        const char *p = match + key_length;

        if (!line_end)
            line_end = match + strlen(match);
        while (p < line_end) {
            if (*p == '#' && parse_hex_color(p, color))
                return 1;
            p++;
        }
        match += key_length;
    }
    return 0;
}

static int parse_rgb_triplet(const char *text, RGBColor *color) {
    double component[3];
    double maximum;
    const char *p = text;

    if (!text || !color)
        return 0;

    for (int i = 0; i < 3; i++) {
        char *end;

        while (*p && !isdigit((unsigned char)*p) &&
               *p != '.' && *p != '-' && *p != '+')
            p++;
        if (!*p)
            return 0;

        errno = 0;
        component[i] = strtod(p, &end);
        if (end == p || errno == ERANGE || !isfinite(component[i]) ||
            component[i] < 0.0)
            return 0;
        p = end;
    }

    maximum = fmax(component[0], fmax(component[1], component[2]));
    if (maximum > 1.0001) {
        if (maximum > 255.0)
            return 0;
        for (int i = 0; i < 3; i++)
            component[i] /= 255.0;
    }

    color->r = clamp_double(component[0], 0.0, 1.0);
    color->g = clamp_double(component[1], 0.0, 1.0);
    color->b = clamp_double(component[2], 0.0, 1.0);
    return 1;
}

static int read_theme_file(const char *path, char *text, size_t size) {
    FILE *file;
    size_t used;
    int okay;

    if (!path || !*path || !text || size < 2)
        return 0;
    file = fopen(path, "r");
    if (!file)
        return 0;
    used = fread(text, 1, size - 1, file);
    okay = !ferror(file);
    text[used] = '\0';
    fclose(file);
    return okay && used > 0;
}

static int make_xdg_path(char *path, size_t size, const char *variable,
                         const char *fallback, const char *suffix) {
    const char *base = getenv(variable);
    int written;

    if (base && *base) {
        written = snprintf(path, size, "%s/%s", base, suffix);
    } else {
        const char *home = getenv("HOME");

        if (!home || !*home)
            return 0;
        written = snprintf(path, size, "%s/%s/%s",
                           home, fallback, suffix);
    }
    return written >= 0 && (size_t)written < size;
}

static int expand_home_path(const char *input, char *path, size_t size) {
    int written;

    if (!input || !*input)
        return 0;
    if (input[0] == '~' && input[1] == '/') {
        const char *home = getenv("HOME");

        if (!home || !*home)
            return 0;
        written = snprintf(path, size, "%s/%s", home, input + 2);
    } else {
        written = snprintf(path, size, "%s", input);
    }
    return written >= 0 && (size_t)written < size;
}

static void set_theme_accent(ThemeAccent *accent, RGBColor color,
                             const char *source) {
    memset(accent, 0, sizeof(*accent));
    accent->color = color;
    accent->has_rgb = 1;
    snprintf(accent->source, sizeof(accent->source), "%s", source);
}

static void watch_theme_path(ThemeAccent *accent, const char *path) {
    struct stat info;

    if (!path || !*path || stat(path, &info) != 0) {
        accent->poll = 1;
        return;
    }

    snprintf(accent->watch_path, sizeof(accent->watch_path), "%s", path);
    accent->watch_device = info.st_dev;
    accent->watch_inode = info.st_ino;
    accent->watch_size = info.st_size;
    accent->watch_mtime = info.st_mtime;
    accent->has_watch = 1;
}

static void watch_dconf(ThemeAccent *accent) {
    char path[THEME_PATH_SIZE];

    if (make_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config",
                      "dconf/user"))
        watch_theme_path(accent, path);
    else
        accent->poll = 1;
}

static int theme_watch_changed(const ThemeAccent *accent) {
    struct stat info;

    if (!accent->has_watch)
        return accent->poll;
    if (stat(accent->watch_path, &info) != 0)
        return 1;
    return info.st_dev != accent->watch_device ||
           info.st_ino != accent->watch_inode ||
           info.st_size != accent->watch_size ||
           info.st_mtime != accent->watch_mtime;
}

static int same_theme_accent(const ThemeAccent *a, const ThemeAccent *b) {
    if (a->has_rgb != b->has_rgb || strcmp(a->source, b->source) != 0)
        return 0;
    if (!a->has_rgb)
        return 1;
    return fabs(a->color.r - b->color.r) < 0.5 / 255.0 &&
           fabs(a->color.g - b->color.g) < 0.5 / 255.0 &&
           fabs(a->color.b - b->color.b) < 0.5 / 255.0;
}

static int command_output(const char *command, char *output, size_t size) {
    FILE *pipe;
    char chunk[512];
    size_t used = 0;

    if (!command || !*command || !output || size < 2)
        return 0;
    output[0] = '\0';
    pipe = popen(command, "r");
    if (!pipe)
        return 0;

    for (;;) {
        size_t got = fread(chunk, 1, sizeof(chunk), pipe);
        size_t room = size - 1 - used;
        size_t copy = got < room ? got : room;

        if (copy > 0) {
            memcpy(output + used, chunk, copy);
            used += copy;
        }
        if (got < sizeof(chunk))
            break;
    }
    output[used] = '\0';
    (void)pclose(pipe);
    return used > 0;
}

static int desktop_is_gnome(void) {
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    const char *session = getenv("DESKTOP_SESSION");

    return (desktop && (strcasestr(desktop, "gnome") ||
                        strcasestr(desktop, "ubuntu"))) ||
           (session && (strcasestr(session, "gnome") ||
                        strcasestr(session, "ubuntu")));
}

static int theme_from_keyed_file(ThemeAccent *accent, const char *path,
                                 const char *key, const char *source) {
    char text[THEME_TEXT_SIZE];
    RGBColor color;

    if (!read_theme_file(path, text, sizeof(text)) ||
        !find_keyed_hex_color(text, key, &color))
        return 0;
    set_theme_accent(accent, color, source);
    watch_theme_path(accent, path);
    return 1;
}

static int theme_from_nth_file(ThemeAccent *accent, const char *path,
                               int index, const char *source) {
    char text[THEME_TEXT_SIZE];
    RGBColor color;

    if (!read_theme_file(path, text, sizeof(text)) ||
        !find_nth_hex_color(text, index, &color))
        return 0;
    set_theme_accent(accent, color, source);
    watch_theme_path(accent, path);
    return 1;
}

static int explicit_theme_accent(ThemeAccent *accent) {
    const char *value = getenv("SIMPLEVIS_COLOR");
    const char *file_value = getenv("SIMPLEVIS_COLOR_FILE");
    const char *command = getenv("SIMPLEVIS_COLOR_CMD");
    char path[THEME_PATH_SIZE];
    char text[THEME_TEXT_SIZE];
    RGBColor color;

    if (value && *value && parse_hex_color(value, &color)) {
        set_theme_accent(accent, color, "override");
        return 1;
    }
    if (file_value && *file_value &&
        expand_home_path(file_value, path, sizeof(path)) &&
        read_theme_file(path, text, sizeof(text)) &&
        find_nth_hex_color(text, 0, &color)) {
        set_theme_accent(accent, color, "override");
        watch_theme_path(accent, path);
        return 1;
    }
    if (command && *command && command_output(command, text, sizeof(text)) &&
        find_nth_hex_color(text, 0, &color)) {
        set_theme_accent(accent, color, "override");
        accent->poll = 1;
        return 1;
    }
    return 0;
}

static int openbar_enabled(void) {
    char output[4096];

    if (!desktop_is_gnome())
        return 0;
    if (command_output("gsettings get org.gnome.shell enabled-extensions "
                       "2>/dev/null", output, sizeof(output)) &&
        strstr(output, "openbar@neuromorph"))
        return 1;
    return command_output("gnome-extensions list --enabled 2>/dev/null",
                          output, sizeof(output)) &&
           strstr(output, "openbar@neuromorph");
}

static int openbar_schema_color(const char *schema_dir, RGBColor *color) {
    char schema_file[THEME_PATH_SIZE];
    char command[THEME_PATH_SIZE + 256];
    char output[4096];
    char *quoted;
    int written;

    written = snprintf(schema_file, sizeof(schema_file),
                       "%s/org.gnome.shell.extensions.openbar.gschema.xml",
                       schema_dir);
    if (written < 0 || (size_t)written >= sizeof(schema_file) ||
        access(schema_file, R_OK) != 0)
        return 0;

    quoted = shell_quote(schema_dir);
    written = snprintf(command, sizeof(command),
                       "gsettings --schemadir %s get "
                       "org.gnome.shell.extensions.openbar mscolor "
                       "2>/dev/null", quoted);
    free(quoted);
    if (written < 0 || (size_t)written >= sizeof(command) ||
        !command_output(command, output, sizeof(output)))
        return 0;
    return parse_rgb_triplet(output, color);
}

static int openbar_theme_accent(ThemeAccent *accent) {
    char output[4096];
    char path[THEME_PATH_SIZE];
    RGBColor color;

    if (!openbar_enabled())
        return 0;

    if ((command_output("dconf read "
                        "/org/gnome/shell/extensions/openbar/mscolor "
                        "2>/dev/null", output, sizeof(output)) &&
         parse_rgb_triplet(output, &color)) ||
        (command_output("gsettings get "
                        "org.gnome.shell.extensions.openbar mscolor "
                        "2>/dev/null", output, sizeof(output)) &&
         parse_rgb_triplet(output, &color))) {
        set_theme_accent(accent, color, "OpenBar");
        watch_dconf(accent);
        return 1;
    }

    if (make_xdg_path(path, sizeof(path), "XDG_DATA_HOME", ".local/share",
                      "gnome-shell/extensions/openbar@neuromorph/schemas") &&
        openbar_schema_color(path, &color)) {
        set_theme_accent(accent, color, "OpenBar");
        watch_dconf(accent);
        return 1;
    }
    if (openbar_schema_color(
            "/usr/share/gnome-shell/extensions/openbar@neuromorph/schemas",
            &color) ||
        openbar_schema_color(
            "/usr/local/share/gnome-shell/extensions/openbar@neuromorph/schemas",
            &color)) {
        set_theme_accent(accent, color, "OpenBar");
        watch_dconf(accent);
        return 1;
    }
    return 0;
}

static int generated_theme_accent(ThemeAccent *accent) {
    char path[THEME_PATH_SIZE];

    if (make_xdg_path(path, sizeof(path), "XDG_STATE_HOME", ".local/state",
                      "quickshell/user/generated/colors.json") &&
        theme_from_keyed_file(accent, path, "\"primary\"", "Quickshell"))
        return 1;
    if (make_xdg_path(path, sizeof(path), "XDG_STATE_HOME", ".local/state",
                      "quickshell/user/generated/material_colors.scss") &&
        theme_from_keyed_file(accent, path, "$primary:", "Quickshell"))
        return 1;
    if (make_xdg_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
                      "matugen/colors.json") &&
        theme_from_keyed_file(accent, path, "\"primary\"", "Matugen"))
        return 1;
    if (make_xdg_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
                      "wal/colors.json") &&
        theme_from_keyed_file(accent, path, "\"color4\"", "pywal"))
        return 1;
    if (make_xdg_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
                      "wal/colors") &&
        theme_from_nth_file(accent, path, 4, "pywal"))
        return 1;
    if (make_xdg_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
                      "wallust/colors.json") &&
        theme_from_keyed_file(accent, path, "\"color4\"", "Wallust"))
        return 1;
    if (make_xdg_path(path, sizeof(path), "XDG_CACHE_HOME", ".cache",
                      "wallust/colors") &&
        theme_from_nth_file(accent, path, 4, "Wallust"))
        return 1;
    return 0;
}

static int gtk_theme_accent(ThemeAccent *accent) {
    static const char *keys[] = {
        "accent_bg_color", "accent-bg-color", "accent_color"
    };
    static const char *files[] = {"gtk-4.0/gtk.css", "gtk-3.0/gtk.css"};
    char path[THEME_PATH_SIZE];

    for (size_t file = 0; file < sizeof(files) / sizeof(files[0]); file++) {
        if (!make_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config",
                           files[file]))
            continue;
        for (size_t key = 0; key < sizeof(keys) / sizeof(keys[0]); key++)
            if (theme_from_keyed_file(accent, path, keys[key], "GTK"))
                return 1;
    }
    return 0;
}

static int kde_theme_accent(ThemeAccent *accent) {
    char path[THEME_PATH_SIZE];
    char text[THEME_TEXT_SIZE];
    const char *key;
    RGBColor color;

    if (!make_xdg_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config",
                       "kdeglobals") ||
        !read_theme_file(path, text, sizeof(text)))
        return 0;
    key = strstr(text, "AccentColor=");
    if (!key || !parse_rgb_triplet(key + strlen("AccentColor="), &color))
        return 0;
    set_theme_accent(accent, color, "KDE");
    watch_theme_path(accent, path);
    return 1;
}

typedef struct {
    const char *name;
    const char *hex;
} NamedAccent;

static int named_theme_accent(ThemeAccent *accent, const char *output,
                              const NamedAccent *colors, size_t count,
                              const char *source) {
    RGBColor color;

    for (size_t i = 0; i < count; i++) {
        if (!strcasestr(output, colors[i].name))
            continue;
        if (!parse_hex_color(colors[i].hex, &color))
            return 0;
        set_theme_accent(accent, color, source);
        watch_dconf(accent);
        return 1;
    }
    return 0;
}

static int gnome_theme_accent(ThemeAccent *accent) {
    static const NamedAccent gnome_colors[] = {
        {"blue", "#3584e4"}, {"teal", "#2190a4"},
        {"green", "#3a944a"}, {"yellow", "#c88800"},
        {"orange", "#ed5b00"}, {"red", "#e62d42"},
        {"pink", "#d56199"}, {"purple", "#9141ac"},
        {"slate", "#6f8396"}
    };
    static const NamedAccent yaru_colors[] = {
        {"prussiangreen", "#308280"}, {"viridian", "#03875B"},
        {"magenta", "#B34CB3"}, {"purple", "#7764D8"},
        {"olive", "#4B8501"}, {"sage", "#657B69"},
        {"bark", "#787859"}, {"blue", "#0073E5"},
        {"red", "#DA3450"}, {"Yaru", "#E95420"}
    };
    char output[4096];

    if (!desktop_is_gnome())
        return 0;
    if (command_output("gsettings get org.gnome.desktop.interface "
                       "accent-color 2>/dev/null", output, sizeof(output)) &&
        named_theme_accent(accent, output, gnome_colors,
                           sizeof(gnome_colors) / sizeof(gnome_colors[0]),
                           "GNOME"))
        return 1;
    if (command_output("gsettings get org.gnome.desktop.interface "
                       "gtk-theme 2>/dev/null", output, sizeof(output)) &&
        strcasestr(output, "Yaru") &&
        named_theme_accent(accent, output, yaru_colors,
                           sizeof(yaru_colors) / sizeof(yaru_colors[0]),
                           "Yaru"))
        return 1;
    return 0;
}

/* Prefer the desktop's final accent over a raw wallpaper swatch. The ANSI
   fallback keeps this independent of the program that recolored a terminal. */
static void discover_theme_accent(ThemeAccent *accent) {
    if (explicit_theme_accent(accent) ||
        openbar_theme_accent(accent) ||
        generated_theme_accent(accent) ||
        gtk_theme_accent(accent) ||
        kde_theme_accent(accent) ||
        gnome_theme_accent(accent))
        return;

    memset(accent, 0, sizeof(*accent));
    snprintf(accent->source, sizeof(accent->source), "terminal");
}

static int xterm_256_color(RGBColor color) {
    int red = clamp_int((int)(color.r * 5.0 + 0.5), 0, 5);
    int green = clamp_int((int)(color.g * 5.0 + 0.5), 0, 5);
    int blue = clamp_int((int)(color.b * 5.0 + 0.5), 0, 5);

    return 16 + red * 36 + green * 6 + blue;
}

static int dynamic_color = 0;
static int basic_color_steps = 0;
static int theme_pair_ready = 0;
static const int basic_bar_colors[HUE_SECTOR_COUNT] = {
    COLOR_RED, COLOR_YELLOW, COLOR_GREEN,
    COLOR_CYAN, COLOR_BLUE, COLOR_MAGENTA
};

static int basic_color_sector(int index, int steps) {
    if (steps <= 0)
        return 0;

    return ((index * HUE_SECTOR_COUNT + steps / 2) / steps) %
           HUE_SECTOR_COUNT;
}

static void setup_bar_colors(void) {
    if (!has_colors())
        return;

    if (COLOR_PAIRS > WHITE_BAR_PAIR) {
        if (can_change_color() && COLORS > WHITE_BAR_COLOR) {
            init_color(WHITE_BAR_COLOR, 1000, 1000, 1000);
            init_pair(WHITE_BAR_PAIR, -1, WHITE_BAR_COLOR);
        } else {
            init_pair(WHITE_BAR_PAIR, -1, COLOR_WHITE);
        }
    }

    if (can_change_color() && COLORS > FIRST_BAR_COLOR &&
        COLOR_PAIRS > FIRST_BAR_PAIR) {
        dynamic_color = 1;
        init_color(FIRST_BAR_COLOR, 1000, 0, 0);
        init_pair(FIRST_BAR_PAIR, -1, FIRST_BAR_COLOR);
    } else if (COLORS >= 256 && COLOR_PAIRS > FIRST_BAR_PAIR) {
        init_pair(FIRST_BAR_PAIR, -1, 196);
    } else {
        basic_color_steps = clamp_int(COLOR_PAIRS - FIRST_BAR_PAIR,
                                      0, HUE_SECTOR_COUNT);
        for (int i = 0; i < basic_color_steps; i++) {
            int sector = basic_color_sector(i, basic_color_steps);

            init_pair(FIRST_BAR_PAIR + i, -1,
                      basic_bar_colors[sector]);
        }
    }

    if (COLOR_PAIRS > THEME_BAR_PAIR) {
        init_pair(THEME_BAR_PAIR, -1, COLOR_BLUE);
        theme_pair_ready = 1;
    }
}

static int current_bar_pair(double now, int update_palette) {
    double r, g, b;

    if (!has_colors())
        return 0;

    color_journey_rgb(now, &r, &g, &b);

    if (dynamic_color) {
        if (update_palette) {
            init_color(FIRST_BAR_COLOR,
                       (short)(r * 1000.0 + 0.5),
                       (short)(g * 1000.0 + 0.5),
                       (short)(b * 1000.0 + 0.5));
        }
        return FIRST_BAR_PAIR;
    }

    if (COLORS >= 256 && COLOR_PAIRS > FIRST_BAR_PAIR) {
        RGBColor color = {r, g, b};

        if (update_palette)
            init_pair(FIRST_BAR_PAIR, -1, xterm_256_color(color));
        return FIRST_BAR_PAIR;
    }

    {
        double best_distance = 1e9;
        int best = 0;

        if (basic_color_steps == 0)
            return 0;

        for (int i = 0; i < basic_color_steps; i++) {
            double cr, cg, cb;
            double distance;
            int sector = basic_color_sector(i, basic_color_steps);

            spectrum_rgb((double)sector / HUE_SECTOR_COUNT,
                         &cr, &cg, &cb);
            distance = (r - cr) * (r - cr) +
                       (g - cg) * (g - cg) +
                       (b - cb) * (b - cb);
            if (distance < best_distance) {
                best_distance = distance;
                best = i;
            }
        }
        return FIRST_BAR_PAIR + best;
    }
}

static int white_bar_pair(void) {
    if (!has_colors() || COLOR_PAIRS <= WHITE_BAR_PAIR)
        return 0;

    return WHITE_BAR_PAIR;
}

static int apply_theme_bar_pair(const ThemeAccent *accent) {
    int color_index = COLOR_BLUE;

    if (!has_colors() || !theme_pair_ready)
        return 0;

    if (accent->has_rgb) {
        if (can_change_color() && COLORS > THEME_BAR_COLOR) {
            init_color(THEME_BAR_COLOR,
                       (short)(accent->color.r * 1000.0 + 0.5),
                       (short)(accent->color.g * 1000.0 + 0.5),
                       (short)(accent->color.b * 1000.0 + 0.5));
            color_index = THEME_BAR_COLOR;
        } else if (COLORS >= 256) {
            color_index = xterm_256_color(accent->color);
        }
    }

    init_pair(THEME_BAR_PAIR, -1, color_index);
    return THEME_BAR_PAIR;
}

static int usable_bars(int requested, int cols, int line_width) {
    int step = line_width + 1;
    int by_width = (cols + 1) / step;

    if (cols < 36)
        by_width = cols / line_width;

    by_width = clamp_int(by_width, MIN_BARS, MAX_BARS);
    return clamp_int(requested, MIN_BARS, by_width);
}

static void configure_bands(Band *bands, int count) {
    int edges[MAX_BARS + 1];
    int first_bin = clamp_int(
        (int)ceil(MIN_FREQUENCY * FRAME_SAMPLES / SAMPLE_RATE),
        1, FRAME_SAMPLES / 2);
    int end_bin = clamp_int(
        (int)floor(MAX_FREQUENCY * FRAME_SAMPLES / SAMPLE_RATE) + 1,
        first_bin + count, FRAME_SAMPLES / 2 + 1);
    double range = (double)end_bin / first_bin;

    edges[0] = first_bin;
    for (int i = 1; i < count; i++) {
        int ideal = (int)lround(first_bin *
                                pow(range, (double)i / count));
        int lowest = edges[i - 1] + 1;
        int highest = end_bin - (count - i);

        edges[i] = clamp_int(ideal, lowest, highest);
    }
    edges[count] = end_bin;

    for (int i = 0; i < count; i++) {
        bands[i].first_bin = edges[i];
        bands[i].last_bin = edges[i + 1] - 1;
    }
}

static void fft(Complex *data) {
    for (unsigned int i = 1, j = 0; i < FRAME_SAMPLES; i++) {
        unsigned int bit = FRAME_SAMPLES >> 1;

        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;

        if (i < j) {
            Complex tmp = data[i];
            data[i] = data[j];
            data[j] = tmp;
        }
    }

    for (unsigned int len = 2; len <= FRAME_SAMPLES; len <<= 1) {
        double angle = -2.0 * M_PI / (double)len;
        Complex step = {cos(angle), sin(angle)};

        for (unsigned int i = 0; i < FRAME_SAMPLES; i += len) {
            Complex w = {1.0, 0.0};

            for (unsigned int j = 0; j < len / 2; j++) {
                Complex even = data[i + j];
                Complex odd = data[i + j + len / 2];
                Complex rotated = {
                    odd.real * w.real - odd.imag * w.imag,
                    odd.real * w.imag + odd.imag * w.real
                };

                data[i + j].real = even.real + rotated.real;
                data[i + j].imag = even.imag + rotated.imag;
                data[i + j + len / 2].real = even.real - rotated.real;
                data[i + j + len / 2].imag = even.imag - rotated.imag;

                double next_real = w.real * step.real - w.imag * step.imag;
                w.imag = w.real * step.imag + w.imag * step.real;
                w.real = next_real;
            }
        }
    }
}

static void spectrum_amplitudes(const int16_t *samples, double *amplitudes) {
    Complex data[FRAME_SAMPLES];
    double mean = 0.0;
    double window_sum = 0.0;

    for (int i = 0; i < FRAME_SAMPLES; i++)
        mean += samples[i];
    mean /= FRAME_SAMPLES;

    for (int i = 0; i < FRAME_SAMPLES; i++) {
        double window = 0.5 - 0.5 *
                        cos(2.0 * M_PI * i / (FRAME_SAMPLES - 1));

        data[i].real = (((double)samples[i] - mean) / 32768.0) * window;
        data[i].imag = 0.0;
        window_sum += window;
    }

    fft(data);

    amplitudes[0] = 0.0;
    for (int i = 1; i <= FRAME_SAMPLES / 2; i++)
        amplitudes[i] = 2.0 * hypot(data[i].real, data[i].imag) /
                        window_sum;
}

static double spectral_tilt_db(const Band *band) {
    double center_bin = sqrt((double)band->first_bin *
                             (band->last_bin + 1));
    double center_hz = center_bin * SAMPLE_RATE / FRAME_SAMPLES;
    double octaves = log(center_hz / SPECTRAL_TILT_START_HZ) / log(2.0);

    return clamp_double(octaves * SPECTRAL_TILT_DB_PER_OCTAVE,
                        0.0, MAX_SPECTRAL_TILT_DB);
}

static void update_bar_motion(Band *band, double target,
                              double frame_scale) {
    double seconds = frame_scale / MOTION_REFERENCE_RATE;
    double response_time = target > band->value ?
                           BAR_ATTACK_SECONDS : BAR_RELEASE_SECONDS;
    double response = -expm1(-seconds / response_time);

    band->target = target;
    band->value += (target - band->value) * response;
}

static int displayed_bar_height(double value, int previous, int height,
                                int repaint_all) {
    int rounded = clamp_int((int)(value + 0.5), 0, height);

    if (repaint_all)
        return rounded;
    if (rounded > previous && value < previous + BAR_HEIGHT_HYSTERESIS)
        return previous;
    if (rounded < previous && value > previous - BAR_HEIGHT_HYSTERESIS)
        return previous;
    return rounded;
}

static void update_bands(Band *bands, int count, const int16_t *samples,
                         int height, double gain, double frame_scale) {
    double amplitudes[FRAME_SAMPLES / 2 + 1];
    double db_values[MAX_BARS];
    double raw[MAX_BARS];
    double frame_peak_db = -240.0;
    static double visual_ceiling_db = -12.0;

    spectrum_amplitudes(samples, amplitudes);

    for (int i = 0; i < count; i++) {
        double power = 0.0;

        for (int bin = bands[i].first_bin;
             bin <= bands[i].last_bin; bin++)
            power += amplitudes[bin] * amplitudes[bin];

        db_values[i] = 20.0 * log10(fmax(sqrt(power) * gain, 1e-12)) +
                       spectral_tilt_db(&bands[i]);
        if (db_values[i] > frame_peak_db)
            frame_peak_db = db_values[i];
    }

    if (frame_peak_db > -90.0) {
        double desired_ceiling = clamp_double(frame_peak_db + 3.0,
                                              -36.0, 0.0);
        double seconds = frame_scale / MOTION_REFERENCE_RATE;
        double response_time = desired_ceiling > visual_ceiling_db ?
                               CEILING_ATTACK_SECONDS :
                               CEILING_RELEASE_SECONDS;
        double response = -expm1(-seconds / response_time);

        visual_ceiling_db += (desired_ceiling - visual_ceiling_db) *
                             response;
    }

    for (int i = 0; i < count; i++) {
        double normalized = (db_values[i] -
                             (visual_ceiling_db - DISPLAY_RANGE_DB)) /
                            DISPLAY_RANGE_DB;

        normalized = pow(clamp_double(normalized, 0.0, 1.0), 1.10);
        raw[i] = normalized * height;
    }

    for (int i = 0; i < count; i++) {
        double left = i > 0 ? raw[i - 1] : raw[i];
        double right = i + 1 < count ? raw[i + 1] : raw[i];
        double target = raw[i] * 0.76 + left * 0.12 + right * 0.12;

        update_bar_motion(&bands[i], target, frame_scale);

        if (bands[i].value < 0.0) {
            bands[i].value = 0.0;
        } else if (bands[i].value > height) {
            bands[i].value = height;
        }
    }
}

static void draw_frame(const Band *bands, int count, const char *status,
                       int line_width, double reach, int color_pair,
                       int repaint_all, int info_visible) {
    int rows, cols, height, left, step;
    static int prev_rows = 0, prev_cols = 0, prev_count = 0;
    static int prev_left = 0, prev_step = 0, prev_width = 0;
    static int prev_info_visible = -1;
    static int prev_h[MAX_BARS];
    int full_repaint = 0;

    getmaxyx(stdscr, rows, cols);
    height = (int)((rows - 4) * reach + 0.5);

    if (height < 4 || cols < 20) {
        erase();
        mvaddnstr(0, 0, "simplevis: terminal too small", cols - 1);
        refresh();
        prev_rows = prev_cols = 0;
        prev_info_visible = -1;
        return;
    }

    step = line_width + 1;
    left = (cols - ((count - 1) * step + line_width)) / 2;
    if (left < 0)
        left = 0;

    full_repaint = repaint_all || rows != prev_rows || cols != prev_cols ||
                   count != prev_count || left != prev_left ||
                   step != prev_step || line_width != prev_width ||
                   info_visible != prev_info_visible;

    if (full_repaint) {
        erase();
        for (int i = 0; i < MAX_BARS; i++)
            prev_h[i] = 0;

        prev_rows = rows;
        prev_cols = cols;
        prev_count = count;
        prev_left = left;
        prev_step = step;
        prev_width = line_width;
        prev_info_visible = info_visible;
    }

    move(0, 0);
    clrtoeol();
    if (info_visible) {
        const char *title = "simplevis";
        const char *help =
            "q quit  i info  c morph  b theme  +/- gain  arrows shape";
        int title_len = (int)strlen(title);
        int help_len = (int)strlen(help);
        int help_col = title_len + 2;

        if (cols >= title_len + 2 + help_len)
            help_col = cols - help_len;

        mvaddstr_clipped(0, 0, title, cols);
        mvaddstr_clipped(0, help_col, help, cols);
    }

    for (int i = 0; i < count; i++) {
        int x = left + i * step;
        int old_h = prev_h[i];
        int h = displayed_bar_height(bands[i].value, old_h, height,
                                     full_repaint);
        int lo = h < old_h ? h : old_h;
        int hi = h > old_h ? h : old_h;

        if (x >= cols)
            break;

        if (full_repaint) {
            for (int y = 0; y < h; y++) {
                int row = rows - 3 - y;

                if (color_pair)
                    attron(COLOR_PAIR(color_pair));

                for (int w = 0; w < line_width && x + w < cols; w++)
                    mvaddch(row, x + w, color_pair ? ' ' : ACS_CKBOARD);

                if (color_pair)
                    attroff(COLOR_PAIR(color_pair));
            }

            for (int y = h; y < old_h; y++) {
                int row = rows - 3 - y;

                for (int w = 0; w < line_width && x + w < cols; w++)
                    mvaddch(row, x + w, ' ');
            }
        } else {
            for (int y = lo; y < hi; y++) {
                int row = rows - 3 - y;

                if (h > old_h) {
                    if (color_pair)
                        attron(COLOR_PAIR(color_pair));

                    for (int w = 0; w < line_width && x + w < cols; w++)
                        mvaddch(row, x + w, color_pair ? ' ' : ACS_CKBOARD);

                    if (color_pair)
                        attroff(COLOR_PAIR(color_pair));
                } else {
                    for (int w = 0; w < line_width && x + w < cols; w++)
                        mvaddch(row, x + w, ' ');
                }
            }
        }

        prev_h[i] = h;
    }

    move(rows - 2, 0);
    clrtoeol();
    if (info_visible)
        mvhline(rows - 2, 0, ACS_HLINE, cols);
    move(rows - 1, 0);
    clrtoeol();
    if (info_visible)
        mvaddstr_clipped(rows - 1, 0, status, cols - 1);
    refresh();
}

int main(int argc, char **argv) {
    const char *source_arg = NULL;
    const char *cmd_arg = NULL;
    int requested_bars = 48;
    int line_width = 3;
    int info_visible = 0;
    ColorMode color_mode = COLOR_MODE_CYCLE;
    int force_repaint = 0;
    double gain = 1.0;
    double reach = 0.72;
    int opt;

    while ((opt = getopt(argc, argv, "hb:g:s:c:")) != -1) {
        switch (opt) {
        case 'b':
            requested_bars = atoi(optarg);
            break;
        case 'g':
            gain = atof(optarg);
            break;
        case 's':
            source_arg = optarg;
            break;
        case 'c':
            cmd_arg = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }

    requested_bars = clamp_int(requested_bars, MIN_BARS, MAX_BARS);
    gain = clamp_double(gain, 0.2, 40.0);

    char *cmd = capture_command(source_arg, cmd_arg);
    FILE *audio = popen(cmd, "r");
    if (!audio)
        die("popen");

    int audio_fd = fileno(audio);
    int audio_flags = fcntl(audio_fd, F_GETFL);
    if (audio_flags < 0 ||
        fcntl(audio_fd, F_SETFL, audio_flags | O_NONBLOCK) < 0)
        die("fcntl");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();
        setup_bar_colors();
    }

    Band bands[MAX_BARS] = {0};
    int16_t samples[FRAME_SAMPLES] = {0};
    int last_count = 0;
    int last_pair = -1;
    int theme_pair = 0;
    ThemeAccent theme_accent = {0};
    char status[256];
    char color_status[64];
    double next_frame = now_seconds();
    double next_theme_refresh = 0.0;
    unsigned char audio_carry = 0;
    int has_audio_carry = 0;

    srand((unsigned int)(time(NULL) ^ getpid()));

    snprintf(status, sizeof(status), "capture: %s", cmd);

    while (!stop) {
        int ch;
        int rows, cols, count, audio_status;
        double frame_now;

        sleep_until(next_frame);
        frame_now = now_seconds();
        if (frame_now - next_frame > 1.0 / TARGET_FRAME_RATE)
            next_frame = frame_now;
        next_frame += 1.0 / TARGET_FRAME_RATE;

        while ((ch = getch()) != ERR) {
            if (ch == 'q' || ch == 'Q') {
                stop = 1;
            } else if (ch == '+' || ch == '=') {
                gain = clamp_double(gain * 1.12, 0.2, 40.0);
            } else if (ch == '-' || ch == '_') {
                gain = clamp_double(gain / 1.12, 0.2, 40.0);
            } else if (ch == KEY_RIGHT) {
                line_width = clamp_int(line_width + 1, MIN_WIDTH, MAX_WIDTH);
            } else if (ch == KEY_LEFT) {
                line_width = clamp_int(line_width - 1, MIN_WIDTH, MAX_WIDTH);
            } else if (ch == KEY_UP) {
                reach = clamp_double(reach + 0.05, 0.20, 1.0);
            } else if (ch == KEY_DOWN) {
                reach = clamp_double(reach - 0.05, 0.20, 1.0);
            } else if (ch == 'i' || ch == 'I') {
                info_visible = !info_visible;
                force_repaint = 1;
            } else if (ch == 'c' || ch == 'C') {
                color_mode = toggle_color_mode(color_mode,
                                               COLOR_MODE_CYCLE);
                force_repaint = 1;
                if (color_mode == COLOR_MODE_CYCLE) {
                    begin_color_journey(now_seconds());
                    if (dynamic_color)
                        current_bar_pair(now_seconds(), 1);
                }
            } else if (ch == 'b' || ch == 'B') {
                if (color_mode == COLOR_MODE_THEME) {
                    color_mode = toggle_color_mode(color_mode,
                                                   COLOR_MODE_THEME);
                } else {
                    discover_theme_accent(&theme_accent);
                    theme_pair = apply_theme_bar_pair(&theme_accent);
                    color_mode = toggle_color_mode(color_mode,
                                                   COLOR_MODE_THEME);
                    next_theme_refresh = frame_now +
                                         THEME_REFRESH_SECONDS;
                }
                force_repaint = 1;
            }
        }

        if (stop)
            break;

        getmaxyx(stdscr, rows, cols);
        count = usable_bars(requested_bars, cols, line_width);

        if (color_mode == COLOR_MODE_THEME &&
            frame_now >= next_theme_refresh) {
            if (theme_watch_changed(&theme_accent)) {
                ThemeAccent refreshed;
                int changed;

                discover_theme_accent(&refreshed);
                changed = !same_theme_accent(&theme_accent, &refreshed);
                theme_accent = refreshed;
                if (changed) {
                    theme_pair = apply_theme_bar_pair(&theme_accent);
                    force_repaint = 1;
                }
            }
            next_theme_refresh = frame_now + THEME_REFRESH_SECONDS;
        }

        if (count != last_count) {
            memset(bands, 0, sizeof(bands));
            configure_bands(bands, count);
            last_count = count;
        }

        audio_status = drain_audio(audio_fd, samples,
                                   &audio_carry, &has_audio_carry);
        if (audio_status != 0)
            break;

        int pair = color_mode == COLOR_MODE_CYCLE ?
                   current_bar_pair(frame_now, 1) :
                   color_mode == COLOR_MODE_THEME ? theme_pair :
                   white_bar_pair();
        int repaint_all = force_repaint || pair != last_pair;

        update_bands(bands, count, samples,
                     rows > 5 ? (int)((rows - 4) * reach + 0.5) : 1,
                     gain, MOTION_REFERENCE_RATE / TARGET_FRAME_RATE);
        if (color_mode == COLOR_MODE_CYCLE) {
            snprintf(color_status, sizeof(color_status), "morph");
        } else if (color_mode == COLOR_MODE_THEME) {
            snprintf(color_status, sizeof(color_status), "theme/%s",
                     theme_accent.source);
        } else {
            snprintf(color_status, sizeof(color_status), "white");
        }
        snprintf(status, sizeof(status),
                 "bars:%d width:%d reach:%d%% gain:%.1f color:%s  %s",
                 count, line_width, (int)(reach * 100.0 + 0.5), gain,
                 color_status, cmd);
        draw_frame(bands, count, status, line_width, reach,
                   pair, repaint_all, info_visible);
        last_pair = pair;
        force_repaint = 0;
    }

    endwin();

    int rc = pclose(audio);
    free(cmd);

    if (!stop && rc != 0) {
        fprintf(stderr,
                "simplevis: capture ended. Try SIMPLEVIS_SOURCE or "
                "SIMPLEVIS_CMD if the default monitor is unavailable.\n");
        return 1;
    }

    return 0;
}
