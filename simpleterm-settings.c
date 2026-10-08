#include "simpleterm-settings.h"
#include <errno.h>
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>

typedef enum { SETTING_BOOLEAN, SETTING_INTEGER, SETTING_DOUBLE, SETTING_COLOR, SETTING_FONT } SettingType;

typedef struct {
    const char *group, *key;
    SettingType type;
    gsize offset;
    double minimum, maximum;
} SettingField;

#define FIELD(group, key, type, member, minimum, maximum) \
    {group, key, type, G_STRUCT_OFFSET(SimpletermSettings, member), minimum, maximum}

static const SettingField fields[] = {
    FIELD("General", "show-menubar", SETTING_BOOLEAN, show_menubar, 0, 0),
    FIELD("Text", "columns", SETTING_INTEGER, columns, 16, 511),
    FIELD("Text", "rows", SETTING_INTEGER, rows, 2, 511),
    FIELD("Text", "custom-font", SETTING_BOOLEAN, custom_font, 0, 0),
    FIELD("Text", "font", SETTING_FONT, font, 0, 0),
    FIELD("Text", "cell-width", SETTING_DOUBLE, cell_width, 1, 2),
    FIELD("Text", "cell-height", SETTING_DOUBLE, cell_height, 1, 2),
    FIELD("Text", "text-blink", SETTING_INTEGER, text_blink, 0, 3),
    FIELD("Text", "cursor-shape", SETTING_INTEGER, cursor_shape, 0, 2),
    FIELD("Text", "cursor-blink", SETTING_INTEGER, cursor_blink, 0, 2),
    FIELD("Text", "audible-bell", SETTING_BOOLEAN, audible_bell, 0, 0),
    FIELD("Colors", "use-theme-colors", SETTING_BOOLEAN, use_theme_colors, 0, 0),
    FIELD("Colors", "foreground", SETTING_COLOR, foreground, 0, 0),
    FIELD("Colors", "background", SETTING_COLOR, background, 0, 0),
    FIELD("Colors", "custom-bold", SETTING_BOOLEAN, custom_bold, 0, 0),
    FIELD("Colors", "bold", SETTING_COLOR, bold, 0, 0),
    FIELD("Colors", "custom-cursor", SETTING_BOOLEAN, custom_cursor, 0, 0),
    FIELD("Colors", "cursor", SETTING_COLOR, cursor, 0, 0),
    FIELD("Colors", "cursor-foreground", SETTING_COLOR, cursor_foreground, 0, 0),
    FIELD("Colors", "custom-highlight", SETTING_BOOLEAN, custom_highlight, 0, 0),
    FIELD("Colors", "highlight", SETTING_COLOR, highlight, 0, 0),
    FIELD("Colors", "highlight-foreground", SETTING_COLOR, highlight_foreground, 0, 0),
    FIELD("Colors", "transparent", SETTING_BOOLEAN, transparent, 0, 0),
    FIELD("Colors", "background-opacity", SETTING_DOUBLE, background_opacity, 0, 1),
    FIELD("Colors", "bold-is-bright", SETTING_BOOLEAN, bold_is_bright, 0, 0),
    FIELD("Scrolling", "show-scrollbar", SETTING_BOOLEAN, show_scrollbar, 0, 0),
    FIELD("Scrolling", "scroll-on-output", SETTING_BOOLEAN, scroll_on_output, 0, 0),
    FIELD("Scrolling", "scroll-on-keystroke", SETTING_BOOLEAN, scroll_on_keystroke, 0, 0),
    FIELD("Scrolling", "scroll-on-paste", SETTING_BOOLEAN, scroll_on_paste, 0, 0),
    FIELD("Scrolling", "limit-scrollback", SETTING_BOOLEAN, limit_scrollback, 0, 0),
    FIELD("Scrolling", "scrollback-lines", SETTING_INTEGER, scrollback_lines, 0, 10000000)
};

#undef FIELD

static const char *palette_names[] = {"GNOME", "Tango", "Solarized", "Custom", NULL};
static const char *palettes[][16] = {
    {"#171421", "#c01c28", "#26a269", "#a2734c", "#12488b", "#a347ba", "#2aa1b3", "#d0cfcc",
     "#5e5c64", "#f66151", "#33da7a", "#e9ad0c", "#2a7bde", "#c061cb", "#33c7de", "#ffffff"},
    {"#2e3436", "#cc0000", "#4e9a06", "#c4a000", "#3465a4", "#75507b", "#06989a", "#d3d7cf",
     "#555753", "#ef2929", "#8ae234", "#fce94f", "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec"},
    {"#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
     "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3"}
};

static const char *scheme_names[] = {
    "Simpleterm", "White on black", "Black on white", "Solarized dark", "Solarized light", "Custom", NULL
};
static const char *schemes[][2] = {
    {"#eeeeec", "#1e1e1e"}, {"#ffffff", "#000000"}, {"#000000", "#ffffff"},
    {"#839496", "#002b36"}, {"#657b83", "#fdf6e3"}
};

void simpleterm_settings_defaults(SimpletermSettings *settings)
{
    *settings = (SimpletermSettings){
        .font = g_strdup("Monospace 12"), .columns = 80, .rows = 24,
        .cell_width = 1, .cell_height = 1, .background_opacity = 0.9,
        .text_blink = 3, .audible_bell = TRUE, .show_scrollbar = TRUE,
        .scroll_on_keystroke = TRUE, .scroll_on_paste = TRUE,
        .limit_scrollback = TRUE, .scrollback_lines = 10000, .show_menubar = TRUE
    };
    gdk_rgba_parse(&settings->foreground, schemes[0][0]);
    gdk_rgba_parse(&settings->background, schemes[0][1]);
    settings->bold = settings->foreground;
    settings->cursor = settings->foreground;
    settings->cursor_foreground = settings->background;
    settings->highlight = settings->foreground;
    settings->highlight_foreground = settings->background;
    for (guint index = 0; index < 16; index++)
        gdk_rgba_parse(&settings->palette[index], palettes[0][index]);
}

void simpleterm_settings_clear(SimpletermSettings *settings)
{
    g_clear_pointer(&settings->font, g_free);
}

GSettings *simpleterm_settings_desktop_interface(void)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    if (!source) return NULL;
    g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE);
    if (!schema || !g_settings_schema_has_key(schema, "monospace-font-name")) return NULL;
    return g_settings_new_full(schema, NULL, NULL);
}

char *simpleterm_settings_font(const SimpletermSettings *settings, GSettings *desktop_interface)
{
    if (settings->custom_font) return g_strdup(settings->font);
    if (desktop_interface) {
        char *name = g_settings_get_string(desktop_interface, "monospace-font-name");
        g_autoptr(PangoFontDescription) font = pango_font_description_from_string(name);
        if (pango_font_description_get_family(font) && pango_font_description_get_size(font) > 0)
            return name;
        g_free(name);
    }
    return g_strdup("Monospace 12");
}

static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "simpleterm", "settings.ini", NULL);
}

gboolean simpleterm_settings_load(SimpletermSettings *settings, GError **error)
{
    simpleterm_settings_defaults(settings);
    g_autofree char *path = settings_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_autoptr(GError) load_error = NULL;
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &load_error)) {
        if (g_error_matches(load_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) return TRUE;
        g_propagate_error(error, g_steal_pointer(&load_error));
        return FALSE;
    }
    for (guint index = 0; index < G_N_ELEMENTS(fields); index++) {
        const SettingField *field = &fields[index];
        gpointer destination = (char *)settings + field->offset;
        g_autoptr(GError) value_error = NULL;
        if (field->type == SETTING_BOOLEAN) {
            gboolean value = g_key_file_get_boolean(file, field->group, field->key, &value_error);
            if (!value_error) *(gboolean *)destination = value;
        } else if (field->type == SETTING_INTEGER) {
            int value = g_key_file_get_integer(file, field->group, field->key, &value_error);
            if (!value_error && value >= field->minimum && value <= field->maximum)
                *(int *)destination = value;
        } else if (field->type == SETTING_DOUBLE) {
            double value = g_key_file_get_double(file, field->group, field->key, &value_error);
            if (!value_error && isfinite(value) && value >= field->minimum && value <= field->maximum)
                *(double *)destination = value;
        } else {
            g_autofree char *value = g_key_file_get_string(file, field->group, field->key, NULL);
            if (!value || !g_utf8_validate(value, -1, NULL)) continue;
            if (field->type == SETTING_COLOR) {
                GdkRGBA color;
                if (gdk_rgba_parse(&color, value)) {
                    color.alpha = 1;
                    *(GdkRGBA *)destination = color;
                }
            } else {
                g_autoptr(PangoFontDescription) font = pango_font_description_from_string(value);
                int size = pango_font_description_get_size(font);
                if (*value && pango_font_description_get_family(font) &&
                    size >= 4 * PANGO_SCALE && size <= 96 * PANGO_SCALE) {
                    g_free(settings->font);
                    settings->font = g_strdup(value);
                }
            }
        }
    }
    for (guint index = 0; index < 16; index++) {
        g_autofree char *key = g_strdup_printf("palette-%u", index);
        g_autofree char *value = g_key_file_get_string(file, "Colors", key, NULL);
        GdkRGBA color;
        if (value && gdk_rgba_parse(&color, value)) {
            color.alpha = 1;
            settings->palette[index] = color;
        }
    }
    return TRUE;
}

gboolean simpleterm_settings_save(const SimpletermSettings *settings, GError **error)
{
    g_autofree char *path = settings_path();
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
            "Cannot create %s: %s", directory, g_strerror(errno));
        return FALSE;
    }
    g_autoptr(GKeyFile) file = g_key_file_new();
    for (guint index = 0; index < G_N_ELEMENTS(fields); index++) {
        const SettingField *field = &fields[index];
        gconstpointer source = (const char *)settings + field->offset;
        if (field->type == SETTING_BOOLEAN)
            g_key_file_set_boolean(file, field->group, field->key, *(const gboolean *)source);
        else if (field->type == SETTING_INTEGER)
            g_key_file_set_integer(file, field->group, field->key, *(const int *)source);
        else if (field->type == SETTING_DOUBLE)
            g_key_file_set_double(file, field->group, field->key, *(const double *)source);
        else if (field->type == SETTING_FONT)
            g_key_file_set_string(file, field->group, field->key, settings->font);
        else {
            g_autofree char *value = gdk_rgba_to_string(source);
            g_key_file_set_string(file, field->group, field->key, value);
        }
    }
    for (guint index = 0; index < 16; index++) {
        g_autofree char *key = g_strdup_printf("palette-%u", index);
        g_autofree char *value = gdk_rgba_to_string(&settings->palette[index]);
        g_key_file_set_string(file, "Colors", key, value);
    }
    gsize length;
    g_autofree char *contents = g_key_file_to_data(file, &length, NULL);
    return g_file_set_contents_full(path, contents, length,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
}

typedef struct {
    GtkWidget *window, *status, *scheme, *palette_scheme;
    GtkWidget *palette[16];
    GHashTable *controls;
    GSettings *desktop_interface;
    SimpletermSettings *settings;
    SimpletermSettingsChanged changed;
    gpointer data;
    gboolean updating;
} Preferences;

static const SettingField *find_field(const char *key)
{
    for (guint index = 0; index < G_N_ELEMENTS(fields); index++)
        if (g_str_equal(fields[index].key, key)) return &fields[index];
    g_assert_not_reached();
}

static GtkWidget *control(Preferences *preferences, const char *key)
{
    return g_hash_table_lookup(preferences->controls, key);
}

static void dependent(Preferences *preferences, const char *key, const char *toggle, gboolean inverse)
{
    gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(preferences, toggle)));
    gtk_widget_set_sensitive(control(preferences, key), inverse ? !active : active);
}

static void refresh(Preferences *preferences)
{
    SimpletermSettings *settings = preferences->settings;
    preferences->updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(preferences, "show-menubar")), settings->show_menubar);
    g_autofree char *font = simpleterm_settings_font(settings, preferences->desktop_interface);
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(control(preferences, "font")), font);
    dependent(preferences, "font", "custom-font", FALSE);
    dependent(preferences, "foreground", "use-theme-colors", TRUE);
    dependent(preferences, "background", "use-theme-colors", TRUE);
    gtk_widget_set_sensitive(preferences->scheme, !settings->use_theme_colors);
    dependent(preferences, "bold", "custom-bold", FALSE);
    dependent(preferences, "cursor", "custom-cursor", FALSE);
    dependent(preferences, "cursor-foreground", "custom-cursor", FALSE);
    dependent(preferences, "highlight", "custom-highlight", FALSE);
    dependent(preferences, "highlight-foreground", "custom-highlight", FALSE);
    dependent(preferences, "background-opacity", "transparent", FALSE);
    dependent(preferences, "scrollback-lines", "limit-scrollback", FALSE);
    int scheme = G_N_ELEMENTS(schemes);
    for (guint index = 0; index < G_N_ELEMENTS(schemes); index++) {
        GdkRGBA foreground, background;
        gdk_rgba_parse(&foreground, schemes[index][0]);
        gdk_rgba_parse(&background, schemes[index][1]);
        if (gdk_rgba_equal(&foreground, &settings->foreground) &&
            gdk_rgba_equal(&background, &settings->background)) scheme = index;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(preferences->scheme), scheme);
    int palette = G_N_ELEMENTS(palettes);
    for (guint index = 0; index < G_N_ELEMENTS(palettes); index++) {
        gboolean matches = TRUE;
        for (guint color_index = 0; color_index < 16; color_index++) {
            GdkRGBA color;
            gdk_rgba_parse(&color, palettes[index][color_index]);
            if (!gdk_rgba_equal(&color, &settings->palette[color_index])) matches = FALSE;
        }
        if (matches) palette = index;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(preferences->palette_scheme), palette);
    preferences->updating = FALSE;
}

void simpleterm_settings_window_refresh(GtkWidget *window)
{
    Preferences *preferences = g_object_get_data(G_OBJECT(window), "preferences");
    refresh(preferences);
}

static void preferences_changed(Preferences *preferences)
{
    refresh(preferences);
    preferences->changed(preferences->data);
    g_autoptr(GError) error = NULL;
    if (simpleterm_settings_save(preferences->settings, &error))
        gtk_label_set_text(GTK_LABEL(preferences->status), "Changes apply immediately and are saved automatically.");
    else {
        g_autofree char *message = g_strdup_printf("Changes apply for this session. Could not save: %s", error->message);
        gtk_label_set_text(GTK_LABEL(preferences->status), message);
    }
}

static void control_changed(GtkWidget *widget, Preferences *preferences)
{
    if (preferences->updating) return;
    const SettingField *field = g_object_get_data(G_OBJECT(widget), "setting-field");
    gpointer destination = (char *)preferences->settings + field->offset;
    if (field->type == SETTING_BOOLEAN)
        *(gboolean *)destination = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    else if (field->type == SETTING_INTEGER)
        *(int *)destination = GTK_IS_COMBO_BOX(widget)
            ? gtk_combo_box_get_active(GTK_COMBO_BOX(widget))
            : gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
    else if (field->type == SETTING_DOUBLE)
        *(double *)destination = GTK_IS_RANGE(widget)
            ? gtk_range_get_value(GTK_RANGE(widget)) : gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    else if (field->type == SETTING_COLOR)
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), destination);
    else {
        g_free(preferences->settings->font);
        preferences->settings->font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(widget));
    }
    preferences_changed(preferences);
}

static GtkWidget *bind_control(Preferences *preferences, const char *key, GtkWidget *widget, const char *signal)
{
    g_object_set_data(G_OBJECT(widget), "setting-field", (gpointer)find_field(key));
    gtk_widget_set_name(widget, key);
    g_hash_table_insert(preferences->controls, (gpointer)key, widget);
    g_signal_connect(widget, signal, G_CALLBACK(control_changed), preferences);
    return widget;
}

static GtkWidget *check(Preferences *preferences, const char *key, const char *label)
{
    GtkWidget *widget = gtk_check_button_new_with_label(label);
    gboolean *value = (gboolean *)((char *)preferences->settings + find_field(key)->offset);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), *value);
    return bind_control(preferences, key, widget, "toggled");
}

static GtkWidget *spin(Preferences *preferences, const char *key)
{
    const SettingField *field = find_field(key);
    GtkWidget *widget = gtk_spin_button_new_with_range(field->minimum, field->maximum,
        field->type == SETTING_DOUBLE ? 0.01 : 1);
    gpointer value = (char *)preferences->settings + field->offset;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget),
        field->type == SETTING_DOUBLE ? *(double *)value : *(int *)value);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widget), TRUE);
    return bind_control(preferences, key, widget, "value-changed");
}

static GtkWidget *combo(Preferences *preferences, const char *key, const char *const *items)
{
    GtkWidget *widget = gtk_combo_box_text_new();
    for (guint index = 0; items[index]; index++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), items[index]);
    int *value = (int *)((char *)preferences->settings + find_field(key)->offset);
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), *value);
    return bind_control(preferences, key, widget, "changed");
}

static GtkWidget *color_button(Preferences *preferences, const char *key)
{
    GdkRGBA *value = (GdkRGBA *)((char *)preferences->settings + find_field(key)->offset);
    GtkWidget *widget = gtk_color_button_new_with_rgba(value);
    gtk_color_button_set_title(GTK_COLOR_BUTTON(widget), "Choose terminal color");
    return bind_control(preferences, key, widget, "color-set");
}

static GtkWidget *page_grid(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 18);
    gtk_widget_set_valign(grid, GTK_ALIGN_START);
    return grid;
}

static void heading(GtkWidget *grid, int row, const char *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    g_autofree char *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    if (row) gtk_widget_set_margin_top(label, 8);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 4, 1);
}

static void row_control(GtkWidget *grid, int row, const char *text, GtkWidget *widget)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 3, 1);
}

static GtkWidget *paired(GtkWidget *first, const char *first_label, GtkWidget *second, const char *second_label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(box), first, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(first_label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), second, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(second_label), FALSE, FALSE, 0);
    return box;
}

static void reset_values(GtkButton *button, Preferences *preferences)
{
    const char *keys[2];
    double values[2];
    if (g_str_equal(gtk_widget_get_name(GTK_WIDGET(button)), "reset-size")) {
        keys[0] = "columns"; keys[1] = "rows"; values[0] = 80; values[1] = 24;
    } else {
        keys[0] = "cell-width"; keys[1] = "cell-height"; values[0] = 1; values[1] = 1;
    }
    preferences->updating = TRUE;
    for (guint index = 0; index < 2; index++) {
        const SettingField *field = find_field(keys[index]);
        gpointer destination = (char *)preferences->settings + field->offset;
        if (field->type == SETTING_INTEGER) *(int *)destination = values[index];
        else *(double *)destination = values[index];
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(preferences, keys[index])), values[index]);
    }
    preferences->updating = FALSE;
    preferences_changed(preferences);
}

static GtkWidget *with_reset(Preferences *preferences, GtkWidget *widget, const char *name)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *reset = gtk_button_new_with_label("Reset");
    gtk_widget_set_name(reset, name);
    gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), reset, FALSE, FALSE, 0);
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_values), preferences);
    return box;
}

static GtkWidget *text_page(Preferences *preferences)
{
    GtkWidget *grid = page_grid();
    heading(grid, 0, "Text Appearance");
    row_control(grid, 1, "Initial terminal size:", with_reset(preferences,
        paired(spin(preferences, "columns"), "columns", spin(preferences, "rows"), "rows"), "reset-size"));
    GtkWidget *font = gtk_font_button_new_with_font(preferences->settings->font);
    gtk_font_button_set_title(GTK_FONT_BUTTON(font), "Choose terminal font");
    bind_control(preferences, "font", font, "font-set");
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "custom-font", "Custom font:"), 0, 2, 1, 1);
    gtk_widget_set_hexpand(font, TRUE);
    gtk_grid_attach(GTK_GRID(grid), font, 1, 2, 3, 1);
    row_control(grid, 3, "Cell spacing:", with_reset(preferences,
        paired(spin(preferences, "cell-width"), "× width", spin(preferences, "cell-height"), "× height"), "reset-spacing"));
    const char *text_blink[] = {"Never", "When focused", "When unfocused", "Always", NULL};
    row_control(grid, 4, "Allow blinking text:", combo(preferences, "text-blink", text_blink));
    heading(grid, 5, "Cursor");
    const char *shape[] = {"Block", "I-beam", "Underline", NULL};
    const char *blink[] = {"Default", "Always", "Never", NULL};
    row_control(grid, 6, "Cursor shape:", combo(preferences, "cursor-shape", shape));
    row_control(grid, 7, "Cursor blinking:", combo(preferences, "cursor-blink", blink));
    heading(grid, 8, "Sound");
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "audible-bell", "Terminal bell"), 0, 9, 4, 1);
    GtkWidget *note = gtk_label_new("Initial size applies to new windows. Uncheck Custom font to use the desktop monospace font.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), note, 0, 10, 4, 1);
    return grid;
}

static void scheme_changed(GtkComboBox *combo_box, Preferences *preferences)
{
    if (preferences->updating) return;
    int index = gtk_combo_box_get_active(combo_box);
    if (index < 0 || index >= (int)G_N_ELEMENTS(schemes)) return;
    gdk_rgba_parse(&preferences->settings->foreground, schemes[index][0]);
    gdk_rgba_parse(&preferences->settings->background, schemes[index][1]);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(control(preferences, "foreground")), &preferences->settings->foreground);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(control(preferences, "background")), &preferences->settings->background);
    preferences_changed(preferences);
}

static void palette_changed(GtkComboBox *combo_box, Preferences *preferences)
{
    if (preferences->updating) return;
    int index = gtk_combo_box_get_active(combo_box);
    if (index < 0 || index >= (int)G_N_ELEMENTS(palettes)) return;
    for (guint color_index = 0; color_index < 16; color_index++) {
        gdk_rgba_parse(&preferences->settings->palette[color_index], palettes[index][color_index]);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(preferences->palette[color_index]),
            &preferences->settings->palette[color_index]);
    }
    preferences_changed(preferences);
}

static void palette_color_changed(GtkColorButton *button, Preferences *preferences)
{
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "palette-index"));
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &preferences->settings->palette[index]);
    preferences_changed(preferences);
}

static GtkWidget *preset_combo(const char *const *names)
{
    GtkWidget *widget = gtk_combo_box_text_new();
    for (guint index = 0; names[index]; index++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), names[index]);
    return widget;
}

static GtkWidget *colors_page(Preferences *preferences)
{
    GtkWidget *grid = page_grid();
    heading(grid, 0, "Text and Background Color");
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "use-theme-colors", "Use colors from system theme"), 0, 1, 4, 1);
    preferences->scheme = preset_combo(scheme_names);
    gtk_widget_set_name(preferences->scheme, "color-scheme");
    row_control(grid, 2, "Built-in schemes:", preferences->scheme);
    g_signal_connect(preferences->scheme, "changed", G_CALLBACK(scheme_changed), preferences);
    row_control(grid, 3, "Default color:", paired(color_button(preferences, "foreground"), "Text",
        color_button(preferences, "background"), "Background"));
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "custom-bold", "Bold color:"), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), color_button(preferences, "bold"), 1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "custom-cursor", "Cursor color:"), 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), paired(color_button(preferences, "cursor-foreground"), "Text",
        color_button(preferences, "cursor"), "Background"), 1, 5, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "custom-highlight", "Highlight color:"), 0, 6, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), paired(color_button(preferences, "highlight-foreground"), "Text",
        color_button(preferences, "highlight"), "Background"), 1, 6, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "transparent", "Transparent background"), 0, 7, 4, 1);
    GtkWidget *opacity = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_scale_set_digits(GTK_SCALE(opacity), 2);
    gtk_range_set_value(GTK_RANGE(opacity), preferences->settings->background_opacity);
    bind_control(preferences, "background-opacity", opacity, "value-changed");
    gtk_widget_set_tooltip_text(opacity, "0 is transparent; 1 is opaque. Requires desktop compositing.");
    row_control(grid, 8, "Background opacity:", opacity);
    heading(grid, 9, "Palette");
    preferences->palette_scheme = preset_combo(palette_names);
    gtk_widget_set_name(preferences->palette_scheme, "palette-scheme");
    row_control(grid, 10, "Built-in schemes:", preferences->palette_scheme);
    g_signal_connect(preferences->palette_scheme, "changed", G_CALLBACK(palette_changed), preferences);
    GtkWidget *palette = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(palette), 4);
    gtk_grid_set_row_spacing(GTK_GRID(palette), 4);
    for (guint index = 0; index < 16; index++) {
        GtkWidget *button = gtk_color_button_new_with_rgba(&preferences->settings->palette[index]);
        preferences->palette[index] = button;
        g_autofree char *name = g_strdup_printf("palette-%u", index);
        gtk_widget_set_name(button, name);
        g_autofree char *title = g_strdup_printf("Palette color %u%s", index % 8 + 1, index >= 8 ? " (bright)" : "");
        gtk_color_button_set_title(GTK_COLOR_BUTTON(button), title);
        gtk_widget_set_tooltip_text(button, title);
        g_object_set_data(G_OBJECT(button), "palette-index", GUINT_TO_POINTER(index));
        gtk_grid_attach(GTK_GRID(palette), button, index % 8, index / 8, 1, 1);
        g_signal_connect(button, "color-set", G_CALLBACK(palette_color_changed), preferences);
    }
    row_control(grid, 11, "Color palette:", palette);
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "bold-is-bright", "Show bold text in bright colors"), 0, 12, 4, 1);
    return grid;
}

static GtkWidget *scrolling_page(Preferences *preferences)
{
    GtkWidget *grid = page_grid();
    const char *keys[] = {"show-scrollbar", "scroll-on-output", "scroll-on-keystroke", "scroll-on-paste"};
    const char *labels[] = {"Show scrollbar", "Scroll on output", "Scroll on keystroke", "Scroll on paste"};
    for (guint index = 0; index < G_N_ELEMENTS(keys); index++)
        gtk_grid_attach(GTK_GRID(grid), check(preferences, keys[index], labels[index]), 0, index, 4, 1);
    gtk_grid_attach(GTK_GRID(grid), check(preferences, "limit-scrollback", "Limit scrollback to:"), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), spin(preferences, "scrollback-lines"), 1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("lines"), 2, 4, 1, 1);
    GtkWidget *note = gtk_label_new("Uncheck the limit to keep unlimited scrollback. Longer histories use more memory.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), note, 0, 5, 4, 1);
    return grid;
}

static GtkWidget *scrolled_page(GtkWidget *child)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), child);
    return scroll;
}

static GtkWidget *shortcuts_page(GtkApplication *app)
{
    GtkWidget *grid = page_grid();
    heading(grid, 0, "Keyboard Shortcuts");
    const char *shortcuts[][2] = {
        {"New window", "new-window"}, {"Close window", "close-window"},
        {"Copy", "copy"}, {"Paste", "paste"}, {"Preferences", "preferences"},
        {"Find", "find"}, {"Find next", "find-next"}, {"Find previous", "find-previous"},
        {"Clear search", "find-clear"},
        {"Zoom in", "zoom-in"}, {"Zoom out", "zoom-out"}, {"Normal size", "zoom-normal"},
        {"Full screen", "fullscreen"}
    };
    for (guint index = 0; index < G_N_ELEMENTS(shortcuts); index++) {
        g_autofree char *action = g_strconcat("win.", shortcuts[index][1], NULL);
        g_auto(GStrv) accelerators = gtk_application_get_accels_for_action(app, action);
        guint key;
        GdkModifierType modifiers;
        gtk_accelerator_parse(accelerators[0] ? accelerators[0] : "", &key, &modifiers);
        g_autofree char *label = gtk_accelerator_get_label(key, modifiers);
        row_control(grid, index + 1, shortcuts[index][0], gtk_label_new(label));
    }
    return grid;
}

static void preferences_free(gpointer data)
{
    Preferences *preferences = data;
    g_clear_object(&preferences->desktop_interface);
    g_hash_table_unref(preferences->controls);
    g_free(preferences);
}

static void desktop_font_changed(GSettings *settings, const char *key, GtkWidget *window)
{
    (void)settings; (void)key;
    Preferences *preferences = g_object_get_data(G_OBJECT(window), "preferences");
    refresh(preferences);
}

GtkWidget *simpleterm_settings_window(GtkWindow *parent, SimpletermSettings *settings,
    GSettings *desktop_interface, SimpletermSettingsChanged changed, gpointer data)
{
    Preferences *preferences = g_new0(Preferences, 1);
    preferences->settings = settings;
    preferences->desktop_interface = desktop_interface ? g_object_ref(desktop_interface) : NULL;
    preferences->changed = changed;
    preferences->data = data;
    preferences->controls = g_hash_table_new(g_str_hash, g_str_equal);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    preferences->window = window;
    g_object_set_data_full(G_OBJECT(window), "preferences", preferences, preferences_free);
    if (desktop_interface)
        g_signal_connect_object(desktop_interface, "changed::monospace-font-name", G_CALLBACK(desktop_font_changed), window, 0);
    gtk_window_set_title(GTK_WINDOW(window), "Preferences — Simpleterm");
    gtk_window_set_default_size(GTK_WINDOW(window), 880, 680);
    gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Preferences — Simpleterm");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Default profile");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), layout);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(layout), body, TRUE, TRUE, 0);
    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_name(stack, "preferences-pages");
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    GtkWidget *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
    gtk_widget_set_size_request(sidebar, 150, -1);
    gtk_box_pack_start(GTK_BOX(body), sidebar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), stack, TRUE, TRUE, 0);
    GtkWidget *general = page_grid();
    heading(general, 0, "General");
    gtk_grid_attach(GTK_GRID(general), check(preferences, "show-menubar", "Show menubar in new windows"), 0, 1, 4, 1);
    GtkWidget *description = gtk_label_new("Text, colors, and scrolling settings apply to all terminals using the Default profile.");
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(description), 55);
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(general), description, 0, 2, 4, 1);
    gtk_stack_add_titled(GTK_STACK(stack), scrolled_page(general), "general", "General");
    gtk_stack_add_titled(GTK_STACK(stack), scrolled_page(shortcuts_page(gtk_window_get_application(parent))), "shortcuts", "Shortcuts");
    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_name(notebook, "profile-tabs");
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_page(text_page(preferences)), gtk_label_new("Text"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_page(colors_page(preferences)), gtk_label_new("Colors"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrolled_page(scrolling_page(preferences)), gtk_label_new("Scrolling"));
    gtk_stack_add_titled(GTK_STACK(stack), notebook, "profile", "Default Profile");
    preferences->status = gtk_label_new("Changes apply immediately and are saved automatically.");
    gtk_widget_set_name(preferences->status, "preferences-status");
    gtk_label_set_line_wrap(GTK_LABEL(preferences->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(preferences->status), 0);
    gtk_widget_set_margin_start(preferences->status, 18);
    gtk_widget_set_margin_end(preferences->status, 18);
    gtk_widget_set_margin_top(preferences->status, 10);
    gtk_widget_set_margin_bottom(preferences->status, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(preferences->status), "dim-label");
    gtk_box_pack_end(GTK_BOX(layout), preferences->status, FALSE, FALSE, 0);
    refresh(preferences);
    gtk_widget_show_all(window);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "profile");
    return window;
}
