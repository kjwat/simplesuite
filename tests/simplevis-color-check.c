#define main simplevis_program_main
#include "../simplevis.c"
#undef main

#include <assert.h>
#include <sys/stat.h>

static void assert_close(double actual, double expected)
{
    assert(fabs(actual - expected) < 1e-9);
}

static void assert_color(double position, const double expected[3])
{
    double red, green, blue;

    spectrum_rgb(position, &red, &green, &blue);
    assert_close(red, expected[0]);
    assert_close(green, expected[1]);
    assert_close(blue, expected[2]);
}

static void assert_cycle_color(double now, const double expected[3])
{
    double red, green, blue;

    color_cycle_rgb(now, &red, &green, &blue);
    assert_close(red, expected[0]);
    assert_close(green, expected[1]);
    assert_close(blue, expected[2]);
}

static void check_wallpaper_accent(void)
{
    char directory[] = "/tmp/simplevis-colors-XXXXXX";
    char theme_directory[256], palette_path[256];
    const char *old_config = getenv("XDG_CONFIG_HOME");
    char *saved_config = old_config ? xstrdup(old_config) : NULL;
    RGBColor accent;

    assert(mkdtemp(directory));
    snprintf(theme_directory, sizeof(theme_directory), "%s/simplewm", directory);
    snprintf(palette_path, sizeof(palette_path),
             "%s/simplewm/colors.Xresources", directory);
    assert(mkdir(theme_directory, 0700) == 0);
    assert(setenv("XDG_CONFIG_HOME", directory, 1) == 0);
    assert(!load_wallpaper_accent(&accent));

    /* A light foreground and the error color must not replace the accent. */
    FILE *file = fopen(palette_path, "w");
    assert(file);
    fputs("*.foreground: #ffffff\n*.color1: #ab302a\n"
          "*.cursorColor: #c9171d\n", file);
    assert(fclose(file) == 0);
    assert(load_wallpaper_accent(&accent));
    assert_close(accent.r, 201.0 / 255.0);
    assert_close(accent.g, 23.0 / 255.0);
    assert_close(accent.b, 29.0 / 255.0);

    file = fopen(palette_path, "w");
    assert(file);
    fputs("*.foreground: #ffffff\n*.cursorColor: #1234Ef\n", file);
    assert(fclose(file) == 0);
    assert(load_wallpaper_accent(&accent));
    assert_close(accent.r, 18.0 / 255.0);
    assert_close(accent.g, 52.0 / 255.0);
    assert_close(accent.b, 239.0 / 255.0);

    const char *invalid[] = {"#12345", "#1234567", "#xyzxyz"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        file = fopen(palette_path, "w");
        assert(file);
        fprintf(file, "*.cursorColor: %s\n", invalid[i]);
        assert(fclose(file) == 0);
        assert(!load_wallpaper_accent(&accent));
    }

    assert(unlink(palette_path) == 0);
    assert(rmdir(theme_directory) == 0);
    assert(rmdir(directory) == 0);
    if (saved_config) {
        assert(setenv("XDG_CONFIG_HOME", saved_config, 1) == 0);
        free(saved_config);
    } else {
        assert(unsetenv("XDG_CONFIG_HOME") == 0);
    }
}

int main(void)
{
    static const double boundaries[HUE_SECTOR_COUNT][3] = {
        {1.0, 0.0, 0.0},
        {1.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 1.0, 1.0},
        {0.0, 0.0, 1.0},
        {1.0, 0.0, 1.0}
    };
    static const double midpoints[HUE_SECTOR_COUNT][3] = {
        {1.0, 0.5, 0.0},
        {0.5, 1.0, 0.0},
        {0.0, 1.0, 0.5},
        {0.0, 0.5, 1.0},
        {0.5, 0.0, 1.0},
        {1.0, 0.0, 0.5}
    };
    const double cycle_start_time = 100.0;
    const double sector_seconds = COLOR_CYCLE_SECONDS / HUE_SECTOR_COUNT;
    RGBColor parsed;

    check_wallpaper_accent();

    assert(toggle_color_mode(COLOR_MODE_CYCLE, COLOR_MODE_CYCLE) ==
           COLOR_MODE_WHITE);
    assert(toggle_color_mode(COLOR_MODE_WHITE, COLOR_MODE_CYCLE) ==
           COLOR_MODE_CYCLE);
    assert(toggle_color_mode(COLOR_MODE_THEME, COLOR_MODE_CYCLE) ==
           COLOR_MODE_CYCLE);
    assert(toggle_color_mode(COLOR_MODE_CYCLE, COLOR_MODE_THEME) ==
           COLOR_MODE_THEME);
    assert(toggle_color_mode(COLOR_MODE_THEME, COLOR_MODE_THEME) ==
           COLOR_MODE_WHITE);
    assert(toggle_color_mode(COLOR_MODE_WHITE, COLOR_MODE_THEME) ==
           COLOR_MODE_THEME);

    parsed = (RGBColor){1.0, 0.0, 0.0};
    assert(xterm_256_color(parsed) == 196);
    parsed = (RGBColor){1.0, 1.0, 1.0};
    assert(xterm_256_color(parsed) == 231);

    for (int sector = 0; sector < HUE_SECTOR_COUNT; sector++) {
        assert_color((double)sector / HUE_SECTOR_COUNT,
                     boundaries[sector]);
        assert_color(((double)sector + 0.5) / HUE_SECTOR_COUNT,
                     midpoints[sector]);
    }

    assert_color(1.0, boundaries[0]);
    assert_color(-1.0 / HUE_SECTOR_COUNT,
                 boundaries[HUE_SECTOR_COUNT - 1]);
    for (int i = 0; i < HUE_SECTOR_COUNT; i++)
        assert(basic_color_sector(i, HUE_SECTOR_COUNT) == i);
    assert(basic_color_sector(0, 3) == 0);
    assert(basic_color_sector(1, 3) == 2);
    assert(basic_color_sector(2, 3) == 4);
    assert(basic_color_sector(0, 0) == 0);

    /* Each activation chooses a new starting hue; complete loops return to it. */
    double initial[3], expected[3];
    srand(1);
    color_cycle_rgb(cycle_start_time, &initial[0], &initial[1], &initial[2]);
    double first_hue = color_cycle.starting_hue;
    assert(first_hue >= 0.0 && first_hue < 1.0);
    spectrum_rgb(first_hue, &expected[0], &expected[1], &expected[2]);
    for (int i = 0; i < 3; i++)
        assert_close(initial[i], expected[i]);
    assert_cycle_color(cycle_start_time + COLOR_CYCLE_SECONDS, initial);
    begin_color_cycle(cycle_start_time + 42.0);
    assert(color_cycle.starting_hue >= 0.0 && color_cycle.starting_hue < 1.0);
    assert(color_cycle.starting_hue != first_hue);
    spectrum_rgb(color_cycle.starting_hue,
                 &expected[0], &expected[1], &expected[2]);
    assert_cycle_color(cycle_start_time + 42.0, expected);

    /* Use a known origin for the timing and per-frame checks below. */
    begin_color_cycle(cycle_start_time);
    color_cycle.starting_hue = 0.0;
    assert_cycle_color(cycle_start_time - 1.0, boundaries[0]);

    /* Each sector has the same duration, including across the loop seam. */
    for (int cycle = 0; cycle < 2; cycle++) {
        double start = cycle_start_time + cycle * COLOR_CYCLE_SECONDS;
        for (int sector = 0; sector < HUE_SECTOR_COUNT; sector++) {
            assert_cycle_color(start + sector * sector_seconds,
                               boundaries[sector]);
            assert_cycle_color(start + (sector + 0.5) * sector_seconds,
                               midpoints[sector]);
        }
        assert_cycle_color(start + COLOR_CYCLE_SECONDS, boundaries[0]);
    }

    /* Check every frame through a complete loop and its wrap: always vivid,
     * always moving, and never jumping by more than one 8-bit channel step. */
    RGBColor previous = {1.0, 0.0, 0.0};
    int frames = (int)(COLOR_CYCLE_SECONDS * TARGET_FRAME_RATE);
    for (int frame = 1; frame <= frames + 1; frame++) {
        RGBColor current;
        color_cycle_rgb(cycle_start_time + (double)frame / TARGET_FRAME_RATE,
                        &current.r, &current.g, &current.b);
        double maximum = fmax(current.r, fmax(current.g, current.b));
        double minimum = fmin(current.r, fmin(current.g, current.b));
        double step = fmax(fabs(current.r - previous.r),
                          fmax(fabs(current.g - previous.g),
                               fabs(current.b - previous.b)));
        assert_close(maximum, 1.0);
        assert_close(minimum, 0.0);
        assert(step > 0.0 && step < 1.0 / 255.0);
        assert(labs(lround(current.r * 255) - lround(previous.r * 255)) <= 1);
        assert(labs(lround(current.g * 255) - lround(previous.g * 255)) <= 1);
        assert(labs(lround(current.b * 255) - lround(previous.b * 255)) <= 1);
        previous = current;
    }

    /* Skipped frames or long uptime cannot change the speed or add holds. */
    assert_cycle_color(cycle_start_time + 1000000 * COLOR_CYCLE_SECONDS +
                       sector_seconds, boundaries[1]);
    assert_cycle_color(cycle_start_time + 2.5 * sector_seconds, midpoints[2]);
    return 0;
}
