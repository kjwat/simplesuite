#ifndef SIMPLETERM_SETTINGS_H
#define SIMPLETERM_SETTINGS_H

#include <gtk/gtk.h>

typedef struct {
    char *font;
    int columns, rows, text_blink, cursor_shape, cursor_blink, scrollback_lines;
    double cell_width, cell_height, background_opacity;
    gboolean custom_font, audible_bell, use_theme_colors, custom_bold;
    gboolean custom_cursor, custom_highlight, transparent, bold_is_bright;
    gboolean show_scrollbar, scroll_on_output, scroll_on_keystroke, scroll_on_paste;
    gboolean limit_scrollback, show_menubar;
    GdkRGBA foreground, background, bold, cursor, cursor_foreground;
    GdkRGBA highlight, highlight_foreground, palette[16];
} SimpletermSettings;

typedef void (*SimpletermSettingsChanged)(gpointer data);

void simpleterm_settings_defaults(SimpletermSettings *settings);
void simpleterm_settings_clear(SimpletermSettings *settings);
gboolean simpleterm_settings_load(SimpletermSettings *settings, GError **error);
gboolean simpleterm_settings_save(const SimpletermSettings *settings, GError **error);
GSettings *simpleterm_settings_desktop_interface(void);
char *simpleterm_settings_font(const SimpletermSettings *settings, GSettings *desktop_interface);
void simpleterm_settings_window_refresh(GtkWidget *window);
GtkWidget *simpleterm_settings_window(GtkWindow *parent, SimpletermSettings *settings,
    GSettings *desktop_interface, SimpletermSettingsChanged changed, gpointer data);

#endif
