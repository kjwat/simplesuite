/* Exercise the actual GTK widgets, X input, VTE PTYs, and clipboard. */
#define main simpleterm_program_main
#include "../simpleterm.c"
#undef main
#include <gdk/gdkx.h>

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
    /* This is a test shell, not a busy foreground custom command. */
    tab->command = FALSE;
    pump(500);
    focus(window);
    expect_text(tab, "simpleterm-test$");
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
    g_assert_true(tab_busy(tab));
    xdo("key ctrl+c");
    pump(200);
    g_assert_false(tab_busy(tab));
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
    send_command(tab, "sleep 30");
    request_close(window, tab);
    g_assert_nonnull(window->close_dialog);
    gtk_dialog_response(GTK_DIALOG(window->close_dialog), GTK_RESPONSE_CANCEL);
    pump(100);
    g_assert_true(tab_busy(tab));
    request_close(window, tab);
    g_assert_nonnull(window->close_dialog);
    /* A process can exit while its close confirmation is open. */
    xdo("key Escape");
    gtk_widget_destroy(window->widget);
    pump(250);
    g_print("OK detach, busy-close confirmation, cancel, and asynchronous teardown\n");

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
    g_object_unref(application);
    return 0;
}
