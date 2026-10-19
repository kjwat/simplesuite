#ifndef SIMPLEPLAY_STATE_H
#define SIMPLEPLAY_STATE_H
#include <glib.h>

typedef struct {
    GKeyFile *file;
    char *path, *last_directory;
    double volume;
    int width, height;
    gboolean remember, auto_next, queue_visible;
} SpSettings;

void sp_settings_load(SpSettings *settings);
void sp_settings_save(SpSettings *settings);
void sp_settings_clear(SpSettings *settings);
double sp_settings_resume(SpSettings *settings, const char *filename);
void sp_settings_position(SpSettings *settings, const char *filename, double position, double duration);
char **sp_settings_recent(SpSettings *settings);
void sp_settings_add_recent(SpSettings *settings, const char *filename);

#endif
