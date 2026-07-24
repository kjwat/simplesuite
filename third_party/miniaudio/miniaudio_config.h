#ifndef SIMPLESUITE_MINIAUDIO_CONFIG_H
#define SIMPLESUITE_MINIAUDIO_CONFIG_H

/* SimpleWords only decodes and mixes its one five-sample typing effect. */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_CUSTOM
#define MA_NO_NULL

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "miniaudio.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif
