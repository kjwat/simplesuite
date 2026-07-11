#define main simplevis_program_main
#include "../simplevis.c"
#undef main

#include <assert.h>

#define TEST_BARS 48

static void make_tone(int16_t *samples, int bin)
{
    for (int i = 0; i < FRAME_SAMPLES; i++)
        samples[i] = (int16_t)lround(24000.0 *
                                    sin(2.0 * M_PI * bin * i /
                                        FRAME_SAMPLES));
}

static void make_burst(int16_t *samples, int bin)
{
    int first = FRAME_SAMPLES * 3 / 8;
    int last = FRAME_SAMPLES * 5 / 8;

    memset(samples, 0, FRAME_SAMPLES * sizeof(samples[0]));
    for (int i = first; i < last; i++)
        samples[i] = (int16_t)lround(24000.0 *
                                    sin(2.0 * M_PI * bin * i /
                                        FRAME_SAMPLES));
}

static int strongest_bin(const int16_t *samples)
{
    double amplitudes[FRAME_SAMPLES / 2 + 1];
    int strongest = 1;

    spectrum_amplitudes(samples, amplitudes);
    for (int bin = 2; bin <= FRAME_SAMPLES / 2; bin++)
        if (amplitudes[bin] > amplitudes[strongest])
            strongest = bin;

    return strongest;
}

static int band_for_bin(const Band *bands, int count, int bin)
{
    int found = -1;

    for (int i = 0; i < count; i++) {
        if (bin < bands[i].first_bin || bin > bands[i].last_bin)
            continue;
        assert(found == -1);
        found = i;
    }

    return found;
}

int main(void)
{
    Band bands[TEST_BARS] = {0};
    int16_t samples[FRAME_SAMPLES];
    const int bass_bin = 3;
    const int hat_bin = 372;
    int bass_band, hat_band, tallest = 0;

    configure_bands(bands, TEST_BARS);
    assert(bands[0].first_bin == 1);
    for (int i = 0; i < TEST_BARS; i++) {
        assert(bands[i].first_bin <= bands[i].last_bin);
        if (i > 0)
            assert(bands[i].first_bin == bands[i - 1].last_bin + 1);
    }

    for (int bin = bands[0].first_bin;
         bin <= bands[TEST_BARS - 1].last_bin; bin++)
        assert(band_for_bin(bands, TEST_BARS, bin) >= 0);

    make_tone(samples, bass_bin);
    assert(strongest_bin(samples) == bass_bin);
    bass_band = band_for_bin(bands, TEST_BARS, bass_bin);
    assert(bass_band >= 0 && bass_band < TEST_BARS / 4);

    make_tone(samples, hat_bin);
    assert(strongest_bin(samples) == hat_bin);
    hat_band = band_for_bin(bands, TEST_BARS, hat_bin);
    assert(hat_band > TEST_BARS * 3 / 4);
    assert(hat_band > bass_band);
    assert(spectral_tilt_db(&bands[bass_band]) == 0.0);
    assert(spectral_tilt_db(&bands[hat_band]) > 6.0);
    assert(spectral_tilt_db(&bands[hat_band]) <= MAX_SPECTRAL_TILT_DB);

    make_burst(samples, hat_bin);
    update_bands(bands, TEST_BARS, samples, 100, 1.0,
                 MOTION_REFERENCE_RATE / TARGET_FRAME_RATE);
    for (int i = 1; i < TEST_BARS; i++)
        if (bands[i].value > bands[tallest].value)
            tallest = i;

    assert(tallest == hat_band);
    assert(bands[tallest].value > 55.0);
    return 0;
}
