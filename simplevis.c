/* simplevis.c
   A small terminal music visualizer.

   Runtime dependency:
     parec and pactl, or SIMPLEVIS_CMD set to a raw PCM capture command.

   The capture command must write signed 16-bit little-endian mono audio at
   44100 Hz to stdout.
*/

#define _GNU_SOURCE

#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define MIN_BARS 8
#define MAX_BARS 96
#define MIN_WIDTH 1
#define MAX_WIDTH 8
#define WHITE_BAR_COLOR 16
#define FIRST_BAR_COLOR 17
#define WHITE_BAR_PAIR 1
#define FIRST_BAR_PAIR 2
#define TARGET_COLOR_STEPS 360
#define COLOR_CYCLE_SECONDS 300.0
#define HUE_SECTOR_COUNT 6
#define FOCUS_IN_KEY (KEY_MAX + 1)
#define FOCUS_OUT_KEY (KEY_MAX + 2)

typedef struct {
    int first_bin;
    int last_bin;
    double target;
    double velocity;
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
            "  SIMPLEVIS_CMD     same as -c\n");
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

static double color_cycle_position(double start_time, double now) {
    double position = fmod((now - start_time) / COLOR_CYCLE_SECONDS, 1.0);

    return position < 0.0 ? position + 1.0 : position;
}

static int color_steps = 1;
static int dynamic_color = 0;

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
        color_steps = clamp_int(TARGET_COLOR_STEPS, 1,
                                COLOR_PAIRS - FIRST_BAR_PAIR);

        for (int i = 0; i < color_steps; i++) {
            double position = (double)i / (double)color_steps;
            double r, g, b;
            int red, green, blue, color;

            spectrum_rgb(position, &r, &g, &b);
            red = (int)(r * 5.0 + 0.5);
            green = (int)(g * 5.0 + 0.5);
            blue = (int)(b * 5.0 + 0.5);
            color = 16 + red * 36 + green * 6 + blue;
            init_pair(FIRST_BAR_PAIR + i, -1, color);
        }
    } else {
        static const int colors[HUE_SECTOR_COUNT] = {
            COLOR_RED, COLOR_YELLOW, COLOR_GREEN,
            COLOR_CYAN, COLOR_BLUE, COLOR_MAGENTA
        };

        color_steps = HUE_SECTOR_COUNT;
        for (int i = 0; i < color_steps; i++)
            init_pair(FIRST_BAR_PAIR + i, -1, colors[i]);
    }
}
static int current_bar_pair(double start_time, int update_palette) {
    double hue = color_cycle_position(start_time, now_seconds());
    int index;

    if (!has_colors())
        return 0;

    if (dynamic_color) {
        if (update_palette) {
            double r, g, b;

            spectrum_rgb(hue, &r, &g, &b);
            init_color(FIRST_BAR_COLOR,
                       (short)(r * 1000.0),
                       (short)(g * 1000.0),
                       (short)(b * 1000.0));
        }
        return FIRST_BAR_PAIR;
    }

    index = clamp_int((int)(hue * color_steps), 0, color_steps - 1);
    return FIRST_BAR_PAIR + index;
}

static int white_bar_pair(void) {
    if (!has_colors() || COLOR_PAIRS <= WHITE_BAR_PAIR)
        return 0;

    return WHITE_BAR_PAIR;
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

static void update_bands(Band *bands, int count, const int16_t *samples,
                         int height, double gain, double frame_scale) {
    double amplitudes[FRAME_SAMPLES / 2 + 1];
    double db_values[MAX_BARS];
    double raw[MAX_BARS];
    double frame_peak_db = -240.0;
    double ceiling_rise = pow(0.25, frame_scale);
    double ceiling_fall = pow(0.985, frame_scale);
    double attack_retention = pow(0.18, frame_scale);
    double target_retention = pow(0.62, frame_scale);
    double velocity_retention = pow(0.70, frame_scale);
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
        double retention = desired_ceiling > visual_ceiling_db ?
                           ceiling_rise : ceiling_fall;

        visual_ceiling_db = visual_ceiling_db * retention +
                            desired_ceiling * (1.0 - retention);
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
        double pull;

        if (target > bands[i].value) {
            /* Show percussive attacks before a short transient is gone. */
            bands[i].target = target;
            bands[i].velocity = 0.0;
            bands[i].value = bands[i].value * attack_retention +
                             target * (1.0 - attack_retention);
        } else {
            bands[i].target = bands[i].target * target_retention +
                              target * (1.0 - target_retention);
            pull = (bands[i].target - bands[i].value) * 0.12 * frame_scale;

            bands[i].velocity = bands[i].velocity * velocity_retention +
                                pull;
            bands[i].value += bands[i].velocity * frame_scale;
        }

        if (bands[i].value < 0.0) {
            bands[i].value = 0.0;
            bands[i].velocity = 0.0;
        } else if (bands[i].value > height) {
            bands[i].value = height;
            bands[i].velocity = 0.0;
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
        const char *help = "q quit  i info  c color  +/- gain  arrows shape";
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
        int h = clamp_int((int)(bands[i].value + 0.5), 0, height);
        int old_h = prev_h[i];
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
    int color_cycle = 0;
    int terminal_focused = 1;
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
    define_key("\033[I", FOCUS_IN_KEY);
    define_key("\033[O", FOCUS_OUT_KEY);
    fputs("\033[?1004h", stdout);
    fflush(stdout);

    if (has_colors()) {
        start_color();
        use_default_colors();
        setup_bar_colors();
    }

    Band bands[MAX_BARS] = {0};
    int16_t samples[FRAME_SAMPLES] = {0};
    int last_count = 0;
    int last_pair = -1;
    char status[256];
    double color_start = now_seconds();
    double next_frame = color_start;
    double focus_out_time = 0.0;
    unsigned char audio_carry = 0;
    int has_audio_carry = 0;
    int suppress_palette_update = 0;

    if (dynamic_color)
        current_bar_pair(color_start, 1);

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
            } else if (ch == FOCUS_OUT_KEY) {
                if (terminal_focused)
                    focus_out_time = now_seconds();
                terminal_focused = 0;
            } else if (ch == FOCUS_IN_KEY) {
                if (!terminal_focused && focus_out_time > 0.0) {
                    if (color_cycle)
                        color_start += now_seconds() - focus_out_time;
                    focus_out_time = 0.0;
                }
                terminal_focused = 1;
                suppress_palette_update = dynamic_color;
                force_repaint = 1;
            } else if (ch == 'c' || ch == 'C') {
                color_cycle = !color_cycle;
                suppress_palette_update = 0;
                force_repaint = 1;
                if (color_cycle) {
                    color_start = now_seconds();
                    if (dynamic_color)
                        current_bar_pair(color_start, 1);
                }
            }
        }

        if (stop)
            break;

        getmaxyx(stdscr, rows, cols);
        count = usable_bars(requested_bars, cols, line_width);

        if (count != last_count) {
            memset(bands, 0, sizeof(bands));
            configure_bands(bands, count);
            last_count = count;
        }

        audio_status = drain_audio(audio_fd, samples,
                                   &audio_carry, &has_audio_carry);
        if (audio_status != 0)
            break;

        int pair = color_cycle ? current_bar_pair(color_start,
                                                  terminal_focused &&
                                                  !suppress_palette_update) :
                   white_bar_pair();
        int repaint_all = force_repaint || pair != last_pair;

        update_bands(bands, count, samples,
                     rows > 5 ? (int)((rows - 4) * reach + 0.5) : 1,
                     gain, MOTION_REFERENCE_RATE / TARGET_FRAME_RATE);
        snprintf(status, sizeof(status),
                 "bars:%d width:%d reach:%d%% gain:%.1f color:%s  %s",
                 count, line_width, (int)(reach * 100.0 + 0.5), gain,
                 color_cycle ? "cycle" : "white", cmd);
        if (terminal_focused) {
            draw_frame(bands, count, status, line_width, reach,
                       pair, repaint_all, info_visible);
            last_pair = pair;
            force_repaint = 0;
            suppress_palette_update = 0;
        }
    }

    fputs("\033[?1004l", stdout);
    fflush(stdout);
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
