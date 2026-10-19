#ifndef SIMPLEPLAY_ENGINE_H
#define SIMPLEPLAY_ENGINE_H

#include <glib.h>
#include <cairo.h>

#define SP_MAX_TRACKS 96
#define SP_MAX_CHAPTERS 256

typedef struct SpEngine SpEngine;
typedef enum { SP_IDLE, SP_LOADING, SP_PLAYING, SP_PAUSED, SP_ENDED, SP_ERROR } SpState;
typedef enum { SP_AUDIO, SP_SUBTITLE } SpTrackType;
typedef enum { SP_DEINTERLACE_OFF, SP_DEINTERLACE_AUTO, SP_DEINTERLACE_ON } SpDeinterlace;

typedef struct {
    int index;
    SpTrackType type;
    char label[256];
} SpTrack;

typedef struct {
    double position;
    char title[256];
} SpChapter;

typedef struct {
    char title[512], artist[256], album[256], stream_title[512], format[128], description[512];
    char video_codec[64], audio_codec[64];
    double duration, fps;
    int width, height, sample_rate, channels;
    gboolean seekable, has_video, has_audio;
    int n_tracks, n_chapters;
    SpTrack tracks[SP_MAX_TRACKS];
    SpChapter chapters[SP_MAX_CHAPTERS];
} SpMediaInfo;

typedef struct {
    SpState state;
    double position, duration, volume, rate;
    int audio_track, subtitle_track;
    int output_sample_rate, output_channels;
    guint revision, seek_revision;
    guint64 video_frames, audio_samples;
    gboolean buffering;
    char error[512];
} SpStatus;

/* Frames are immutable and reference counted; Cairo uses native RGB24 bytes. */
typedef struct {
    gint refs;
    guint8 *pixels;
    int width, height, stride, rotation;
    double pts, aspect;
    guint serial;
} SpFrame;

SpEngine *sp_engine_new(void);
void sp_engine_free(SpEngine *engine);
void sp_engine_open(SpEngine *engine, const char *filename, double position, gboolean paused);
void sp_engine_stop(SpEngine *engine);
void sp_engine_video_enabled(SpEngine *engine, gboolean enabled);
void sp_engine_timeout(SpEngine *engine, int seconds);
void sp_engine_input_format(SpEngine *engine, const char *format, int sample_rate, int channels);
void sp_engine_pause(SpEngine *engine, gboolean paused);
void sp_engine_seek(SpEngine *engine, double position);
void sp_engine_volume(SpEngine *engine, double volume);
void sp_engine_rate(SpEngine *engine, double rate);
void sp_engine_deinterlace(SpEngine *engine, SpDeinterlace state, gboolean double_rate);
void sp_engine_audio(SpEngine *engine, int stream);
void sp_engine_subtitle(SpEngine *engine, int stream);
void sp_engine_subtitle_file(SpEngine *engine, const char *filename);
void sp_engine_subtitle_delay(SpEngine *engine, double seconds);
void sp_engine_status(SpEngine *engine, SpStatus *status);
void sp_engine_info(SpEngine *engine, SpMediaInfo *info);
SpFrame *sp_engine_frame(SpEngine *engine);
SpFrame *sp_engine_frame_at(SpEngine *engine, double presentation_delay);
SpFrame *sp_frame_ref(SpFrame *frame);
void sp_frame_unref(SpFrame *frame);
void sp_engine_draw_subtitles(SpEngine *engine, cairo_t *cr, double x, double y,
                              double width, double height);

#endif
