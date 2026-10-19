/* The two music families not decoded by FFmpeg: MIDI synthesis and C64 SID.
 * These render PCM directly into an AVIO stream, using the same FFmpeg audio
 * decoder/filter, clock and SDL output path as every other file. No subprocess.
 */
#include "simpleplay-synth.h"
#include <fluidsynth.h>
#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/builders/residfp.h>
extern "C" {
#include <libavutil/error.h>
#include <libavformat/avio.h>
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int rate = 48000, frame_bytes = 8;
struct Tempo { uint64_t tick; uint32_t micros; double seconds; };
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t variable(const uint8_t *&p, const uint8_t *end) {
    uint32_t result = 0;
    for (int n = 0; n < 4 && p < end; n++) { uint8_t b = *p++; result = result << 7 | (b & 127); if (!(b & 128)) return result; }
    throw std::runtime_error("Malformed MIDI variable-length value");
}
}

struct SpSynth {
    int kind = 0, song = 1;
    int64_t position = 0, total = 0;
    int division = 480;
    double seconds_per_tick = 0;
    std::vector<Tempo> tempos;
    std::string title, artist;
    std::vector<uint8_t> file;
    std::array<std::vector<uint8_t>, 3> roms;
    fluid_settings_t *settings = nullptr;
    fluid_synth_t *midi = nullptr;
    fluid_player_t *player = nullptr;
    /* Destruction order matters: the player releases the builder's devices. */
    std::unique_ptr<ReSIDfpBuilder> builder;
    std::unique_ptr<SidTune> tune;
    std::unique_ptr<sidplayfp> sid;
    int (*cancel)(void *) = nullptr;
    void *opaque = nullptr;
    ~SpSynth() {
        if (player) { fluid_player_stop(player); delete_fluid_player(player); }
        if (midi) delete_fluid_synth(midi);
        if (settings) delete_fluid_settings(settings);
    }
    bool cancelled() const { return cancel && cancel(opaque); }
    double tick_seconds(uint64_t tick) const {
        if (seconds_per_tick) return tick * seconds_per_tick;
        const Tempo *tempo = &tempos.front();
        for (const auto &t : tempos) { if (t.tick > tick) break; tempo = &t; }
        return tempo->seconds + (tick - tempo->tick) * tempo->micros / (1000000.0 * division);
    }
    int seconds_tick(double seconds) const {
        if (seconds_per_tick) return int(seconds / seconds_per_tick);
        const Tempo *tempo = &tempos.front();
        for (const auto &t : tempos) { if (t.seconds > seconds) break; tempo = &t; }
        return int(tempo->tick + (seconds - tempo->seconds) * 1000000.0 * division / tempo->micros);
    }
};

int sp_synth_detect(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file) return 0;
    unsigned char header[12] = {};
    size_t size = fread(header, 1, sizeof(header), file);
    fclose(file);
    if (size >= 4 && (!memcmp(header, "PSID", 4) || !memcmp(header, "RSID", 4))) return 2;
    if (size >= 4 && !memcmp(header, "MThd", 4)) return 1;
    if (size >= 12 && !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "RMID", 4)) return 1;
    return 0;
}

static void midi_timing(SpSynth &s, const uint8_t *data, size_t size)
{
    if (size < 14 || memcmp(data, "MThd", 4)) throw std::runtime_error("Invalid MIDI header");
    uint32_t header = be32(data + 4);
    if (header < 6 || header > size - 8) throw std::runtime_error("Invalid MIDI header length");
    uint16_t division = be16(data + 12);
    if (division & 0x8000) {
        int frames = -int(int8_t(division >> 8));
        s.seconds_per_tick = 1.0 / ((frames == 29 ? 29.97 : frames) * (division & 255));
    } else s.division = division;
    if (!division || (!s.seconds_per_tick && !s.division)) throw std::runtime_error("Invalid MIDI time division");
    s.tempos.push_back({0, 500000, 0});
    const uint8_t *p = data + 8 + header, *end = data + size;
    uint64_t last_tick = 0;
    while (size_t(end - p) >= 8) {
        uint32_t length = be32(p + 4);
        if (length > size_t(end - p - 8)) throw std::runtime_error("Truncated MIDI track");
        if (memcmp(p, "MTrk", 4)) { p += 8 + length; continue; }
        p += 8;
        const uint8_t *track_end = p + length;
        uint64_t tick = 0;
        uint8_t running = 0;
        while (p < track_end) {
            tick += variable(p, track_end);
            last_tick = std::max(last_tick, tick);
            if (p >= track_end) break;
            uint8_t status = *p;
            if (status & 128) p++;
            else if (running) status = running;
            else throw std::runtime_error("MIDI running status has no preceding event");
            if (status < 0xf0) {
                running = status;
                unsigned bytes = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 1 : 2;
                if (size_t(track_end - p) < bytes) throw std::runtime_error("Truncated MIDI event");
                p += bytes;
            } else if (status == 0xff) {
                if (p >= track_end) break;
                uint8_t type = *p++;
                uint32_t bytes = variable(p, track_end);
                if (bytes > size_t(track_end - p)) throw std::runtime_error("Truncated MIDI meta event");
                if (type == 0x51 && bytes == 3) {
                    uint32_t tempo = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
                    if (tempo) s.tempos.push_back({tick, tempo, 0});
                }
                if (type == 3 && s.title.empty()) s.title.assign(reinterpret_cast<const char *>(p), bytes);
                p += bytes;
            } else if (status == 0xf0 || status == 0xf7) {
                uint32_t bytes = variable(p, track_end);
                if (bytes > size_t(track_end - p)) throw std::runtime_error("Truncated MIDI SysEx event");
                p += bytes; running = 0;
            } else throw std::runtime_error("Unsupported MIDI system event");
        }
        p = track_end;
    }
    std::stable_sort(s.tempos.begin(), s.tempos.end(), [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });
    for (size_t n = 1; n < s.tempos.size(); n++) {
        const auto &previous = s.tempos[n - 1];
        s.tempos[n].seconds = previous.seconds + (s.tempos[n].tick - previous.tick) * previous.micros / (1000000.0 * s.division);
    }
    double seconds = s.tick_seconds(last_tick) + 2;
    if (!std::isfinite(seconds) || seconds <= 0 || seconds >= INT64_MAX / double(rate))
        throw std::runtime_error("MIDI duration is outside the supported time range");
    s.total = int64_t(seconds * rate);
}

SpSynth *sp_synth_open(const char *filename, int (*cancel)(void *), void *opaque, char **error)
{
    auto s = std::make_unique<SpSynth>();
    s->kind = sp_synth_detect(filename);
    s->cancel = cancel; s->opaque = opaque;
    if (!s->kind) return nullptr;
    try {
        gchar *contents = nullptr;
        gsize size = 0;
        if (!g_file_get_contents(filename, &contents, &size, nullptr)) throw std::runtime_error("Cannot read synthesized music file");
        if (size > 64 * 1024 * 1024) { g_free(contents); throw std::runtime_error("Synthesized music file is too large"); }
        s->file.assign(contents, contents + size); g_free(contents);
        if (s->kind == 1) {
            const uint8_t *data = s->file.data(); size_t bytes = s->file.size();
            if (bytes >= 12 && !memcmp(data, "RIFF", 4)) {
                size_t offset = 12;
                while (offset + 8 <= bytes) {
                    uint32_t length = uint32_t(data[offset + 4]) | uint32_t(data[offset + 5]) << 8 |
                        uint32_t(data[offset + 6]) << 16 | uint32_t(data[offset + 7]) << 24;
                    if (length > bytes - offset - 8) throw std::runtime_error("Truncated RMID chunk");
                    if (!memcmp(data + offset, "data", 4)) { data += offset + 8; bytes = length; break; }
                    offset += 8 + length + (length & 1);
                }
            }
            midi_timing(*s, data, bytes);
            s->settings = new_fluid_settings();
            fluid_settings_setnum(s->settings, "synth.sample-rate", rate);
            fluid_settings_setnum(s->settings, "synth.gain", 0.6);
            fluid_settings_setint(s->settings, "synth.cpu-cores", 1);
            fluid_settings_setstr(s->settings, "player.timing-source", "sample");
            s->midi = new_fluid_synth(s->settings);
            if (!s->midi) throw std::runtime_error("Cannot create MIDI synthesizer");
            const char *fonts[] = {g_getenv("SIMPLEPLAY_SOUNDFONT"), "/usr/share/sounds/sf2/default-GM.sf2",
                "/usr/share/sounds/sf2/TimGM6mb.sf2", "/usr/share/soundfonts/default.sf2", "/usr/share/soundfonts/FluidR3_GM.sf2",
                "/usr/share/sounds/sf2/FluidR3_GM.sf2", "/usr/local/share/sounds/sf2/FluidR3_GM.sf2",
                "/usr/local/share/sounds/sf2/GeneralUser_GS.sf2", nullptr};
            int loaded = -1;
            for (size_t n = 0; n < sizeof(fonts) / sizeof(fonts[0]); n++) {
                if (fonts[n] && g_file_test(fonts[n], G_FILE_TEST_IS_REGULAR)) { loaded = fluid_synth_sfload(s->midi, fonts[n], 1); if (loaded >= 0) break; }
            }
            if (loaded < 0) throw std::runtime_error("MIDI needs a SoundFont. Install timgm6mb-soundfont or set SIMPLEPLAY_SOUNDFONT to an SF2/SF3 file.");
            s->player = new_fluid_player(s->midi);
            if (!s->player || fluid_player_add_mem(s->player, data, bytes) != FLUID_OK || fluid_player_play(s->player) != FLUID_OK)
                throw std::runtime_error("Cannot start MIDI playback");
        } else {
            s->builder = std::make_unique<ReSIDfpBuilder>("SimplePlay reSIDfp");
            s->builder->create(3);
            s->tune = std::make_unique<SidTune>(s->file.data(), s->file.size());
            if (!s->tune->getStatus()) throw std::runtime_error(s->tune->statusString());
            s->sid = std::make_unique<sidplayfp>();
            if (const char *directory = g_getenv("SIMPLEPLAY_C64_ROM_DIR")) {
                const char *names[] = {"kernal", "basic", "chargen"};
                const size_t sizes[] = {8192, 8192, 4096};
                for (int n = 0; n < 3; n++) {
                    gchar *data = nullptr; gsize bytes = 0;
                    std::string path = std::string(directory) + "/" + names[n];
                    if (g_file_get_contents(path.c_str(), &data, &bytes, nullptr)) {
                        if (bytes != sizes[n]) { g_free(data); throw std::runtime_error("Invalid C64 ROM size: " + path); }
                        s->roms[n].assign(data, data + bytes); g_free(data);
                    }
                }
                if (s->roms[0].empty()) throw std::runtime_error("SIMPLEPLAY_C64_ROM_DIR must contain an 8 KiB kernal ROM");
                s->sid->setRoms(s->roms[0].data(), s->roms[1].empty() ? nullptr : s->roms[1].data(),
                    s->roms[2].empty() ? nullptr : s->roms[2].data());
            }
            SidConfig config = s->sid->config();
            config.frequency = rate; config.playback = SidConfig::STEREO; config.sidEmulation = s->builder.get();
            if (!s->sid->config(config) || !s->sid->load(s->tune.get())) throw std::runtime_error(s->sid->error());
            const SidTuneInfo *info = s->tune->getInfo();
            if (info->numberOfInfoStrings() > 0) s->title = info->infoString(0);
            if (info->numberOfInfoStrings() > 1) s->artist = info->infoString(1);
            s->song = info->startSong();
        }
        return s.release();
    } catch (const std::exception &e) { if (error) *error = g_strdup(e.what()); return nullptr; }
}

void sp_synth_free(SpSynth *s) { delete s; }
double sp_synth_duration(SpSynth *s) { return s->total / double(rate); }
const char *sp_synth_codec(SpSynth *s) { return s->kind == 1 ? "midi" : "sid"; }
const char *sp_synth_title(SpSynth *s) { return s->title.c_str(); }
const char *sp_synth_artist(SpSynth *s) { return s->artist.c_str(); }
int sp_synth_songs(SpSynth *s) { return s->tune ? int(s->tune->getInfo()->songs()) : 1; }
int sp_synth_song(SpSynth *s, int song) {
    if (!s->sid) return 1;
    if (song > 0 && song <= sp_synth_songs(s)) {
        s->tune->selectSong(song);
        s->sid->load(s->tune.get()); s->position = 0; s->song = song;
    }
    return s->song;
}

int sp_synth_read(void *opaque, uint8_t *buffer, int bytes)
{
    auto &s = *static_cast<SpSynth *>(opaque);
    if (s.cancelled()) return AVERROR_EXIT;
    int frames = bytes / frame_bytes;
    if (s.total) frames = std::min<int64_t>(frames, s.total - s.position);
    if (frames <= 0) return AVERROR_EOF;
    float *samples = reinterpret_cast<float *>(buffer);
    if (s.midi) {
        if (fluid_synth_write_float(s.midi, frames, samples, 0, 2, samples, 1, 2) != FLUID_OK) return AVERROR(EIO);
    } else {
        std::vector<short> pcm(size_t(frames) * 2);
        unsigned got = s.sid->play(pcm.data(), pcm.size());
        if (!got) return AVERROR_EOF;
        frames = got / 2;
        for (int n = 0; n < frames * 2; n++) samples[n] = pcm[n] / 32768.0f;
    }
    s.position += frames;
    return frames * frame_bytes;
}

int64_t sp_synth_seek(void *opaque, int64_t offset, int whence)
{
    auto &s = *static_cast<SpSynth *>(opaque);
    if (whence == AVSEEK_SIZE) return s.total ? s.total * frame_bytes : AVERROR(ENOSYS);
    int64_t target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? s.position * frame_bytes + offset :
                     whence == SEEK_END && s.total ? s.total * frame_bytes + offset : -1;
    if (target < 0) return AVERROR(EINVAL);
    int64_t frame = target / frame_bytes;
    if (s.total && frame > s.total) frame = s.total;
    if (s.midi) {
        fluid_synth_system_reset(s.midi);
        for (int channel = 0; channel < 16; channel++) fluid_synth_all_sounds_off(s.midi, channel);
        if (fluid_player_get_status(s.player) == FLUID_PLAYER_DONE) fluid_player_play(s.player);
        fluid_player_seek(s.player, s.seconds_tick(std::min(frame / double(rate), s.total / double(rate) - 2)));
        s.position = frame;
    } else {
        if (frame < s.position) { s.sid->load(s.tune.get()); s.position = 0; }
        std::array<float, 2048> discard;
        while (s.position < frame && !s.cancelled()) {
            int count = std::min<int64_t>(sizeof(discard), (frame - s.position) * frame_bytes);
            if (sp_synth_read(&s, reinterpret_cast<uint8_t *>(discard.data()), count) <= 0) return AVERROR(EIO);
        }
        if (s.cancelled()) return AVERROR_EXIT;
    }
    return s.position * frame_bytes;
}
