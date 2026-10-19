#include "../simpleplay-engine.h"
#include "../simpleplay-formats.h"
#include "../simpleplay-synth.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static SpEngine *engine;
static SpStatus status;

static void pump(double seconds)
{
    gint64 end = g_get_monotonic_time() + seconds * 1000000;
    do {
        SpFrame *frame = sp_engine_frame(engine);
        sp_frame_unref(frame);
        sp_engine_status(engine, &status);
        g_usleep(5000);
    } while (g_get_monotonic_time() < end);
}

static void wait_state(SpState state, double timeout)
{
    gint64 end = g_get_monotonic_time() + timeout * 1000000;
    do {
        pump(0.01);
        if (status.state == state) return;
        if (status.state == SP_ERROR && state != SP_ERROR) break;
    } while (g_get_monotonic_time() < end);
    g_error("Expected state %d, got %d at %.3f: %s", state, status.state, status.position, status.error);
}

static void wait_seek(double position, double timeout)
{
    gint64 end = g_get_monotonic_time() + timeout * 1000000;
    do {
        pump(0.01);
        SpFrame *frame = sp_engine_frame(engine);
        gboolean ready = frame && fabs(frame->pts - position) < 0.1;
        sp_frame_unref(frame);
        if (ready && fabs(status.position - position) < 0.1) return;
    } while (g_get_monotonic_time() < end);
    g_error("Accurate paused seek to %.3f failed (position %.3f, %s)", position, status.position, status.error);
}

static int subtitle_pixels(void)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 640, 360);
    cairo_t *cr = cairo_create(surface);
    sp_engine_draw_subtitles(engine, cr, 0, 0, 640, 360);
    cairo_surface_flush(surface);
    guint32 *data = (guint32 *)cairo_image_surface_get_data(surface);
    int count = 0;
    for (int n = 0; n < 640 * 360; n++) if (data[n] >> 24) count++;
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return count;
}

static void check_audio(const char *filename, const char *codec)
{
    sp_engine_open(engine, filename, 0, FALSE);
    wait_state(SP_PLAYING, 5);
    pump(0.25);
    SpMediaInfo info;
    sp_engine_info(engine, &info);
    g_assert_true(info.has_audio);
    g_assert_false(info.has_video);
    g_assert_cmpstr(info.audio_codec, ==, codec);
    g_assert_cmpuint(status.audio_samples, >, 1000);
    g_assert_cmpfloat(status.position, >, 0.05);
    sp_engine_pause(engine, TRUE);
    pump(0.02);
    double position = status.position;
    pump(0.12);
    g_assert_cmpfloat(fabs(status.position - position), <, 0.005);
    sp_engine_seek(engine, 1.5);
    pump(0.2);
    g_assert_cmpfloat(fabs(status.position - 1.5), <, 0.05);
    sp_engine_pause(engine, FALSE);
    pump(0.2);
    g_assert_cmpfloat(status.position, >, 1.55);
    g_assert_cmpint(status.state, !=, SP_ERROR);
    g_print("  decoded %s: %s\n", codec, filename);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--decode-only")) {
        char *contents = NULL;
        g_assert_true(g_file_get_contents(argv[2], &contents, NULL, NULL));
        char **lines = g_strsplit(contents, "\n", -1);
        engine = sp_engine_new();
        for (int n = 0; lines[n] && *lines[n]; n++) {
            char **fields = g_strsplit(lines[n], "|", 3);
            sp_engine_open(engine, fields[2], 0, FALSE);
            wait_state(SP_PLAYING, 8);
            pump(0.12);
            SpMediaInfo info;
            sp_engine_info(engine, &info);
            if (!strcmp(fields[0], "audio")) {
                g_assert_true(info.has_audio);
                g_assert_cmpstr(info.audio_codec, ==, fields[1]);
                g_assert_cmpuint(status.audio_samples, >, 100);
                if (!strcmp(fields[1], "midi") || !strcmp(fields[1], "sid")) {
                    char *error = NULL;
                    SpSynth *synth = sp_synth_open(fields[2], NULL, NULL, &error);
                    g_assert_nonnull(synth); g_assert_null(error);
                    float samples[8192];
                    int count = sp_synth_read(synth, (guint8 *)samples, sizeof(samples));
                    g_assert_cmpint(count, >, 0);
                    double energy = 0;
                    for (int k = 0; k < count / (int)sizeof(float); k++) { g_assert_true(isfinite(samples[k])); energy += samples[k] * samples[k]; }
                    g_assert_cmpfloat(energy, >, 0.001);
                    if (!strcmp(fields[1], "midi")) g_assert_cmpfloat(fabs(sp_synth_duration(synth) - 3.25), <, 0.01);
                    else { g_assert_cmpint(sp_synth_songs(synth), ==, 2); g_assert_cmpint(sp_synth_song(synth, 2), ==, 2); }
                    sp_synth_free(synth);
                }
            } else {
                g_assert_true(info.has_video);
                g_assert_cmpstr(info.video_codec, ==, fields[1]);
                g_assert_cmpuint(status.video_frames, >, 0);
                SpFrame *frame = sp_engine_frame(engine);
                g_assert_nonnull(frame); sp_frame_unref(frame);
            }
            g_assert_cmpint(status.state, !=, SP_ERROR);
            g_print("  decoded %s\n", fields[1]);
            g_strfreev(fields);
        }
        sp_engine_free(engine); g_strfreev(lines); g_free(contents);
        g_print("SimplePlay format corpus passed\n");
        return 0;
    }
    g_assert_cmpint(argc, >=, 5);
    engine = sp_engine_new();
    sp_engine_open(engine, argv[1], 0, FALSE);
    wait_state(SP_PLAYING, 5);
    pump(0.35);
    SpMediaInfo info;
    sp_engine_info(engine, &info);
    g_assert_true(info.has_video);
    g_assert_true(info.has_audio);
    g_assert_cmpint(info.width, ==, 320);
    g_assert_cmpint(info.height, ==, 180);
    g_assert_cmpint(info.n_chapters, ==, 2);
    g_assert_cmpint(info.n_tracks, ==, 3);
    g_assert_cmpuint(status.video_frames, >, 0);
    g_assert_cmpuint(status.audio_samples, >, 0);
    SpFrame *frame = sp_engine_frame(engine);
    g_assert_nonnull(frame);
    guint32 first = ((guint32 *)frame->pixels)[0];
    gboolean varied = FALSE;
    for (int n = 1; n < frame->width * frame->height; n++)
        if (((guint32 *)frame->pixels)[n] != first) { varied = TRUE; break; }
    g_assert_true(varied);
    sp_frame_unref(frame);
    sp_engine_pause(engine, TRUE);
    pump(0.03);
    double frozen = status.position;
    pump(0.15);
    g_assert_cmpfloat(fabs(status.position - frozen), <, 0.005);
    sp_engine_seek(engine, 3.25);
    wait_seek(3.25, 4);
    g_assert_cmpint(status.state, ==, SP_PAUSED);
    sp_engine_seek(engine, 1);
    wait_seek(1, 4);
    pump(0.08);
    g_assert_cmpint(subtitle_pixels(), >, 50);
    sp_engine_subtitle_delay(engine, 10);
    g_assert_cmpint(subtitle_pixels(), ==, 0);
    sp_engine_subtitle_delay(engine, 0);
    sp_engine_subtitle(engine, -1);
    pump(0.2);
    g_assert_cmpint(subtitle_pixels(), ==, 0);
    sp_engine_subtitle_file(engine, argv[3]);
    pump(0.2);
    g_assert_cmpint(status.subtitle_track, ==, -3);
    g_assert_cmpint(subtitle_pixels(), >, 50);
    int audio = -1;
    for (int n = 0; n < info.n_tracks; n++)
        if (info.tracks[n].type == SP_AUDIO && info.tracks[n].index != status.audio_track) audio = info.tracks[n].index;
    g_assert_cmpint(audio, >=, 0);
    sp_engine_audio(engine, audio);
    pump(0.2);
    g_assert_cmpint(status.audio_track, ==, audio);
    sp_engine_rate(engine, 2);
    pump(0.15);
    g_assert_cmpfloat(status.rate, ==, 2);
    double start = status.position;
    sp_engine_pause(engine, FALSE);
    pump(0.45);
    g_assert_cmpfloat(status.position - start, >, 0.55);
    g_assert_cmpfloat(status.position - start, <, 1.3);
    sp_engine_seek(engine, 5.5);
    wait_state(SP_ENDED, 4);
    frame = sp_engine_frame(engine);
    g_assert_nonnull(frame);
    g_assert_cmpfloat(frame->pts, >, 5.85);  /* Delayed B-frames must drain. */
    sp_frame_unref(frame);
    g_print("  MKV: audio/video, chapters, pause, accurate seeks, subtitle pixels, tracks, speed, EOF\n");
    check_audio(argv[2], "flac");
    if (argc > 5 && *argv[5]) check_audio(argv[5], "ape");
    sp_engine_open(engine, "/nonexistent/simpleplay/no-such-media.mkv", 0, FALSE);
    wait_state(SP_ERROR, 3);
    g_assert_true(*status.error != 0);
    sp_engine_open(engine, argv[4], 0, FALSE);
    wait_state(SP_PLAYING, 3);
    sp_engine_rate(engine, 1);
    wait_state(SP_ENDED, 4);
    sp_engine_info(engine, &info);
    g_assert_false(info.has_audio);
    g_assert_true(info.has_video);
    g_print("  silent video, error recovery and decoder drain\n");
    for (int n = 0; n < 16; n++) {
        sp_engine_open(engine, n % 2 ? argv[1] : argv[2], 0, n % 3 == 0);
        pump(0.025);
    }
    sp_engine_stop(engine);
    sp_engine_status(engine, &status);
    g_assert_cmpint(status.state, ==, SP_IDLE);
    if (argc > 6) {
        sp_engine_open(engine, argv[6], 0, FALSE);
        g_usleep(150000);
        gint64 began = g_get_monotonic_time();
        sp_engine_stop(engine);
        g_assert_cmpint(g_get_monotonic_time() - began, <, 2000000);
        g_print("  stalled network open cancels promptly\n");
    }
    if (argc > 8) {
        sp_engine_open(engine, argv[7], 0, FALSE);
        wait_state(SP_PLAYING, 3);
        wait_state(SP_ENDED, 4);
        g_assert_cmpfloat(status.position, >, 1.9);
        sp_engine_open(engine, argv[8], 0, FALSE);
        wait_state(SP_PLAYING, 3);
        pump(0.2);
        g_assert_cmpfloat(status.position, <, 0.5);
        wait_state(SP_ENDED, 3);
        g_print("  unequal stream lengths and delayed audio preserve the video timeline\n");
    }
    sp_engine_free(engine);
    g_print("SimplePlay engine checks passed\n");
    return 0;
}
