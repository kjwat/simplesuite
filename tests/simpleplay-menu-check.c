/* Exercise menu grabs and outside clicks through real X input. */
#define main simpleplay_program_main
#include "../simpleplay.c"
#undef main

static void pump(unsigned milliseconds)
{
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < end);
}

static void xdo(const char *arguments)
{
    g_autofree char *command = g_strconcat("xdotool ", arguments, NULL);
    int status;
    g_assert_true(g_spawn_command_line_sync(command, NULL, NULL, &status, NULL));
    g_assert_true(g_spawn_check_wait_status(status, NULL));
    pump(250);
}

static GdkRectangle click_at(Player *p, GtkWidget *widget, int x, int y, int button)
{
    GdkRectangle point = {0, 0, 1, 1};
    g_assert_true(gtk_widget_translate_coordinates(widget, p->window, x, y, &point.x, &point.y));
    int wx, wy;
    gdk_window_get_origin(gtk_widget_get_window(p->window), &wx, &wy);
    g_autofree char *command = g_strdup_printf("mousemove %d %d click %d", wx + point.x, wy + point.y, button);
    xdo(command);
    return point;
}

static GdkRectangle click_widget(Player *p, GtkWidget *widget, int button)
{
    return click_at(p, widget, gtk_widget_get_allocated_width(widget) / 2,
                    gtk_widget_get_allocated_height(widget) / 2, button);
}

static GtkWidget *menu(void)
{
    GtkWidget *popover = gtk_grab_get_current();
    g_assert_true(GTK_IS_POPOVER(popover));
    g_assert_true(gtk_widget_get_mapped(popover));
    return popover;
}

static void expect_anchor(Player *p, const GdkRectangle *expected)
{
    GtkWidget *popover = menu();
    GdkRectangle point;
    g_assert_true(gtk_popover_get_pointing_to(GTK_POPOVER(popover), &point));
    GtkWidget *relative = gtk_popover_get_relative_to(GTK_POPOVER(popover));
    int x, y;
    g_assert_true(gtk_widget_translate_coordinates(relative, p->window, point.x, point.y, &x, &y));
    g_assert_cmpint(x, ==, expected->x);
    g_assert_cmpint(y, ==, expected->y);
}

static GtkWidget *find_option(GtkWidget *widget, const char *text)
{
    if (GTK_IS_COMBO_BOX_TEXT(widget)) {
        g_autofree char *active = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
        if (g_strcmp0(active, text) == 0) return widget;
    }
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *found = NULL;
    for (GList *n = children; n && !found; n = n->next) found = find_option(n->data, text);
    g_list_free(children);
    return found;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    Player *p = &player;
    p->current = -1;
    p->queue = g_ptr_array_new_with_free_func(g_free);
    p->status.rate = 1;
    p->engine = sp_engine_new();
    sp_settings_load(&p->settings);
    p->settings.width = 1100;
    p->settings.height = 800;
    p->application = gtk_application_new("org.simplesuite.Simpleplay.MenuCheck", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(p->application), NULL, NULL));
    build_window(p);
    /* Keep the UI in a paused video state without decoding a media fixture. */
    g_source_remove(p->tick); p->tick = 0;
    gtk_widget_remove_tick_callback(p->drawing, p->frame_tick); p->frame_tick = 0;
    p->info.has_video = p->info.seekable = TRUE;
    p->status.state = SP_PAUSED;
    gtk_widget_hide(p->empty);
    gtk_widget_set_sensitive(p->tracks_button, TRUE);
    g_object_set(gtk_settings_get_default(), "gtk-double-click-time", 2000, NULL);
    pump(250);

    GdkRectangle first = click_at(p, p->drawing, 200, 20, 3);
    expect_anchor(p, &first);
    click_at(p, p->drawing, 15, gtk_widget_get_allocated_height(p->drawing) - 15, 1);
    g_assert_null(gtk_grab_get_current());
    g_assert_cmpuint(p->click_timer, ==, 0);
    g_print("OK left click over the picture dismisses its context menu without toggling playback\n");

    click_widget(p, p->tracks_button, 1);
    menu();
    click_at(p, p->drawing, 15, 20, 1);
    g_assert_null(gtk_grab_get_current());
    g_assert_cmpuint(p->click_timer, ==, 0);
    g_print("OK left click over the picture dismisses the toolbar options menu\n");

    click_at(p, p->drawing, 200, 20, 3);
    GtkWidget *old = menu();
    g_object_add_weak_pointer(G_OBJECT(old), (gpointer *)&old);
    GdkRectangle moved = click_at(p, p->drawing,
        gtk_widget_get_allocated_width(p->drawing) - 15,
        gtk_widget_get_allocated_height(p->drawing) - 15, 3);
    g_assert_null(old);
    expect_anchor(p, &moved);
    g_assert_nonnull(find_option(menu(), "Original proportions"));
    xdo("key Escape");
    g_assert_null(gtk_grab_get_current());
    g_print("OK one outside right click replaces the context menu at the new pointer; Escape still closes it\n");

    click_widget(p, p->tracks_button, 1);
    old = menu();
    g_object_add_weak_pointer(G_OBJECT(old), (gpointer *)&old);
    moved = click_at(p, p->drawing, 200, 20, 3);
    g_assert_null(old);
    expect_anchor(p, &moved);
    g_print("OK outside right click moves toolbar options to the picture\n");

    GtkWidget *popover = menu();
    GtkWidget *aspect = find_option(popover, "Original proportions");
    g_assert_nonnull(aspect);
    click_widget(p, aspect, 1);
    xdo("key Down Return");
    g_assert_true(menu() == popover);
    g_assert_cmpint(p->aspect, ==, 1);
    double volume = p->settings.volume;
    click_widget(p, p->volume_button, 1);
    g_assert_null(gtk_grab_get_current());
    g_assert_cmpfloat(p->settings.volume, ==, volume);
    g_print("OK dropdown selection stays open; outside control click dismisses without muting\n");

    click_widget(p, p->speed_button, 1);
    moved = click_at(p, p->drawing, 200, 20, 3);
    expect_anchor(p, &moved);
    old = menu();
    g_object_add_weak_pointer(G_OBJECT(old), (gpointer *)&old);
    moved = click_widget(p, p->volume_button, 3);
    g_assert_null(old);
    expect_anchor(p, &moved);
    xdo("key Escape");
    g_assert_null(gtk_grab_get_current());
    g_print("OK speed menu and outside right clicks on controls use the same behavior\n");

    gtk_window_close(GTK_WINDOW(p->window));
    pump(50);
    sp_engine_free(p->engine);
    g_object_unref(p->application);
    sp_settings_clear(&p->settings);
    g_ptr_array_free(p->queue, TRUE);
    return 0;
}
