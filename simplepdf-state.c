/* SimplePDF reading state. GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "simplepdf-state.h"
#include <glib/gstdio.h>
#include <math.h>
#include <sys/stat.h>

static char *state_path(const char *uri)
{
    const char *base = g_getenv("XDG_STATE_HOME");
    g_autofree char *fallback = NULL;
    if (!base || !g_path_is_absolute(base)) {
        fallback = g_build_filename(g_get_home_dir(), ".local", "state", NULL);
        base = fallback;
    }
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
    g_autofree char *name = g_strconcat(hash, ".ini", NULL);
    return g_build_filename(base, "simplepdf", name, NULL);
}

static gboolean valid(const PdfPosition *p)
{
    return p->page >= 0 && p->rotation >= 0 && p->rotation < 360 && p->rotation % 90 == 0 &&
           p->sizing >= 0 && p->sizing <= 3 && isfinite(p->scale) && p->scale > 0 && p->scale <= 20 &&
           isfinite(p->x) && isfinite(p->y) && p->x >= 0 && p->x <= 1 && p->y >= 0 && p->y <= 1 &&
           p->width >= 320 && p->width <= 16384 && p->height >= 240 && p->height <= 16384;
}

gboolean pdf_state_load(const char *uri, PdfPosition *position)
{
    g_autofree char *path = state_path(uri);
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL) ||
        g_key_file_get_integer(file, "view", "version", NULL) != 1) return FALSE;
    PdfPosition p = {
        .page = g_key_file_get_integer(file, "view", "page", NULL),
        .rotation = g_key_file_get_integer(file, "view", "rotation", NULL),
        .sizing = g_key_file_get_integer(file, "view", "sizing", NULL),
        .scale = g_key_file_get_double(file, "view", "scale", NULL),
        .x = g_key_file_get_double(file, "view", "x", NULL),
        .y = g_key_file_get_double(file, "view", "y", NULL),
        .width = g_key_file_get_integer(file, "view", "width", NULL),
        .height = g_key_file_get_integer(file, "view", "height", NULL)
    };
    if (!valid(&p)) return FALSE;
    *position = p;
    return TRUE;
}

static gint oldest_first(gconstpointer a, gconstpointer b)
{
    GStatBuf sa = {0}, sb = {0};
    g_stat(*(char *const *)a, &sa); g_stat(*(char *const *)b, &sb);
    return (sa.st_mtime > sb.st_mtime) - (sa.st_mtime < sb.st_mtime);
}

gboolean pdf_state_save(const char *uri, const PdfPosition *p, GError **error)
{
    if (!valid(p)) return FALSE;
    g_autofree char *path = state_path(uri);
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Cannot create reading-state directory");
        return FALSE;
    }
    g_autoptr(GKeyFile) f = g_key_file_new();
    g_key_file_set_integer(f, "view", "version", 1);
    g_key_file_set_integer(f, "view", "page", p->page);
    g_key_file_set_integer(f, "view", "rotation", p->rotation);
    g_key_file_set_integer(f, "view", "sizing", p->sizing);
    g_key_file_set_double(f, "view", "scale", p->scale);
    g_key_file_set_double(f, "view", "x", p->x);
    g_key_file_set_double(f, "view", "y", p->y);
    g_key_file_set_integer(f, "view", "width", p->width);
    g_key_file_set_integer(f, "view", "height", p->height);
    gsize length;
    g_autofree char *data = g_key_file_to_data(f, &length, NULL);
    if (!g_file_set_contents_full(path, data, length,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error)) return FALSE;
    /* Bound retained history without storing a separate index or document text. */
    g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while (dir && (name = g_dir_read_name(dir)))
        if (strlen(name) == 68 && g_str_has_suffix(name, ".ini"))
            g_ptr_array_add(paths, g_build_filename(directory, name, NULL));
    g_ptr_array_sort(paths, oldest_first);
    for (guint i = 0; i + 200 < paths->len; i++) g_unlink(g_ptr_array_index(paths, i));
    return TRUE;
}
