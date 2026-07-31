#include "simplefiles-udisks.h"

#include <ctype.h>
#include <string.h>

#define UDISKS_SERVICE "org.freedesktop.UDisks2"
#define UDISKS_ROOT "/org/freedesktop/UDisks2"
#define DBUS_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define UDISKS_BLOCK "org.freedesktop.UDisks2.Block"
#define UDISKS_FILESYSTEM "org.freedesktop.UDisks2.Filesystem"
#define UDISKS_OBJECTS_TYPE "a{oa{sa{sv}}}"
#define UDISKS_CALL_TIMEOUT_MS G_MAXINT

typedef struct {
    char *device;
    char *object_path;
    GDBusConnection *connection;
    GCancellable *cancellable;
    SimpleFilesUDisksProgressCallback progress;
    SimpleFilesUDisksCompleteCallback complete;
    gpointer user_data;
    gboolean repaired;
} SimpleFilesUDisksRequest;

gboolean simplefiles_udisks_device_allowed(const char *device)
{
    const unsigned char *p;

    if (!device || strncmp(device, "/dev/", 5) != 0 || !device[5])
        return FALSE;
    for (p = (const unsigned char *)device + 5; *p; p++) {
        if (iscntrl(*p))
            return FALSE;
    }
    return strstr(device, "/../") == NULL &&
           strcmp(device + strlen(device) - 3, "/..") != 0 &&
           strstr(device, "/./") == NULL &&
           strcmp(device + strlen(device) - 2, "/.") != 0;
}

static gboolean simplefiles_udisks_device_bytes_match(GVariant *value,
                                                      const char *device)
{
    const guint8 *bytes;
    gsize count = 0;
    gsize length;

    if (!value || !device ||
        !g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING))
        return FALSE;
    bytes = g_variant_get_fixed_array(value, &count, sizeof(*bytes));
    length = strlen(device);
    return bytes && count == length + 1 && bytes[length] == '\0' &&
           memcmp(bytes, device, length) == 0;
}

gboolean simplefiles_udisks_find_filesystem_object(
    GVariant *managed_objects, const char *device, char **object_path)
{
    GVariantIter iterator;
    const char *candidate_path;
    GVariant *interfaces;

    if (object_path)
        *object_path = NULL;
    if (!managed_objects || !object_path ||
        !simplefiles_udisks_device_allowed(device) ||
        !g_variant_is_of_type(
            managed_objects, G_VARIANT_TYPE(UDISKS_OBJECTS_TYPE)))
        return FALSE;

    g_variant_iter_init(&iterator, managed_objects);
    while (g_variant_iter_next(
               &iterator, "{&o@a{sa{sv}}}", &candidate_path, &interfaces)) {
        GVariant *block_properties = g_variant_lookup_value(
            interfaces, UDISKS_BLOCK, G_VARIANT_TYPE_VARDICT);
        GVariant *filesystem_properties = g_variant_lookup_value(
            interfaces, UDISKS_FILESYSTEM, G_VARIANT_TYPE_VARDICT);
        GVariant *device_value = block_properties ?
            g_variant_lookup_value(
                block_properties, "Device", G_VARIANT_TYPE_BYTESTRING) :
            NULL;
        gboolean match =
            filesystem_properties &&
            simplefiles_udisks_device_bytes_match(device_value, device);

        g_clear_pointer(&device_value, g_variant_unref);
        g_clear_pointer(&filesystem_properties, g_variant_unref);
        g_clear_pointer(&block_properties, g_variant_unref);
        g_variant_unref(interfaces);
        if (match) {
            *object_path = g_strdup(candidate_path);
            return TRUE;
        }
    }
    return FALSE;
}

gboolean simplefiles_udisks_boolean_reply(GVariant *reply, gboolean *value)
{
    if (!reply || !value ||
        !g_variant_is_of_type(reply, G_VARIANT_TYPE("(b)")))
        return FALSE;
    g_variant_get(reply, "(b)", value);
    return TRUE;
}

static void simplefiles_udisks_request_free(SimpleFilesUDisksRequest *request)
{
    if (!request)
        return;
    g_free(request->device);
    g_free(request->object_path);
    g_clear_object(&request->connection);
    g_clear_object(&request->cancellable);
    g_free(request);
}

static void simplefiles_udisks_finish(SimpleFilesUDisksRequest *request,
                                     gboolean ready_to_mount,
                                     const char *error_detail)
{
    request->complete(
        ready_to_mount, request->repaired, error_detail, request->user_data);
    simplefiles_udisks_request_free(request);
}

static void simplefiles_udisks_report(
    SimpleFilesUDisksRequest *request, SimpleFilesUDisksStage stage)
{
    if (request->progress)
        request->progress(stage, request->user_data);
}

static GVariant *simplefiles_udisks_empty_options(void)
{
    GVariantBuilder builder;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    return g_variant_new("(a{sv})", &builder);
}

static void simplefiles_udisks_check_finished(
    GObject *source_object, GAsyncResult *result, gpointer user_data);

static void simplefiles_udisks_call_check(SimpleFilesUDisksRequest *request)
{
    g_dbus_connection_call(
        request->connection, UDISKS_SERVICE, request->object_path,
        UDISKS_FILESYSTEM, "Check", simplefiles_udisks_empty_options(),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
        UDISKS_CALL_TIMEOUT_MS, request->cancellable,
        simplefiles_udisks_check_finished, request);
}

static void simplefiles_udisks_repair_finished(
    GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    SimpleFilesUDisksRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);
    gboolean repaired = FALSE;

    if (!reply) {
        if (error)
            g_dbus_error_strip_remote_error(error);
        simplefiles_udisks_finish(
            request, FALSE,
            error ? error->message : "filesystem repair call failed");
        g_clear_error(&error);
        return;
    }
    if (!simplefiles_udisks_boolean_reply(reply, &repaired) || !repaired) {
        g_variant_unref(reply);
        simplefiles_udisks_finish(
            request, FALSE, "filesystem repair did not complete");
        return;
    }

    g_variant_unref(reply);
    request->repaired = TRUE;
    simplefiles_udisks_report(request, SIMPLEFILES_UDISKS_VERIFYING);
    simplefiles_udisks_call_check(request);
}

static void simplefiles_udisks_call_repair(
    SimpleFilesUDisksRequest *request)
{
    g_dbus_connection_call(
        request->connection, UDISKS_SERVICE, request->object_path,
        UDISKS_FILESYSTEM, "Repair", simplefiles_udisks_empty_options(),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
        UDISKS_CALL_TIMEOUT_MS, request->cancellable,
        simplefiles_udisks_repair_finished, request);
}

static void simplefiles_udisks_check_finished(
    GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    SimpleFilesUDisksRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);
    gboolean consistent = FALSE;

    if (!reply) {
        if (error)
            g_dbus_error_strip_remote_error(error);
        simplefiles_udisks_finish(
            request, FALSE,
            error ? error->message : "filesystem check call failed");
        g_clear_error(&error);
        return;
    }
    if (!simplefiles_udisks_boolean_reply(reply, &consistent)) {
        g_variant_unref(reply);
        simplefiles_udisks_finish(
            request, FALSE, "filesystem checker returned an invalid result");
        return;
    }
    g_variant_unref(reply);

    if (consistent) {
        simplefiles_udisks_finish(request, TRUE, NULL);
        return;
    }
    if (request->repaired) {
        simplefiles_udisks_finish(
            request, FALSE,
            "filesystem remains inconsistent after repair");
        return;
    }

    simplefiles_udisks_report(request, SIMPLEFILES_UDISKS_REPAIRING);
    simplefiles_udisks_call_repair(request);
}

static void simplefiles_udisks_objects_finished(
    GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    SimpleFilesUDisksRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);
    GVariant *objects = NULL;

    if (!reply) {
        if (error)
            g_dbus_error_strip_remote_error(error);
        simplefiles_udisks_finish(
            request, FALSE,
            error ? error->message : "could not inspect storage devices");
        g_clear_error(&error);
        return;
    }
    if (!g_variant_is_of_type(
            reply, G_VARIANT_TYPE("(a{oa{sa{sv}}})"))) {
        g_variant_unref(reply);
        simplefiles_udisks_finish(
            request, FALSE, "storage service returned an invalid inventory");
        return;
    }
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    if (!simplefiles_udisks_find_filesystem_object(
            objects, request->device, &request->object_path)) {
        g_variant_unref(objects);
        g_variant_unref(reply);
        simplefiles_udisks_finish(
            request, FALSE,
            "drive has no matching UDisks filesystem object");
        return;
    }
    g_variant_unref(objects);
    g_variant_unref(reply);

    simplefiles_udisks_report(request, SIMPLEFILES_UDISKS_CHECKING);
    simplefiles_udisks_call_check(request);
}

static void simplefiles_udisks_bus_finished(
    GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    SimpleFilesUDisksRequest *request = user_data;
    GError *error = NULL;

    (void)source_object;
    request->connection = g_bus_get_finish(result, &error);
    if (!request->connection) {
        simplefiles_udisks_finish(
            request, FALSE,
            error ? error->message : "could not connect to system storage");
        g_clear_error(&error);
        return;
    }

    g_dbus_connection_call(
        request->connection, UDISKS_SERVICE, UDISKS_ROOT,
        DBUS_OBJECT_MANAGER, "GetManagedObjects", NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
        30000, request->cancellable,
        simplefiles_udisks_objects_finished, request);
}

gboolean simplefiles_udisks_preflight_start(
    const char *device, GCancellable *cancellable,
    SimpleFilesUDisksProgressCallback progress,
    SimpleFilesUDisksCompleteCallback complete, gpointer user_data)
{
    SimpleFilesUDisksRequest *request;

    if (!simplefiles_udisks_device_allowed(device) || !complete)
        return FALSE;
    request = g_new0(SimpleFilesUDisksRequest, 1);
    request->device = g_strdup(device);
    request->cancellable = cancellable ?
        g_object_ref(cancellable) : g_cancellable_new();
    request->progress = progress;
    request->complete = complete;
    request->user_data = user_data;

    simplefiles_udisks_report(request, SIMPLEFILES_UDISKS_LOCATING);
    g_bus_get(G_BUS_TYPE_SYSTEM, request->cancellable,
              simplefiles_udisks_bus_finished, request);
    return TRUE;
}
