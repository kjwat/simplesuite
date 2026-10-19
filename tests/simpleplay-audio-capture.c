/* Test-only SDL interposer: capture exactly what the engine sends to output.
 * The real SDL dummy device still supplies queue pacing and pause/seek behavior.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *name, int capture,
    const SDL_AudioSpec *want, SDL_AudioSpec *have, int changes)
{
    SDL_AudioDeviceID (*real_open)(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
    *(void **)(&real_open) = dlsym(RTLD_NEXT, "SDL_OpenAudioDevice");
    SDL_AudioSpec requested = *want;
    const char *forced = getenv("SIMPLEPLAY_TEST_OUTPUT_RATE");
    if (forced && (changes & SDL_AUDIO_ALLOW_FREQUENCY_CHANGE)) requested.freq = atoi(forced);
    SDL_AudioDeviceID device = real_open(name, capture, &requested, have, changes);
    const char *path = getenv("SIMPLEPLAY_TEST_AUDIO_SPEC");
    if (device && path) {
        FILE *file = fopen(path, "a");
        if (!file) abort();
        fprintf(file, "%d %d %d\n", have->freq, have->channels, have->format);
        fclose(file);
    }
    return device;
}

int SDL_QueueAudio(SDL_AudioDeviceID device, const void *data, Uint32 size)
{
    int (*real_queue)(SDL_AudioDeviceID, const void *, Uint32);
    *(void **)(&real_queue) = dlsym(RTLD_NEXT, "SDL_QueueAudio");
    int result = real_queue(device, data, size);
    const char *path = getenv("SIMPLEPLAY_TEST_AUDIO_PCM");
    if (result == 0 && path) {
        FILE *file = fopen(path, "ab");
        if (!file || fwrite(data, 1, size, file) != size) abort();
        fclose(file);
    }
    return result;
}
