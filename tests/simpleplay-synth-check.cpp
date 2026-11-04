/* Exercise SID streaming boundaries, rewinds and track changes on SID 2/3. */
#include "../simpleplay-synth.h"
extern "C" {
#include <libavutil/error.h>
}
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static bool cancelled = false;
static int cancel_read(void *) { return cancelled; }

int main()
{
    char *directory = g_dir_make_tmp("simpleplay-synth-XXXXXX", nullptr);
    g_assert_nonnull(directory);
    char *filename = g_build_filename(directory, "tone.sid", nullptr);
    std::vector<unsigned char> tune(124 + 49, 0);
    std::memcpy(tune.data(), "PSID", 4);
    const unsigned short fields[] = {2, 124, 0x1000, 0x1000, 0x1030, 2, 1};
    for (int n = 0; n < 7; n++) { tune[4+n*2] = fields[n] >> 8; tune[5+n*2] = fields[n]; }
    std::memcpy(tune.data()+22, "Original SID test", 17);
    const unsigned char code[] = {0xa9,0x0f,0x8d,0x18,0xd4,0xa9,0x11,0x8d,0x04,0xd4,
        0xa9,0x00,0x8d,0x05,0xd4,0xa9,0xf0,0x8d,0x06,0xd4,0xa9,0x4c,0x8d,0x00,
        0xd4,0xa9,0x1d,0x8d,0x01,0xd4,0x60};
    std::memcpy(tune.data()+124, code, sizeof code);
    tune.back() = 0x60;
    g_assert_true(g_file_set_contents(filename, reinterpret_cast<const char *>(tune.data()), tune.size(), nullptr));
    char *error = nullptr;
    SpSynth *synth = sp_synth_open(filename, cancel_read, nullptr, &error);
    g_assert_nonnull(synth); g_assert_null(error);
    g_assert_cmpint(sp_synth_songs(synth), ==, 2);
    const int chunks[] = {1, 7, 127, 257, 1024, 4096, 3, 8192};
    long long frames = 0;
    double energy = 0;
    for (int chunk : chunks) {
        std::vector<float> samples(chunk*2 + 2, -123456.0f);
        int bytes = sp_synth_read(synth, reinterpret_cast<unsigned char *>(samples.data()+1), chunk*8);
        g_assert_cmpint(bytes, ==, chunk*8);
        g_assert_cmpfloat(samples.front(), ==, -123456.0f);
        g_assert_cmpfloat(samples.back(), ==, -123456.0f);
        for (int n = 1; n <= chunk*2; n++) {
            g_assert_true(std::isfinite(samples[n]));
            energy += double(samples[n])*samples[n];
        }
        frames += chunk;
        g_assert_cmpint(sp_synth_seek(synth, 0, SEEK_CUR), ==, frames*8);
    }
    g_assert_cmpfloat(energy, >, 0.001);
    g_assert_cmpint(sp_synth_seek(synth, 0, SEEK_SET), ==, 0);
    float after[64];
    g_assert_cmpint(sp_synth_read(synth, reinterpret_cast<unsigned char *>(after), sizeof after), ==, sizeof after);
    g_assert_cmpint(sp_synth_song(synth, 2), ==, 2);
    g_assert_cmpint(sp_synth_seek(synth, 0, SEEK_CUR), ==, 0);
    g_assert_cmpint(sp_synth_read(synth, reinterpret_cast<unsigned char *>(after), sizeof after), ==, sizeof after);
    cancelled = true;
    g_assert_cmpint(sp_synth_read(synth, reinterpret_cast<unsigned char *>(after), sizeof after), ==, AVERROR_EXIT);
    sp_synth_free(synth);
    std::remove(filename); std::remove(directory);
    g_free(filename); g_free(directory);
    g_print("OK SID streaming boundaries, stereo samples, rewind, track changes and cancellation\n");
}
