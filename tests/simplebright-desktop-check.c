/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test the real capability probe and supported KWin calls on an isolated bus. */
#include "../simplebright-backend.c"
#include <assert.h>

static GAsyncQueue *ready;
static GMainLoop *mock_loop;
static int mock_temperature = 4500, mock_inhibited, mock_previews;
static const char full_xml[] =
    "<node><interface name='org.kde.KWin.NightLight'>"
    "<method name='inhibit'><arg type='u' direction='out'/></method>"
    "<method name='uninhibit'><arg type='u' direction='in'/></method>"
    "<method name='preview'><arg type='u' direction='in'/></method>"
    "<method name='stopPreview'/>"
    "<property name='running' type='b' access='read'/>"
    "<property name='enabled' type='b' access='read'/>"
    "<property name='available' type='b' access='read'/>"
    "</interface></node>";
static void mock_method(GDBusConnection *connection, const char *sender, const char *path,
                        const char *interface, const char *method, GVariant *args,
                        GDBusMethodInvocation *invocation, gpointer user)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)user;
    if (!strcmp(method, "inhibit")) {
        mock_inhibited = 1;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 42u));
    } else {
        if (!strcmp(method, "uninhibit")) {
            guint cookie; g_variant_get(args, "(u)", &cookie); assert(cookie == 42);
            mock_inhibited = 0;
        }
        if (!strcmp(method, "preview")) {
            guint kelvin; g_variant_get(args, "(u)", &kelvin);
            mock_temperature = (int)kelvin; mock_previews++;
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}
static GVariant *mock_property(GDBusConnection *connection, const char *sender, const char *path,
                               const char *interface, const char *name, GError **error, gpointer user)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)error; (void)user;
    return g_variant_new_boolean(strcmp(name, "running") || !mock_inhibited);
}
static gpointer mock_server(gpointer address)
{
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GDBusConnection *connection = g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    assert(connection);
    GVariant *reply = bus_call(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                              "RequestName", g_variant_new("(su)", "org.kde.KWin.NightLight", 0u), NULL);
    assert(reply); g_variant_unref(reply);
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(full_xml, NULL); assert(node);
    static const GDBusInterfaceVTable vtable = {.method_call=mock_method, .get_property=mock_property};
    guint registration = g_dbus_connection_register_object(connection, "/org/kde/KWin/NightLight", node->interfaces[0], &vtable, NULL, NULL, NULL);
    assert(registration);
    mock_loop = g_main_loop_new(context, FALSE);
    g_async_queue_push(ready, GINT_TO_POINTER(1));
    g_main_loop_run(mock_loop);
    g_dbus_connection_unregister_object(connection, registration);
    g_dbus_node_info_unref(node); g_object_unref(connection);
    g_main_loop_unref(mock_loop); g_main_context_pop_thread_default(context); g_main_context_unref(context);
    return NULL;
}
int main(void)
{
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    ready = g_async_queue_new();
    GThread *thread = g_thread_new("mock-kwin", mock_server, (gpointer)g_test_dbus_get_bus_address(bus));
    g_async_queue_pop(ready);
    daemon_night.kelvin = 4000;
    desktop_detect();
    assert(desktop && !strcmp(daemon_night.backend, "KDE Night Color"));
    assert(daemon_night.can_toggle && daemon_night.can_temperature);
    char error[SB_TEXT];
    assert(!kde_apply(1, 3200, error, sizeof(error)));
    assert(mock_temperature == 3200 && mock_previews == 1);
    assert(!kde_apply(0, 3200, error, sizeof(error)) && mock_inhibited);
    assert(!kde_apply(1, 4000, error, sizeof(error)) && !mock_inhibited);
    assert(mock_temperature == 4000 && mock_previews == 2);
    /* No color-service API must ever touch a hardware brightness device. */
    assert(!direct_open);
    g_clear_object(&desktop_settings); g_clear_object(&session_bus);
    g_main_loop_quit(mock_loop); g_thread_join(thread); g_async_queue_unref(ready);
    g_test_dbus_down(bus); g_object_unref(bus);
    puts("OK isolated desktop bus: automatic KWin fallback, independent capabilities, preview temperature, inhibition and release");
    return 0;
}
