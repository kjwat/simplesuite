#ifndef SIMPLEFILES_UDISKS_H
#define SIMPLEFILES_UDISKS_H

#include <gio/gio.h>

typedef enum {
    SIMPLEFILES_UDISKS_LOCATING = 1,
    SIMPLEFILES_UDISKS_CHECKING,
    SIMPLEFILES_UDISKS_REPAIRING,
    SIMPLEFILES_UDISKS_VERIFYING
} SimpleFilesUDisksStage;

typedef void (*SimpleFilesUDisksProgressCallback)(
    SimpleFilesUDisksStage stage, gpointer user_data);
typedef void (*SimpleFilesUDisksCompleteCallback)(
    gboolean ready_to_mount, gboolean repaired, const char *error_detail,
    gpointer user_data);

gboolean simplefiles_udisks_device_allowed(const char *device);
gboolean simplefiles_udisks_find_filesystem_object(
    GVariant *managed_objects, const char *device, char **object_path);
gboolean simplefiles_udisks_boolean_reply(GVariant *reply, gboolean *value);

/*
 * Check an unmounted Linux filesystem through UDisks2.  A clean filesystem
 * completes immediately; an inconsistent one is repaired and checked again.
 * The callback reports success only after a clean check.  error_detail is
 * valid only for the duration of the completion callback.
 */
gboolean simplefiles_udisks_preflight_start(
    const char *device, GCancellable *cancellable,
    SimpleFilesUDisksProgressCallback progress,
    SimpleFilesUDisksCompleteCallback complete, gpointer user_data);

#endif
