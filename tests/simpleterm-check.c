/* Exercise the actual GTK widgets, X input, VTE PTYs, and clipboard. */
#define main simpleterm_program_main
#include "../simpleterm.c"
#undef main
#include <gdk/gdkx.h>
#include <glib/gstdio.h>

static void pump(unsigned milliseconds)
{
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < end);
}

static char *screen_text(Tab *tab)
{
    return vte_terminal_get_text_format(tab->terminal, VTE_FORMAT_TEXT);
}

static void expect_text(Tab *tab, const char *needle)
{
    for (int i = 0; i < 100; i++) {
        g_autofree char *text = screen_text(tab);
        if (text && strstr(text, needle)) return;
        pump(30);
    }
    g_autofree char *text = screen_text(tab);
    g_error("Missing terminal output '%s'; contents: %s", needle, text);
}

static void send_command(Tab *tab, const char *command)
{
    vte_terminal_feed_child(tab->terminal, command, -1);
    vte_terminal_feed_child(tab->terminal, "\n", 1);
    pump(100);
}

static void activate(TerminalWindow *window, const char *action)
{
    g_action_group_activate_action(G_ACTION_GROUP(window->widget), action, NULL);
    pump(120);
}

static void xdo(const char *arguments)
{
    g_autofree char *command = g_strconcat("xdotool ", arguments, NULL);
    g_autoptr(GError) error = NULL;
    int status;
    g_assert_true(g_spawn_command_line_sync(command, NULL, NULL, &status, &error));
    g_assert_true(g_spawn_check_wait_status(status, &error));
    pump(150);
}

static void focus(TerminalWindow *window)
{
    GdkWindow *gdk = gtk_widget_get_window(window->widget);
    g_autofree char *command = g_strdup_printf("windowfocus %lu", gdk_x11_window_get_xid(gdk));
    xdo(command);
    Tab *tab = current_tab(window);
    gtk_widget_grab_focus(GTK_WIDGET(tab->terminal));
    pump(50);
}

static void expect_opaque_terminal(Tab *tab, gboolean opaque)
{
    GdkWindow *window = gtk_widget_get_window(tab->window->widget);
    GdkAtom type;
    int format, length;
    g_autofree guchar *data = NULL;
    cairo_region_t *region = cairo_region_create();
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gboolean present = gdk_property_get(window, gdk_atom_intern_static_string("_NET_WM_OPAQUE_REGION"),
        GDK_NONE, 0, 4096, FALSE, &type, &format, &length, &data);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (present) {
        g_assert_cmpint(format, ==, 32);
        g_assert_cmpint(length % (4 * sizeof(gulong)), ==, 0);
        const gulong *values = (const gulong *)data;
        for (gsize index = 0; index < length / sizeof(gulong); index += 4) {
            cairo_rectangle_int_t rectangle = {
                (int)values[index], (int)values[index + 1],
                (int)values[index + 2], (int)values[index + 3]
            };
            cairo_region_union_rectangle(region, &rectangle);
        }
    }
    cairo_rectangle_int_t terminal = {0};
    g_assert_true(gtk_widget_translate_coordinates(GTK_WIDGET(tab->terminal), tab->window->widget,
        0, 0, &terminal.x, &terminal.y));
    terminal.width = gtk_widget_get_allocated_width(GTK_WIDGET(tab->terminal));
    terminal.height = gtk_widget_get_allocated_height(GTK_WIDGET(tab->terminal));
    gboolean covered = cairo_region_contains_rectangle(region, &terminal) == CAIRO_REGION_OVERLAP_IN;
    cairo_region_destroy(region);
    g_assert_cmpint(covered, ==, opaque);
}

static void mouse(Tab *tab, int column, int row, const char *event)
{
    int x, y, wx, wy;
    gtk_widget_translate_coordinates(GTK_WIDGET(tab->terminal), tab->window->widget, 0, 0, &x, &y);
    gdk_window_get_origin(gtk_widget_get_window(tab->window->widget), &wx, &wy);
    GtkBorder padding;
    gtk_style_context_get_padding(gtk_widget_get_style_context(GTK_WIDGET(tab->terminal)), GTK_STATE_FLAG_NORMAL, &padding);
    x += wx + padding.left + (int)vte_terminal_get_char_width(tab->terminal) * column + 3;
    y += wy + padding.top + (int)vte_terminal_get_char_height(tab->terminal) * row + 5;
    g_autofree char *command = g_strdup_printf("mousemove --sync %d %d %s", x, y, event);
    xdo(command);
}

static gboolean menu_has(GMenuModel *menu, const char *action)
{
    for (int i = 0; i < g_menu_model_get_n_items(menu); i++) {
        g_autofree char *name = NULL;
        if (g_menu_model_get_item_attribute(menu, i, G_MENU_ATTRIBUTE_ACTION, "s", &name) &&
            g_str_equal(name, action)) return TRUE;
        g_autoptr(GMenuModel) section = g_menu_model_get_item_link(menu, i, G_MENU_LINK_SECTION);
        if (section && menu_has(section, action)) return TRUE;
    }
    return FALSE;
}

static GtkWidget *find_control(GtkWidget *widget, const char *name)
{
    if (g_str_equal(gtk_widget_get_name(widget), name)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next)
        found = find_control(item->data, name);
    g_list_free(children);
    return found;
}

static GtkWidget *setting_control(const char *name)
{
    GtkWidget *widget = find_control(preferences_window, name);
    g_assert_nonnull(widget);
    return widget;
}

static void toggle_setting(const char *name, gboolean active)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(setting_control(name)), active);
    pump(80);
}

static void test_preferences(const Options *options, char **environment)
{
    TerminalWindow *first = new_window();
    Tab *tab = new_tab(first, options, environment);
    TerminalWindow *second = new_window();
    Tab *other = new_tab(second, options, environment);
    pump(300);
    focus(first);
    xdo("key ctrl+comma");
    g_assert_nonnull(preferences_window);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(setting_control("preferences-pages"))), ==, "profile");
    g_assert_true(gtk_widget_get_mapped(setting_control("cursor-shape")));
    GtkWidget *original = preferences_window;
    activate(second, "preferences");
    g_assert_true(preferences_window == original);
    g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(original)) == GTK_WINDOW(second->widget));
    g_assert_false(gtk_widget_get_sensitive(setting_control("font")));
    g_autofree char *fallback = simpleterm_settings_font(&settings, NULL);
    g_assert_cmpstr(fallback, ==, "Monospace 12");
    if (desktop_interface) {
        g_assert_true(g_settings_set_string(desktop_interface, "monospace-font-name", "DejaVu Sans Mono 15"));
        pump(120);
        const PangoFontDescription *font = vte_terminal_get_font(tab->terminal);
        g_assert_cmpstr(pango_font_description_get_family(font), ==, "DejaVu Sans Mono");
        g_assert_cmpint(pango_font_description_get_size(font), ==, 15 * PANGO_SCALE);
        g_assert_cmpint(pango_font_description_get_size(vte_terminal_get_font(other->terminal)), ==, 15 * PANGO_SCALE);
        g_autofree char *shown = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(setting_control("font")));
        g_assert_cmpstr(shown, ==, "DejaVu Sans Mono 15");
        g_assert_true(g_settings_set_string(desktop_interface, "monospace-font-name", ""));
        pump(120);
        g_assert_cmpint(pango_font_description_get_size(vte_terminal_get_font(tab->terminal)), ==, 12 * PANGO_SCALE);
        g_assert_true(g_settings_set_string(desktop_interface, "monospace-font-name", "DejaVu Sans Mono 13"));
        pump(120);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(setting_control("cursor-shape")), 2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(setting_control("cursor-blink")), 2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(setting_control("text-blink")), 0);
    toggle_setting("audible-bell", FALSE);
    g_assert_cmpint(vte_terminal_get_cursor_shape(tab->terminal), ==, VTE_CURSOR_SHAPE_UNDERLINE);
    g_assert_cmpint(vte_terminal_get_cursor_shape(other->terminal), ==, VTE_CURSOR_SHAPE_UNDERLINE);
    g_assert_cmpint(vte_terminal_get_cursor_blink_mode(tab->terminal), ==, VTE_CURSOR_BLINK_OFF);
    g_assert_cmpint(vte_terminal_get_text_blink_mode(tab->terminal), ==, VTE_TEXT_BLINK_NEVER);
    g_assert_false(vte_terminal_get_audible_bell(other->terminal));
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(setting_control("font")), "Monospace 14");
    g_signal_emit_by_name(setting_control("font"), "font-set");
    toggle_setting("custom-font", TRUE);
    g_assert_true(gtk_widget_get_sensitive(setting_control("font")));
    g_assert_cmpint(pango_font_description_get_size(vte_terminal_get_font(other->terminal)), ==, 14 * PANGO_SCALE);
    if (desktop_interface) {
        g_assert_true(g_settings_set_string(desktop_interface, "monospace-font-name", "DejaVu Sans Mono 16"));
        pump(120);
        g_assert_cmpint(pango_font_description_get_size(vte_terminal_get_font(tab->terminal)), ==, 14 * PANGO_SCALE);
        g_assert_cmpint(pango_font_description_get_size(vte_terminal_get_font(other->terminal)), ==, 14 * PANGO_SCALE);
    }
    vte_terminal_set_font_scale(other->terminal, 1.2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(setting_control("cell-width")), 1.25);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(setting_control("cell-height")), 1.15);
    pump(100);
    g_assert_cmpfloat(vte_terminal_get_cell_width_scale(other->terminal), ==, 1.25);
    g_assert_cmpfloat(vte_terminal_get_cell_height_scale(tab->terminal), ==, 1.15);
    g_assert_cmpfloat(vte_terminal_get_font_scale(other->terminal), ==, 1.2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(setting_control("columns")), 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(setting_control("rows")), 30);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(setting_control("profile-tabs")), 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(setting_control("color-scheme")), 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(setting_control("palette-scheme")), 1);
    GdkRGBA background, expected;
    vte_terminal_get_color_background_for_draw(other->terminal, &background);
    gdk_rgba_parse(&expected, "#000000");
    g_assert_true(gdk_rgba_equal(&background, &expected));
    gdk_rgba_parse(&expected, "#123456");
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(setting_control("palette-3")), &expected);
    g_signal_emit_by_name(setting_control("palette-3"), "color-set");
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(setting_control("palette-scheme"))), ==, 3);
    toggle_setting("use-theme-colors", TRUE);
    g_assert_false(gtk_widget_get_sensitive(setting_control("foreground")));
    toggle_setting("use-theme-colors", FALSE);
    toggle_setting("custom-cursor", TRUE);
    g_assert_true(gtk_widget_get_sensitive(setting_control("cursor")));
    toggle_setting("transparent", TRUE);
    gtk_range_set_value(GTK_RANGE(setting_control("background-opacity")), 0.65);
    pump(100);
    vte_terminal_get_color_background_for_draw(tab->terminal, &background);
    g_assert_cmpfloat_with_epsilon(background.alpha, 0.65, 0.0001);
    expect_opaque_terminal(tab, FALSE);
    expect_opaque_terminal(other, FALSE);
    gtk_range_set_value(GTK_RANGE(setting_control("background-opacity")), 1);
    pump(100);
    expect_opaque_terminal(tab, TRUE);
    expect_opaque_terminal(other, TRUE);
    gtk_range_set_value(GTK_RANGE(setting_control("background-opacity")), 0.65);
    pump(100);
    expect_opaque_terminal(tab, FALSE);
    toggle_setting("transparent", FALSE);
    expect_opaque_terminal(tab, TRUE);
    expect_opaque_terminal(other, TRUE);
    toggle_setting("transparent", TRUE);
    expect_opaque_terminal(tab, FALSE);
    g_print("OK compositor opaque regions follow transparency and opacity in every window\n");
    toggle_setting("bold-is-bright", TRUE);
    g_assert_true(vte_terminal_get_bold_is_bright(other->terminal));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(setting_control("profile-tabs")), 2);
    toggle_setting("show-scrollbar", FALSE);
    toggle_setting("scroll-on-output", TRUE);
    toggle_setting("scroll-on-keystroke", FALSE);
    toggle_setting("scroll-on-paste", FALSE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(setting_control("scrollback-lines")), 12345);
    g_assert_false(gtk_widget_get_visible(other->scrollbar));
    g_assert_true(vte_terminal_get_scroll_on_output(tab->terminal));
    g_assert_false(vte_terminal_get_scroll_on_keystroke(other->terminal));
    g_assert_false(vte_terminal_get_scroll_on_insert(tab->terminal));
    g_assert_cmpint(vte_terminal_get_scrollback_lines(other->terminal), ==, 12345);
    toggle_setting("limit-scrollback", FALSE);
    g_assert_cmpint(vte_terminal_get_scrollback_lines(tab->terminal), ==, G_MAXLONG);
    g_assert_false(gtk_widget_get_sensitive(setting_control("scrollback-lines")));
    activate(first, "menubar");
    g_assert_false(gtk_widget_get_visible(first->menubar));
    g_assert_true(gtk_widget_get_visible(second->menubar));
    g_assert_false(settings.show_menubar);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(setting_control("show-menubar"))));
    toggle_setting("custom-bold", TRUE);
    SimpletermSettings saved_menubar;
    g_assert_true(simpleterm_settings_load(&saved_menubar, NULL));
    g_assert_false(saved_menubar.show_menubar);
    simpleterm_settings_clear(&saved_menubar);
    activate(first, "menubar");
    g_assert_true(gtk_widget_get_visible(first->menubar));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(setting_control("show-menubar"))));
    g_assert_true(simpleterm_settings_load(&saved_menubar, NULL));
    g_assert_true(saved_menubar.show_menubar);
    simpleterm_settings_clear(&saved_menubar);
    g_print("OK menubar toggles persist and stay synchronized with preferences\n");
    toggle_setting("show-menubar", FALSE);
    g_assert_true(gtk_widget_get_visible(first->menubar));
    TerminalWindow *fresh = new_window();
    Tab *inherited = new_tab(fresh, options, environment);
    pump(200);
    g_assert_false(gtk_widget_get_visible(fresh->menubar));
    g_assert_false(gtk_widget_get_visible(inherited->scrollbar));
    g_assert_cmpint(vte_terminal_get_column_count(inherited->terminal), ==, 100);
    g_assert_cmpint(vte_terminal_get_row_count(inherited->terminal), ==, 30);
    g_assert_cmpint(vte_terminal_get_cursor_shape(inherited->terminal), ==, VTE_CURSOR_SHAPE_UNDERLINE);
    g_autoptr(GError) error = NULL;
    SimpletermSettings restored;
    g_assert_true(simpleterm_settings_load(&restored, &error));
    g_assert_cmpint(restored.columns, ==, 100);
    g_assert_cmpint(restored.rows, ==, 30);
    g_assert_cmpstr(restored.font, ==, "Monospace 14");
    g_assert_true(restored.custom_font);
    g_assert_false(restored.show_scrollbar);
    g_assert_false(restored.limit_scrollback);
    g_assert_true(gdk_rgba_equal(&restored.palette[3], &expected));
    g_assert_cmpfloat_with_epsilon(restored.background_opacity, 0.65, 0.0001);
    simpleterm_settings_clear(&restored);
    gtk_widget_destroy(fresh->widget);
    gtk_widget_destroy(second->widget);
    pump(100);
    g_assert_null(preferences_window);
    activate(first, "preferences");
    g_assert_nonnull(preferences_window);
    gtk_button_clicked(GTK_BUTTON(setting_control("reset-size")));
    gtk_button_clicked(GTK_BUTTON(setting_control("reset-spacing")));
    g_assert_cmpint(settings.columns, ==, 80);
    g_assert_cmpint(settings.rows, ==, 24);
    g_assert_cmpfloat(vte_terminal_get_cell_width_scale(tab->terminal), ==, 1);
    g_assert_cmpfloat(vte_terminal_get_cell_height_scale(tab->terminal), ==, 1);
    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "simpleterm", "settings.ini", NULL);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
    toggle_setting("audible-bell", TRUE);
    g_assert_true(vte_terminal_get_audible_bell(tab->terminal));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(setting_control("preferences-status"))), "Could not save:"));
    g_assert_cmpint(g_rmdir(path), ==, 0);
    toggle_setting("audible-bell", FALSE);
    g_assert_null(strstr(gtk_label_get_text(GTK_LABEL(setting_control("preferences-status"))), "Could not save:"));
    if (g_getenv("SIMPLETERM_PREFERENCES_SCREENSHOT")) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(setting_control("profile-tabs")), 1);
        pump(200);
        GdkWindow *root = gdk_get_default_root_window();
        GdkPixbuf *shot = gdk_pixbuf_get_from_window(root, 0, 0, 1280, 900);
        g_assert_nonnull(shot);
        g_assert_true(gdk_pixbuf_save(shot, g_getenv("SIMPLETERM_PREFERENCES_SCREENSHOT"), "png", NULL, NULL));
        g_object_unref(shot);
    }
    gtk_widget_destroy(first->widget);
    pump(100);
    g_assert_null(preferences_window);
    g_print("OK preferences shortcut, singleton, live multi-window settings, inheritance, persistence, and teardown\n");
    g_print("OK desktop monospace font, live changes, displayed font, custom overrides, and fallback\n");
    g_assert_true(g_file_set_contents(path,
        "[Text]\ncolumns=-1\nrows=invalid\ncell-width=nan\ncursor-shape=99\nfont=Monospace 99999\n"
        "[Colors]\nbackground=invalid\nbackground-opacity=2\npalette-0=invalid\n"
        "[Scrolling]\nscrollback-lines=-1\nshow-scrollbar=invalid\n", -1, &error));
    g_assert_true(simpleterm_settings_load(&restored, &error));
    g_assert_cmpint(restored.columns, ==, 80);
    g_assert_cmpint(restored.rows, ==, 24);
    g_assert_cmpfloat(restored.cell_width, ==, 1);
    g_assert_cmpstr(restored.font, ==, "Monospace 12");
    g_assert_cmpint(restored.cursor_shape, ==, 0);
    g_assert_true(restored.show_scrollbar);
    g_assert_cmpint(restored.scrollback_lines, ==, 10000);
    g_assert_cmpfloat(restored.background_opacity, ==, 0.9);
    simpleterm_settings_clear(&restored);
    g_assert_true(g_file_set_contents(path, "not a valid key file", -1, &error));
    g_assert_false(simpleterm_settings_load(&restored, &error));
    g_assert_cmpint(restored.columns, ==, 80);
    simpleterm_settings_clear(&restored);
    g_clear_error(&error);
    g_print("OK invalid preferences safely fall back to defaults\n");
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    application = gtk_application_new(APP_ID ".Tests", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "startup", G_CALLBACK(startup), NULL);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, NULL));
    TerminalWindow *window = new_window();
    g_auto(GStrv) environment = g_get_environ();
    environment = g_environ_setenv(environment, "SHELL", "/bin/sh", TRUE);
    environment = g_environ_setenv(environment, "PS1", "simpleterm-test$ ", TRUE);
    environment = g_environ_setenv(environment, "SIMPLETERM_MARKER", "environment-ok", TRUE);
    char *shell[] = {"/bin/bash", "--noprofile", "--norc", NULL};
    Options options = {.directory = "/tmp", .command = shell};
    Tab *tab = new_tab(window, &options, environment);
    pump(500);
    focus(window);
    expect_text(tab, "simpleterm-test$");
    g_autofree char *default_font = simpleterm_settings_font(&settings, desktop_interface);
    g_autoptr(PangoFontDescription) expected_font = pango_font_description_from_string(default_font);
    g_assert_true(pango_font_description_equal(vte_terminal_get_font(tab->terminal), expected_font));
    g_assert_null(vte_terminal_get_font_options(tab->terminal));
    expect_opaque_terminal(tab, TRUE);
    g_assert_cmpint(tab->pid, >, 0);
    g_assert_cmpint(vte_terminal_get_column_count(tab->terminal), ==, 80);
    g_assert_cmpint(vte_terminal_get_row_count(tab->terminal), ==, 24);
    g_assert_false(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(window->notebook)));
    send_command(tab, "printf '%s:%s:%s\\n' \"$TERM\" \"$COLORTERM\" \"$SIMPLETERM_MARKER\"");
    expect_text(tab, "xterm-256color:truecolor:environment-ok");
    send_command(tab, "stty -a");
    expect_text(tab, "rows 24; columns 80");
    g_print("OK real PTY, initial grid, and child environment\n");

    /* Use real X mouse events to select a word and open VTE's context menu. */
    send_command(tab, "printf '\\033[2J\\033[Hmouseword anotherword\\n'");
    mouse(tab, 3, 0, "click --repeat 2 --delay 70 1");
    g_assert_true(vte_terminal_get_has_selection(tab->terminal));
    g_autofree char *selected = vte_terminal_get_text_selected(tab->terminal, VTE_FORMAT_TEXT);
    g_assert_cmpstr(selected, ==, "mouseword");
    mouse(tab, 3, 0, "click 3");
    GMenuModel *context = vte_terminal_get_context_menu_model(tab->terminal);
    g_assert_nonnull(context);
    g_assert_true(menu_has(context, "win.copy"));
    g_assert_true(menu_has(context, "win.paste"));
    g_assert_true(menu_has(context, "win.preferences"));
    if (g_getenv("SIMPLETERM_SCREENSHOT")) {
        GdkWindow *root = gdk_get_default_root_window();
        GdkPixbuf *shot = gdk_pixbuf_get_from_window(root, 0, 0, 1000, 750);
        g_assert_nonnull(shot);
        g_assert_true(gdk_pixbuf_save(shot, g_getenv("SIMPLETERM_SCREENSHOT"), "png", NULL, NULL));
        g_object_unref(shot);
    }
    /* Click the first row (Copy) in the context menu beside the pointer. */
    xdo("mousemove_relative --sync -- 45 16 click 1");
    g_autofree char *clipboard = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    g_assert_cmpstr(clipboard, ==, "mouseword");
    g_autofree char *primary = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY));
    g_assert_cmpstr(primary, ==, "mouseword");
    mouse(tab, 1, 1, "click 2");
    expect_text(tab, "simpleterm-test$ mouseword");
    xdo("key ctrl+u");
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), "printf 'clip%s\\n' board-ok", -1);
    xdo("key ctrl+shift+v Return");
    expect_text(tab, "clipboard-ok");
    g_print("OK mouse selection, right-click Copy, PRIMARY middle-click, clipboard paste\n");

    send_command(tab, "sleep 30");
    g_assert_cmpint(tcgetpgrp(vte_pty_get_fd(vte_terminal_get_pty(tab->terminal))), !=, tab->pid);
    xdo("key ctrl+c");
    pump(200);
    g_assert_cmpint(tcgetpgrp(vte_pty_get_fd(vte_terminal_get_pty(tab->terminal))), ==, tab->pid);
    send_command(tab, "printf 'interrupt%s\\n' -ok");
    expect_text(tab, "interrupt-ok");
    g_print("OK Ctrl+C interrupts the foreground process\n");

    xdo("key ctrl+shift+t");
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(window->notebook)), ==, 2);
    g_assert_true(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(window->notebook)));
    Tab *second = current_tab(window);
    g_assert_true(second != tab);
    g_autofree char *cwd = tab_directory(second);
    g_assert_cmpstr(cwd, ==, "/tmp");
    xdo("key alt+1");
    g_assert_true(current_tab(window) == tab);
    xdo("key ctrl+shift+Page_Down");
    g_assert_cmpint(gtk_notebook_page_num(GTK_NOTEBOOK(window->notebook), tab->page), ==, 1);
    xdo("key ctrl+Page_Up");
    g_assert_true(current_tab(window) == second);
    xdo("key ctrl+shift+w");
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(window->notebook)), ==, 1);
    g_assert_true(current_tab(window) == tab);
    g_assert_false(gtk_notebook_get_show_tabs(GTK_NOTEBOOK(window->notebook)));
    g_print("OK tab shortcuts, switching, reordering, inherited directory, and close\n");

    xdo("key ctrl+shift+f");
    g_assert_true(gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(window->searchbar)));
    gtk_entry_set_text(GTK_ENTRY(window->search), "clipboard-ok");
    pump(350);
    g_assert_nonnull(vte_terminal_search_get_regex(tab->terminal));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(window->search_status)), ==, "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(window->regex), TRUE);
    gtk_entry_set_text(GTK_ENTRY(window->search), "[");
    pump(350);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(window->search_status)), ==, "Invalid expression");
    xdo("key ctrl+shift+j");
    g_assert_false(gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(window->searchbar)));
    g_assert_null(vte_terminal_search_get_regex(tab->terminal));
    activate(window, "find");
    /* The search bar's close button changes this property directly. */
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(window->searchbar), FALSE);
    pump(100);
    g_assert_true(gtk_window_get_focus(GTK_WINDOW(window->widget)) == GTK_WIDGET(tab->terminal));
    activate(window, "read-only");
    g_assert_false(vte_terminal_get_input_enabled(tab->terminal));
    g_assert_false(g_action_group_get_action_enabled(G_ACTION_GROUP(window->widget), "paste"));
    activate(window, "read-only");
    xdo("key ctrl+equal");
    g_assert_cmpfloat(vte_terminal_get_font_scale(tab->terminal), >, 1.0);
    xdo("key ctrl+0");
    g_assert_cmpfloat(vte_terminal_get_font_scale(tab->terminal), ==, 1.0);
    g_print("OK search, invalid regex recovery, read-only, and zoom\n");

    send_command(tab, "printf '\\033[2J\\033[Hhttps://example.org/path\\n'");
    mouse(tab, 10, 0, "click 3");
    g_assert_cmpstr(window->link, ==, "https://example.org/path");
    g_assert_true(menu_has(vte_terminal_get_context_menu_model(tab->terminal), "win.open-link"));
    xdo("key Escape");
    /* Mouse reporting gets ordinary clicks; Shift+right-click overrides it. */
    send_command(tab, "printf '\\033[?1000h'");
    vte_terminal_set_context_menu_model(tab->terminal, NULL);
    mouse(tab, 30, 3, "click 3");
    g_assert_null(vte_terminal_get_context_menu_model(tab->terminal));
    xdo("keydown Shift_L");
    mouse(tab, 30, 3, "click 3");
    xdo("keyup Shift_L");
    g_assert_nonnull(vte_terminal_get_context_menu_model(tab->terminal));
    xdo("key Escape ctrl+u");
    send_command(tab, "printf '\\033[?1000l'");
    g_print("OK link context actions and mouse-reporting override\n");

    activate(window, "new-tab");
    Tab *detached = current_tab(window);
    activate(window, "detach");
    TerminalWindow *other = detached->window;
    g_assert_true(other != window);
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(window->notebook)), ==, 1);
    g_assert_true(current_tab(other) == detached);
    close_tab(detached);
    pump(200);
    focus(window);
    activate(window, "new-tab");
    Tab *busy = current_tab(window);
    send_command(busy, "sleep 30");
    g_assert_cmpint(tcgetpgrp(vte_pty_get_fd(vte_terminal_get_pty(busy->terminal))), !=, busy->pid);
    xdo("key ctrl+shift+w");
    g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(window->notebook)), ==, 1);
    g_assert_true(current_tab(window) == tab);
    send_command(tab, "sleep 30");
    g_assert_cmpint(tcgetpgrp(vte_pty_get_fd(vte_terminal_get_pty(tab->terminal))), !=, tab->pid);
    GWeakRef closed_window;
    g_weak_ref_init(&closed_window, window->widget);
    gtk_window_close(GTK_WINDOW(window->widget));
    pump(250);
    g_autoptr(GObject) remaining_window = g_weak_ref_get(&closed_window);
    g_assert_null(remaining_window);
    g_weak_ref_clear(&closed_window);
    g_print("OK detach, immediate busy tab/window close, and asynchronous teardown\n");

    /* Closing while the spawn is still in flight must not use freed memory. */
    for (int i = 0; i < 10; i++) {
        window = new_window();
        tab = new_tab(window, &options, environment);
        gtk_widget_destroy(window->widget);
        pump(20);
    }
    pump(500);
    window = new_window();
    char *missing[] = {"/definitely-no-such-simpleterm-command", NULL};
    options.command = missing;
    tab = new_tab(window, &options, environment);
    expect_text(tab, "Simpleterm could not start the command");
    g_assert_false(vte_terminal_get_input_enabled(tab->terminal));
    gtk_widget_destroy(window->widget);
    pump(100);
    g_print("OK failed command and rapid create/close lifecycle\n");
    options.command = shell;
    test_preferences(&options, environment);
    g_object_unref(application);
    g_clear_object(&desktop_interface);
    simpleterm_settings_clear(&settings);
    return 0;
}
