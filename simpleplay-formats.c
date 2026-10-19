#include "simpleplay-formats.h"
#include "simpleplay-synth.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <string.h>

static void format_free(SpFormat *format)
{
    g_free(format->kind); g_free(format->name);
    g_free(format->description); g_free(format->extensions); g_free(format);
}

static void format_add(GPtrArray *list, const char *kind, const char *name, const char *description, const char *extensions)
{
    SpFormat *f = g_new0(SpFormat, 1);
    f->kind = g_strdup(kind); f->name = g_strdup(name);
    f->description = g_strdup(description ? description : name);
    f->extensions = g_strdup(extensions ? extensions : "");
    g_ptr_array_add(list, f);
}

GPtrArray *sp_formats_list(void)
{
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)format_free);
    void *iterator = NULL;
    const AVCodec *codec;
    while ((codec = av_codec_iterate(&iterator))) {
        if (!av_codec_is_decoder(codec)) continue;
        const char *kind = codec->type == AVMEDIA_TYPE_AUDIO ? "Audio" : codec->type == AVMEDIA_TYPE_VIDEO ? "Video" :
                           codec->type == AVMEDIA_TYPE_SUBTITLE ? "Subtitle" : "Other";
        format_add(list, kind, codec->name, codec->long_name, NULL);
    }
    iterator = NULL;
    const AVInputFormat *format;
    while ((format = av_demuxer_iterate(&iterator)))
        format_add(list, "Container", format->name, format->long_name, format->extensions);
    format_add(list, "Audio", "midi", "Standard MIDI / RMID / karaoke · FluidSynth", "mid,midi,kar,rmi");
    format_add(list, "Audio", "sid", "Commodore 64 PSID / RSID · libsidplayfp", "sid");
    return list;
}

typedef struct { GCancellable *cancel; gint64 started; } Probe;
gboolean sp_formats_is_uri(const char *filename)
{
    const char *protocol = avio_find_protocol_name(filename);
    return g_str_has_prefix(filename, "file://") || (protocol && strcmp(protocol, "file"));
}
static int probe_interrupt(void *opaque)
{
    Probe *probe = opaque;
    return (probe->cancel && g_cancellable_is_cancelled(probe->cancel)) ||
        g_get_monotonic_time() - probe->started > 1500000;
}

gboolean sp_formats_candidate(const char *filename, GCancellable *cancel)
{
    /* Extension tables are generated from the linked demuxers, never a media
     * whitelist. Unrecognized/missing extensions get a bounded content probe. */
    static gsize initialized;
    static GHashTable *extensions;
    if (g_once_init_enter(&initialized)) {
        extensions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        void *iterator = NULL;
        const AVInputFormat *f;
        while ((f = av_demuxer_iterate(&iterator))) {
            if (!f->extensions) continue;
            g_auto(GStrv) names = g_strsplit(f->extensions, ",", -1);
            for (int n = 0; names[n]; n++) g_hash_table_add(extensions, g_ascii_strdown(names[n], -1));
        }
        const char *extra[] = {"mid", "midi", "rmi", "kar", "sid", "mod", "xm", "it", "s3m", "mptm", "umx", "mo3",
            "nsf", "nsfe", "spc", "vgm", "vgz", "gbs", "gym", "ay", "hes", "kss", "sap", NULL};
        for (int n = 0; extra[n]; n++) g_hash_table_add(extensions, g_strdup(extra[n]));
        g_once_init_leave(&initialized, 1);
    }
    const char *dot = strrchr(filename, '.');
    g_autofree char *extension = dot ? g_ascii_strdown(dot + 1, -1) : NULL;
    const char *subtitle_extensions[] = {"srt", "ass", "ssa", "vtt", "sub", "idx", "smi", "sami", "txt", "xml", "lrc", "mpl", "stl", "ttml", "jss", NULL};
    gboolean subtitle = FALSE;
    for (int n = 0; extension && subtitle_extensions[n]; n++)
        if (!strcmp(extension, subtitle_extensions[n])) subtitle = TRUE;
    if (extension && !subtitle && g_hash_table_contains(extensions, extension)) return TRUE;
    if (sp_synth_detect(filename)) return TRUE;
    Probe probe = {cancel, g_get_monotonic_time()};
    AVFormatContext *f = avformat_alloc_context();
    f->interrupt_callback = (AVIOInterruptCB){probe_interrupt, &probe};
    f->probesize = 128 * 1024;
    f->format_probesize = 128 * 1024;
    f->max_analyze_duration = AV_TIME_BASE / 10;
    gboolean playable = FALSE;
    if (avformat_open_input(&f, filename, NULL, NULL) >= 0) {
        avformat_find_stream_info(f, NULL);
        for (unsigned n = 0; n < f->nb_streams; n++) {
            AVCodecParameters *p = f->streams[n]->codecpar;
            if ((p->codec_type == AVMEDIA_TYPE_AUDIO || p->codec_type == AVMEDIA_TYPE_VIDEO) && avcodec_find_decoder(p->codec_id))
                playable = TRUE;
        }
    }
    avformat_close_input(&f);
    return playable;
}

char *sp_formats_report(gboolean full, gboolean *complete)
{
    const enum AVCodecID baseline[] = {
        AV_CODEC_ID_MPEG1VIDEO, AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_MPEG4, AV_CODEC_ID_MSMPEG4V1,
        AV_CODEC_ID_MSMPEG4V2, AV_CODEC_ID_MSMPEG4V3, AV_CODEC_ID_H261, AV_CODEC_ID_H263, AV_CODEC_ID_H264,
        AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1, AV_CODEC_ID_VP8, AV_CODEC_ID_VP9, AV_CODEC_ID_CINEPAK, AV_CODEC_ID_THEORA,
        AV_CODEC_ID_DIRAC, AV_CODEC_ID_MJPEG, AV_CODEC_ID_MJPEGB, AV_CODEC_ID_WMV1, AV_CODEC_ID_WMV2, AV_CODEC_ID_WMV3,
        AV_CODEC_ID_VC1, AV_CODEC_ID_SVQ1, AV_CODEC_ID_SVQ3, AV_CODEC_ID_DVVIDEO, AV_CODEC_ID_VP3, AV_CODEC_ID_VP5,
        AV_CODEC_ID_VP6, AV_CODEC_ID_INDEO3, AV_CODEC_ID_RV10, AV_CODEC_ID_RV20, AV_CODEC_ID_RV30, AV_CODEC_ID_RV40,
        AV_CODEC_ID_MP1, AV_CODEC_ID_MP2, AV_CODEC_ID_MP3, AV_CODEC_ID_AAC, AV_CODEC_ID_AC3, AV_CODEC_ID_EAC3,
        AV_CODEC_ID_MLP, AV_CODEC_ID_TRUEHD, AV_CODEC_ID_DTS, AV_CODEC_ID_WMAV1, AV_CODEC_ID_WMAV2, AV_CODEC_ID_WMAPRO,
        AV_CODEC_ID_WMALOSSLESS, AV_CODEC_ID_FLAC, AV_CODEC_ID_ALAC, AV_CODEC_ID_SPEEX, AV_CODEC_ID_MUSEPACK7,
        AV_CODEC_ID_MUSEPACK8, AV_CODEC_ID_ATRAC3, AV_CODEC_ID_ATRAC3P, AV_CODEC_ID_ATRAC9, AV_CODEC_ID_WAVPACK,
        AV_CODEC_ID_TTA, AV_CODEC_ID_APE, AV_CODEC_ID_RA_144, AV_CODEC_ID_RA_288, AV_CODEC_ID_COOK,
        AV_CODEC_ID_PCM_ALAW, AV_CODEC_ID_PCM_MULAW, AV_CODEC_ID_AMR_NB, AV_CODEC_ID_AMR_WB, AV_CODEC_ID_QCELP,
        AV_CODEC_ID_QDM2, AV_CODEC_ID_QDMC, AV_CODEC_ID_MACE3, AV_CODEC_ID_MACE6, AV_CODEC_ID_SHORTEN, AV_CODEC_ID_TAK
    };
    GString *missing = g_string_new(NULL);
    for (guint n = 0; n < G_N_ELEMENTS(baseline); n++) if (!avcodec_find_decoder(baseline[n]))
        g_string_append_printf(missing, "  missing decoder: %s\n", avcodec_get_name(baseline[n]));
    const char *demuxers[] = {"libgme", "libopenmpt", "matroska", "mov", "rm", "asf", "ogg", "wav", "mpegts", NULL};
    for (int n = 0; demuxers[n]; n++) if (!av_find_input_format(demuxers[n]))
        g_string_append_printf(missing, "  missing demuxer: %s\n", demuxers[n]);
    GPtrArray *formats = sp_formats_list();
    int audio = 0, video = 0, subtitle = 0, container = 0;
    for (guint n = 0; n < formats->len; n++) {
        SpFormat *f = g_ptr_array_index(formats, n);
        if (!strcmp(f->kind, "Audio")) audio++;
        else if (!strcmp(f->kind, "Video")) video++;
        else if (!strcmp(f->kind, "Subtitle")) subtitle++;
        else if (!strcmp(f->kind, "Container")) container++;
    }
    GString *report = g_string_new(NULL);
    g_string_append_printf(report, "SimplePlay format support · FFmpeg %s\n%d registered audio decoders · %d video decoders · %d subtitle decoders · %d demuxers\n",
        av_version_info(), audio, video, subtitle, container);
    g_string_append(report, missing->len ? "VLC audio/video compatibility baseline has gaps:\n" : "VLC audio/video compatibility baseline: available\n");
    g_string_append(report, missing->str);
    g_string_append(report, "Additional native decoders: MIDI (FluidSynth + system SoundFont), SID (libsidplayfp).\n");
    if (full) for (guint n = 0; n < formats->len; n++) {
        SpFormat *f = g_ptr_array_index(formats, n);
        g_string_append_printf(report, "%-10s %-25s %s%s%s\n", f->kind, f->name, f->description, *f->extensions ? " · " : "", f->extensions);
    }
    if (complete) *complete = missing->len == 0;
    g_string_free(missing, TRUE); g_ptr_array_unref(formats);
    return g_string_free(report, FALSE);
}
