#include "simpleplay-state.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

static double setting_number(GKeyFile *file, const char *key, double fallback)
{
    GError *error = NULL;
    double value = g_key_file_get_double(file, "player", key, &error);
    if (error || !isfinite(value)) value = fallback;
    g_clear_error(&error);
    return value;
}

static gboolean setting_bool(GKeyFile *file, const char *key, gboolean fallback)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(file, "player", key, &error);
    if (error) value = fallback;
    g_clear_error(&error);
    return value;
}

void sp_settings_load(SpSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->file = g_key_file_new();
    s->path = g_build_filename(g_get_user_config_dir(), "simpleplay", "settings.ini", NULL);
    g_key_file_load_from_file(s->file, s->path, G_KEY_FILE_NONE, NULL);
    s->volume = CLAMP(setting_number(s->file, "volume", 0.8), 0, 1);
    s->width = CLAMP((int)setting_number(s->file, "width", 1120), 640, 7680);
    s->height = CLAMP((int)setting_number(s->file, "height", 740), 420, 4320);
    s->remember = setting_bool(s->file, "remember", TRUE);
    s->auto_next = setting_bool(s->file, "auto-next", TRUE);
    s->queue_visible = setting_bool(s->file, "queue", FALSE);
    s->last_directory = g_key_file_get_string(s->file, "player", "directory", NULL);
}

void sp_settings_save(SpSettings *s)
{
    g_key_file_set_double(s->file, "player", "volume", s->volume);
    g_key_file_set_integer(s->file, "player", "width", s->width);
    g_key_file_set_integer(s->file, "player", "height", s->height);
    g_key_file_set_boolean(s->file, "player", "remember", s->remember);
    g_key_file_set_boolean(s->file, "player", "auto-next", s->auto_next);
    g_key_file_set_boolean(s->file, "player", "queue", s->queue_visible);
    if (s->last_directory) g_key_file_set_string(s->file, "player", "directory", s->last_directory);
    g_autofree char *directory = g_path_get_dirname(s->path);
    if (g_mkdir_with_parents(directory, 0700) < 0) { g_warning("Cannot create %s", directory); return; }
    gsize size;
    g_autofree char *data = g_key_file_to_data(s->file, &size, NULL);
    GError *error = NULL;
    if (!g_file_set_contents_full(s->path, data, size, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error)) {
        g_warning("Cannot save SimplePlay settings: %s", error->message);
        g_error_free(error);
    }
}

void sp_settings_clear(SpSettings *s)
{
    g_key_file_unref(s->file);
    g_free(s->path);
    g_free(s->last_directory);
}

double sp_settings_resume(SpSettings *s, const char *filename)
{
    if (!s->remember) return 0;
    g_autofree char *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, filename, -1);
    double value = g_key_file_get_double(s->file, "positions", key, NULL);
    return isfinite(value) && value > 5 ? value : 0;
}

void sp_settings_position(SpSettings *s, const char *filename, double position, double duration)
{
    if (!filename || !s->remember || duration <= 0 || !isfinite(position)) return;
    g_autofree char *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, filename, -1);
    /* Keep short songs and almost-completed media starting from the beginning. */
    if (duration < 180 || position < 5 || position >= duration - 10)
        g_key_file_remove_key(s->file, "positions", key, NULL);
    else g_key_file_set_double(s->file, "positions", key, position);
    gsize length = 0;
    g_auto(GStrv) keys = g_key_file_get_keys(s->file, "positions", &length, NULL);
    for (gsize n = 0; n + 200 < length; n++) g_key_file_remove_key(s->file, "positions", keys[n], NULL);
}

char **sp_settings_recent(SpSettings *s)
{
    char **list = g_key_file_get_string_list(s->file, "player", "recent", NULL, NULL);
    return list ? list : g_new0(char *, 1);
}

void sp_settings_add_recent(SpSettings *s, const char *filename)
{
    g_auto(GStrv) old = sp_settings_recent(s);
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(list, g_strdup(filename));
    for (int n = 0; old[n] && list->len < 20; n++)
        if (strcmp(filename, old[n])) g_ptr_array_add(list, g_strdup(old[n]));
    g_key_file_set_string_list(s->file, "player", "recent", (const char *const *)list->pdata, list->len);
    g_ptr_array_free(list, TRUE);
}
