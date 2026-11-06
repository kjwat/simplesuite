#include "../simplevis-wallpaper.c"
#include <assert.h>
#include <stdlib.h>

static char *fixture;

static void write_fixture(const char *relative, const char *contents) {
    g_autofree char *path = g_build_filename(fixture, relative, NULL);
    g_autofree char *parent = g_path_get_dirname(path);
    assert(g_mkdir_with_parents(parent, 0700) == 0);
    assert(g_file_set_contents(path, contents, -1, NULL));
}

static void expect_rgb(RGBColor color, double r, double g, double b) {
    assert(fabs(color.r - r) < 0.01);
    assert(fabs(color.g - g) < 0.01);
    assert(fabs(color.b - b) < 0.01);
}

static char *save_image(const char *name, guint32 rgba) {
    char *path = g_build_filename(fixture, name, NULL);
    g_autoptr(GdkPixbuf) image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 32, 32);
    gdk_pixbuf_fill(image, rgba);
    assert(gdk_pixbuf_save(image, path, "png", NULL, NULL));
    return path;
}

static void schema_fixtures(void) {
    write_fixture("schemas/test.gschema.xml",
        "<schemalist>"
        "<schema id='org.gnome.desktop.interface' path='/org/gnome/desktop/interface/'>"
        "<key name='color-scheme' type='s'><default>'default'</default></key></schema>"
        "<schema id='org.gnome.desktop.background' path='/org/gnome/desktop/background/'>"
        "<key name='picture-uri' type='s'><default>''</default></key>"
        "<key name='picture-uri-dark' type='s'><default>''</default></key>"
        "<key name='picture-options' type='s'><default>'zoom'</default></key>"
        "<key name='primary-color' type='s'><default>''</default></key></schema>"
        "<schema id='org.cinnamon.desktop.background' path='/org/cinnamon/desktop/background/'>"
        "<key name='picture-uri' type='s'><default>''</default></key>"
        "<key name='picture-uri-list' type='aa{sv}'><default>[]</default></key></schema>"
        "<schema id='org.mate.background' path='/org/mate/background/'>"
        "<key name='picture-filename' type='s'><default>''</default></key></schema>"
        "</schemalist>");
    g_autofree char *directory = g_build_filename(fixture, "schemas", NULL);
    char *argv[] = {"glib-compile-schemas", directory, NULL};
    int status;
    assert(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                        NULL, NULL, &status, NULL));
    assert(g_spawn_check_wait_status(status, NULL));
    g_setenv("GSETTINGS_SCHEMA_DIR", directory, TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
}

static void set_setting(const char *schema, const char *key, const char *value) {
    g_autoptr(GSettings) settings = g_settings_new(schema);
    assert(g_settings_set_string(settings, key, value));
}

static void expect_path(const char *desktop, GDBusConnection *bus, const char *expected) {
    g_setenv("XDG_CURRENT_DESKTOP", desktop, TRUE);
    Wallpaper wallpaper = {0};
    assert(query_wallpaper(bus, &wallpaper));
    assert(g_strcmp0(wallpaper.path, expected) == 0);
    g_free(wallpaper.path);
}

typedef struct {
    GMutex mutex;
    GCond ready;
    gboolean started;
    GMainLoop *loop;
    const char *image;
} DesktopService;

static void desktop_method(GDBusConnection *connection, const char *sender,
                            const char *path, const char *interface,
                            const char *method, GVariant *parameters,
                            GDBusMethodInvocation *invocation, gpointer data) {
    (void)connection; (void)sender; (void)path; (void)interface;
    DesktopService *service = data;
    if (g_str_equal(method, "GetAllProperties")) {
        GVariantBuilder properties;
        g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&properties, "{sv}",
            "/backdrop/screen0/monitorHDMI-1/workspace0/last-image",
            g_variant_new_string(service->image));
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(a{sv})", &properties));
    } else {
        if (g_str_equal(method, "evaluateScript")) {
            const char *script;
            g_variant_get(parameters, "(&s)", &script);
            assert(strstr(script, "currentActivity()"));
            assert(!strstr(script, "writeConfig"));
        }
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(s)", service->image));
    }
}

static gpointer desktop_service(gpointer data) {
    DesktopService *service = data;
    g_autoptr(GMainContext) context = g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
        g_getenv("DBUS_SESSION_BUS_ADDRESS"),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
    assert(bus);
    const char *names[] = {"org.kde.plasmashell", "org.xfce.Xfconf",
                           "org.deepin.dde.Appearance1"};
    const char *paths[] = {"/PlasmaShell", "/org/xfce/Xfconf",
                           "/org/deepin/dde/Appearance1"};
    const char *interfaces[] = {"org.kde.PlasmaShell", "org.xfce.Xfconf",
                                "org.deepin.dde.Appearance1"};
    const char *methods[] = {
        "<method name='evaluateScript'><arg type='s' direction='in'/>"
        "<arg type='s' direction='out'/></method>",
        "<method name='GetAllProperties'><arg type='s' direction='in'/>"
        "<arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method>",
        "<method name='GetCurrentWorkspaceBackground'><arg type='s' direction='out'/></method>"
    };
    guint registrations[3];
    static const GDBusInterfaceVTable vtable = {.method_call = desktop_method};
    for (int i = 0; i < 3; i++) {
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus,
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName",
            g_variant_new("(su)", names[i], 0u), NULL, 0, 1000, NULL, NULL);
        assert(reply);
        g_autofree char *xml = g_strdup_printf(
            "<node><interface name='%s'>%s</interface></node>", interfaces[i], methods[i]);
        g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
        registrations[i] = g_dbus_connection_register_object(bus, paths[i],
            node->interfaces[0], &vtable, service, NULL, NULL);
        assert(registrations[i]);
    }
    g_mutex_lock(&service->mutex);
    service->loop = g_main_loop_new(context, FALSE);
    service->started = TRUE;
    g_cond_signal(&service->ready);
    g_mutex_unlock(&service->mutex);
    g_main_loop_run(service->loop);
    for (int i = 0; i < 3; i++)
        g_dbus_connection_unregister_object(bus, registrations[i]);
    g_dbus_connection_close_sync(bus, NULL, NULL);
    g_main_loop_unref(service->loop);
    g_main_context_pop_thread_default(context);
    return NULL;
}

static void check_slideshow(const char *red, const char *blue) {
    g_autofree char *contents = g_strdup_printf(
        "<background><starttime><year>2020</year><month>1</month><day>1</day>"
        "<hour>0</hour><minute>0</minute><second>0</second></starttime>"
        "<static><duration>10</duration><file>%s</file></static>"
        "<transition><duration>4</duration><from>%s</from><to>%s</to></transition>"
        "<static><duration>10</duration><file><size width='1920' height='1080'>"
        "%s</size></file></static></background>", red, red, blue, blue);
    write_fixture("slides.xml", contents);
    g_autofree char *xml = g_build_filename(fixture, "slides.xml", NULL);
    struct tm start = {.tm_year = 120, .tm_mday = 1, .tm_isdst = -1};
    time_t now = mktime(&start);
    g_autofree char *first = slideshow_image(xml, now + 1);
    g_autofree char *transition = slideshow_image(xml, now + 13);
    g_autofree char *second = slideshow_image(xml, now + 16);
    assert(g_strcmp0(first, red) == 0);
    assert(g_strcmp0(transition, blue) == 0);
    assert(g_strcmp0(second, blue) == 0);
}

int main(int argc, char **argv) {
    /* A manual live probe is useful without changing the user's wallpaper. */
    if (argc == 2 && g_str_equal(argv[1], "--live")) {
        g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        ImageCache cache = {0};
        RGBColor color;
        assert(resolve_color(bus, &cache, &color));
        printf("wallpaper accent: #%02x%02x%02x\n", (int)round(color.r * 255),
               (int)round(color.g * 255), (int)round(color.b * 255));
        g_free(cache.path);
        return 0;
    }
    fixture = g_dir_make_tmp("simplevis-wallpaper-XXXXXX", NULL);
    assert(fixture);
    g_autofree char *config = g_build_filename(fixture, "config", NULL);
    g_autofree char *state = g_build_filename(fixture, "state", NULL);
    g_setenv("HOME", fixture, TRUE);
    g_setenv("XDG_CONFIG_HOME", config, TRUE);
    g_setenv("XDG_STATE_HOME", state, TRUE);
    g_setenv("GIO_USE_VFS", "local", TRUE);
    g_autoptr(GTestDBus) test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);
    schema_fixtures();
    g_autofree char *red = save_image("red.png", 0xff0000ff);
    g_autofree char *blue = save_image("blue ' ; $(touch forbidden).png", 0x0044ffff);
    g_autofree char *black = save_image("black.png", 0x000000ff);
    g_autofree char *transparent = save_image("transparent.png", 0xff000000);
    RGBColor color;
    assert(image_accent(red, &color)); expect_rgb(color, 1, 0, 0);
    assert(image_accent(blue, &color)); expect_rgb(color, 0, 68.0 / 255, 1);
    assert(image_accent(black, &color)); expect_rgb(color, 1, 1, 1);
    assert(!image_accent(transparent, &color));
    assert(!local_path("file://elsewhere/tmp/image.png"));
    assert(!local_path("https://example.invalid/image.png"));
    g_autoptr(GdkPixbuf) bordered = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 32, 32);
    gdk_pixbuf_fill(bordered, 0x000000ff);
    guchar *pixels = gdk_pixbuf_get_pixels(bordered);
    int stride = gdk_pixbuf_get_rowstride(bordered);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 12; x++) {
            guchar *p = pixels + y * stride + x * 4;
            p[0] = x < 4 ? 255 : 0;
            p[1] = x < 4 ? 255 : 68;
            p[2] = 255;
        }
    }
    g_autofree char *bordered_path = g_build_filename(fixture, "bordered.png", NULL);
    assert(gdk_pixbuf_save(bordered, bordered_path, "png", NULL, NULL));
    assert(image_accent(bordered_path, &color));
    assert(color.b > 0.8 && color.b > color.r * 2 &&
           color.g > 0.1 && color.g < 0.4);

    g_autofree char *blue_uri = g_filename_to_uri(blue, NULL, NULL);
    set_setting("org.gnome.desktop.background", "picture-uri", red);
    set_setting("org.gnome.desktop.background", "picture-uri-dark", blue_uri);
    set_setting("org.gnome.desktop.interface", "color-scheme", "prefer-dark");
    write_fixture("config/simplewm/colors.Xresources", "*.cursorColor: #00ff00\n");
    const char *gnome[] = {"GNOME", "ubuntu:GNOME", "Budgie:GNOME", "Unity",
                           "Pantheon", NULL};
    for (int i = 0; gnome[i]; i++)
        expect_path(gnome[i], NULL, blue);
    g_setenv("DESKTOP_SESSION", "plasma", TRUE);
    expect_path("GNOME", NULL, blue); /* XDG_CURRENT_DESKTOP wins. */
    set_setting("org.gnome.desktop.interface", "color-scheme", "default");
    expect_path("GNOME", NULL, red);
    set_setting("org.gnome.desktop.background", "picture-uri", "");
    g_setenv("XDG_CURRENT_DESKTOP", "GNOME", TRUE);
    Wallpaper unavailable = {0};
    assert(!query_wallpaper(NULL, &unavailable)); /* No stale SimpleWM palette. */
    set_setting("org.gnome.desktop.background", "picture-options", "none");
    set_setting("org.gnome.desktop.background", "primary-color", "rgb(255,128,0)");
    assert(query_wallpaper(NULL, &unavailable) && unavailable.solid);
    expect_rgb(unavailable.color, 1, 128.0 / 255, 0);
    set_setting("org.gnome.desktop.background", "picture-options", "zoom");
    set_setting("org.gnome.desktop.background", "picture-uri", red);
    set_setting("org.gnome.desktop.background", "primary-color", "");
    set_setting("org.mate.background", "picture-filename", blue);
    expect_path("MATE", NULL, blue);
    set_setting("org.cinnamon.desktop.background", "picture-uri", red);
    expect_path("X-Cinnamon", NULL, red);
    g_autoptr(GSettings) cinnamon = g_settings_new("org.cinnamon.desktop.background");
    GVariantBuilder list, entry;
    g_variant_builder_init(&list, G_VARIANT_TYPE("aa{sv}"));
    g_variant_builder_init(&entry, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&entry, "{sv}", "picture-uri", g_variant_new_string(blue_uri));
    g_variant_builder_add_value(&list, g_variant_builder_end(&entry));
    assert(g_settings_set_value(cinnamon, "picture-uri-list", g_variant_builder_end(&list)));
    expect_path("X-Cinnamon:GNOME", NULL, blue);

    g_autofree char *lxqt = g_strdup_printf("[Desktop]\nWallpaper=%s\n", blue);
    write_fixture("config/pcmanfm-qt/lxqt/settings.conf", lxqt);
    expect_path("LXQt", NULL, blue);
    g_autofree char *lxde = g_strdup_printf("[*]\nwallpaper=%s\n", red);
    write_fixture("config/pcmanfm/LXDE/desktop-items-0.conf", lxde);
    expect_path("LXDE", NULL, red);
    g_autofree char *escaped = g_markup_escape_text(blue, -1);
    g_autofree char *xfce = g_strdup_printf(
        "<channel><property name='backdrop'><property name='last-image' "
        "type='string' value='%s'/></property></channel>", escaped);
    write_fixture("config/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml", xfce);
    expect_path("XFCE", NULL, blue);
    g_autofree char *cosmic = g_strdup_printf("[(\"HDMI-1\",Path(\"%s\"))]", blue);
    write_fixture("state/cosmic/com.system76.CosmicBackground/v1/wallpapers", cosmic);
    expect_path("COSMIC", NULL, blue);
    write_fixture("state/cosmic/com.system76.CosmicBackground/v1/wallpapers",
                  "[(\"HDMI-1\", Color(Single([0.25,0.5,1.0])))]");
    g_setenv("XDG_CURRENT_DESKTOP", "COSMIC", TRUE);
    Wallpaper solid_cosmic = {0};
    assert(query_wallpaper(NULL, &solid_cosmic) && solid_cosmic.solid);
    expect_rgb(solid_cosmic.color, 0.25, 0.5, 1);
    g_setenv("XDG_CURRENT_DESKTOP", "SimpleWM", TRUE);
    Wallpaper wallpaper = {0};
    assert(query_wallpaper(NULL, &wallpaper) && wallpaper.solid);
    expect_rgb(wallpaper.color, 0, 1, 0);
    g_unsetenv("DESKTOP_SESSION");
    g_unsetenv("GDMSESSION");
    g_unsetenv("XDG_CURRENT_DESKTOP");
    Wallpaper bare_simplewm = {0};
    assert(query_wallpaper(NULL, &bare_simplewm) && bare_simplewm.solid);
    expect_rgb(bare_simplewm.color, 0, 1, 0);
    check_slideshow(red, blue);

    DesktopService service = {.image = blue};
    GThread *thread = g_thread_new("desktop-fixtures", desktop_service, &service);
    g_mutex_lock(&service.mutex);
    while (!service.started)
        g_cond_wait(&service.ready, &service.mutex);
    g_mutex_unlock(&service.mutex);
    g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
        g_getenv("DBUS_SESSION_BUS_ADDRESS"),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, NULL);
    assert(bus);
    expect_path("KDE", bus, blue);
    expect_path("XFCE", bus, blue);
    expect_path("Deepin", bus, blue);

    /* The UI path must return promptly while the worker discovers/decodes. */
    g_setenv("XDG_CURRENT_DESKTOP", "GNOME", TRUE);
    gint64 begin = g_get_monotonic_time();
    (void)simplevis_wallpaper_color(&color);
    assert(g_get_monotonic_time() - begin < 50000);
    gboolean ready = FALSE;
    for (int i = 0; i < 150 && !ready; i++) {
        g_usleep(10000);
        ready = simplevis_wallpaper_color(&color);
    }
    assert(ready);
    expect_rgb(color, 1, 0, 0);
    set_setting("org.gnome.desktop.background", "picture-uri", blue_uri);
    for (int i = 0; i < 250; i++) {
        g_usleep(10000);
        simplevis_wallpaper_color(&color);
        if (color.b > 0.99 && color.r < 0.01)
            break;
    }
    expect_rgb(color, 0, 68.0 / 255, 1);
    simplevis_wallpaper_shutdown();
    g_main_loop_quit(service.loop);
    g_thread_join(thread);
    g_dbus_connection_close_sync(bus, NULL, NULL);
    g_clear_object(&bus);
    g_test_dbus_down(test_bus);
    printf("OK desktop backends, dominant colors, URI safety, slideshow timing and live worker updates\n");
    g_free(fixture);
    return 0;
}
