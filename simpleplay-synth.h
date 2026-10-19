#ifndef SIMPLEPLAY_SYNTH_H
#define SIMPLEPLAY_SYNTH_H
#include <glib.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SpSynth SpSynth;
int sp_synth_detect(const char *filename);
SpSynth *sp_synth_open(const char *filename, int (*cancel)(void *), void *opaque, char **error);
void sp_synth_free(SpSynth *synth);
int sp_synth_read(void *synth, uint8_t *buffer, int bytes);
int64_t sp_synth_seek(void *synth, int64_t offset, int whence);
double sp_synth_duration(SpSynth *synth);
const char *sp_synth_codec(SpSynth *synth);
const char *sp_synth_title(SpSynth *synth);
const char *sp_synth_artist(SpSynth *synth);
int sp_synth_songs(SpSynth *synth);
int sp_synth_song(SpSynth *synth, int song);
#ifdef __cplusplus
}
#endif
#endif
