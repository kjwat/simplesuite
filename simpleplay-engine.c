/* SimplePlay's playback engine. No player process, libVLC, mpv or GStreamer.
 * The demuxer, audio decoder and video decoder have independent bounded queues.
 * Only the demux thread touches AVFormatContext's read/seek state. A generation
 * number invalidates every queued packet/frame after seeks and track changes.
 */
#include "simpleplay-engine.h"
#include "simpleplay-synth.h"
#include <SDL.h>
#include <ass/ass.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define PACKET_LIMIT (16 * 1024 * 1024)
#define FRAME_LIMIT 5
#define IO_TIMEOUT (15 * G_USEC_PER_SEC)

typedef struct {
    AVPacket *packet;
    guint serial;
    int stream;                    /* A NULL packet resets a decoder. */
    gboolean eof;
} Packet;

typedef struct {
    GMutex mutex;
    GCond changed;
    GQueue items;
    gsize bytes;
} PacketQueue;

typedef struct {
    guint8 *pixels;
    int x, y, width, height, canvas_width, canvas_height;
    double start, end;
} BitmapSubtitle;

typedef struct Session Session;
typedef struct {
    double current, target, step;
    int remaining;
} AudioGain;

struct SpEngine {
    GMutex mutex;
    GCond changed;
    GThread *thread;
    gint cancel, commands;
    gint timeout_seconds;
    gboolean video_enabled;
    char input_format[128];
    int raw_sample_rate, raw_channels;
    gint64 io_started;
    char *filename, *subtitle_file;
    gboolean subtitle_file_pending;
    gboolean paused, seek_pending;
    SpDeinterlace deinterlace;
    gboolean deinterlace_double_rate, video_filter_pending;
    double seek_target, drop_before, rate, requested_rate, volume, subtitle_delay;
    double anchor_position, anchor_time, audio_end, audio_started_at, audio_queue_end_time;
    gboolean audio_clock;
    SDL_AudioDeviceID audio_device;
    int audio_hz;
    double audio_period;
    AudioGain audio_gain;
    int requested_audio, requested_subtitle;
    guint serial;
    SpStatus status;
    SpMediaInfo info;
    GQueue frames, bitmaps;
    SpFrame *current;
    ASS_Library *ass;
    ASS_Renderer *renderer;
    ASS_Track *subtitles;
};

struct Session {
    SpEngine *engine;
    AVFormatContext *format;
    SpSynth *synth;
    AVIOContext *synth_io;
    PacketQueue audioq, videoq;
    GThread *audio_thread, *video_thread;
    AVCodecContext *sub_decoder;
    int audio, video, subtitle;
    double start_time;
    gint audio_done, video_done;
    gboolean eof;
};

static double now_seconds(void) { return g_get_monotonic_time() / 1000000.0; }
static gboolean cancelled(SpEngine *e) { return g_atomic_int_get(&e->cancel) != 0; }

SpFrame *sp_frame_ref(SpFrame *f)
{
    if (f) g_atomic_int_inc(&f->refs);
    return f;
}

void sp_frame_unref(SpFrame *f)
{
    if (f && g_atomic_int_dec_and_test(&f->refs)) {
        av_free(f->pixels);
        g_free(f);
    }
}

static void bitmap_free(BitmapSubtitle *b) { g_free(b->pixels); g_free(b); }

/* SDL's queue excludes the period already handed to the output backend. */
static double audio_queued(SpEngine *e)
{
    return e->audio_device ? SDL_GetQueuedAudioSize(e->audio_device) /
        (double)(e->audio_hz * 2 * sizeof(float)) : 0;
}

static void frames_clear(SpEngine *e)
{
    g_queue_clear_full(&e->frames, (GDestroyNotify)sp_frame_unref);
    sp_frame_unref(e->current);
    e->current = NULL;
}

static double position_locked(SpEngine *e)
{
    double position = e->anchor_position;
    if (e->status.state == SP_PLAYING && !e->paused) {
        if (e->audio_clock && e->audio_device) {
            double queued = audio_queued(e);
            /* SDL's current hardware period is outside the software queue. */
            position = e->audio_end - queued * e->rate;
            /* Continue trailing video after the audio stream finishes. */
            if (queued == 0)
                position += MAX(0, now_seconds() - e->audio_queue_end_time) * e->rate;
            position = MAX(e->audio_started_at, position - e->audio_period * e->rate);
        } else {
            position += (now_seconds() - e->anchor_time) * e->rate;
        }
    }
    return CLAMP(position, 0.0, e->info.duration > 0 ? e->info.duration : G_MAXDOUBLE);
}

static void error_set(SpEngine *e, gboolean fatal, const char *format, ...)
{
    va_list args;
    g_mutex_lock(&e->mutex);
    va_start(args, format);
    g_vsnprintf(e->status.error, sizeof(e->status.error), format, args);
    va_end(args);
    if (fatal) e->status.state = SP_ERROR;
    g_mutex_unlock(&e->mutex);
}

static void av_error(SpEngine *e, gboolean fatal, const char *message, int result)
{
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, detail, sizeof(detail));
    error_set(e, fatal, "%s: %s", message, detail);
}

static void packet_free(Packet *p)
{
    av_packet_free(&p->packet);
    g_free(p);
}

static void queue_init(PacketQueue *q)
{
    g_mutex_init(&q->mutex);
    g_cond_init(&q->changed);
    g_queue_init(&q->items);
}

static void queue_destroy(PacketQueue *q)
{
    g_queue_clear_full(&q->items, (GDestroyNotify)packet_free);
    g_mutex_clear(&q->mutex);
    g_cond_clear(&q->changed);
}

static void queue_reset(PacketQueue *q, guint serial, int stream)
{
    Packet *p = g_new0(Packet, 1);
    p->serial = serial;
    p->stream = stream;
    g_mutex_lock(&q->mutex);
    g_queue_clear_full(&q->items, (GDestroyNotify)packet_free);
    q->bytes = 0;
    g_queue_push_tail(&q->items, p);
    g_cond_broadcast(&q->changed);
    g_mutex_unlock(&q->mutex);
}

static gboolean queue_push(PacketQueue *q, SpEngine *e, AVPacket *packet,
                           guint serial, int stream, gboolean eof)
{
    g_mutex_lock(&q->mutex);
    while ((q->bytes >= PACKET_LIMIT || q->items.length >= 512) &&
           !cancelled(e) && !g_atomic_int_get(&e->commands))
        g_cond_wait_until(&q->changed, &q->mutex, g_get_monotonic_time() + 10000);
    if (cancelled(e) || g_atomic_int_get(&e->commands)) {
        g_mutex_unlock(&q->mutex);
        return FALSE;
    }
    Packet *p = g_new0(Packet, 1);
    p->packet = packet ? av_packet_clone(packet) : NULL;
    p->eof = eof;
    p->serial = serial;
    p->stream = stream;
    if (p->packet) q->bytes += p->packet->size;
    g_queue_push_tail(&q->items, p);
    g_cond_broadcast(&q->changed);
    g_mutex_unlock(&q->mutex);
    return TRUE;
}

static Packet *queue_pop(PacketQueue *q, SpEngine *e)
{
    g_mutex_lock(&q->mutex);
    while (g_queue_is_empty(&q->items) && !cancelled(e))
        g_cond_wait_until(&q->changed, &q->mutex, g_get_monotonic_time() + 10000);
    Packet *p = cancelled(e) ? NULL : g_queue_pop_head(&q->items);
    if (p && p->packet) q->bytes -= p->packet->size;
    g_cond_broadcast(&q->changed);
    g_mutex_unlock(&q->mutex);
    return p;
}

static gboolean generation_current(SpEngine *e, guint serial)
{
    g_mutex_lock(&e->mutex);
    gboolean current = e->serial == serial && !cancelled(e);
    g_mutex_unlock(&e->mutex);
    return current;
}

static int io_interrupt(void *opaque)
{
    SpEngine *e = opaque;
    return cancelled(e) || (e->io_started &&
           g_get_monotonic_time() - e->io_started > (gint64)g_atomic_int_get(&e->timeout_seconds) * G_USEC_PER_SEC);
}

static AVCodecContext *decoder_open(Session *s, int stream)
{
    if (stream < 0) return NULL;
    AVStream *st = s->format->streams[stream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    gboolean fatal = st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE;
    if (!codec) { error_set(s->engine, fatal, "No decoder for %s", avcodec_get_name(st->codecpar->codec_id)); return NULL; }
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    if (!decoder) return NULL;
    decoder->thread_count = 4;
    decoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    decoder->pkt_timebase = st->time_base;
    int result = avcodec_parameters_to_context(decoder, st->codecpar);
    if (result >= 0) result = avcodec_open2(decoder, codec, NULL);
    if (result < 0) {
        av_error(s->engine, fatal, "Cannot open decoder", result);
        avcodec_free_context(&decoder);
    }
    return decoder;
}

static const char *tag(AVDictionary *metadata, const char *name)
{
    const AVDictionaryEntry *entry = av_dict_get(metadata, name, NULL, 0);
    return entry ? entry->value : "";
}

static void text_copy(char *destination, const char *source, gsize size)
{
    g_autofree char *valid = g_utf8_make_valid(source ? source : "", -1);
    g_strlcpy(destination, valid, size);
    const char *end = NULL;
    if (!g_utf8_validate(destination, -1, &end)) destination[end - destination] = 0;
}

static void read_info(Session *s)
{
    SpEngine *e = s->engine;
    AVFormatContext *f = s->format;
    g_mutex_lock(&e->mutex);
    SpMediaInfo *i = &e->info;
    text_copy(i->title, tag(f->metadata, "title"), sizeof(i->title));
    text_copy(i->artist, tag(f->metadata, "artist"), sizeof(i->artist));
    text_copy(i->album, tag(f->metadata, "album"), sizeof(i->album));
    g_strlcpy(i->format, f->iformat->long_name ? f->iformat->long_name : f->iformat->name, sizeof(i->format));
    i->duration = f->duration != AV_NOPTS_VALUE ? MAX(0, f->duration / (double)AV_TIME_BASE) : 0;
    i->seekable = f->pb && (f->pb->seekable & AVIO_SEEKABLE_NORMAL);
    i->has_audio = s->audio >= 0;
    i->has_video = s->video >= 0;
    if (s->video >= 0) {
        AVStream *st = f->streams[s->video];
        i->width = st->codecpar->width;
        i->height = st->codecpar->height;
        i->fps = av_q2d(av_guess_frame_rate(f, st, NULL));
        g_strlcpy(i->video_codec, avcodec_get_name(st->codecpar->codec_id), sizeof(i->video_codec));
    }
    if (s->audio >= 0) {
        AVCodecParameters *a = f->streams[s->audio]->codecpar;
        i->sample_rate = a->sample_rate;
        i->channels = a->ch_layout.nb_channels;
        g_strlcpy(i->audio_codec, avcodec_get_name(a->codec_id), sizeof(i->audio_codec));
    }
    for (unsigned n = 0; n < f->nb_streams && i->n_tracks < SP_MAX_TRACKS; n++) {
        AVStream *st = f->streams[n];
        enum AVMediaType type = st->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_SUBTITLE) continue;
        SpTrack *t = &i->tracks[i->n_tracks++];
        t->index = n;
        t->type = type == AVMEDIA_TYPE_AUDIO ? SP_AUDIO : SP_SUBTITLE;
        g_autofree char *title = g_utf8_make_valid(tag(st->metadata, "title"), -1);
        g_autofree char *language = g_utf8_make_valid(tag(st->metadata, "language"), -1);
        char layout[64] = "";
        if (type == AVMEDIA_TYPE_AUDIO)
            av_channel_layout_describe(&st->codecpar->ch_layout, layout, sizeof(layout));
        g_snprintf(t->label, sizeof(t->label), "%s%s%s%s%s · %s%s%s%s",
                   *title ? title : (type == AVMEDIA_TYPE_AUDIO ? "Audio" : "Subtitles"),
                   *language ? " (" : "", language, *language ? ")" : "",
                   st->disposition & AV_DISPOSITION_FORCED ? " · forced" : "",
                   avcodec_get_name(st->codecpar->codec_id), *layout ? " · " : "", layout,
                   st->disposition & AV_DISPOSITION_DEFAULT ? " · default" : "");
        const char *end = NULL;
        if (!g_utf8_validate(t->label, -1, &end)) t->label[end - t->label] = 0;
    }
    for (unsigned n = 0; n < f->nb_chapters && i->n_chapters < SP_MAX_CHAPTERS; n++) {
        AVChapter *ch = f->chapters[n];
        SpChapter *out = &i->chapters[i->n_chapters++];
        out->position = MAX(0, ch->start * av_q2d(ch->time_base) - s->start_time);
        const char *title = tag(ch->metadata, "title");
        if (*title) text_copy(out->title, title, sizeof(out->title));
        else g_snprintf(out->title, sizeof(out->title), "Chapter %u", n + 1);
    }
    if (i->has_video)
        g_snprintf(i->description, sizeof(i->description), "%s · %d × %d · %.3g fps%s%s",
                   i->video_codec, i->width, i->height, i->fps,
                   i->has_audio ? " · " : "", i->audio_codec);
    else
        g_snprintf(i->description, sizeof(i->description), "%s · %.1f kHz · %d channel%s",
                   i->audio_codec, i->sample_rate / 1000.0, i->channels, i->channels == 1 ? "" : "s");
    if (s->synth) {
        text_copy(i->audio_codec, sp_synth_codec(s->synth), sizeof(i->audio_codec));
        text_copy(i->format, !strcmp(i->audio_codec, "midi") ? "Standard MIDI · FluidSynth" : "Commodore 64 SID · libsidplayfp", sizeof(i->format));
        g_snprintf(i->description, sizeof(i->description), "%s · 48 kHz · stereo", i->format);
        int songs = sp_synth_songs(s->synth);
        if (songs > 1) {
            i->n_tracks = 0;
            for (int n = 1; n <= songs && i->n_tracks < SP_MAX_TRACKS; n++) {
                SpTrack *t = &i->tracks[i->n_tracks++];
                t->index = 10000 + n; t->type = SP_AUDIO;
                g_snprintf(t->label, sizeof(t->label), "Subsong %d", n);
            }
        }
    }
    e->status.revision++;
    g_mutex_unlock(&e->mutex);
}

static int audio_filter(AVFilterGraph **graph, AVFilterContext **source,
                        AVFilterContext **sink, const AVFrame *frame, double rate, int output_hz)
{
    *graph = avfilter_graph_alloc();
    if (!*graph) return AVERROR(ENOMEM);
    (*graph)->nb_threads = 1;
    char layout[128], options[512], filters[256];
    av_channel_layout_describe(&frame->ch_layout, layout, sizeof(layout));
    g_snprintf(options, sizeof(options), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
               frame->sample_rate, frame->sample_rate, av_get_sample_fmt_name(frame->format), layout);
    int result = avfilter_graph_create_filter(source, avfilter_get_by_name("abuffer"),
                                               "input", options, NULL, *graph);
    if (result < 0) return result;
    result = avfilter_graph_create_filter(sink, avfilter_get_by_name("abuffersink"),
                                          "output", NULL, NULL, *graph);
    if (result < 0) return result;
    AVFilterInOut *inputs = avfilter_inout_alloc(), *outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        return AVERROR(ENOMEM);
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = *source;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = *sink;
    inputs->pad_idx = 0;
    /* At unity speed the samples must never pass through a time stretcher.
     * Negotiate the source rate with SDL; resample only if the device needs it.
     * Normalize surround downmix sums; retain stereo float headroom for the
     * system mixer's final volume gain. */
    g_snprintf(filters, sizeof(filters),
        "aresample=%d:filter_size=64:cutoff=0.97:rematrix_maxval=1,aformat=sample_fmts=flt:channel_layouts=stereo", output_hz);
    if (fabs(rate - 1.0) > 0.00001) {
        char tempo[48];
        g_snprintf(tempo, sizeof(tempo), ",atempo=%.5f", rate);
        g_strlcat(filters, tempo, sizeof(filters));
    }
    result = avfilter_graph_parse_ptr(*graph, filters, &inputs, &outputs, NULL);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return result < 0 ? result : avfilter_graph_config(*graph, NULL);
}

/* Called with the engine mutex held. Steady gain remains a single exact scale;
 * changes ramp for five milliseconds to avoid abrupt control discontinuities.
 * Preserve finite floating-point headroom for the system mixer's final gain. */
static int audio_gain(SpEngine *e, AVFrame *frame)
{
    AudioGain *gain = &e->audio_gain;
    if (gain->target != e->volume) {
        gain->target = e->volume;
        gain->remaining = MAX(1, (e->audio_hz + 199) / 200);
        gain->step = (gain->target - gain->current) / gain->remaining;
    }
    gboolean adjust = gain->remaining || gain->current != 1.0;
    const float *input = (const float *)frame->data[0];
    if (!adjust) {
        for (int n = 0; n < frame->nb_samples * 2; n++) {
            if (!isfinite(input[n])) { adjust = TRUE; break; }
        }
    }
    if (!adjust) return 0;
    int result = av_frame_make_writable(frame);
    if (result < 0) return result;
    float *samples = (float *)frame->data[0];
    for (int n = 0; n < frame->nb_samples; n++) {
        if (gain->remaining) {
            gain->current += gain->step;
            if (!--gain->remaining) gain->current = gain->target;
        }
        for (int channel = 0; channel < 2; channel++) {
            int index = n * 2 + channel;
            float value = samples[index];
            value = isfinite(value) ? value * gain->current : 0.0f;
            samples[index] = isfinite(value) ? value : 0.0f;
        }
    }
    return 0;
}

static gboolean audio_output(Session *s, AVFrame *frame, guint serial,
                              double *audio_end, double rate)
{
    SpEngine *e = s->engine;
    while (!cancelled(e)) {
        g_mutex_lock(&e->mutex);
        if (serial != e->serial) { g_mutex_unlock(&e->mutex); return FALSE; }
        SDL_AudioDeviceID device = e->audio_device;
        gboolean wait = e->paused || audio_queued(e) > 0.15;
        if (!wait) {
            if (audio_gain(e, frame) < 0) {
                g_mutex_unlock(&e->mutex);
                error_set(e, TRUE, "Cannot allocate audio volume buffer");
                return FALSE;
            }
            int result = device ? SDL_QueueAudio(device, frame->data[0], frame->nb_samples * 2 * sizeof(float)) : 0;
            *audio_end += frame->nb_samples / (double)e->audio_hz * rate;
            e->audio_end = *audio_end;
            if (device) e->audio_queue_end_time = now_seconds() + audio_queued(e);
            if (device && result == 0) e->audio_clock = TRUE;
            e->status.audio_samples += frame->nb_samples;
            g_mutex_unlock(&e->mutex);
            if (result < 0) error_set(e, FALSE, "Audio output: %s", SDL_GetError());
            /* With no audio device, pace decoding using the fallback clock. */
            if (!device) {
                while (!cancelled(e) && generation_current(e, serial)) {
                    g_mutex_lock(&e->mutex);
                    double ahead = *audio_end - position_locked(e);
                    g_mutex_unlock(&e->mutex);
                    if (ahead < 0.3) break;
                    g_usleep(5000);
                }
            }
            return TRUE;
        }
        g_cond_wait_until(&e->changed, &e->mutex, g_get_monotonic_time() + 5000);
        g_mutex_unlock(&e->mutex);
    }
    return FALSE;
}

static void audio_close(SpEngine *e, SDL_AudioDeviceID *device)
{
    g_mutex_lock(&e->mutex);
    e->anchor_position = position_locked(e);
    e->anchor_time = now_seconds();
    e->audio_device = 0;
    e->audio_clock = FALSE;
    e->status.output_sample_rate = e->status.output_channels = 0;
    g_mutex_unlock(&e->mutex);
    if (*device) SDL_CloseAudioDevice(*device);
    *device = 0;
}

static SDL_AudioDeviceID audio_open(SpEngine *e, int sample_rate)
{
    SDL_AudioSpec want = {0}, have = {0};
    want.freq = sample_rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!device) { error_set(e, TRUE, "Cannot open audio output: %s", SDL_GetError()); return 0; }
    g_mutex_lock(&e->mutex);
    e->audio_device = device;
    e->audio_hz = have.freq;
    e->audio_period = have.samples / (double)have.freq;
    e->audio_gain = (AudioGain){.current = e->volume, .target = e->volume};
    e->status.output_sample_rate = have.freq;
    e->status.output_channels = have.channels;
    e->status.revision++;
    SDL_PauseAudioDevice(device, e->paused);
    g_mutex_unlock(&e->mutex);
    return device;
}

static gpointer audio_worker(gpointer opaque)
{
    Session *s = opaque;
    SpEngine *e = s->engine;
    AVCodecContext *decoder = NULL;
    AVFilterGraph *graph = NULL;
    AVFilterContext *source = NULL, *sink = NULL;
    AVFrame *frame = av_frame_alloc(), *out = av_frame_alloc();
    guint serial = 0;
    int stream = -1, input_hz = 0, input_format = -1;
    AVChannelLayout input_layout = {0};
    double rate = 1, audio_end = 0, drop = 0;
    gboolean started = FALSE;
    SDL_AudioDeviceID device = 0;
    while (!cancelled(e)) {
        Packet *p = queue_pop(&s->audioq, e);
        if (!p) break;
        if (!p->packet && !p->eof) {
            avcodec_free_context(&decoder);
            avfilter_graph_free(&graph);
            source = sink = NULL;
            audio_close(e, &device);
            stream = p->stream;
            serial = p->serial;
            decoder = decoder_open(s, stream);
            g_mutex_lock(&e->mutex);
            if (device) SDL_ClearQueuedAudio(device);
            rate = e->rate;
            drop = e->drop_before;
            audio_end = drop;
            e->audio_clock = FALSE;
            e->audio_end = drop;
            if (device) SDL_PauseAudioDevice(device, e->paused);
            g_mutex_unlock(&e->mutex);
            started = FALSE;
            g_atomic_int_set(&s->audio_done, 0);
            packet_free(p);
            continue;
        }
        if (!decoder || p->serial != serial || !generation_current(e, serial)) {
            if (p->eof) g_atomic_int_set(&s->audio_done, 1);
            packet_free(p);
            continue;
        }
        int result = avcodec_send_packet(decoder, p->packet);
        if (result >= 0) {
            while (!cancelled(e) && avcodec_receive_frame(decoder, frame) >= 0) {
                double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ?
                    frame->best_effort_timestamp * av_q2d(s->format->streams[stream]->time_base) - s->start_time : audio_end;
                double end = pts + frame->nb_samples / (double)frame->sample_rate;
                if (end <= drop) { av_frame_unref(frame); continue; }
                if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
                    int channels = frame->ch_layout.nb_channels;
                    av_channel_layout_uninit(&frame->ch_layout);
                    av_channel_layout_default(&frame->ch_layout, channels);
                }
                /* Radio streams may change format without opening a new file.
                 * Drain the old conversion before renegotiating the output. */
                if (graph && (input_hz != frame->sample_rate || input_format != frame->format ||
                              av_channel_layout_compare(&input_layout, &frame->ch_layout))) {
                    if (av_buffersrc_add_frame_flags(source, NULL, 0) >= 0)
                        while (av_buffersink_get_frame(sink, out) >= 0) {
                            gboolean current = audio_output(s, out, serial, &audio_end, rate);
                            av_frame_unref(out);
                            if (!current) break;
                        }
                    while (generation_current(e, serial)) {
                        g_mutex_lock(&e->mutex);
                        gboolean drained = !e->paused && audio_queued(e) == 0 &&
                            now_seconds() >= e->audio_queue_end_time + e->audio_period;
                        g_mutex_unlock(&e->mutex);
                        if (drained) break;
                        g_usleep(5000);
                    }
                    avfilter_graph_free(&graph);
                    source = sink = NULL;
                    audio_close(e, &device);
                }
                if (!generation_current(e, serial)) { av_frame_unref(frame); break; }
                if (!graph) {
                    device = audio_open(e, frame->sample_rate);
                    if (!device) { av_frame_unref(frame); break; }
                    result = audio_filter(&graph, &source, &sink, frame, rate, e->audio_hz);
                    if (result < 0) {
                        av_error(e, TRUE, "Cannot prepare audio", result);
                        avfilter_graph_free(&graph); source = sink = NULL;
                        av_frame_unref(frame); break;
                    }
                    input_hz = frame->sample_rate; input_format = frame->format;
                    av_channel_layout_uninit(&input_layout);
                    av_channel_layout_copy(&input_layout, &frame->ch_layout);
                }
                if (!started) {
                    audio_end = drop;
                    g_mutex_lock(&e->mutex);
                    e->audio_started_at = audio_end;
                    g_mutex_unlock(&e->mutex);
                    /* Pad an intentionally delayed audio start; jumping the
                     * master clock would skip the beginning of the picture. */
                    double gap = pts - drop;
                    while (gap > 0.001 && generation_current(e, serial)) {
                        out->format = AV_SAMPLE_FMT_FLT;
                        out->sample_rate = e->audio_hz;
                        av_channel_layout_default(&out->ch_layout, 2);
                        out->nb_samples = MIN(1024, (int)ceil(gap / rate * e->audio_hz));
                        if (av_frame_get_buffer(out, 0) < 0) break;
                        memset(out->data[0], 0, out->nb_samples * 2 * sizeof(float));
                        int samples = out->nb_samples;
                        gboolean current = audio_output(s, out, serial, &audio_end, rate);
                        av_frame_unref(out);
                        gap -= samples / (double)e->audio_hz * rate;
                        if (!current) break;
                    }
                    started = TRUE;
                }
                /* Trim pre-roll from the first frame after an accurate seek. */
                if (pts < drop) {
                    int skip = MIN(frame->nb_samples, (int)llround((drop - pts) * frame->sample_rate));
                    int planar = av_sample_fmt_is_planar(frame->format);
                    int planes = planar ? frame->ch_layout.nb_channels : 1;
                    int step = av_get_bytes_per_sample(frame->format) * (planar ? 1 : frame->ch_layout.nb_channels);
                    for (int n = 0; n < planes; n++) frame->extended_data[n] += skip * step;
                    frame->nb_samples -= skip;
                }
                frame->pts = llround(MAX(pts, drop) * frame->sample_rate);
                result = av_buffersrc_add_frame_flags(source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
                av_frame_unref(frame);
                if (result < 0) { av_error(e, TRUE, "Audio filter rejected a frame", result); break; }
                while (av_buffersink_get_frame(sink, out) >= 0) {
                    gboolean current = audio_output(s, out, serial, &audio_end, rate);
                    av_frame_unref(out);
                    if (!current) break;
                }
                if (!generation_current(e, serial)) break;
            }
        } else if (result != AVERROR_EOF) av_error(e, FALSE, "Damaged audio packet", result);
        if (p->eof) {
            if (source && av_buffersrc_add_frame_flags(source, NULL, 0) >= 0)
                while (av_buffersink_get_frame(sink, out) >= 0) {
                    audio_output(s, out, serial, &audio_end, rate);
                    av_frame_unref(out);
                }
            g_atomic_int_set(&s->audio_done, 1);
        }
        packet_free(p);
    }
    audio_close(e, &device);
    av_channel_layout_uninit(&input_layout);
    avfilter_graph_free(&graph);
    avcodec_free_context(&decoder);
    av_frame_free(&frame);
    av_frame_free(&out);
    return NULL;
}

typedef struct {
    struct SwsContext *context;
    int width, height, format, colorspace, range, chroma;
} VideoScale;

static int video_colorspace(const AVFrame *frame, const AVStream *stream)
{
    enum AVColorSpace space = frame->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED) space = stream->codecpar->color_space;
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
    default:
        /* Match VLC's fallback for untagged HD, while preserving tagged SD. */
        return frame->height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

static int video_range(const AVFrame *frame, const AVStream *stream)
{
    enum AVColorRange range = frame->color_range;
    if (range == AVCOL_RANGE_UNSPECIFIED) range = stream->codecpar->color_range;
    if (range != AVCOL_RANGE_UNSPECIFIED) return range == AVCOL_RANGE_JPEG;
    const AVPixFmtDescriptor *format = av_pix_fmt_desc_get(frame->format);
    if (format && (format->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL))) return 1;
    return frame->format == AV_PIX_FMT_YUVJ420P || frame->format == AV_PIX_FMT_YUVJ422P ||
        frame->format == AV_PIX_FMT_YUVJ444P || frame->format == AV_PIX_FMT_YUVJ440P ||
        frame->format == AV_PIX_FMT_YUVJ411P;
}

static gboolean video_scale_open(VideoScale *v, const AVFrame *frame, const AVStream *stream)
{
    int colorspace = video_colorspace(frame, stream), range = video_range(frame, stream);
    enum AVChromaLocation chroma = frame->chroma_location;
    if (chroma == AVCHROMA_LOC_UNSPECIFIED) chroma = stream->codecpar->chroma_location;
    if (chroma == AVCHROMA_LOC_UNSPECIFIED) chroma = AVCHROMA_LOC_LEFT;
    if (v->context && v->width == frame->width && v->height == frame->height &&
        v->format == frame->format && v->colorspace == colorspace && v->range == range &&
        v->chroma == (int)chroma) return TRUE;
    sws_freeContext(v->context);
    v->context = sws_alloc_context();
    if (!v->context) return FALSE;
    av_opt_set_int(v->context, "srcw", frame->width, 0);
    av_opt_set_int(v->context, "srch", frame->height, 0);
    av_opt_set_int(v->context, "dstw", frame->width, 0);
    av_opt_set_int(v->context, "dsth", frame->height, 0);
    av_opt_set_int(v->context, "src_format", frame->format, 0);
    av_opt_set_int(v->context, "dst_format", AV_PIX_FMT_RGB32, 0);
    av_opt_set_int(v->context, "sws_flags", SWS_LANCZOS | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, 0);
    /* Locations are measured in 1/256 of a luma sample. */
    int x = chroma == AVCHROMA_LOC_LEFT || chroma == AVCHROMA_LOC_TOPLEFT || chroma == AVCHROMA_LOC_BOTTOMLEFT ? 0 : 128;
    int y = chroma == AVCHROMA_LOC_TOPLEFT || chroma == AVCHROMA_LOC_TOP ? 0 :
            chroma == AVCHROMA_LOC_BOTTOMLEFT || chroma == AVCHROMA_LOC_BOTTOM ? 256 : 128;
    av_opt_set_int(v->context, "src_h_chr_pos", x, 0);
    av_opt_set_int(v->context, "src_v_chr_pos", y, 0);
    if (sws_init_context(v->context, NULL, NULL) < 0 ||
        sws_setColorspaceDetails(v->context, sws_getCoefficients(colorspace), range,
            sws_getCoefficients(colorspace), 1, 0, 1 << 16, 1 << 16) < 0) {
        sws_freeContext(v->context); v->context = NULL;
        return FALSE;
    }
    v->width = frame->width; v->height = frame->height; v->format = frame->format;
    v->colorspace = colorspace; v->range = range; v->chroma = chroma;
    return TRUE;
}

static gboolean video_output(Session *s, AVFrame *frame, VideoScale *scale,
                              guint serial, double pts)
{
    SpEngine *e = s->engine;
    AVStream *stream = s->format->streams[s->video];
    g_mutex_lock(&e->mutex);
    if (pts + 0.001 < e->drop_before || serial != e->serial) {
        g_mutex_unlock(&e->mutex);
        return TRUE;
    }
    while (e->frames.length >= FRAME_LIMIT && serial == e->serial && !cancelled(e))
        g_cond_wait_until(&e->changed, &e->mutex, g_get_monotonic_time() + 10000);
    gboolean current = e->serial == serial && !cancelled(e);
    g_mutex_unlock(&e->mutex);
    if (!current) return FALSE;
    SpFrame *f = g_new0(SpFrame, 1);
    f->refs = 1;
    f->width = frame->width;
    f->height = frame->height;
    f->stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, f->width);
    f->pixels = av_malloc((gsize)f->stride * f->height);
    f->pts = MAX(0, pts);
    f->serial = serial;
    AVRational sar = av_guess_sample_aspect_ratio(s->format, stream, frame);
    f->aspect = frame->width / (double)frame->height * (sar.num > 0 ? av_q2d(sar) : 1);
    size_t matrix_size = 0;
    const int32_t *matrix = NULL;
    AVFrameSideData *frame_matrix = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (frame_matrix) {
        matrix = (const int32_t *)frame_matrix->data;
        matrix_size = frame_matrix->size;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const AVPacketSideData *stream_matrix = av_packet_side_data_get(stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!matrix && stream_matrix) {
        matrix = (const int32_t *)stream_matrix->data;
        matrix_size = stream_matrix->size;
    }
#else
    if (!matrix) matrix = (const int32_t *)av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &matrix_size);
#endif
    if (matrix && matrix_size >= 9 * sizeof(int32_t)) {
        double rotation = -av_display_rotation_get(matrix);
        if (isfinite(rotation)) f->rotation = ((int)llround(rotation / 90) * 90 % 360 + 360) % 360;
    }
    if (!f->pixels || !video_scale_open(scale, frame, stream)) {
        error_set(e, TRUE, "Cannot convert the decoded picture for display"); sp_frame_unref(f); return FALSE;
    }
    guint8 *dest[4] = { f->pixels, NULL, NULL, NULL };
    int strides[4] = { f->stride, 0, 0, 0 };
    sws_scale(scale->context, (const guint8 *const *)frame->data, frame->linesize, 0, frame->height, dest, strides);
    g_mutex_lock(&e->mutex);
    if (serial == e->serial) {
        g_queue_push_tail(&e->frames, f);
        e->status.video_frames++;
    } else sp_frame_unref(f);
    g_mutex_unlock(&e->mutex);
    return TRUE;
}

typedef struct {
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    int width, height, format, colorspace, range;
    AVRational sar;
    SpDeinterlace state;
    gboolean double_rate;
} VideoFilter;

static void video_filter_clear(VideoFilter *filter)
{
    avfilter_graph_free(&filter->graph);
    filter->source = filter->sink = NULL;
}

static int video_filter_open(VideoFilter *v, const AVFrame *frame, AVRational time_base,
                              AVRational fps, AVRational sar, SpDeinterlace state, gboolean double_rate)
{
    const AVFilter *buffer = avfilter_get_by_name("buffer");
    const AVFilter *yadif = avfilter_get_by_name("yadif");
    const AVFilter *sink = avfilter_get_by_name("buffersink");
    if (!buffer || !yadif || !sink) return AVERROR_FILTER_NOT_FOUND;
    v->graph = avfilter_graph_alloc();
    if (!v->graph) return AVERROR(ENOMEM);
    v->graph->nb_threads = 2;
    char options[256];
    g_snprintf(options, sizeof(options),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
        frame->width, frame->height, frame->format, time_base.num, time_base.den,
        sar.num, sar.den, fps.num, fps.den);
    int result = avfilter_graph_create_filter(&v->source, buffer, "input", options, NULL, v->graph);
    if (result < 0) return result;
#if LIBAVFILTER_VERSION_MAJOR >= 10
    AVBufferSrcParameters *parameters = av_buffersrc_parameters_alloc();
    if (!parameters) return AVERROR(ENOMEM);
    parameters->color_space = frame->colorspace;
    parameters->color_range = frame->color_range;
    result = av_buffersrc_parameters_set(v->source, parameters);
    av_free(parameters);
    if (result < 0) return result;
#endif
    AVFilterContext *deinterlace = NULL;
    g_snprintf(options, sizeof(options), "mode=%s:parity=auto:deint=%s",
        double_rate ? "send_field" : "send_frame", state == SP_DEINTERLACE_AUTO ? "interlaced" : "all");
    result = avfilter_graph_create_filter(&deinterlace, yadif, "deinterlace", options, NULL, v->graph);
    if (result < 0) return result;
    result = avfilter_graph_create_filter(&v->sink, sink, "output", NULL, NULL, v->graph);
    if (result < 0) return result;
    result = avfilter_link(v->source, 0, deinterlace, 0);
    if (result >= 0) result = avfilter_link(deinterlace, 0, v->sink, 0);
    if (result >= 0) result = avfilter_graph_config(v->graph, NULL);
    if (result < 0) return result;
    v->width = frame->width; v->height = frame->height; v->format = frame->format;
    v->colorspace = frame->colorspace; v->range = frame->color_range;
    v->sar = sar; v->state = state; v->double_rate = double_rate;
    return 0;
}

static gboolean video_filter_drain(Session *s, VideoFilter *v, AVFrame *out,
                                    VideoScale *scale, guint serial)
{
    int result;
    while ((result = av_buffersink_get_frame(v->sink, out)) >= 0) {
        /* Yadif's field-rate output uses a different time base from the stream. */
        double pts = out->pts * av_q2d(av_buffersink_get_time_base(v->sink)) - s->start_time;
        gboolean current = video_output(s, out, scale, serial, pts);
        av_frame_unref(out);
        if (!current) return FALSE;
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        av_error(s->engine, TRUE, "Cannot read the deinterlaced picture", result);
        return FALSE;
    }
    return TRUE;
}

static gboolean video_process(Session *s, AVFrame *frame, VideoFilter *v, AVFrame *out,
                               VideoScale *scale, guint serial, double *next_pts)
{
    SpEngine *e = s->engine;
    AVStream *stream = s->format->streams[s->video];
    AVRational fps = av_guess_frame_rate(s->format, stream, frame);
    if (fps.num <= 0 || fps.den <= 0) fps = (AVRational){25, 1};
    double pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? *next_pts :
        frame->best_effort_timestamp * av_q2d(stream->time_base) - s->start_time;
    *next_pts = pts + av_q2d(av_inv_q(fps));
    g_mutex_lock(&e->mutex);
    SpDeinterlace state = e->deinterlace;
    gboolean double_rate = e->deinterlace_double_rate;
    g_mutex_unlock(&e->mutex);
    if (v->graph && (v->state != state || v->double_rate != double_rate)) video_filter_clear(v);
    if (state == SP_DEINTERLACE_OFF) return video_output(s, frame, scale, serial, pts);
    AVRational sar = frame->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = (AVRational){1, 1};
    if (v->graph && (v->width != frame->width || v->height != frame->height ||
        v->format != frame->format || v->colorspace != (int)frame->colorspace ||
        v->range != (int)frame->color_range || av_cmp_q(v->sar, sar))) {
        int result = av_buffersrc_add_frame_flags(v->source, NULL, 0);
        if (result < 0 || !video_filter_drain(s, v, out, scale, serial)) {
            if (result < 0) av_error(e, TRUE, "Cannot drain the deinterlacer", result);
            return FALSE;
        }
        video_filter_clear(v);
    }
    if (!v->graph) {
        int result = video_filter_open(v, frame, stream->time_base, fps, sar, state, double_rate);
        if (result < 0) {
            av_error(e, TRUE, "Cannot start Yadif deinterlacing", result);
            video_filter_clear(v);
            return FALSE;
        }
    }
    frame->pts = llround((pts + s->start_time) / av_q2d(stream->time_base));
    int result = av_buffersrc_add_frame_flags(v->source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (result < 0) { av_error(e, TRUE, "Deinterlacer rejected a picture", result); return FALSE; }
    return video_filter_drain(s, v, out, scale, serial);
}

static gpointer video_worker(gpointer opaque)
{
    Session *s = opaque;
    SpEngine *e = s->engine;
    AVCodecContext *decoder = NULL;
    AVFrame *frame = av_frame_alloc(), *out = av_frame_alloc();
    VideoFilter filter = {0};
    VideoScale scale = {0};
    guint serial = 0;
    double next_pts = 0;
    while (!cancelled(e)) {
        Packet *p = queue_pop(&s->videoq, e);
        if (!p) break;
        if (!p->packet && !p->eof) {
            video_filter_clear(&filter);
            avcodec_free_context(&decoder);
            decoder = decoder_open(s, p->stream);
            serial = p->serial;
            next_pts = 0;
            g_atomic_int_set(&s->video_done, 0);
            packet_free(p);
            continue;
        }
        if (decoder && p->serial == serial && generation_current(e, serial)) {
            int result = avcodec_send_packet(decoder, p->packet);
            if (result >= 0)
                while (!cancelled(e) && avcodec_receive_frame(decoder, frame) >= 0) {
                    gboolean current = video_process(s, frame, &filter, out, &scale, serial, &next_pts);
                    av_frame_unref(frame);
                    if (!current) break;
                }
            else if (result != AVERROR_EOF) av_error(e, FALSE, "Damaged video packet", result);
        }
        if (p->eof && generation_current(e, serial)) {
            /* Flush the look-ahead frame and both final fields before reporting EOF. */
            if (filter.graph) {
                int result = av_buffersrc_add_frame_flags(filter.source, NULL, 0);
                if (result < 0) av_error(e, TRUE, "Cannot finish deinterlacing", result);
                else video_filter_drain(s, &filter, out, &scale, serial);
            }
            if (generation_current(e, serial)) g_atomic_int_set(&s->video_done, 1);
        }
        packet_free(p);
    }
    video_filter_clear(&filter);
    sws_freeContext(scale.context);
    avcodec_free_context(&decoder);
    av_frame_free(&frame);
    av_frame_free(&out);
    return NULL;
}

static const char default_ass_header[] =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n"
    "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Sans,52,&H00FFFFFF,&H00FFFFFF,&H00141414,&H80000000,0,0,0,0,100,100,0,0,1,2.5,1,2,65,65,50,1\n"
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

static void subtitles_reset(Session *s, int stream)
{
    SpEngine *e = s->engine;
    avcodec_free_context(&s->sub_decoder);
    s->subtitle = stream;
    s->sub_decoder = decoder_open(s, stream);
    g_mutex_lock(&e->mutex);
    if (e->subtitles) ass_free_track(e->subtitles);
    e->subtitles = ass_new_track(e->ass);
    g_queue_clear_full(&e->bitmaps, (GDestroyNotify)bitmap_free);
    if (s->sub_decoder && s->sub_decoder->subtitle_header_size)
        ass_process_codec_private(e->subtitles, (char *)s->sub_decoder->subtitle_header,
                                   s->sub_decoder->subtitle_header_size);
    else ass_process_codec_private(e->subtitles, (char *)default_ass_header, strlen(default_ass_header));
    g_mutex_unlock(&e->mutex);
}

static void subtitle_packet(Session *s, AVCodecContext *decoder, AVPacket *packet,
                             double start_offset)
{
    if (!decoder) return;
    AVSubtitle sub = {0};
    int got = 0;
    if (avcodec_decode_subtitle2(decoder, &sub, &got, packet) < 0 || !got) return;
    double base = sub.pts != AV_NOPTS_VALUE ? sub.pts / (double)AV_TIME_BASE :
                  packet->pts != AV_NOPTS_VALUE ? packet->pts * av_q2d(decoder->pkt_timebase) : 0;
    double start = base - start_offset + sub.start_display_time / 1000.0;
    double duration = (sub.end_display_time - sub.start_display_time) / 1000.0;
    if (duration <= 0 || duration > 86400)
        duration = packet->duration > 0 ? packet->duration * av_q2d(decoder->pkt_timebase) : 5;
    SpEngine *e = s->engine;
    g_mutex_lock(&e->mutex);
    for (unsigned n = 0; n < sub.num_rects; n++) {
        AVSubtitleRect *r = sub.rects[n];
        if (r->ass) ass_process_chunk(e->subtitles, r->ass, strlen(r->ass),
                                       llround(start * 1000), llround(duration * 1000));
        if (r->type == SUBTITLE_BITMAP && r->w > 0 && r->h > 0 && r->w <= 8192 && r->h <= 8192) {
            BitmapSubtitle *b = g_new0(BitmapSubtitle, 1);
            b->x = r->x; b->y = r->y; b->width = r->w; b->height = r->h;
            b->canvas_width = decoder->width > 0 ? decoder->width : MAX(1, e->info.width);
            b->canvas_height = decoder->height > 0 ? decoder->height : MAX(1, e->info.height);
            b->start = start; b->end = start + duration;
            b->pixels = g_malloc((gsize)r->w * r->h * 4);
            const guint32 *palette = (const guint32 *)r->data[1];
            guint32 *dest = (guint32 *)b->pixels;
            for (int y = 0; y < r->h; y++) for (int x = 0; x < r->w; x++) {
                guint32 color = palette[r->data[0][y * r->linesize[0] + x]];
                guint a = color >> 24;
                dest[y * r->w + x] = (a << 24) | (((color >> 16 & 255) * a / 255) << 16) |
                                     (((color >> 8 & 255) * a / 255) << 8) | ((color & 255) * a / 255);
            }
            while (e->bitmaps.length >= 64) bitmap_free(g_queue_pop_head(&e->bitmaps));
            g_queue_push_tail(&e->bitmaps, b);
        }
    }
    g_mutex_unlock(&e->mutex);
    avsubtitle_free(&sub);
}

static void subtitle_file_load(Session *s, const char *filename)
{
    SpEngine *e = s->engine;
    AVFormatContext *f = avformat_alloc_context();
    f->interrupt_callback = (AVIOInterruptCB){io_interrupt, e};
    e->io_started = g_get_monotonic_time();
    int result = avformat_open_input(&f, filename, NULL, NULL);
    if (result >= 0) result = avformat_find_stream_info(f, NULL);
    if (result < 0) { av_error(e, FALSE, "Cannot open subtitles", result); avformat_close_input(&f); return; }
    int stream = av_find_best_stream(f, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (stream < 0) { error_set(e, FALSE, "This file contains no subtitle track."); avformat_close_input(&f); return; }
    const AVCodec *codec = avcodec_find_decoder(f->streams[stream]->codecpar->codec_id);
    AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!decoder) { avformat_close_input(&f); return; }
    decoder->pkt_timebase = f->streams[stream]->time_base;
    avcodec_parameters_to_context(decoder, f->streams[stream]->codecpar);
    if (avcodec_open2(decoder, codec, NULL) < 0) { avcodec_free_context(&decoder); avformat_close_input(&f); return; }
    subtitles_reset(s, -1);
    g_mutex_lock(&e->mutex);
    if (decoder->subtitle_header_size)
        ass_process_codec_private(e->subtitles, (char *)decoder->subtitle_header, decoder->subtitle_header_size);
    e->status.subtitle_track = -3;
    g_mutex_unlock(&e->mutex);
    AVPacket *packet = av_packet_alloc();
    gsize bytes = 0;
    while (!cancelled(e) && bytes < 16 * 1024 * 1024 && av_read_frame(f, packet) >= 0) {
        bytes += packet->size;
        if (packet->stream_index == stream) subtitle_packet(s, decoder, packet, 0);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&f);
    e->io_started = 0;
    g_mutex_lock(&e->mutex);
    e->status.revision++;
    g_mutex_unlock(&e->mutex);
}

static gboolean handle_commands(Session *s)
{
    SpEngine *e = s->engine;
    g_mutex_lock(&e->mutex);
    gboolean seek = e->seek_pending;
    double target = e->seek_target;
    int audio = e->requested_audio, subtitle = e->requested_subtitle;
    gboolean external = e->subtitle_file_pending;
    gboolean video_filter_change = e->video_filter_pending && e->info.has_video && e->info.seekable;
    char *external_file = external ? g_strdup(e->subtitle_file) : NULL;
    e->seek_pending = FALSE;
    e->subtitle_file_pending = FALSE;
    e->video_filter_pending = FALSE;
    e->requested_audio = e->requested_subtitle = -2;
    gboolean rate_change = fabs(e->rate - e->requested_rate) > 0.0001;
    if ((audio != -2 || rate_change || subtitle != -2 || video_filter_change) && !seek) target = position_locked(e);
    g_atomic_int_set(&e->commands, 0);
    g_mutex_unlock(&e->mutex);
    gboolean reset = seek || audio != -2 || rate_change || subtitle != -2 || video_filter_change;
    if (reset) {
        if (audio >= 10000 && s->synth) target = 0;
        e->io_started = g_get_monotonic_time();
        int64_t timestamp = llround((target + s->start_time) * AV_TIME_BASE);
        int result = avformat_seek_file(s->format, -1, INT64_MIN, timestamp, timestamp, 0);
        e->io_started = 0;
        if (result < 0) {
            av_error(e, FALSE, "This source cannot seek", result);
            g_free(external_file);
            return FALSE;
        }
        if (audio >= 10000 && s->synth) { sp_synth_song(s->synth, audio - 10000); target = 0; }
        else if (audio != -2) s->audio = audio;
        g_mutex_lock(&e->mutex);
        guint serial = ++e->serial;
        e->rate = e->requested_rate;
        e->drop_before = target;
        e->anchor_position = target;
        e->anchor_time = now_seconds();
        e->audio_clock = FALSE;
        e->status.audio_track = s->synth && sp_synth_songs(s->synth) > 1 ? 10000 + sp_synth_song(s->synth, 0) : s->audio;
        e->status.seek_revision++;
        e->status.state = e->paused ? SP_PAUSED : SP_PLAYING;
        frames_clear(e);
        if (e->audio_device) SDL_ClearQueuedAudio(e->audio_device);
        int selected_subtitle = subtitle != -2 ? subtitle : e->status.subtitle_track;
        e->status.subtitle_track = selected_subtitle;
        g_cond_broadcast(&e->changed);
        g_mutex_unlock(&e->mutex);
        queue_reset(&s->audioq, serial, s->audio);
        queue_reset(&s->videoq, serial, s->video);
        g_atomic_int_set(&s->audio_done, s->audio < 0);
        g_atomic_int_set(&s->video_done, s->video < 0);
        if (selected_subtitle != -3) subtitles_reset(s, selected_subtitle);
        s->eof = FALSE;
    }
    if (external_file) { subtitle_file_load(s, external_file); g_free(external_file); }
    return reset;
}

static void ass_log(int level, const char *format, va_list args, void *data)
{
    (void)data;
    if (level <= 1) { fputs("SimplePlay subtitles: ", stderr); vfprintf(stderr, format, args); fputc('\n', stderr); }
}

static int synth_cancel(void *opaque) { return cancelled(opaque); }

static void refresh_metadata(Session *s)
{
    AVFormatContext *f = s->format;
    if (!(f->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED)) return;
    f->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
    SpEngine *e = s->engine;
    g_mutex_lock(&e->mutex);
    const char *title = tag(f->metadata, "StreamTitle");
    if (!*title) title = tag(f->metadata, "icy-title");
    if (*title) text_copy(e->info.stream_title, title, sizeof(e->info.stream_title));
    if (*tag(f->metadata, "title")) text_copy(e->info.title, tag(f->metadata, "title"), sizeof(e->info.title));
    if (*tag(f->metadata, "artist")) text_copy(e->info.artist, tag(f->metadata, "artist"), sizeof(e->info.artist));
    e->status.revision++;
    g_mutex_unlock(&e->mutex);
}

static gpointer demux_worker(gpointer opaque)
{
    SpEngine *e = opaque;
    Session s = {0};
    s.engine = e;
    s.audio = s.video = s.subtitle = -1;
    queue_init(&s.audioq);
    queue_init(&s.videoq);
    s.format = avformat_alloc_context();
    s.format->interrupt_callback = (AVIOInterruptCB){io_interrupt, e};
    AVDictionary *options = NULL;
    av_dict_set_int(&options, "rw_timeout", (gint64)g_atomic_int_get(&e->timeout_seconds) * G_USEC_PER_SEC, 0);
    av_dict_set(&options, "icy", "1", 0);
    const AVInputFormat *forced = *e->input_format ? av_find_input_format(e->input_format) : NULL;
    if (*e->input_format && !forced) { error_set(e, TRUE, "Unknown input format: %s", e->input_format); av_dict_free(&options); goto done; }
    if (forced) {
        av_dict_set_int(&options, "sample_rate", e->raw_sample_rate, 0);
        AVChannelLayout layout;
        av_channel_layout_default(&layout, e->raw_channels);
        char description[128]; av_channel_layout_describe(&layout, description, sizeof(description));
        av_dict_set(&options, "ch_layout", description, 0); av_channel_layout_uninit(&layout);
    }
    if (!forced && sp_synth_detect(e->filename)) {
        char *detail = NULL;
        s.synth = sp_synth_open(e->filename, synth_cancel, e, &detail);
        if (!s.synth) { error_set(e, TRUE, "%s", detail ? detail : "Cannot decode synthesized music"); g_free(detail); av_dict_free(&options); goto done; }
        guint8 *buffer = av_malloc(32768);
        s.synth_io = avio_alloc_context(buffer, 32768, 0, s.synth, sp_synth_read, NULL, sp_synth_seek);
        if (!s.synth_io) { av_free(buffer); av_dict_free(&options); error_set(e, TRUE, "Cannot allocate synthesized audio stream"); goto done; }
        s.format->pb = s.synth_io;
        s.format->flags |= AVFMT_FLAG_CUSTOM_IO;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
        forced = av_find_input_format("f32le");
#else
        forced = av_find_input_format("f32be");
#endif
        av_dict_set(&options, "sample_rate", "48000", 0);
        av_dict_set(&options, "ch_layout", "stereo", 0);
    }
    e->io_started = g_get_monotonic_time();
    int result = avformat_open_input(&s.format, s.synth ? NULL : e->filename, forced, &options);
    av_dict_free(&options);
    if (result >= 0) result = avformat_find_stream_info(s.format, NULL);
    e->io_started = 0;
    if (result < 0) { if (!cancelled(e)) av_error(e, TRUE, "Cannot open media", result); goto done; }
    if (s.synth) {
        s.format->duration = sp_synth_duration(s.synth) > 0 ? llround(sp_synth_duration(s.synth) * AV_TIME_BASE) : AV_NOPTS_VALUE;
        av_dict_set(&s.format->metadata, "title", sp_synth_title(s.synth), 0);
        av_dict_set(&s.format->metadata, "artist", sp_synth_artist(s.synth), 0);
    }
    s.start_time = s.format->start_time == AV_NOPTS_VALUE ? 0 : s.format->start_time / (double)AV_TIME_BASE;
    s.audio = av_find_best_stream(s.format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (s.audio < 0) s.audio = -1;
    s.video = av_find_best_stream(s.format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (s.video >= 0 && s.format->streams[s.video]->disposition & AV_DISPOSITION_ATTACHED_PIC) s.video = -1;
    if (s.video < 0) s.video = -1;
    if (!e->video_enabled) s.video = -1;
    if (s.audio < 0 && s.video < 0) { error_set(e, TRUE, "No playable audio or video track was found."); goto done; }
    /* Only marked default/forced subtitles start enabled. Other tracks remain selectable. */
    g_mutex_lock(&e->mutex);
    for (unsigned n = 0; n < s.format->nb_streams; n++) {
        AVStream *st = s.format->streams[n];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            st->disposition & (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED) && s.subtitle < 0)
            s.subtitle = n;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT && st->codecpar->extradata_size) {
            const char *mime = tag(st->metadata, "mimetype");
            if (strstr(mime, "font") || strstr(mime, "opentype"))
                ass_add_font(e->ass, (char *)tag(st->metadata, "filename"),
                              (char *)st->codecpar->extradata, st->codecpar->extradata_size);
        }
    }
    ass_set_fonts(e->renderer, NULL, "Sans", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    g_mutex_unlock(&e->mutex);
    read_info(&s);
    g_mutex_lock(&e->mutex);
    e->status.audio_track = s.synth && sp_synth_songs(s.synth) > 1 ? 10000 + sp_synth_song(s.synth, 0) : s.audio;
    e->status.subtitle_track = s.subtitle;
    e->status.state = e->paused ? SP_PAUSED : SP_PLAYING;
    e->anchor_time = now_seconds();
    guint serial = e->serial;
    g_mutex_unlock(&e->mutex);
    subtitles_reset(&s, s.subtitle);
    queue_reset(&s.audioq, serial, s.audio);
    queue_reset(&s.videoq, serial, s.video);
    g_atomic_int_set(&s.audio_done, s.audio < 0);
    g_atomic_int_set(&s.video_done, s.video < 0);
    if (s.audio >= 0) s.audio_thread = g_thread_new("simpleplay-audio", audio_worker, &s);
    if (s.video >= 0) s.video_thread = g_thread_new("simpleplay-video", video_worker, &s);
    AVPacket *packet = av_packet_alloc();
    while (!cancelled(e)) {
        if (g_atomic_int_get(&e->commands)) handle_commands(&s);
        g_mutex_lock(&e->mutex);
        serial = e->serial;
        gboolean failed = e->status.state == SP_ERROR;
        g_mutex_unlock(&e->mutex);
        if (failed) break;
        if (s.eof) {
            g_mutex_lock(&e->mutex);
            gboolean drained = g_atomic_int_get(&s.audio_done) && g_atomic_int_get(&s.video_done) &&
                g_queue_is_empty(&e->frames) && (!e->audio_device || (audio_queued(e) == 0 &&
                    now_seconds() >= e->audio_queue_end_time + e->audio_period));
            if (drained && !e->paused) {
                e->anchor_position = e->info.duration > 0 ? e->info.duration : position_locked(e);
                e->status.state = SP_ENDED;
            }
            g_cond_wait_until(&e->changed, &e->mutex, g_get_monotonic_time() + 10000);
            g_mutex_unlock(&e->mutex);
            continue;
        }
        e->io_started = g_get_monotonic_time();
        result = av_read_frame(s.format, packet);
        e->io_started = 0;
        if (result == AVERROR_EOF) {
            queue_push(&s.audioq, e, NULL, serial, s.audio, TRUE);
            queue_push(&s.videoq, e, NULL, serial, s.video, TRUE);
            s.eof = TRUE;
            continue;
        }
        if (result == AVERROR(EAGAIN)) { g_usleep(5000); continue; }
        if (result < 0) { if (!cancelled(e)) av_error(e, TRUE, "Media read failed", result); break; }
        if (packet->stream_index == s.audio && s.audio_thread)
            queue_push(&s.audioq, e, packet, serial, s.audio, FALSE);
        else if (packet->stream_index == s.video && s.video_thread)
            queue_push(&s.videoq, e, packet, serial, s.video, FALSE);
        else if (packet->stream_index == s.subtitle)
            subtitle_packet(&s, s.sub_decoder, packet, s.start_time);
        av_packet_unref(packet);
        refresh_metadata(&s);
    }
    av_packet_free(&packet);
done:
    /* Wake both decoders before joining, including when only one has failed. */
    g_atomic_int_set(&e->cancel, 1);
    g_mutex_lock(&e->mutex);
    g_cond_broadcast(&e->changed);
    g_mutex_unlock(&e->mutex);
    if (s.audio_thread) g_thread_join(s.audio_thread);
    if (s.video_thread) g_thread_join(s.video_thread);
    avcodec_free_context(&s.sub_decoder);
    avformat_close_input(&s.format);
    if (s.synth_io) { av_freep(&s.synth_io->buffer); avio_context_free(&s.synth_io); }
    sp_synth_free(s.synth);
    queue_destroy(&s.audioq);
    queue_destroy(&s.videoq);
    return NULL;
}

SpEngine *sp_engine_new(void)
{
    SpEngine *e = g_new0(SpEngine, 1);
    g_mutex_init(&e->mutex);
    g_cond_init(&e->changed);
    e->volume = 0.8;
    e->audio_hz = 48000;
    e->video_enabled = TRUE;
    e->deinterlace = SP_DEINTERLACE_AUTO;
    e->deinterlace_double_rate = TRUE;
    e->timeout_seconds = 15;
    e->raw_sample_rate = 48000; e->raw_channels = 2;
    e->rate = e->requested_rate = 1;
    e->requested_audio = e->requested_subtitle = -2;
    e->status.audio_track = e->status.subtitle_track = -1;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    avformat_network_init();
    e->ass = ass_library_init();
    ass_set_message_cb(e->ass, ass_log, NULL);
    e->renderer = ass_renderer_init(e->ass);
    ass_set_fonts(e->renderer, NULL, "Sans", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    return e;
}

void sp_engine_stop(SpEngine *e)
{
    g_atomic_int_set(&e->cancel, 1);
    g_mutex_lock(&e->mutex);
    g_cond_broadcast(&e->changed);
    g_mutex_unlock(&e->mutex);
    if (e->thread) { g_thread_join(e->thread); e->thread = NULL; }
    g_mutex_lock(&e->mutex);
    e->status.state = SP_IDLE;
    e->anchor_position = 0;
    e->audio_clock = FALSE;
    frames_clear(e);
    g_queue_clear_full(&e->bitmaps, (GDestroyNotify)bitmap_free);
    if (e->subtitles) { ass_free_track(e->subtitles); e->subtitles = NULL; }
    ass_clear_fonts(e->ass);
    g_mutex_unlock(&e->mutex);
}

void sp_engine_free(SpEngine *e)
{
    if (!e) return;
    sp_engine_stop(e);
    g_free(e->filename);
    g_free(e->subtitle_file);
    if (e->subtitles) ass_free_track(e->subtitles);
    ass_renderer_done(e->renderer);
    ass_library_done(e->ass);
    g_mutex_clear(&e->mutex);
    g_cond_clear(&e->changed);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    g_free(e);
}

void sp_engine_open(SpEngine *e, const char *filename, double position, gboolean paused)
{
    sp_engine_stop(e);
    g_mutex_lock(&e->mutex);
    guint revision = e->status.revision + 1;
    memset(&e->status, 0, sizeof(e->status));
    memset(&e->info, 0, sizeof(e->info));
    e->status.revision = revision;
    e->status.state = SP_LOADING;
    e->status.audio_track = e->status.subtitle_track = -1;
    e->paused = paused;
    e->anchor_position = e->drop_before = MAX(0, position);
    e->seek_target = position;
    e->seek_pending = position > 0;
    e->requested_audio = e->requested_subtitle = -2;
    e->subtitle_delay = 0;
    e->subtitle_file_pending = FALSE;
    e->video_filter_pending = FALSE;
    g_clear_pointer(&e->subtitle_file, g_free);
    g_free(e->filename);
    e->filename = g_strdup(filename);
    e->serial++;
    g_atomic_int_set(&e->cancel, 0);
    g_atomic_int_set(&e->commands, e->seek_pending);
    g_mutex_unlock(&e->mutex);
    e->thread = g_thread_new("simpleplay-demux", demux_worker, e);
}

void sp_engine_pause(SpEngine *e, gboolean paused)
{
    g_mutex_lock(&e->mutex);
    e->anchor_position = position_locked(e);
    e->anchor_time = now_seconds();
    e->paused = paused;
    if (e->status.state == SP_PLAYING || e->status.state == SP_PAUSED)
        e->status.state = paused ? SP_PAUSED : SP_PLAYING;
    if (e->audio_device) {
        if (!paused) e->audio_queue_end_time = now_seconds() + audio_queued(e) -
            MAX(0, (e->anchor_position - e->audio_end) / e->rate);
        SDL_PauseAudioDevice(e->audio_device, paused);
    }
    g_cond_broadcast(&e->changed);
    g_mutex_unlock(&e->mutex);
}

void sp_engine_seek(SpEngine *e, double position)
{
    if (!isfinite(position)) return;
    g_mutex_lock(&e->mutex);
    if (e->info.seekable) {
        e->seek_target = CLAMP(position, 0.0, e->info.duration > 0 ? MAX(0, e->info.duration - 0.05) : G_MAXDOUBLE);
        e->seek_pending = TRUE;
        g_atomic_int_set(&e->commands, 1);
        g_cond_broadcast(&e->changed);
    }
    g_mutex_unlock(&e->mutex);
}

void sp_engine_volume(SpEngine *e, double volume)
{
    if (!isfinite(volume)) return;
    g_mutex_lock(&e->mutex);
    e->volume = CLAMP(volume, 0, 1.3);
    g_mutex_unlock(&e->mutex);
}

void sp_engine_rate(SpEngine *e, double rate)
{
    if (!isfinite(rate)) return;
    g_mutex_lock(&e->mutex);
    if (e->info.seekable) {
        e->requested_rate = CLAMP(rate, 0.5, 2);
        g_atomic_int_set(&e->commands, 1);
    }
    g_mutex_unlock(&e->mutex);
}

void sp_engine_deinterlace(SpEngine *e, SpDeinterlace state, gboolean double_rate)
{
    if (state < SP_DEINTERLACE_OFF || state > SP_DEINTERLACE_ON) return;
    double_rate = !!double_rate;
    g_mutex_lock(&e->mutex);
    gboolean changed = e->deinterlace != state ||
        (state != SP_DEINTERLACE_OFF && e->deinterlace_double_rate != double_rate);
    e->deinterlace = state;
    e->deinterlace_double_rate = double_rate;
    if (changed && e->info.has_video && e->info.seekable) {
        /* Seekable files refresh the paused picture and discard old filter history.
         * Live sources switch at the next decoded frame without attempting a seek. */
        e->video_filter_pending = TRUE;
        g_atomic_int_set(&e->commands, 1);
        g_cond_broadcast(&e->changed);
    }
    g_mutex_unlock(&e->mutex);
}

static gboolean track_exists(SpEngine *e, int stream, SpTrackType type)
{
    for (int n = 0; n < e->info.n_tracks; n++)
        if (e->info.tracks[n].index == stream && e->info.tracks[n].type == type) return TRUE;
    return FALSE;
}

void sp_engine_audio(SpEngine *e, int stream)
{
    g_mutex_lock(&e->mutex);
    if (e->info.seekable && track_exists(e, stream, SP_AUDIO)) {
        e->requested_audio = stream;
        g_atomic_int_set(&e->commands, 1);
    }
    g_mutex_unlock(&e->mutex);
}

void sp_engine_subtitle(SpEngine *e, int stream)
{
    g_mutex_lock(&e->mutex);
    if (e->info.seekable && (stream == -1 || track_exists(e, stream, SP_SUBTITLE))) {
        e->requested_subtitle = stream;
        g_atomic_int_set(&e->commands, 1);
    }
    g_mutex_unlock(&e->mutex);
}

void sp_engine_subtitle_file(SpEngine *e, const char *filename)
{
    g_mutex_lock(&e->mutex);
    g_free(e->subtitle_file);
    e->subtitle_file = g_strdup(filename);
    e->subtitle_file_pending = TRUE;
    g_atomic_int_set(&e->commands, 1);
    g_mutex_unlock(&e->mutex);
}

void sp_engine_subtitle_delay(SpEngine *e, double seconds)
{
    if (!isfinite(seconds)) return;
    g_mutex_lock(&e->mutex);
    e->subtitle_delay = CLAMP(seconds, -60, 60);
    g_mutex_unlock(&e->mutex);
}

void sp_engine_status(SpEngine *e, SpStatus *status)
{
    g_mutex_lock(&e->mutex);
    *status = e->status;
    status->position = position_locked(e);
    status->duration = e->info.duration;
    status->volume = e->volume;
    status->rate = e->rate;
    status->buffering = e->status.state == SP_LOADING || (e->status.state == SP_PLAYING && e->audio_clock &&
        e->audio_device && SDL_GetQueuedAudioSize(e->audio_device) == 0 && now_seconds() - e->audio_queue_end_time > 0.25);
    g_mutex_unlock(&e->mutex);
}

void sp_engine_info(SpEngine *e, SpMediaInfo *info)
{
    g_mutex_lock(&e->mutex);
    *info = e->info;
    g_mutex_unlock(&e->mutex);
}

void sp_engine_video_enabled(SpEngine *e, gboolean enabled)
{
    /* Set before opening; changing this never touches a running decoder. */
    if (!e->thread) e->video_enabled = enabled;
}

void sp_engine_timeout(SpEngine *e, int seconds)
{
    g_atomic_int_set(&e->timeout_seconds, CLAMP(seconds, 1, 3600));
}

void sp_engine_input_format(SpEngine *e, const char *format, int sample_rate, int channels)
{
    /* Input options apply to the next open, never a live demuxer. */
    sp_engine_stop(e);
    g_strlcpy(e->input_format, format ? format : "", sizeof(e->input_format));
    e->raw_sample_rate = CLAMP(sample_rate, 1000, 768000);
    e->raw_channels = CLAMP(channels, 1, 32);
}

SpFrame *sp_engine_frame_at(SpEngine *e, double presentation_delay)
{
    g_mutex_lock(&e->mutex);
    double position = position_locked(e);
    if (e->status.state == SP_PLAYING && !e->paused && isfinite(presentation_delay))
        position += CLAMP(presentation_delay, 0, 0.05) * e->rate;
    SpFrame *next;
    while ((next = g_queue_peek_head(&e->frames)) && (!e->current || next->pts <= position)) {
        sp_frame_unref(e->current);
        e->current = g_queue_pop_head(&e->frames);
    }
    SpFrame *result = sp_frame_ref(e->current);
    g_cond_broadcast(&e->changed);
    g_mutex_unlock(&e->mutex);
    return result;
}

SpFrame *sp_engine_frame(SpEngine *e) { return sp_engine_frame_at(e, 0); }

void sp_engine_draw_subtitles(SpEngine *e, cairo_t *cr, double x, double y, double width, double height)
{
    if (width < 1 || height < 1) return;
    g_mutex_lock(&e->mutex);
    if (e->status.subtitle_track == -1) { g_mutex_unlock(&e->mutex); return; }
    double position = position_locked(e) - e->subtitle_delay;
    cairo_save(cr);
    cairo_rectangle(cr, x, y, width, height);
    cairo_clip(cr);
    cairo_translate(cr, x, y);
    if (e->subtitles) {
        ass_set_frame_size(e->renderer, (int)width, (int)height);
        int changed = 0;
        ASS_Image *image = ass_render_frame(e->renderer, e->subtitles, llround(position * 1000), &changed);
        for (; image; image = image->next) {
            if (!image->w || !image->h) continue;
            /* A8 surfaces require a four-byte-aligned stride; libass does not. */
            int stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, image->w);
            guint8 *mask = g_malloc0((gsize)stride * image->h);
            for (int row = 0; row < image->h; row++)
                memcpy(mask + row * stride, image->bitmap + row * image->stride, image->w);
            cairo_surface_t *surface = cairo_image_surface_create_for_data(mask, CAIRO_FORMAT_A8, image->w, image->h, stride);
            guint32 color = image->color;
            cairo_set_source_rgba(cr, (color >> 24 & 255) / 255.0, (color >> 16 & 255) / 255.0,
                                   (color >> 8 & 255) / 255.0, (255 - (color & 255)) / 255.0);
            cairo_mask_surface(cr, surface, image->dst_x, image->dst_y);
            cairo_surface_destroy(surface);
            g_free(mask);
        }
    }
    for (GList *node = e->bitmaps.head; node; node = node->next) {
        BitmapSubtitle *b = node->data;
        if (position < b->start || position >= b->end) continue;
        cairo_save(cr);
        cairo_scale(cr, width / b->canvas_width, height / b->canvas_height);
        cairo_surface_t *surface = cairo_image_surface_create_for_data(b->pixels, CAIRO_FORMAT_ARGB32,
                                                                        b->width, b->height, b->width * 4);
        cairo_set_source_surface(cr, surface, b->x, b->y);
        cairo_paint(cr);
        cairo_surface_destroy(surface);
        cairo_restore(cr);
    }
    cairo_restore(cr);
    g_mutex_unlock(&e->mutex);
}
