#include "../simplefiles-udisks.h"

#include <assert.h>
#include <string.h>

#define BLOCK_INTERFACE "org.freedesktop.UDisks2.Block"
#define FILESYSTEM_INTERFACE "org.freedesktop.UDisks2.Filesystem"
#define UDISKS_SERVICE "org.freedesktop.UDisks2"
#define UDISKS_ROOT "/org/freedesktop/UDisks2"
#define UDISKS_DEVICE_OBJECT \
    "/org/freedesktop/UDisks2/block_devices/sdb1"

typedef enum {
    MOCK_CLEAN = 1,
    MOCK_DIRTY_REPAIRABLE,
    MOCK_DIRTY_UNREPAIRABLE
} MockMode;

typedef struct {
    MockMode mode;
    int check_calls;
    int repair_calls;
} MockService;

typedef struct {
    GMainLoop *loop;
    gboolean completed;
    gboolean ready;
    gboolean repaired;
    char *error;
    SimpleFilesUDisksStage stages[8];
    int stage_count;
} PreflightResult;

static GVariant *make_block_properties(const char *device)
{
    GVariantBuilder properties;
    GVariant *bytes = g_variant_new_fixed_array(
        G_VARIANT_TYPE_BYTE, device, strlen(device) + 1, 1);

    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&properties, "{sv}", "Device", bytes);
    return g_variant_builder_end(&properties);
}

static GVariant *make_interfaces(const char *device, gboolean filesystem)
{
    GVariantBuilder interfaces;
    GVariantBuilder filesystem_properties;

    g_variant_builder_init(
        &interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_add(
        &interfaces, "{s@a{sv}}", BLOCK_INTERFACE,
        make_block_properties(device));
    if (filesystem) {
        g_variant_builder_init(
            &filesystem_properties, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(
            &interfaces, "{s@a{sv}}", FILESYSTEM_INTERFACE,
            g_variant_builder_end(&filesystem_properties));
    }
    return g_variant_builder_end(&interfaces);
}

static GVariant *make_managed_objects(const char *device)
{
    GVariantBuilder objects;

    g_variant_builder_init(
        &objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    g_variant_builder_add(
        &objects, "{o@a{sa{sv}}}", UDISKS_DEVICE_OBJECT,
        make_interfaces(device, TRUE));
    return g_variant_builder_end(&objects);
}

static void mock_method_call(GDBusConnection *connection, const char *sender,
                             const char *object_path,
                             const char *interface_name,
                             const char *method_name, GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer user_data)
{
    MockService *service = user_data;

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)parameters;
    if (strcmp(interface_name, "org.freedesktop.DBus.ObjectManager") == 0 &&
        strcmp(method_name, "GetManagedObjects") == 0) {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new(
                "(@a{oa{sa{sv}}})", make_managed_objects("/dev/sdb1")));
        return;
    }
    if (strcmp(interface_name, FILESYSTEM_INTERFACE) == 0 &&
        strcmp(method_name, "Check") == 0) {
        gboolean clean;

        service->check_calls++;
        clean = service->mode == MOCK_CLEAN ||
                (service->mode == MOCK_DIRTY_REPAIRABLE &&
                 service->repair_calls > 0);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(b)", clean));
        return;
    }
    if (strcmp(interface_name, FILESYSTEM_INTERFACE) == 0 &&
        strcmp(method_name, "Repair") == 0) {
        service->repair_calls++;
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new(
                "(b)", service->mode == MOCK_DIRTY_REPAIRABLE));
        return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod",
        "unexpected mock method");
}

static const GDBusInterfaceVTable mock_vtable = {
    .method_call = mock_method_call
};

static void preflight_progress(SimpleFilesUDisksStage stage,
                               gpointer user_data)
{
    PreflightResult *result = user_data;

    assert(result->stage_count <
           (int)(sizeof(result->stages) / sizeof(result->stages[0])));
    result->stages[result->stage_count++] = stage;
}

static void preflight_complete(gboolean ready, gboolean repaired,
                               const char *error, gpointer user_data)
{
    PreflightResult *result = user_data;

    result->completed = TRUE;
    result->ready = ready;
    result->repaired = repaired;
    result->error = g_strdup(error);
    g_main_loop_quit(result->loop);
}

static gboolean preflight_timeout(gpointer user_data)
{
    PreflightResult *result = user_data;

    g_main_loop_quit(result->loop);
    return G_SOURCE_REMOVE;
}

static PreflightResult run_preflight(const char *device)
{
    PreflightResult result = {0};
    GCancellable *cancellable = g_cancellable_new();
    guint timeout;

    result.loop = g_main_loop_new(NULL, FALSE);
    assert(simplefiles_udisks_preflight_start(
        device, cancellable, preflight_progress, preflight_complete, &result));
    timeout = g_timeout_add_seconds(5, preflight_timeout, &result);
    g_main_loop_run(result.loop);
    if (result.completed)
        assert(g_source_remove(timeout));
    g_main_loop_unref(result.loop);
    g_object_unref(cancellable);
    assert(result.completed);
    return result;
}

static void test_async_preflight(void)
{
    static const char introspection_xml[] =
        "<node>"
        " <interface name='org.freedesktop.DBus.ObjectManager'>"
        "  <method name='GetManagedObjects'>"
        "   <arg type='a{oa{sa{sv}}}' direction='out'/>"
        "  </method>"
        " </interface>"
        " <interface name='org.freedesktop.UDisks2.Filesystem'>"
        "  <method name='Check'>"
        "   <arg type='a{sv}' direction='in'/>"
        "   <arg type='b' direction='out'/>"
        "  </method>"
        "  <method name='Repair'>"
        "   <arg type='a{sv}' direction='in'/>"
        "   <arg type='b' direction='out'/>"
        "  </method>"
        " </interface>"
        "</node>";
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    GDBusConnection *service_connection;
    GDBusConnection *client_connection;
    GDBusNodeInfo *node;
    GVariant *request_name_reply;
    GError *error = NULL;
    MockService service = {0};
    guint root_registration;
    guint filesystem_registration;
    PreflightResult result;
    char *dbus_daemon = g_find_program_in_path("dbus-daemon");

    if (!dbus_daemon) {
        g_object_unref(test_bus);
        return;
    }
    g_free(dbus_daemon);
    g_test_dbus_up(test_bus);
    assert(g_setenv("DBUS_SYSTEM_BUS_ADDRESS",
                    g_test_dbus_get_bus_address(test_bus), TRUE));
    service_connection = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(test_bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    assert(service_connection);
    assert(!error);
    request_name_reply = g_dbus_connection_call_sync(
        service_connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", UDISKS_SERVICE, 0U),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    assert(request_name_reply);
    assert(!error);
    g_variant_unref(request_name_reply);

    node = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    assert(node);
    assert(!error);
    root_registration = g_dbus_connection_register_object(
        service_connection, UDISKS_ROOT,
        g_dbus_node_info_lookup_interface(
            node, "org.freedesktop.DBus.ObjectManager"),
        &mock_vtable, &service, NULL, &error);
    assert(root_registration != 0);
    assert(!error);
    filesystem_registration = g_dbus_connection_register_object(
        service_connection, UDISKS_DEVICE_OBJECT,
        g_dbus_node_info_lookup_interface(node, FILESYSTEM_INTERFACE),
        &mock_vtable, &service, NULL, &error);
    assert(filesystem_registration != 0);
    assert(!error);

    service.mode = MOCK_CLEAN;
    result = run_preflight("/dev/sdb1");
    assert(result.ready);
    assert(!result.repaired);
    assert(result.error == NULL);
    assert(service.check_calls == 1);
    assert(service.repair_calls == 0);
    assert(result.stage_count == 2);
    assert(result.stages[0] == SIMPLEFILES_UDISKS_LOCATING);
    assert(result.stages[1] == SIMPLEFILES_UDISKS_CHECKING);

    memset(&service, 0, sizeof(service));
    service.mode = MOCK_DIRTY_REPAIRABLE;
    result = run_preflight("/dev/sdb1");
    assert(result.ready);
    assert(result.repaired);
    assert(result.error == NULL);
    assert(service.check_calls == 2);
    assert(service.repair_calls == 1);
    assert(result.stage_count == 4);
    assert(result.stages[0] == SIMPLEFILES_UDISKS_LOCATING);
    assert(result.stages[1] == SIMPLEFILES_UDISKS_CHECKING);
    assert(result.stages[2] == SIMPLEFILES_UDISKS_REPAIRING);
    assert(result.stages[3] == SIMPLEFILES_UDISKS_VERIFYING);

    memset(&service, 0, sizeof(service));
    service.mode = MOCK_DIRTY_UNREPAIRABLE;
    result = run_preflight("/dev/sdb1");
    assert(!result.ready);
    assert(!result.repaired);
    assert(result.error);
    assert(strstr(result.error, "repair did not complete"));
    assert(service.check_calls == 1);
    assert(service.repair_calls == 1);
    g_free(result.error);

    memset(&service, 0, sizeof(service));
    service.mode = MOCK_CLEAN;
    result = run_preflight("/dev/sdc1");
    assert(!result.ready);
    assert(!result.repaired);
    assert(result.error);
    assert(strstr(result.error, "no matching UDisks"));
    assert(service.check_calls == 0);
    assert(service.repair_calls == 0);
    g_free(result.error);

    assert(g_dbus_connection_unregister_object(
        service_connection, filesystem_registration));
    assert(g_dbus_connection_unregister_object(
        service_connection, root_registration));
    client_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    assert(client_connection);
    assert(!error);
    assert(g_dbus_connection_close_sync(
        client_connection, NULL, &error));
    assert(!error);
    g_object_unref(client_connection);
    assert(g_dbus_connection_close_sync(
        service_connection, NULL, &error));
    assert(!error);
    g_object_unref(service_connection);
    g_dbus_node_info_unref(node);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

int main(void)
{
    GVariantBuilder objects;
    GVariant *managed_objects;
    GVariant *reply;
    char *path = NULL;
    gboolean value = FALSE;

    assert(simplefiles_udisks_device_allowed("/dev/sdb1"));
    assert(simplefiles_udisks_device_allowed("/dev/mapper/portable"));
    assert(!simplefiles_udisks_device_allowed("/tmp/sdb1"));
    assert(!simplefiles_udisks_device_allowed("/dev/../etc/passwd"));
    assert(!simplefiles_udisks_device_allowed("/dev/disk/../sdb1"));
    assert(!simplefiles_udisks_device_allowed("/dev/sdb1\n"));

    g_variant_builder_init(
        &objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    g_variant_builder_add(
        &objects, "{o@a{sa{sv}}}",
        "/org/freedesktop/UDisks2/block_devices/sdb",
        make_interfaces("/dev/sdb", FALSE));
    g_variant_builder_add(
        &objects, "{o@a{sa{sv}}}",
        "/org/freedesktop/UDisks2/block_devices/sdb10",
        make_interfaces("/dev/sdb10", TRUE));
    g_variant_builder_add(
        &objects, "{o@a{sa{sv}}}",
        "/org/freedesktop/UDisks2/block_devices/sdb1",
        make_interfaces("/dev/sdb1", TRUE));
    managed_objects = g_variant_ref_sink(g_variant_builder_end(&objects));

    assert(simplefiles_udisks_find_filesystem_object(
        managed_objects, "/dev/sdb1", &path));
    assert(strcmp(
        path, "/org/freedesktop/UDisks2/block_devices/sdb1") == 0);
    g_free(path);
    path = NULL;
    assert(!simplefiles_udisks_find_filesystem_object(
        managed_objects, "/dev/sdb", &path));
    assert(path == NULL);
    assert(!simplefiles_udisks_find_filesystem_object(
        managed_objects, "/dev/sdc1", &path));
    assert(path == NULL);
    g_variant_unref(managed_objects);

    reply = g_variant_ref_sink(g_variant_new("(b)", TRUE));
    assert(simplefiles_udisks_boolean_reply(reply, &value));
    assert(value);
    g_variant_unref(reply);

    reply = g_variant_ref_sink(g_variant_new("(s)", "bad"));
    assert(!simplefiles_udisks_boolean_reply(reply, &value));
    g_variant_unref(reply);

    test_async_preflight();
    return 0;
}
