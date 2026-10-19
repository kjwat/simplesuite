#ifndef SIMPLEPLAY_FORMATS_H
#define SIMPLEPLAY_FORMATS_H
#include <gio/gio.h>

typedef struct {
    char *kind, *name, *description, *extensions;
} SpFormat;

GPtrArray *sp_formats_list(void);
gboolean sp_formats_candidate(const char *filename, GCancellable *cancel);
gboolean sp_formats_is_uri(const char *filename);
char *sp_formats_report(gboolean full, gboolean *complete);

#endif
