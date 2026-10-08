/* Simpleterm: an optional GTK/VTE terminal for SimpleSuite. GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <vte/vte.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "simpleterm-settings.h"

#define VERSION "0.2.4"
#define APP_ID "org.simplesuite.Simpleterm"

typedef struct TerminalWindow TerminalWindow;
typedef struct {
    TerminalWindow *window;
    GtkWidget *view, *scrollbar;
    VteTerminal *terminal;
    GCancellable *spawn;
    GPid pid;
    char *directory, *title, **environment;
    gboolean closing;
} TerminalSession;

struct TerminalWindow {
    GtkWidget *widget, *content, *menubar, *searchbar, *search, *search_status;
    GtkWidget *match_case, *regex;
    TerminalSession *session;
    char *link;
    gboolean closing, fullscreen, fullscreen_chord;
};

typedef struct {
    char *directory, *title, **command;
    int columns, rows;
    double zoom;
    gboolean fullscreen, maximize, help, version, preferences;
    int menubar;
} Options;

static GtkApplication *application;
static SimpletermSettings settings;
static GSettings *desktop_interface;
static GtkWidget *preferences_window;

static TerminalSession *new_session(TerminalWindow *, const Options *, char **);
static TerminalWindow *new_window(void);
static void update_window(TerminalWindow *);
static void search_changed(GtkWidget *, TerminalWindow *);
static void request_close(TerminalWindow *);

static void update_window_transparency(TerminalWindow *window)
{
    GtkStyleContext *style = gtk_widget_get_style_context(window->widget);
    gboolean transparent = settings.transparent && settings.background_opacity < 1;
    if (gtk_style_context_has_class(style, "simpleterm-transparent") == transparent) return;
    if (transparent) gtk_style_context_add_class(style, "simpleterm-transparent");
    else gtk_style_context_remove_class(style, "simpleterm-transparent");
    /* GTK updates the compositor's opaque region when the window is allocated. */
    gtk_widget_queue_resize(window->widget);
}

static void configure_terminal(TerminalSession *session)
{
    update_window_transparency(session->window);
    VteTerminal *vte = session->terminal;
    g_autofree char *font_name = simpleterm_settings_font(&settings, desktop_interface);
    PangoFontDescription *font = pango_font_description_from_string(font_name);
    vte_terminal_set_font(vte, font);
    pango_font_description_free(font);
    vte_terminal_set_cell_width_scale(vte, settings.cell_width);
    vte_terminal_set_cell_height_scale(vte, settings.cell_height);
    GdkRGBA foreground = settings.foreground, background = settings.background;
    if (settings.use_theme_colors) {
        GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(vte));
        gtk_style_context_lookup_color(style, "theme_fg_color", &foreground);
        gtk_style_context_lookup_color(style, "theme_base_color", &background);
    }
    background.alpha = settings.transparent ? settings.background_opacity : 1;
    vte_terminal_set_colors(vte, &foreground, &background, settings.palette, 16);
    vte_terminal_set_color_bold(vte, settings.custom_bold ? &settings.bold : NULL);
    vte_terminal_set_color_cursor(vte, settings.custom_cursor ? &settings.cursor : NULL);
    vte_terminal_set_color_cursor_foreground(vte, settings.custom_cursor ? &settings.cursor_foreground : NULL);
    vte_terminal_set_color_highlight(vte, settings.custom_highlight ? &settings.highlight : NULL);
    vte_terminal_set_color_highlight_foreground(vte, settings.custom_highlight ? &settings.highlight_foreground : NULL);
    vte_terminal_set_scrollback_lines(vte, settings.limit_scrollback ? settings.scrollback_lines : -1);
    vte_terminal_set_scroll_on_output(vte, settings.scroll_on_output);
    vte_terminal_set_scroll_on_keystroke(vte, settings.scroll_on_keystroke);
    vte_terminal_set_scroll_on_insert(vte, settings.scroll_on_paste);
    vte_terminal_set_audible_bell(vte, settings.audible_bell);
    vte_terminal_set_bold_is_bright(vte, settings.bold_is_bright);
    gtk_widget_set_visible(session->scrollbar, settings.show_scrollbar);
    vte_terminal_set_allow_hyperlink(vte, TRUE);
    vte_terminal_set_mouse_autohide(vte, TRUE);
    vte_terminal_set_enable_bidi(vte, TRUE);
    vte_terminal_set_enable_shaping(vte, TRUE);
    vte_terminal_set_enable_sixel(vte, FALSE);
    vte_terminal_set_cjk_ambiguous_width(vte, 1);
    vte_terminal_set_cursor_shape(vte, settings.cursor_shape);
    vte_terminal_set_cursor_blink_mode(vte, settings.cursor_blink);
    vte_terminal_set_text_blink_mode(vte, settings.text_blink);
    vte_terminal_set_backspace_binding(vte, VTE_ERASE_ASCII_DELETE);
    vte_terminal_set_delete_binding(vte, VTE_ERASE_DELETE_SEQUENCE);
}

static void set_enabled(TerminalWindow *window, const char *name, gboolean enabled)
{
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window->widget), name);
    if (action) g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

static void set_state(TerminalWindow *window, const char *name, gboolean state)
{
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window->widget), name);
    g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(state));
}

static const char *session_title(TerminalSession *session)
{
    const char *title = session->title;
    if (!title) {
#if VTE_CHECK_VERSION(0, 78, 0)
        title = vte_terminal_get_termprop_string(session->terminal, VTE_TERMPROP_XTERM_TITLE, NULL);
#else
        title = vte_terminal_get_window_title(session->terminal);
#endif
    }
    return title && *title ? title : "Terminal";
}

static void update_window(TerminalWindow *window)
{
    if (window->closing) return;
    TerminalSession *session = window->session;
    if (!session) return;
    g_autofree char *title = g_strdup_printf("%s — Simpleterm", session_title(session));
    gtk_window_set_title(GTK_WINDOW(window->widget), title);
    set_enabled(window, "copy", vte_terminal_get_has_selection(session->terminal));
    set_enabled(window, "copy-html", vte_terminal_get_has_selection(session->terminal));
    set_enabled(window, "paste", vte_terminal_get_input_enabled(session->terminal));
    set_state(window, "read-only", !vte_terminal_get_input_enabled(session->terminal));
    set_state(window, "menubar", gtk_widget_get_visible(window->menubar));
}

static void title_changed(VteTerminal *terminal, TerminalSession *session)
{
    (void)terminal;
    if (session->closing) return;
    update_window(session->window);
}

static void selection_changed(VteTerminal *terminal, TerminalSession *session)
{
    (void)terminal;
    if (!session->closing) update_window(session->window);
}

static char *session_directory(TerminalSession *session)
{
    if (!session) return g_strdup(g_get_home_dir());
#if VTE_CHECK_VERSION(0, 78, 0)
    g_autoptr(GUri) location = vte_terminal_ref_termprop_uri(session->terminal, VTE_TERMPROP_CURRENT_DIRECTORY_URI);
    g_autofree char *uri = location ? g_uri_to_string(location) : NULL;
#else
    const char *uri = vte_terminal_get_current_directory_uri(session->terminal);
#endif
    if (uri) {
        g_autofree char *hostname = NULL;
        char *path = g_filename_from_uri(uri, &hostname, NULL);
        if (path && (!hostname || !*hostname || g_str_equal(hostname, "localhost") ||
                     g_ascii_strcasecmp(hostname, g_get_host_name()) == 0) &&
            g_file_test(path, G_FILE_TEST_IS_DIR)) return path;
        g_free(path);
    }
#ifdef __linux__
    if (session->pid > 0) {
        g_autofree char *proc = g_strdup_printf("/proc/%d/cwd", (int)session->pid);
        char *path = g_file_read_link(proc, NULL);
        if (path && g_file_test(path, G_FILE_TEST_IS_DIR)) return path;
        g_free(path);
    }
#endif
    return g_strdup(session->directory);
}

static void session_destroyed(GtkWidget *view, TerminalSession *session)
{
    (void)view;
    session->closing = TRUE;
    session->window->session = NULL;
    g_cancellable_cancel(session->spawn);
}

static void session_free(gpointer data)
{
    TerminalSession *session = data;
    g_object_unref(session->spawn);
    g_strfreev(session->environment);
    g_free(session->directory);
    g_free(session->title);
    g_free(session);
}

static void window_destroyed(GtkWidget *widget, TerminalWindow *window)
{
    (void)widget;
    window->closing = TRUE;
}

static void window_free(gpointer data)
{
    TerminalWindow *window = data;
    g_free(window->link);
    g_free(window);
}

static void request_close(TerminalWindow *window)
{
    gtk_widget_destroy(window->widget);
}

static gboolean window_delete(GtkWidget *widget, GdkEvent *event, TerminalWindow *window)
{
    (void)widget; (void)event;
    request_close(window);
    return TRUE;
}

static void child_exited(VteTerminal *terminal, int status, TerminalSession *session)
{
    (void)terminal; (void)status;
    session->pid = 0;
    if (!session->closing && !session->window->closing) request_close(session->window);
}

static void spawn_finished(VteTerminal *terminal, GPid pid, GError *error, gpointer data)
{
    GWeakRef *ref = data;
    g_autoptr(GObject) object = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!object || !terminal) return;
    TerminalSession *session = g_object_get_data(object, "session");
    if (session->closing) return;
    if (error) {
        g_autofree char *message = g_strdup_printf(
            "Simpleterm could not start the command:\r\n%s\r\n\r\nClose this window or open a new terminal.\r\n",
            error->message);
        vte_terminal_feed(terminal, message, -1);
        vte_terminal_set_input_enabled(terminal, FALSE);
        update_window(session->window);
    } else session->pid = pid;
}

static void spawn_session(TerminalSession *session, char **command)
{
    g_auto(GStrv) environment = g_strdupv(session->environment);
    static const char *remove[] = {"COLUMNS", "LINES", "WINDOWID", "GNOME_TERMINAL_SCREEN", "GNOME_TERMINAL_SERVICE"};
    for (guint i = 0; i < G_N_ELEMENTS(remove); i++) environment = g_environ_unsetenv(environment, remove[i]);
    environment = g_environ_setenv(environment, "TERM", "xterm-256color", TRUE);
    environment = g_environ_setenv(environment, "COLORTERM", "truecolor", TRUE);
    environment = g_environ_setenv(environment, "TERM_PROGRAM", "simpleterm", TRUE);
    environment = g_environ_setenv(environment, "TERM_PROGRAM_VERSION", VERSION, TRUE);
    environment = g_environ_setenv(environment, "PWD", session->directory, TRUE);
    const char *shell = g_environ_getenv(environment, "SHELL");
    if (!shell || !*shell) shell = vte_get_user_shell();
    if (!shell || !*shell) shell = "/bin/sh";
    g_autofree char *basename = g_path_get_basename(shell);
    g_autofree char *argv0 = g_strdup(basename);
    char *shell_command[] = {(char *)shell, argv0, NULL};
    GSpawnFlags flags = G_SPAWN_SEARCH_PATH_FROM_ENVP | VTE_SPAWN_NO_PARENT_ENVV;
    if (!command) flags |= G_SPAWN_FILE_AND_ARGV_ZERO;
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, session->view);
    vte_terminal_spawn_async(session->terminal, VTE_PTY_DEFAULT, session->directory,
        command ? command : shell_command, environment, flags, NULL, NULL, NULL,
        -1, session->spawn, spawn_finished, ref);
}

static GMenu *menu_section(GMenu *menu)
{
    GMenu *section = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    return section;
}

static GMenu *submenu(GMenu *menu, const char *label)
{
    GMenu *child = g_menu_new();
    g_menu_append_submenu(menu, label, G_MENU_MODEL(child));
    g_object_unref(child);
    return child;
}

static char *link_at(VteTerminal *terminal, GdkEvent *event)
{
    if (!event) return NULL;
    char *link = vte_terminal_hyperlink_check_event(terminal, event);
    if (!link) link = vte_terminal_match_check_event(terminal, event, NULL);
    return link;
}

static void open_link(TerminalWindow *window, const char *link)
{
    if (!link) return;
    g_autofree char *uri = g_str_has_prefix(link, "www.") ? g_strconcat("https://", link, NULL)
        : !g_uri_peek_scheme(link) && strchr(link, '@') ? g_strconcat("mailto:", link, NULL)
        : g_strdup(link);
    g_autoptr(GError) error = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(window->widget), uri, GDK_CURRENT_TIME, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window->widget),
            GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not open the link");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", error->message);
        g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_widget_show(dialog);
    }
}

static gboolean terminal_button(GtkWidget *widget, GdkEventButton *event, TerminalSession *session)
{
    if ((event->button == 1 || event->button == 2) && (event->state & GDK_CONTROL_MASK)) {
        g_autofree char *link = link_at(VTE_TERMINAL(widget), (GdkEvent *)event);
        if (link) { open_link(session->window, link); return TRUE; }
    }
    /* VTE owns selection, PRIMARY paste, and mouse reporting to terminal apps. */
    return FALSE;
}

static gboolean terminal_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)data;
    GdkModifierType modifiers = event->state & gtk_accelerator_get_default_mod_mask();
    if (gdk_keyval_to_lower(event->keyval) != GDK_KEY_c ||
        modifiers != (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) return FALSE;
    VteTerminal *terminal = VTE_TERMINAL(widget);
    if (vte_terminal_get_has_selection(terminal)) {
        vte_terminal_copy_clipboard_format(terminal, VTE_FORMAT_TEXT);
    } else if (vte_terminal_get_input_enabled(terminal)) {
        VtePty *pty = vte_terminal_get_pty(terminal);
        struct termios mode;
        /* A disabled Copy accelerator otherwise reaches VTE as Ctrl-C.
         * Preserve Shift for raw-mode apps; copying nothing at a shell prompt
         * must not interrupt a command or insert an escape sequence. */
        if (pty && tcgetattr(vte_pty_get_fd(pty), &mode) == 0 &&
            !(mode.c_lflag & (ICANON | ISIG)))
            vte_terminal_feed_child(terminal, "\033[99;6u", -1);
    }
    return TRUE;
}

static void setup_context(VteTerminal *terminal, const VteEventContext *context, TerminalSession *session)
{
    if (!context || session->closing) return;
    TerminalWindow *window = session->window;
    gtk_widget_grab_focus(GTK_WIDGET(terminal));
    g_free(window->link);
    window->link = link_at(terminal, vte_event_context_get_event(context));
    update_window(window);
    GMenu *menu = g_menu_new();
    GMenu *section;
    if (window->link) {
        section = menu_section(menu);
        g_menu_append(section, "_Open Link", "win.open-link");
        g_menu_append(section, "Copy Link _Address", "win.copy-link");
    }
    section = menu_section(menu);
    g_menu_append(section, "_Copy", "win.copy");
    g_menu_append(section, "Copy as _HTML", "win.copy-html");
    g_menu_append(section, "_Paste", "win.paste");
    g_menu_append(section, "Select _All", "win.select-all");
    section = menu_section(menu);
    g_menu_append(section, "_Read-Only", "win.read-only");
    section = menu_section(menu);
    g_menu_append(section, "New _Window", "win.new-window");
    section = menu_section(menu);
    g_menu_append(section, "Show _Menubar", "win.menubar");
    g_menu_append(section, "_Full Screen", "win.fullscreen");
    g_menu_append(section, "_Preferences…", "win.preferences");
    section = menu_section(menu);
    g_menu_append(section, "C_lose Terminal", "win.close-window");
    vte_terminal_set_context_menu_model(terminal, G_MENU_MODEL(menu));
    g_object_unref(menu);
}

static void search_step(TerminalWindow *window, gboolean previous)
{
    TerminalSession *session = window->session;
    if (!session || !vte_terminal_search_get_regex(session->terminal)) return;
    gboolean found = previous ? vte_terminal_search_find_previous(session->terminal)
        : vte_terminal_search_find_next(session->terminal);
    gtk_label_set_text(GTK_LABEL(window->search_status), found ? "" : "No matches");
}

static void search_changed(GtkWidget *widget, TerminalWindow *window)
{
    (void)widget;
    TerminalSession *session = window->session;
    if (!session || window->closing) return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(window->search));
    g_autofree char *pattern = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window->regex))
        ? g_strdup(text) : g_regex_escape_string(text, -1);
    guint32 flags = PCRE2_MULTILINE | PCRE2_UTF | PCRE2_UCP;
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(window->match_case))) flags |= PCRE2_CASELESS;
    g_autoptr(GError) error = NULL;
    VteRegex *regex = *text ? vte_regex_new_for_search(pattern, -1, flags, &error) : NULL;
    vte_terminal_search_set_regex(session->terminal, regex, 0);
    vte_terminal_search_set_wrap_around(session->terminal, TRUE);
    if (regex) vte_regex_unref(regex);
    gtk_label_set_text(GTK_LABEL(window->search_status), error ? "Invalid expression" : "");
    gtk_widget_set_tooltip_text(window->search_status, error ? error->message : NULL);
    if (!error && *text) search_step(window, FALSE);
}

static void search_stop(GtkSearchEntry *entry, TerminalWindow *window)
{
    (void)entry;
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(window->searchbar), FALSE);
    TerminalSession *session = window->session;
    if (session) gtk_widget_grab_focus(GTK_WIDGET(session->terminal));
}

static void search_mode_changed(GObject *bar, GParamSpec *spec, TerminalWindow *window)
{
    (void)spec;
    if (window->closing || gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar))) return;
    TerminalSession *session = window->session;
    if (session) gtk_widget_grab_focus(GTK_WIDGET(session->terminal));
}

static void search_next(GtkWidget *widget, TerminalWindow *window)
{
    (void)widget; search_step(window, FALSE);
}

static void search_previous(GtkWidget *widget, TerminalWindow *window)
{
    (void)widget; search_step(window, TRUE);
}

static void resize_grid(TerminalWindow *window, TerminalSession *session, int columns, int rows)
{
    int width = (int)vte_terminal_get_char_width(session->terminal);
    int height = (int)vte_terminal_get_char_height(session->terminal);
    GtkBorder padding;
    gtk_style_context_get_padding(gtk_widget_get_style_context(GTK_WIDGET(session->terminal)),
        GTK_STATE_FLAG_NORMAL, &padding);
    GtkRequisition min, natural;
    gtk_widget_get_preferred_size(window->menubar, &min, &natural);
    int chrome_height = gtk_widget_get_visible(window->menubar) ? natural.height : 0;
    gtk_widget_get_preferred_size(session->scrollbar, &min, &natural);
    int chrome_width = gtk_widget_get_visible(session->scrollbar) ? natural.width : 0;
    GdkGeometry geometry = {0};
    geometry.base_width = padding.left + padding.right + chrome_width;
    geometry.base_height = padding.top + padding.bottom + chrome_height;
    geometry.width_inc = MAX(width, 1);
    geometry.height_inc = MAX(height, 1);
    geometry.min_width = geometry.base_width + width * 16;
    geometry.min_height = geometry.base_height + height * 2;
    gtk_window_set_geometry_hints(GTK_WINDOW(window->widget), NULL, &geometry,
        GDK_HINT_RESIZE_INC | GDK_HINT_MIN_SIZE | GDK_HINT_BASE_SIZE);
    gtk_window_resize(GTK_WINDOW(window->widget), geometry.base_width + width * columns,
        geometry.base_height + height * rows);
}

static void apply_settings(gpointer data)
{
    (void)data;
    for (GList *item = gtk_application_get_windows(application); item; item = item->next) {
        TerminalWindow *window = g_object_get_data(G_OBJECT(item->data), "window");
        if (!window || window->closing) continue;
        TerminalSession *active = window->session;
        if (!active) continue;
        int columns = vte_terminal_get_column_count(active->terminal);
        int rows = vte_terminal_get_row_count(active->terminal);
        glong width = vte_terminal_get_char_width(active->terminal);
        glong height = vte_terminal_get_char_height(active->terminal);
        gboolean scrollbar = gtk_widget_get_visible(active->scrollbar);
        configure_terminal(active);
        gboolean resized = width != vte_terminal_get_char_width(active->terminal) ||
            height != vte_terminal_get_char_height(active->terminal) ||
            scrollbar != gtk_widget_get_visible(active->scrollbar);
        if (resized && !window->fullscreen && !gtk_window_is_maximized(GTK_WINDOW(window->widget)))
            resize_grid(window, active, columns, rows);
    }
}

static void preferences_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    preferences_window = NULL;
}

static void desktop_font_changed(GSettings *desktop_settings, const char *key, gpointer data)
{
    (void)desktop_settings; (void)key; (void)data;
    if (!settings.custom_font) apply_settings(NULL);
}

static void show_preferences(TerminalWindow *window)
{
    if (!preferences_window) {
        preferences_window = simpleterm_settings_window(GTK_WINDOW(window->widget), &settings,
            desktop_interface, apply_settings, NULL);
        g_signal_connect(preferences_window, "destroy", G_CALLBACK(preferences_destroyed), NULL);
    } else gtk_window_set_transient_for(GTK_WINDOW(preferences_window), GTK_WINDOW(window->widget));
    gtk_window_present(GTK_WINDOW(preferences_window));
}

static TerminalSession *new_session(TerminalWindow *window, const Options *options, char **environment)
{
    g_assert_null(window->session);
    TerminalSession *session = g_new0(TerminalSession, 1);
    session->window = window;
    window->session = session;
    session->directory = g_strdup(options->directory ? options->directory : g_get_home_dir());
    session->title = g_strdup(options->title);
    session->environment = environment ? g_strdupv(environment) : g_get_environ();
    session->spawn = g_cancellable_new();
    session->view = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    session->terminal = VTE_TERMINAL(vte_terminal_new());
    gtk_widget_set_hexpand(GTK_WIDGET(session->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(session->terminal), TRUE);
    gtk_box_pack_start(GTK_BOX(session->view), GTK_WIDGET(session->terminal), TRUE, TRUE, 0);
    session->scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
        gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(session->terminal)));
    gtk_box_pack_start(GTK_BOX(session->view), session->scrollbar, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(session->scrollbar, TRUE);
    configure_terminal(session);
    vte_terminal_set_font_scale(session->terminal, options->zoom > 0 ? options->zoom : 1.0);
    int columns = options->columns ? options->columns : settings.columns;
    int rows = options->rows ? options->rows : settings.rows;
    vte_terminal_set_size(session->terminal, columns, rows);
    const char *patterns[] = {
        "(?:https?|ftp|file)://[^\\s<>\"']*[^\\s<>\"'.,;:!?)]",
        "www\\.[^\\s<>\"']*[^\\s<>\"'.,;:!?)]",
        "(?:mailto:)?[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"
    };
    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        VteRegex *regex = vte_regex_new_for_match(patterns[i], -1, PCRE2_MULTILINE | PCRE2_CASELESS | PCRE2_UTF, NULL);
        if (regex) {
            int tag = vte_terminal_match_add_regex(session->terminal, regex, 0);
            vte_terminal_match_set_cursor_name(session->terminal, tag, "pointer");
            vte_regex_unref(regex);
        }
    }
    g_object_set_data_full(G_OBJECT(session->view), "session", session, session_free);
    g_signal_connect(session->view, "destroy", G_CALLBACK(session_destroyed), session);
    g_signal_connect(session->terminal, "child-exited", G_CALLBACK(child_exited), session);
    g_signal_connect(session->terminal, "window-title-changed", G_CALLBACK(title_changed), session);
    g_signal_connect(session->terminal, "selection-changed", G_CALLBACK(selection_changed), session);
    g_signal_connect(session->terminal, "button-press-event", G_CALLBACK(terminal_button), session);
    g_signal_connect(session->terminal, "key-press-event", G_CALLBACK(terminal_key), NULL);
    g_signal_connect(session->terminal, "setup-context-menu", G_CALLBACK(setup_context), session);
    gtk_box_pack_start(GTK_BOX(window->content), session->view, TRUE, TRUE, 0);
    gtk_widget_show_all(session->view);
    update_window(window);
    gtk_widget_grab_focus(GTK_WIDGET(session->terminal));
    resize_grid(window, session, columns, rows);
    spawn_session(session, options->command);
    return session;
}

static void action_activate(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void)parameter;
    TerminalWindow *window = data;
    TerminalSession *session = window->session;
    if (!session) return;
    VteTerminal *vte = session->terminal;
    const char *name = g_action_get_name(G_ACTION(action));
    if (g_str_equal(name, "preferences")) show_preferences(window);
    else if (g_str_equal(name, "new-window")) {
        g_autofree char *directory = session_directory(session);
        Options options = {.directory = directory, .zoom = vte_terminal_get_font_scale(vte)};
        TerminalWindow *target = new_window();
        new_session(target, &options, session->environment);
        gtk_window_present(GTK_WINDOW(target->widget));
    } else if (g_str_equal(name, "close-window")) request_close(window);
    else if (g_str_equal(name, "copy")) vte_terminal_copy_clipboard_format(vte, VTE_FORMAT_TEXT);
    else if (g_str_equal(name, "copy-html")) vte_terminal_copy_clipboard_format(vte, VTE_FORMAT_HTML);
    else if (g_str_equal(name, "paste")) vte_terminal_paste_clipboard(vte);
    else if (g_str_equal(name, "select-all")) vte_terminal_select_all(vte);
    else if (g_str_equal(name, "reset")) vte_terminal_reset(vte, TRUE, FALSE);
    else if (g_str_equal(name, "reset-clear")) vte_terminal_reset(vte, TRUE, TRUE);
    else if (g_str_equal(name, "read-only")) {
        vte_terminal_set_input_enabled(vte, !vte_terminal_get_input_enabled(vte));
        update_window(window);
    } else if (g_str_equal(name, "menubar")) {
        settings.show_menubar = !gtk_widget_get_visible(window->menubar);
        gtk_widget_set_visible(window->menubar, settings.show_menubar);
        if (preferences_window) simpleterm_settings_window_refresh(preferences_window);
        g_autoptr(GError) error = NULL;
        if (!simpleterm_settings_save(&settings, &error)) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window->widget),
                GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                "Could not save the menubar preference");
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", error->message);
            g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
            gtk_widget_show(dialog);
        }
        update_window(window);
    } else if (g_str_equal(name, "fullscreen")) {
        if (window->fullscreen) gtk_window_unfullscreen(GTK_WINDOW(window->widget));
        else gtk_window_fullscreen(GTK_WINDOW(window->widget));
    } else if (g_str_has_prefix(name, "zoom-")) {
        double scale = g_str_equal(name, "zoom-normal") ? 1.0
            : vte_terminal_get_font_scale(vte) * (g_str_equal(name, "zoom-in") ? 1.2 : 1.0 / 1.2);
        int columns = (int)vte_terminal_get_column_count(vte), rows = (int)vte_terminal_get_row_count(vte);
        vte_terminal_set_font_scale(vte, CLAMP(scale, 0.5, 3.0));
        if (!window->fullscreen && !gtk_window_is_maximized(GTK_WINDOW(window->widget)))
            resize_grid(window, session, columns, rows);
    } else if (g_str_equal(name, "find")) {
        /* Key releases may arrive before the search revealer's first frame. */
        gtk_widget_realize(window->search);
        gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(window->searchbar), TRUE);
        gtk_widget_grab_focus(window->search);
    } else if (g_str_equal(name, "find-next")) search_step(window, FALSE);
    else if (g_str_equal(name, "find-previous")) search_step(window, TRUE);
    else if (g_str_equal(name, "find-clear")) {
        gtk_entry_set_text(GTK_ENTRY(window->search), "");
        vte_terminal_search_set_regex(vte, NULL, 0);
        vte_terminal_unselect_all(vte);
        search_stop(NULL, window);
    } else if (g_str_equal(name, "open-link")) open_link(window, window->link);
    else if (g_str_equal(name, "copy-link") && window->link)
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), window->link, -1);
    else if (g_str_equal(name, "about"))
        gtk_show_about_dialog(GTK_WINDOW(window->widget), "program-name", "Simpleterm",
            "version", VERSION, "comments", "A straightforward GTK terminal for SimpleSuite.",
            "logo-icon-name", "utilities-terminal", "license-type", GTK_LICENSE_GPL_3_0, NULL);
}

static gboolean window_state(GtkWidget *widget, GdkEventWindowState *event, TerminalWindow *window)
{
    (void)widget;
    window->fullscreen = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
    set_state(window, "fullscreen", window->fullscreen);
    return FALSE;
}

static gboolean fullscreen_key(GtkWidget *widget, GdkEventKey *event, TerminalWindow *window)
{
    GdkModifierType pressed;
    switch (event->keyval) {
    case GDK_KEY_Super_L: case GDK_KEY_Super_R: pressed = GDK_SUPER_MASK; break;
    case GDK_KEY_Control_L: case GDK_KEY_Control_R: pressed = GDK_CONTROL_MASK; break;
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: pressed = GDK_SHIFT_MASK; break;
    default: return FALSE;
    }
    if (event->type == GDK_KEY_RELEASE) {
        gboolean consumed = window->fullscreen_chord;
        window->fullscreen_chord = FALSE;
        return consumed;
    }
    GdkModifierType modifiers = event->state;
    gdk_keymap_add_virtual_modifiers(gdk_keymap_get_for_display(gtk_widget_get_display(widget)), &modifiers);
    /* A physical Super key can also map to Hyper on X11. Ignore that alias. */
    modifiers = (modifiers & (GDK_SUPER_MASK | GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK)) | pressed;
    if (modifiers != (GDK_SUPER_MASK | GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        window->fullscreen_chord = FALSE;
        return FALSE;
    }
    /* Modifier-only shortcut: trigger once, then consume repeats until release. */
    if (!window->fullscreen_chord) {
        window->fullscreen_chord = TRUE;
        g_action_group_activate_action(G_ACTION_GROUP(widget), "fullscreen", NULL);
    }
    return TRUE;
}

static TerminalWindow *new_window(void)
{
    TerminalWindow *window = g_new0(TerminalWindow, 1);
    window->widget = gtk_application_window_new(application);
    g_object_set_data_full(G_OBJECT(window->widget), "window", window, window_free);
    gtk_window_set_title(GTK_WINDOW(window->widget), "Simpleterm");
    gtk_window_set_icon_name(GTK_WINDOW(window->widget), "utilities-terminal");
    GdkVisual *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window->widget));
    if (visual) gtk_widget_set_visual(window->widget, visual);
    gtk_style_context_add_class(gtk_widget_get_style_context(window->widget), "simpleterm-window");
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(window->widget), FALSE);
    g_signal_connect(window->widget, "destroy", G_CALLBACK(window_destroyed), window);
    g_signal_connect(window->widget, "delete-event", G_CALLBACK(window_delete), window);
    g_signal_connect(window->widget, "window-state-event", G_CALLBACK(window_state), window);
    g_signal_connect(window->widget, "key-press-event", G_CALLBACK(fullscreen_key), window);
    g_signal_connect(window->widget, "key-release-event", G_CALLBACK(fullscreen_key), window);
    static const char *actions[] = {"new-window", "close-window", "copy", "copy-html",
        "paste", "select-all", "reset", "reset-clear", "zoom-in", "zoom-out", "zoom-normal", "find",
        "find-next", "find-previous", "find-clear", "open-link", "copy-link", "about", "read-only", "menubar", "fullscreen", "preferences"};
    for (guint i = 0; i < G_N_ELEMENTS(actions); i++) {
        gboolean toggle = g_str_equal(actions[i], "read-only") || g_str_equal(actions[i], "menubar") ||
            g_str_equal(actions[i], "fullscreen");
        GSimpleAction *action = toggle ? g_simple_action_new_stateful(actions[i], NULL, g_variant_new_boolean(FALSE))
            : g_simple_action_new(actions[i], NULL);
        g_signal_connect(action, "activate", G_CALLBACK(action_activate), window);
        g_action_map_add_action(G_ACTION_MAP(window->widget), G_ACTION(action));
        g_object_unref(action);
    }
    GMenu *menu = g_menu_new(), *child, *section;
    child = submenu(menu, "_File");
    section = menu_section(child);
    g_menu_append(section, "New _Window", "win.new-window");
    section = menu_section(child);
    g_menu_append(section, "_Close Terminal", "win.close-window");
    child = submenu(menu, "_Edit");
    g_menu_append(child, "_Copy", "win.copy");
    g_menu_append(child, "Copy as _HTML", "win.copy-html");
    g_menu_append(child, "_Paste", "win.paste");
    g_menu_append(child, "Select _All", "win.select-all");
    section = menu_section(child);
    g_menu_append(section, "_Preferences…", "win.preferences");
    child = submenu(menu, "_View");
    section = menu_section(child);
    g_menu_append(section, "Show _Menubar", "win.menubar");
    g_menu_append(section, "_Full Screen", "win.fullscreen");
    section = menu_section(child);
    g_menu_append(section, "Zoom _In", "win.zoom-in");
    g_menu_append(section, "Zoom _Out", "win.zoom-out");
    g_menu_append(section, "_Normal Size", "win.zoom-normal");
    child = submenu(menu, "_Search");
    g_menu_append(child, "_Find…", "win.find");
    g_menu_append(child, "Find _Next", "win.find-next");
    g_menu_append(child, "Find _Previous", "win.find-previous");
    g_menu_append(child, "_Clear Highlight", "win.find-clear");
    child = submenu(menu, "_Terminal");
    g_menu_append(child, "_Read-Only", "win.read-only");
    g_menu_append(child, "_Reset", "win.reset");
    g_menu_append(child, "Reset and C_lear", "win.reset-clear");
    child = submenu(menu, "_Help");
    g_menu_append(child, "_About Simpleterm", "win.about");
    window->menubar = gtk_menu_bar_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    window->content = box;
    gtk_container_add(GTK_CONTAINER(window->widget), box);
    gtk_box_pack_start(GTK_BOX(box), window->menubar, FALSE, FALSE, 0);
    window->searchbar = gtk_search_bar_new();
    gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(window->searchbar), TRUE);
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    window->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(window->search), "Find in terminal");
    gtk_entry_set_width_chars(GTK_ENTRY(window->search), 24);
    gtk_box_pack_start(GTK_BOX(search_box), window->search, TRUE, TRUE, 0);
    GtkWidget *previous = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *next = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(previous, "Previous match");
    gtk_widget_set_tooltip_text(next, "Next match");
    gtk_box_pack_start(GTK_BOX(search_box), previous, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), next, FALSE, FALSE, 0);
    window->match_case = gtk_toggle_button_new_with_label("Aa");
    window->regex = gtk_toggle_button_new_with_label(".*");
    gtk_widget_set_tooltip_text(window->match_case, "Match case");
    gtk_widget_set_tooltip_text(window->regex, "Regular expression");
    gtk_box_pack_start(GTK_BOX(search_box), window->match_case, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), window->regex, FALSE, FALSE, 0);
    window->search_status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(search_box), window->search_status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(window->searchbar), search_box);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(window->searchbar), GTK_ENTRY(window->search));
    gtk_box_pack_start(GTK_BOX(box), window->searchbar, FALSE, FALSE, 0);
    g_signal_connect(window->search, "search-changed", G_CALLBACK(search_changed), window);
    g_signal_connect(window->search, "activate", G_CALLBACK(search_next), window);
    g_signal_connect(window->search, "stop-search", G_CALLBACK(search_stop), window);
    g_signal_connect(window->searchbar, "notify::search-mode-enabled", G_CALLBACK(search_mode_changed), window);
    g_signal_connect(previous, "clicked", G_CALLBACK(search_previous), window);
    g_signal_connect(next, "clicked", G_CALLBACK(search_next), window);
    g_signal_connect(window->match_case, "toggled", G_CALLBACK(search_changed), window);
    g_signal_connect(window->regex, "toggled", G_CALLBACK(search_changed), window);
    gtk_widget_show_all(window->widget);
    gtk_widget_set_visible(window->menubar, settings.show_menubar);
    return window;
}

static void startup(GApplication *app, gpointer data)
{
    (void)data;
    g_autoptr(GError) error = NULL;
    if (!simpleterm_settings_load(&settings, &error))
        g_printerr("Simpleterm could not load preferences: %s. Using defaults.\n", error->message);
    desktop_interface = simpleterm_settings_desktop_interface();
    if (desktop_interface)
        g_signal_connect(desktop_interface, "changed::monospace-font-name", G_CALLBACK(desktop_font_changed), NULL);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".simpleterm-window.simpleterm-transparent, "
        ".simpleterm-window.simpleterm-transparent notebook, "
        ".simpleterm-window.simpleterm-transparent notebook > stack { background-color: transparent; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    /* Keep Alt-letter keystrokes available to the shell, as GNOME Terminal does. */
    g_object_set(gtk_settings_get_default(), "gtk-enable-mnemonics", FALSE, NULL);
    static const char *shortcuts[][2] = {
        {"new-window", "<Primary><Shift>n"}, {"close-window", "<Primary><Shift>w"},
        {"copy", "<Primary><Shift>c"}, {"paste", "<Primary><Shift>v"},
        {"find", "<Primary><Shift>f"}, {"find-next", "<Primary><Shift>g"},
        {"find-previous", "<Primary><Shift>h"}, {"find-clear", "<Primary><Shift>j"},
        {"fullscreen", "F11"}, {"zoom-in", "<Primary>plus"},
        {"zoom-out", "<Primary>minus"}, {"zoom-normal", "<Primary>0"},
        {"preferences", "<Primary>comma"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        g_autofree char *action = g_strconcat("win.", shortcuts[i][0], NULL);
        const char *accels[] = {shortcuts[i][1], NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), action, accels);
    }
    const char *zoom_in[] = {"<Primary>plus", "<Primary>equal", "<Primary>KP_Add", NULL};
    const char *zoom_out[] = {"<Primary>minus", "<Primary>KP_Subtract", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-in", zoom_in);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.zoom-out", zoom_out);
    const char *close_window[] = {"<Primary><Shift>w", "<Primary><Shift>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.close-window", close_window);
}

static void options_clear(Options *options)
{
    g_free(options->directory);
    g_free(options->title);
    g_strfreev(options->command);
}

static gboolean parse_options(int argc, char **argv, Options *options, GError **error)
{
    *options = (Options){.zoom = 1.0, .menubar = -1};
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i], *value = NULL;
        g_autofree char *key = NULL;
        if (g_str_equal(arg, "--") || g_str_equal(arg, "-e")) {
            if (i + 1 >= argc) goto invalid;
            options->command = g_strdupv(argv + i + 1);
            return TRUE;
        }
        if (g_str_equal(arg, "--window")) continue;
        if (g_str_equal(arg, "--full-screen")) { options->fullscreen = TRUE; continue; }
        if (g_str_equal(arg, "--maximize")) { options->maximize = TRUE; continue; }
        if (g_str_equal(arg, "--preferences")) { options->preferences = TRUE; continue; }
        if (g_str_equal(arg, "--show-menubar")) { options->menubar = 1; continue; }
        if (g_str_equal(arg, "--hide-menubar")) { options->menubar = 0; continue; }
        if (g_str_equal(arg, "--help") || g_str_equal(arg, "-h")) { options->help = TRUE; continue; }
        if (g_str_equal(arg, "--version")) { options->version = TRUE; continue; }
        const char *equals = strchr(arg, '=');
        key = equals ? g_strndup(arg, equals - arg) : g_strdup(arg);
        if (equals) value = equals + 1;
        else if (i + 1 < argc) value = argv[++i];
        if (!value || !*value) goto invalid;
        if (g_str_equal(key, "--working-directory")) {
            g_free(options->directory); options->directory = g_strdup(value);
        } else if (g_str_equal(key, "--title") || g_str_equal(key, "-t")) {
            g_free(options->title); options->title = g_strdup(value);
        } else if (g_str_equal(key, "--geometry")) {
            char *end;
            long columns = strtol(value, &end, 10);
            if (end == value || *end != 'x') goto invalid;
            const char *row_start = end + 1;
            long rows = strtol(row_start, &end, 10);
            if (end == row_start || *end || columns < 16 || columns > 511 || rows < 2 || rows > 511) goto invalid;
            options->columns = (int)columns; options->rows = (int)rows;
        } else if (g_str_equal(key, "--zoom")) {
            char *end;
            double zoom = g_ascii_strtod(value, &end);
            if (end == value || *end || !isfinite(zoom) || zoom < 0.5 || zoom > 3.0) goto invalid;
            options->zoom = zoom;
        } else goto invalid;
        continue;
invalid:
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            "Invalid option or value: %s. Try simpleterm --help.", arg);
        return FALSE;
    }
    return TRUE;
}

static int command_line(GApplication *app, GApplicationCommandLine *line, gpointer data)
{
    (void)app; (void)data;
    int argc;
    g_auto(GStrv) argv = g_application_command_line_get_arguments(line, &argc);
    Options options;
    g_autoptr(GError) error = NULL;
    if (!parse_options(argc, argv, &options, &error)) {
        g_application_command_line_printerr(line, "%s\n", error->message);
        options_clear(&options);
        return 2;
    }
    char *directory = g_canonicalize_filename(options.directory ? options.directory : ".",
        g_application_command_line_get_cwd(line));
    g_free(options.directory); options.directory = directory;
    if (!g_file_test(directory, G_FILE_TEST_IS_DIR) || access(directory, X_OK) != 0) {
        g_application_command_line_printerr(line, "Cannot open working directory: %s\n", directory);
        options_clear(&options);
        return 2;
    }
    TerminalWindow *window = new_window();
    if (options.menubar >= 0) gtk_widget_set_visible(window->menubar, options.menubar);
    new_session(window, &options, (char **)g_application_command_line_get_environ(line));
    if (options.fullscreen) gtk_window_fullscreen(GTK_WINDOW(window->widget));
    if (options.maximize) gtk_window_maximize(GTK_WINDOW(window->widget));
    update_window(window);
    gtk_window_present(GTK_WINDOW(window->widget));
    if (options.preferences) show_preferences(window);
    options_clear(&options);
    return 0;
}

static const char help[] =
    "Usage: simpleterm [OPTIONS] [-- COMMAND [ARGUMENTS…]]\n\n"
    "  --window                 Open a new window (default)\n"
    "  --working-directory DIR  Start in DIR\n"
    "  --title TITLE, -t TITLE  Use a fixed terminal title\n"
    "  --geometry COLSxROWS     Initial grid, for example 100x30\n"
    "  --zoom FACTOR            Font scale between 0.5 and 3.0\n"
    "  --full-screen            Start in full screen\n"
    "  --maximize               Start maximized\n"
    "  --hide-menubar           Hide the menu bar (right-click restores it)\n"
    "  --show-menubar           Show the menu bar\n"
    "  --preferences            Open the preferences panel\n"
    "  -e COMMAND [ARGS…]       Same as -- COMMAND [ARGS…]; no shell expansion\n"
    "  --version                Print the version\n"
    "  --help, -h               Show this help\n\n"
    "Copy/Paste: Ctrl+Shift+C/V; select with the mouse, middle-click to paste.\n"
    "Windows: Ctrl+Shift+N to open; Ctrl+Shift+W/Q to close.\n"
    "Find: Ctrl+Shift+F; zoom: Ctrl+plus/minus/0; full screen: Super+Ctrl+Shift or F11.\n";

int main(int argc, char **argv)
{
    /* Report bad CLI arguments even without a running graphical session. */
    Options options;
    g_autoptr(GError) error = NULL;
    if (!parse_options(argc, argv, &options, &error)) {
        g_printerr("%s\n", error->message); options_clear(&options); return 2;
    }
    if (options.help || options.version) {
        if (options.help) g_print("%s", help);
        else g_print("simpleterm %s\n", VERSION);
        options_clear(&options);
        return 0;
    }
    options_clear(&options);
    g_set_application_name("Simpleterm");
    g_set_prgname("simpleterm");
    application = gtk_application_new(APP_ID,
        G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_SEND_ENVIRONMENT);
    g_signal_connect(application, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(application, "command-line", G_CALLBACK(command_line), NULL);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    g_clear_object(&desktop_interface);
    simpleterm_settings_clear(&settings);
    return status;
}
