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

    for (int sector = 0; sector < HUE_SECTOR_COUNT; sector++) {
        assert_color((double)sector / HUE_SECTOR_COUNT,
                     boundaries[sector]);
        assert_color(((double)sector + 0.5) / HUE_SECTOR_COUNT,
                     midpoints[sector]);
    }

    assert_color(1.0, boundaries[0]);

    for (int sector = 0; sector <= HUE_SECTOR_COUNT; sector++) {
        double expected = sector == HUE_SECTOR_COUNT ? 0.0 :
                          (double)sector / HUE_SECTOR_COUNT;

        assert_close(color_cycle_position(
                         100.0,
                         100.0 + sector * COLOR_CYCLE_SECONDS /
                                 HUE_SECTOR_COUNT),
                     expected);
    }

    return 0;
}
