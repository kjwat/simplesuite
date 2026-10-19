#ifndef SIMPLEPLAY_CLIENT_H
#define SIMPLEPLAY_CLIENT_H
#include <stdlib.h>
#include <unistd.h>

/* Frontends share the headless engine without linking its media libraries.
 * An explicit executable override is useful for development and other hosts. */
static const char *simplesuite_audio_player(void)
{
    const char *override = getenv("SIMPLESUITE_AUDIO_PLAYER");
    if (override && *override) return override;
    if (access("/usr/local/bin/simpleplay-core", X_OK) == 0) return "/usr/local/bin/simpleplay-core";
    return "simpleplay-core";
}
#endif
