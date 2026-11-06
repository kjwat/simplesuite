#define _GNU_SOURCE
#include "simplevis-wallpaper.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Desktop settings are read, never changed. No shell commands or screenshots
 * are needed, including on Wayland. Keep bus waits off the rendering thread. */
#define DESKTOP_TIMEOUT_MS 350
#define SAMPLE_SIZE 96

typedef struct {
    char *path;
    RGBColor color;
    gboolean solid;
} Wallpaper;

static char *config_file(const char *relative) {
    const char *base = g_getenv("XDG_CONFIG_HOME");
    if (base && g_path_is_absolute(base))
        return g_build_filename(base, relative, NULL);
    return g_build_filename(g_get_home_dir(), ".config", relative, NULL);
}

static gboolean parse_hex(const char *text, RGBColor *color) {
    if (!text || text[0] != '#' || strlen(text) != 7)
        return FALSE;
    for (int i = 1; i < 7; i++)
        if (!g_ascii_isxdigit(text[i]))
            return FALSE;
    unsigned int r, g, b;
    if (sscanf(text + 1, "%2x%2x%2x", &r, &g, &b) != 3)
        return FALSE;
    *color = (RGBColor){r / 255.0, g / 255.0, b / 255.0};
    return TRUE;
}

static gboolean parse_desktop_color(const char *text, RGBColor *color) {
    if (parse_hex(text, color))
        return TRUE;
    unsigned int r, g, b;
    char extra;
    gboolean long_hex = text && strlen(text) == 13 && text[0] == '#';
    for (int i = 1; long_hex && i < 13; i++)
        long_hex = g_ascii_isxdigit(text[i]);
    if (long_hex &&
        sscanf(text + 1, "%4x%4x%4x%c", &r, &g, &b, &extra) == 3) {
        *color = (RGBColor){r / 65535.0, g / 65535.0, b / 65535.0};
        return TRUE;
    }
    if (text && sscanf(text, "rgb( %u , %u , %u ) %c",
                       &r, &g, &b, &extra) == 3 && r <= 255 && g <= 255 && b <= 255) {
        *color = (RGBColor){r / 255.0, g / 255.0, b / 255.0};
        return TRUE;
    }
    return FALSE;
}

int simplevis_simplewm_accent(RGBColor *color) {
    g_autofree char *path = config_file("simplewm/colors.Xresources");
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;
    char line[256], hex[8], extra;
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, " *.cursorColor: %7s %c", hex, &extra) == 1 &&
            parse_hex(hex, color)) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

/* Local filenames and file URIs only; wallpaper paths are data, not commands. */
static char *local_path(const char *value) {
    if (!value || !*value)
        return NULL;
    if (g_path_is_absolute(value))
        return g_strdup(value);
    if (g_str_has_prefix(value, "~/"))
        return g_build_filename(g_get_home_dir(), value + 2, NULL);
    if (g_str_has_prefix(value, "file:")) {
        g_autofree char *host = NULL;
        char *path = g_filename_from_uri(value, &host, NULL);
        if (host && *host && g_ascii_strcasecmp(host, "localhost") != 0) {
            g_free(path);
            return NULL;
        }
        return path;
    }
    return NULL;
}

static gboolean set_path(Wallpaper *wallpaper, const char *value) {
    char *path = local_path(value);
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return FALSE;
    }
    g_free(wallpaper->path);
    wallpaper->path = path;
    return TRUE;
}

static GVariant *setting_value(const char *schema_id, const char *key) {
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    if (!source)
        return NULL;
    g_autoptr(GSettingsSchema) schema =
        g_settings_schema_source_lookup(source, schema_id, TRUE);
    if (!schema || !g_settings_schema_has_key(schema, key) ||
        !g_settings_schema_get_path(schema))
        return NULL;
    g_autoptr(GSettings) settings = g_settings_new_full(schema, NULL, NULL);
    return g_settings_get_value(settings, key);
}

static char *setting_string(const char *schema_id, const char *key) {
    g_autoptr(GVariant) value = setting_value(schema_id, key);
    return value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ?
        g_variant_dup_string(value, NULL) : NULL;
}

static gboolean prefer_dark(void) {
    g_autofree char *value =
        setting_string("org.gnome.desktop.interface", "color-scheme");
    return g_strcmp0(value, "prefer-dark") == 0;
}

static gboolean settings_wallpaper(const char *schema, const char *key,
                                   Wallpaper *wallpaper) {
    /* Cinnamon 6.6+ publishes its live per-monitor images in an aa{sv}.
     * Older Cinnamon continues to use the single picture-uri setting. */
    if (g_str_equal(schema, "org.cinnamon.desktop.background")) {
        g_autoptr(GVariant) list = setting_value(schema, "picture-uri-list");
        if (list && g_variant_is_of_type(list, G_VARIANT_TYPE("aa{sv}"))) {
            for (gsize i = 0; i < g_variant_n_children(list); i++) {
                g_autoptr(GVariant) entry = g_variant_get_child_value(list, i);
                const char *uri = NULL;
                if (g_variant_lookup(entry, "picture-uri", "&s", &uri) &&
                    set_path(wallpaper, uri))
                    return TRUE;
            }
        }
    }
    g_autofree char *options = setting_string(schema, "picture-options");
    if (g_strcmp0(options, "none") != 0) {
        if (g_str_equal(key, "picture-uri") && prefer_dark()) {
            g_autofree char *dark = setting_string(schema, "picture-uri-dark");
            if (set_path(wallpaper, dark))
                return TRUE;
        }
        g_autofree char *value = setting_string(schema, key);
        if (set_path(wallpaper, value))
            return TRUE;
    }
    g_autofree char *solid = setting_string(schema, "primary-color");
    wallpaper->solid = parse_desktop_color(solid, &wallpaper->color);
    return wallpaper->solid;
}

static GVariant *desktop_call(GDBusConnection *bus, const char *name,
                              const char *path, const char *interface,
                              const char *method, GVariant *parameters,
                              const GVariantType *type) {
    if (!bus)
        return NULL;
    return g_dbus_connection_call_sync(bus, name, path, interface, method,
        parameters, type, G_DBUS_CALL_FLAGS_NO_AUTO_START,
        DESKTOP_TIMEOUT_MS, NULL, NULL);
}

static gboolean plasma_wallpaper(GDBusConnection *bus, Wallpaper *wallpaper) {
    /* Read only the current activity. Do not accidentally pick an old
     * containment, panel or wallpaper from another activity. */
    const char script[] =
        "var ds=desktopsForActivity(currentActivity());"
        "ds.sort(function(a,b){return a.screen-b.screen;});"
        "for(var i=0;i<ds.length;i++){var d=ds[i];"
        "d.currentConfigGroup=['Wallpaper',d.wallpaperPlugin,'General'];"
        "var image=d.readConfig('Image','');"
        "if(image){print(image);break;}"
        "if(d.wallpaperPlugin==='org.kde.color'){"
        "print(d.readConfig('Color',''));break;}}";
    g_autoptr(GVariant) reply = desktop_call(bus, "org.kde.plasmashell",
        "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript",
        g_variant_new("(s)", script), G_VARIANT_TYPE("(s)"));
    if (!reply)
        return FALSE;
    const char *result;
    g_variant_get(reply, "(&s)", &result);
    g_autofree char *value = g_strdup(result);
    g_strstrip(value);
    if (set_path(wallpaper, value))
        return TRUE;
    wallpaper->solid = parse_hex(value, &wallpaper->color);
    return wallpaper->solid;
}

typedef struct {
    Wallpaper *wallpaper;
    GPtrArray *names;
} XfceXml;

static const char *attribute(const char **names, const char **values,
                             const char *name) {
    for (int i = 0; names && names[i]; i++)
        if (g_str_equal(names[i], name))
            return values[i];
    return NULL;
}

static void xfce_element(GMarkupParseContext *context, const char *element,
                          const char **names, const char **values,
                          gpointer data, GError **error) {
    (void)context; (void)error;
    XfceXml *xml = data;
    if (!g_str_equal(element, "property"))
        return;
    const char *name = attribute(names, values, "name");
    const char *value = attribute(names, values, "value");
    g_ptr_array_add(xml->names, g_strdup(name ? name : ""));
    if (xml->wallpaper->path || !name || !value)
        return;
    if (g_str_equal(name, "last-image") || g_str_equal(name, "image-path"))
        set_path(xml->wallpaper, value);
}

static void xfce_end(GMarkupParseContext *context, const char *element,
                      gpointer data, GError **error) {
    (void)context; (void)error;
    XfceXml *xml = data;
    if (g_str_equal(element, "property") && xml->names->len)
        g_ptr_array_remove_index(xml->names, xml->names->len - 1);
}

static gboolean xfce_wallpaper(GDBusConnection *bus, Wallpaper *wallpaper) {
    g_autoptr(GVariant) reply = desktop_call(bus, "org.xfce.Xfconf",
        "/org/xfce/Xfconf", "org.xfce.Xfconf", "GetAllProperties",
        g_variant_new("(ss)", "xfce4-desktop", "/backdrop"),
        G_VARIANT_TYPE("(a{sv})"));
    if (reply) {
        g_autoptr(GVariant) properties = g_variant_get_child_value(reply, 0);
        GVariantIter iter;
        const char *key;
        GVariant *value;
        g_variant_iter_init(&iter, properties);
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
            gboolean found = (g_str_has_suffix(key, "/last-image") ||
                              g_str_has_suffix(key, "/image-path")) &&
                g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) &&
                set_path(wallpaper, g_variant_get_string(value, NULL));
            g_variant_unref(value);
            if (found)
                return TRUE;
        }
    }
    g_autofree char *path =
        config_file("xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml");
    g_autofree char *contents = NULL;
    gsize size;
    if (!g_file_get_contents(path, &contents, &size, NULL))
        return FALSE;
    XfceXml xml = {wallpaper, g_ptr_array_new_with_free_func(g_free)};
    GMarkupParser parser = {.start_element = xfce_element, .end_element = xfce_end};
    g_autoptr(GMarkupParseContext) context =
        g_markup_parse_context_new(&parser, 0, &xml, NULL);
    gboolean valid = g_markup_parse_context_parse(context, contents, size, NULL) &&
                     g_markup_parse_context_end_parse(context, NULL);
    g_ptr_array_unref(xml.names);
    return valid && wallpaper->path;
}

static gboolean keyfile_wallpaper(const char *path, Wallpaper *wallpaper) {
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
        return FALSE;
    g_auto(GStrv) groups = g_key_file_get_groups(file, NULL);
    for (int i = 0; groups[i]; i++) {
        /* LXQt uses [Desktop], LXDE uses [*] or a monitor name. */
        const char *keys[] = {"Wallpaper", "wallpaper", "wallpaper0", NULL};
        for (int j = 0; keys[j]; j++) {
            g_autofree char *value =
                g_key_file_get_string(file, groups[i], keys[j], NULL);
            if (set_path(wallpaper, value))
                return TRUE;
        }
    }
    return FALSE;
}

static gint compare_names(gconstpointer a, gconstpointer b) {
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

static gboolean pcmanfm_wallpaper(gboolean qt, Wallpaper *wallpaper) {
    g_autofree char *base = config_file(qt ? "pcmanfm-qt" : "pcmanfm");
    g_autoptr(GDir) directory = g_dir_open(base, 0, NULL);
    if (!directory)
        return FALSE;
    g_autoptr(GPtrArray) profiles = g_ptr_array_new_with_free_func(g_free);
    const char *name;
    while ((name = g_dir_read_name(directory)))
        g_ptr_array_add(profiles, g_strdup(name));
    g_ptr_array_sort(profiles, compare_names);
    /* The normal profiles go first; old profiles are only fallbacks. */
    g_autofree char *normal = g_build_filename(base,
        qt ? "lxqt/settings.conf" : "LXDE/desktop-items-0.conf", NULL);
    if (keyfile_wallpaper(normal, wallpaper))
        return TRUE;
    for (guint i = 0; i < profiles->len; i++) {
        g_autofree char *profile = g_build_filename(base, profiles->pdata[i], NULL);
        if (qt) {
            g_autofree char *path = g_build_filename(profile, "settings.conf", NULL);
            if (keyfile_wallpaper(path, wallpaper))
                return TRUE;
        } else {
            g_autoptr(GDir) files = g_dir_open(profile, 0, NULL);
            if (!files)
                continue;
            while ((name = g_dir_read_name(files))) {
                if (!g_str_has_prefix(name, "desktop-items-") ||
                    !g_str_has_suffix(name, ".conf"))
                    continue;
                g_autofree char *path = g_build_filename(profile, name, NULL);
                if (keyfile_wallpaper(path, wallpaper))
                    return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean cosmic_wallpaper(Wallpaper *wallpaper) {
    /* The state file follows the actual slideshow image; config/all only
     * describes the slideshow source. Both are local RON data. */
    const char *state_home = g_getenv("XDG_STATE_HOME");
    g_autofree char *fallback = g_build_filename(g_get_home_dir(), ".local/state", NULL);
    if (!state_home || !g_path_is_absolute(state_home))
        state_home = fallback;
    g_autofree char *state = g_build_filename(state_home,
        "cosmic/com.system76.CosmicBackground/v1/wallpapers", NULL);
    g_autofree char *config =
        config_file("cosmic/com.system76.CosmicBackground/v1/all");
    const char *paths[] = {state, config, NULL};
    g_autoptr(GRegex) pattern = g_regex_new(
        "Path\\s*\\(\\s*\"((?:\\\\.|[^\"\\\\])*)\"", 0, 0, NULL);
    for (int i = 0; paths[i]; i++) {
        g_autofree char *contents = NULL;
        if (!g_file_get_contents(paths[i], &contents, NULL, NULL))
            continue;
        g_autoptr(GMatchInfo) matches = NULL;
        g_regex_match(pattern, contents, 0, &matches);
        while (g_match_info_matches(matches)) {
            g_autofree char *escaped = g_match_info_fetch(matches, 1);
            g_autofree char *path = g_strcompress(escaped);
            if (set_path(wallpaper, path))
                return TRUE;
            g_match_info_next(matches, NULL);
        }
        g_autoptr(GRegex) solid = g_regex_new(
            "Single\\s*\\(\\s*\\[\\s*([0-9.eE+-]+)\\s*,\\s*"
            "([0-9.eE+-]+)\\s*,\\s*([0-9.eE+-]+)\\s*\\]", 0, 0, NULL);
        g_clear_pointer(&matches, g_match_info_free);
        g_regex_match(solid, contents, 0, &matches);
        if (g_match_info_matches(matches)) {
            double values[3];
            gboolean valid = TRUE;
            for (int n = 0; n < 3; n++) {
                g_autofree char *value = g_match_info_fetch(matches, n + 1);
                values[n] = g_ascii_strtod(value, NULL);
                valid &= isfinite(values[n]) && values[n] >= 0 && values[n] <= 1;
            }
            if (valid) {
                wallpaper->color = (RGBColor){values[0], values[1], values[2]};
                wallpaper->solid = TRUE;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static gboolean deepin_wallpaper(GDBusConnection *bus, Wallpaper *wallpaper) {
    const char *services[] = {
        "org.deepin.dde.Appearance1", "com.deepin.daemon.Appearance", NULL
    };
    const char *paths[] = {
        "/org/deepin/dde/Appearance1", "/com/deepin/daemon/Appearance"
    };
    for (int i = 0; services[i]; i++) {
        g_autoptr(GVariant) reply = desktop_call(bus, services[i], paths[i],
            services[i], "GetCurrentWorkspaceBackground", NULL,
            G_VARIANT_TYPE("(s)"));
        if (reply) {
            const char *value;
            g_variant_get(reply, "(&s)", &value);
            if (set_path(wallpaper, value))
                return TRUE;
        }
    }
    return settings_wallpaper("com.deepin.wrap.gnome.desktop.background",
                              "picture-uri", wallpaper) ||
           settings_wallpaper("org.gnome.desktop.background",
                              "picture-uri", wallpaper);
}

static gboolean query_wallpaper(GDBusConnection *bus, Wallpaper *wallpaper) {
    const char *current = g_getenv("XDG_CURRENT_DESKTOP");
    const char *session = g_getenv("DESKTOP_SESSION");
    const char *gdm = g_getenv("GDMSESSION");
    const char *name = current && *current ? current :
                       session && *session ? session : gdm && *gdm ? gdm : "";
    g_autofree char *desktop = g_ascii_strdown(name, -1);
#ifdef __APPLE__
    if (!*desktop || g_str_equal(desktop, "aqua")) {
        extern char *simplevis_macos_wallpaper(void);
        g_autofree char *mac_path = simplevis_macos_wallpaper();
        if (set_path(wallpaper, mac_path))
            return TRUE;
    }
#endif
    if (strstr(desktop, "simplewm") || !*desktop) {
        wallpaper->solid = simplevis_simplewm_accent(&wallpaper->color);
        if (wallpaper->solid || *desktop)
            return wallpaper->solid;
    }
    if (strstr(desktop, "kde") || strstr(desktop, "plasma"))
        return plasma_wallpaper(bus, wallpaper);
    if (strstr(desktop, "xfce"))
        return xfce_wallpaper(bus, wallpaper);
    if (strstr(desktop, "cinnamon"))
        return settings_wallpaper("org.cinnamon.desktop.background",
                                  "picture-uri", wallpaper);
    if (strstr(desktop, "mate"))
        return settings_wallpaper("org.mate.background",
                                  "picture-filename", wallpaper);
    if (strstr(desktop, "lxqt"))
        return pcmanfm_wallpaper(TRUE, wallpaper);
    if (strstr(desktop, "lxde"))
        return pcmanfm_wallpaper(FALSE, wallpaper);
    if (strstr(desktop, "deepin") || strstr(desktop, "dde"))
        return deepin_wallpaper(bus, wallpaper);
    if (strstr(desktop, "cosmic"))
        return cosmic_wallpaper(wallpaper);
    /* GNOME, Ubuntu, Budgie, Unity and Pantheon share this interface. The
     * schema is looked up before constructing GSettings, so other desktops
     * do not abort when GNOME's schemas are absent. */
    gboolean gnome_family = strstr(desktop, "gnome") || strstr(desktop, "ubuntu") ||
        strstr(desktop, "budgie") || strstr(desktop, "unity") ||
        strstr(desktop, "pantheon") || strstr(desktop, "elementary");
    gboolean found = settings_wallpaper("org.gnome.desktop.background",
                                        "picture-uri", wallpaper);
    if (found || gnome_family)
        return found;
    wallpaper->solid = simplevis_simplewm_accent(&wallpaper->color);
    return wallpaper->solid;
}

static char *image_in_package(const char *path) {
    if (!g_file_test(path, G_FILE_TEST_IS_DIR))
        return g_strdup(path);
    const char *subdirs[] = {"contents/images", "contents/images_dark", NULL};
    int first = prefer_dark() ? 1 : 0;
    for (int n = 0; n < 2; n++) {
        g_autofree char *directory = g_build_filename(path, subdirs[(first + n) % 2], NULL);
        g_autoptr(GDir) dir = g_dir_open(directory, 0, NULL);
        if (!dir)
            continue;
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            g_autofree char *file = g_build_filename(directory, name, NULL);
            if (gdk_pixbuf_get_file_info(file, NULL, NULL))
                return g_steal_pointer(&file);
        }
    }
    return NULL;
}

typedef struct {
    char *from, *to;
    double duration;
} Slide;

typedef struct {
    GPtrArray *slides;
    Slide *slide;
    GString *text;
    struct tm start;
    gboolean in_start, to;
} Slideshow;

static void slide_free(gpointer data) {
    Slide *slide = data;
    g_free(slide->from); g_free(slide->to); g_free(slide);
}

static void slide_start(GMarkupParseContext *context, const char *element,
                         const char **names, const char **values,
                         gpointer data, GError **error) {
    (void)context; (void)names; (void)values; (void)error;
    Slideshow *show = data;
    g_string_truncate(show->text, 0);
    if (g_str_equal(element, "starttime"))
        show->in_start = TRUE;
    if (g_str_equal(element, "static") || g_str_equal(element, "transition")) {
        show->slide = g_new0(Slide, 1);
        show->to = FALSE;
    }
    if (g_str_equal(element, "to"))
        show->to = TRUE;
    if (g_str_equal(element, "from") || g_str_equal(element, "file"))
        show->to = FALSE;
}

static void slide_text(GMarkupParseContext *context, const char *text,
                        gsize size, gpointer data, GError **error) {
    (void)context; (void)error;
    Slideshow *show = data;
    g_string_append_len(show->text, text, size);
}

static void slide_end(GMarkupParseContext *context, const char *element,
                       gpointer data, GError **error) {
    (void)context; (void)error;
    Slideshow *show = data;
    char *text = g_strstrip(show->text->str);
    if (show->in_start) {
        int value = (int)g_ascii_strtoll(text, NULL, 10);
        if (g_str_equal(element, "year")) show->start.tm_year = value - 1900;
        if (g_str_equal(element, "month")) show->start.tm_mon = value - 1;
        if (g_str_equal(element, "day")) show->start.tm_mday = value;
        if (g_str_equal(element, "hour")) show->start.tm_hour = value;
        if (g_str_equal(element, "minute")) show->start.tm_min = value;
        if (g_str_equal(element, "second")) show->start.tm_sec = value;
    }
    if (g_str_equal(element, "starttime"))
        show->in_start = FALSE;
    if (show->slide) {
        if (g_str_equal(element, "duration"))
            show->slide->duration = g_ascii_strtod(text, NULL);
        if (*text && (g_str_equal(element, "file") || g_str_equal(element, "from") ||
                      g_str_equal(element, "to") || g_str_equal(element, "size"))) {
            char **target = show->to ? &show->slide->to : &show->slide->from;
            if (!*target)
                *target = g_strdup(text);
        }
        if (g_str_equal(element, "static") || g_str_equal(element, "transition")) {
            g_ptr_array_add(show->slides, show->slide);
            show->slide = NULL;
        }
    }
    g_string_truncate(show->text, 0);
}

static char *slideshow_image(const char *path, time_t now) {
    g_autofree char *contents = NULL;
    gsize size;
    if (!g_file_get_contents(path, &contents, &size, NULL))
        return NULL;
    Slideshow show = {.slides = g_ptr_array_new_with_free_func(slide_free),
                     .text = g_string_new(NULL),
                     .start = {.tm_year = 70, .tm_mday = 1, .tm_isdst = -1}};
    GMarkupParser parser = {.start_element = slide_start, .end_element = slide_end,
                            .text = slide_text};
    g_autoptr(GMarkupParseContext) context =
        g_markup_parse_context_new(&parser, 0, &show, NULL);
    gboolean valid = g_markup_parse_context_parse(context, contents, size, NULL) &&
                     g_markup_parse_context_end_parse(context, NULL);
    double duration = 0;
    for (guint i = 0; i < show.slides->len; i++) {
        Slide *slide = show.slides->pdata[i];
        if (isfinite(slide->duration) && slide->duration > 0)
            duration += slide->duration;
    }
    char *result = NULL;
    if (valid && duration > 0) {
        double elapsed = fmod(difftime(now, mktime(&show.start)), duration);
        if (elapsed < 0)
            elapsed += duration;
        for (guint i = 0; i < show.slides->len; i++) {
            Slide *slide = show.slides->pdata[i];
            if (!isfinite(slide->duration) || slide->duration <= 0)
                continue;
            if (elapsed >= slide->duration) {
                elapsed -= slide->duration;
                continue;
            }
            const char *image = slide->to && elapsed >= slide->duration / 2 ?
                                slide->to : slide->from;
            result = local_path(image);
            if (!result && image && *image && !strstr(image, "://")) {
                g_autofree char *directory = g_path_get_dirname(path);
                result = g_build_filename(directory, image, NULL);
            }
            break;
        }
    }
    if (show.slide)
        slide_free(show.slide);
    g_string_free(show.text, TRUE);
    g_ptr_array_unref(show.slides);
    return result;
}

typedef struct {
    double weight, r, g, b;
} ColorBin;

static gboolean image_accent(const char *path, RGBColor *color) {
    g_autoptr(GdkPixbuf) image =
        gdk_pixbuf_new_from_file_at_scale(path, SAMPLE_SIZE, SAMPLE_SIZE, TRUE, NULL);
    if (!image)
        return FALSE;
    ColorBin bins[36] = {{0}}, neutral[16] = {{0}};
    int width = gdk_pixbuf_get_width(image), height = gdk_pixbuf_get_height(image);
    int channels = gdk_pixbuf_get_n_channels(image), stride = gdk_pixbuf_get_rowstride(image);
    const guchar *pixels = gdk_pixbuf_read_pixels(image);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const guchar *p = pixels + y * stride + x * channels;
            double alpha = channels == 4 ? p[3] / 255.0 : 1.0;
            if (alpha < 0.1)
                continue;
            double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
            double max = fmax(r, fmax(g, b)), min = fmin(r, fmin(g, b));
            double delta = max - min, saturation = max > 0 ? delta / max : 0;
            int gray = MIN(15, (int)(max * 15.999));
            ColorBin *bin = &neutral[gray];
            double weight = alpha;
            if (saturation >= 0.18 && max >= 0.12) {
                double hue = max == r ? (g - b) / delta :
                             max == g ? (b - r) / delta + 2 :
                                        (r - g) / delta + 4;
                if (hue < 0)
                    hue += 6;
                bin = &bins[MIN(35, (int)(hue * 6))];
                weight *= sqrt(saturation) * (0.3 + sqrt(max));
            }
            bin->weight += weight;
            bin->r += weight * r; bin->g += weight * g; bin->b += weight * b;
        }
    }
    int best = 0;
    double best_weight = 0;
    for (int i = 0; i < 36; i++) {
        double weight = bins[(i + 35) % 36].weight + bins[i].weight +
                        bins[(i + 1) % 36].weight;
        if (weight > best_weight) {
            best = i;
            best_weight = weight;
        }
    }
    ColorBin chosen = {0};
    if (best_weight > 0) {
        for (int n = -1; n <= 1; n++) {
            ColorBin *bin = &bins[(best + n + 36) % 36];
            chosen.weight += bin->weight;
            chosen.r += bin->r; chosen.g += bin->g; chosen.b += bin->b;
        }
    } else {
        for (int i = 0; i < 16; i++)
            if (neutral[i].weight > chosen.weight)
                chosen = neutral[i];
    }
    if (chosen.weight == 0)
        return FALSE;
    *color = (RGBColor){chosen.r / chosen.weight, chosen.g / chosen.weight,
                       chosen.b / chosen.weight};
    /* Black/near-black backgrounds still need visible bars. */
    if (fmax(color->r, fmax(color->g, color->b)) < 0.12)
        *color = (RGBColor){1, 1, 1};
    return TRUE;
}

typedef struct {
    char *path;
    GStatBuf stat;
    RGBColor color;
    gboolean valid;
} ImageCache;

static long modified_nsec(const GStatBuf *info) {
#ifdef __APPLE__
    return info->st_mtimespec.tv_nsec;
#else
    return info->st_mtim.tv_nsec;
#endif
}

static gboolean resolve_color(GDBusConnection *bus, ImageCache *cache, RGBColor *color) {
    Wallpaper wallpaper = {0};
    if (!query_wallpaper(bus, &wallpaper)) {
        g_free(wallpaper.path);
        return FALSE;
    }
    if (wallpaper.solid) {
        *color = wallpaper.color;
        return TRUE;
    }
    g_autofree char *path = image_in_package(wallpaper.path);
    g_free(wallpaper.path);
    if (path && g_str_has_suffix(path, ".xml")) {
        char *image = slideshow_image(path, time(NULL));
        g_free(path);
        path = image;
    }
    GStatBuf file_stat;
    if (!path || g_stat(path, &file_stat) != 0)
        return FALSE;
    if (g_strcmp0(cache->path, path) || !cache->valid ||
        file_stat.st_mtime != cache->stat.st_mtime || file_stat.st_size != cache->stat.st_size ||
        file_stat.st_ino != cache->stat.st_ino ||
        modified_nsec(&file_stat) != modified_nsec(&cache->stat)) {
        g_free(cache->path);
        cache->path = g_strdup(path);
        cache->stat = file_stat;
        cache->valid = image_accent(path, &cache->color);
    }
    if (cache->valid)
        *color = cache->color;
    return cache->valid;
}

static struct {
    GMutex mutex;
    GCond condition;
    GThread *thread;
    gboolean stop, valid, ready;
    RGBColor color;
} accent_worker;

static gpointer wallpaper_worker(gpointer unused) {
    (void)unused;
    g_autoptr(GDBusConnection) bus = NULL;
    const char *address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address)
        bus = g_dbus_connection_new_for_address_sync(address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
    if (bus)
        g_dbus_connection_set_exit_on_close(bus, FALSE);
    ImageCache cache = {0};
    for (;;) {
        /* Dispatch dconf's change notifications before reading cached keys. */
        while (g_main_context_pending(NULL))
            g_main_context_iteration(NULL, FALSE);
        RGBColor color;
        gboolean valid = resolve_color(bus, &cache, &color);
        g_mutex_lock(&accent_worker.mutex);
        accent_worker.valid = valid;
        accent_worker.ready = TRUE;
        if (valid)
            accent_worker.color = color;
        g_cond_broadcast(&accent_worker.condition);
        if (!accent_worker.stop)
            g_cond_wait_until(&accent_worker.condition, &accent_worker.mutex,
                             g_get_monotonic_time() + G_TIME_SPAN_SECOND);
        gboolean stop = accent_worker.stop;
        g_mutex_unlock(&accent_worker.mutex);
        if (stop)
            break;
    }
    g_free(cache.path);
    if (bus)
        g_dbus_connection_close_sync(bus, NULL, NULL);
    return NULL;
}

int simplevis_wallpaper_status(RGBColor *color) {
    g_mutex_lock(&accent_worker.mutex);
    if (!accent_worker.thread) {
        accent_worker.stop = FALSE;
        accent_worker.thread =
            g_thread_try_new("wallpaper-color", wallpaper_worker, NULL, NULL);
        if (!accent_worker.thread)
            accent_worker.ready = TRUE;
    }
    int status = accent_worker.ready ? accent_worker.valid : -1;
    if (status > 0)
        *color = accent_worker.color;
    g_mutex_unlock(&accent_worker.mutex);
    return status;
}

int simplevis_wallpaper_color(RGBColor *color) {
    return simplevis_wallpaper_status(color) > 0;
}

int simplevis_wallpaper_prepare(int timeout_ms) {
    RGBColor color;
    (void)simplevis_wallpaper_color(&color);
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    g_mutex_lock(&accent_worker.mutex);
    while (!accent_worker.ready &&
           g_cond_wait_until(&accent_worker.condition, &accent_worker.mutex, deadline))
        ;
    gboolean ready = accent_worker.ready;
    g_mutex_unlock(&accent_worker.mutex);
    return ready;
}

void simplevis_wallpaper_shutdown(void) {
    g_mutex_lock(&accent_worker.mutex);
    GThread *thread = accent_worker.thread;
    accent_worker.stop = TRUE;
    g_cond_signal(&accent_worker.condition);
    g_mutex_unlock(&accent_worker.mutex);
    if (thread)
        g_thread_join(thread);
    g_mutex_lock(&accent_worker.mutex);
    accent_worker.thread = NULL;
    accent_worker.valid = FALSE;
    accent_worker.ready = FALSE;
    g_mutex_unlock(&accent_worker.mutex);
}
