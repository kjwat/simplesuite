#define main simplevis_program_main
#include "../simplevis.c"
#undef main

#include <assert.h>

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

static void assert_journey_color(double now, const double expected[3])
{
    double red, green, blue;

    color_journey_rgb(now, &red, &green, &blue);
    assert_close(red, expected[0]);
    assert_close(green, expected[1]);
    assert_close(blue, expected[2]);
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
    static const double journey_start[3] = {1.0, 0.0, 0.0};
    static const double journey_quarter[3] = {0.84375, 0.0, 0.15625};
    static const double journey_middle[3] = {0.5, 0.0, 0.5};
    static const double journey_end[3] = {0.0, 0.0, 1.0};
    const double journey_start_time = 100.0;
    const double transition_end = journey_start_time +
                                  COLOR_TRANSITION_SECONDS;
    const double next_journey_start = transition_end +
                                      COLOR_HOLD_SECONDS;
    RGBColor parsed;
    ThemeAccent detected;

    assert(toggle_color_mode(COLOR_MODE_CYCLE, COLOR_MODE_CYCLE) ==
           COLOR_MODE_WHITE);
    assert(toggle_color_mode(COLOR_MODE_WHITE, COLOR_MODE_CYCLE) ==
           COLOR_MODE_CYCLE);
    assert(toggle_color_mode(COLOR_MODE_THEME, COLOR_MODE_CYCLE) ==
           COLOR_MODE_CYCLE);
    assert(toggle_color_mode(COLOR_MODE_THEME, COLOR_MODE_THEME) ==
           COLOR_MODE_WHITE);
    assert(toggle_color_mode(COLOR_MODE_WHITE, COLOR_MODE_THEME) ==
           COLOR_MODE_THEME);

    assert(parse_hex_color("#3584e4", &parsed));
    assert_close(parsed.r, 0x35 / 255.0);
    assert_close(parsed.g, 0x84 / 255.0);
    assert_close(parsed.b, 0xe4 / 255.0);
    assert(parse_hex_color("  BEC7D5", &parsed));
    assert_close(parsed.r, 0xbe / 255.0);
    assert(!parse_hex_color("#12345", &parsed));

    assert(find_keyed_hex_color(
        "{\n  \"on_primary\": \"#010203\",\n"
        "  \"primary\": \"#a1b2c3\"\n}\n",
        "\"primary\"", &parsed));
    assert_close(parsed.r, 0xa1 / 255.0);
    assert(find_keyed_hex_color("$primary: #112233;\n",
                                "$primary:", &parsed));
    assert_close(parsed.b, 0x33 / 255.0);
    assert(find_nth_hex_color("#000000\n#111111\n#abcdef\n", 2,
                              &parsed));
    assert_close(parsed.g, 0xcd / 255.0);

    assert(parse_rgb_triplet("['0.25', '0.5', '1.0']", &parsed));
    assert_close(parsed.r, 0.25);
    assert_close(parsed.g, 0.5);
    assert_close(parsed.b, 1.0);
    assert(parse_rgb_triplet("53,132,228", &parsed));
    assert_close(parsed.r, 53.0 / 255.0);
    assert_close(parsed.g, 132.0 / 255.0);
    assert_close(parsed.b, 228.0 / 255.0);
    assert(!parse_rgb_triplet("300,0,0", &parsed));

    parsed = (RGBColor){1.0, 0.0, 0.0};
    assert(xterm_256_color(parsed) == 196);
    parsed = (RGBColor){1.0, 1.0, 1.0};
    assert(xterm_256_color(parsed) == 231);

    assert(setenv("SIMPLEVIS_COLOR", "#123456", 1) == 0);
    discover_theme_accent(&detected);
    assert(detected.has_rgb);
    assert(strcmp(detected.source, "override") == 0);
    assert_close(detected.color.r, 0x12 / 255.0);
    assert_close(detected.color.g, 0x34 / 255.0);
    assert_close(detected.color.b, 0x56 / 255.0);
    assert(unsetenv("SIMPLEVIS_COLOR") == 0);

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

    color_journey = (ColorJourney) {
        .from = {1.0, 0.0, 0.0},
        .to = {0.0, 0.0, 1.0},
        .segment_start = journey_start_time,
        .segment_seconds = COLOR_TRANSITION_SECONDS,
        .initialized = 1
    };
    assert_journey_color(journey_start_time - 1.0, journey_start);
    assert_journey_color(journey_start_time, journey_start);
    assert_journey_color(journey_start_time +
                         COLOR_TRANSITION_SECONDS * 0.25,
                         journey_quarter);
    assert_journey_color(journey_start_time +
                         COLOR_TRANSITION_SECONDS * 0.5,
                         journey_middle);
    assert_journey_color(transition_end, journey_end);
    assert_journey_color(next_journey_start - 0.001, journey_end);

    srand(1);
    assert_journey_color(next_journey_start, journey_end);
    assert_close(color_journey.segment_start, next_journey_start);
    assert_close(color_journey.from.r, journey_end[0]);
    assert_close(color_journey.from.g, journey_end[1]);
    assert_close(color_journey.from.b, journey_end[2]);
    assert(color_distance(color_journey.from, color_journey.to) >=
           MIN_COLOR_DISTANCE);

    for (int i = 0; i < 1000; i++) {
        RGBColor visible = random_visible_color();
        double maximum = fmax(visible.r, fmax(visible.g, visible.b));
        double minimum = fmin(visible.r, fmin(visible.g, visible.b));

        assert(visible.r >= -1e-12 && visible.r <= 1.0 + 1e-12);
        assert(visible.g >= -1e-12 && visible.g <= 1.0 + 1e-12);
        assert(visible.b >= -1e-12 && visible.b <= 1.0 + 1e-12);
        assert(maximum >= 0.88 - 1e-12 && maximum <= 1.0 + 1e-12);
        assert((maximum - minimum) / maximum >= 0.68 - 1e-9);
    }

    return 0;
}
