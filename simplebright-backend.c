/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "simplebright-backend.h"
#include "simplebright-filter.h"
#include <gio/gio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SIMPLEBRIGHT_BACKLIGHT_DIR
#define SIMPLEBRIGHT_BACKLIGHT_DIR "/sys/class/backlight"
#endif

int sb_percent(unsigned value, unsigned maximum)
{
    return maximum ? (int)(((uint64_t)value * 100 + maximum / 2) / maximum) : 0;
}
unsigned sb_raw(int percent, unsigned maximum)
{
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    uint64_t raw = ((uint64_t)percent * maximum + 50) / 100;
    return (unsigned)(raw ? raw : 1);
}

static int number_file(const char *device, const char *attribute, unsigned *value)
{
    char path[PATH_MAX], text[80], *end;
    if (snprintf(path, sizeof(path), "%s/%s/%s", SIMPLEBRIGHT_BACKLIGHT_DIR, device, attribute) >= (int)sizeof(path)) return -1;
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int ok = fgets(text, sizeof(text), file) != NULL;
    fclose(file);
    if (!ok || text[0] == '-') return -1;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    while (*end == ' ' || *end == '\n' || *end == '\t') end++;
    if (errno || end == text || *end || n > UINT_MAX) return -1;
    *value = (unsigned)n;
    return 0;
}

/* Bounded, shell-free helper execution. DDC transactions never block the UI. */
static int command(const char *const argv[], char *output, size_t capacity, int milliseconds)
{
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) < 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    gchar **environment = g_get_environ();
    environment = g_environ_setenv(environment, "LC_ALL", "C", TRUE);
    pid_t pid;
    int result = posix_spawnp(&pid, argv[0], &actions, NULL, (char *const *)argv, environment);
    g_strfreev(environment);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (result) { close(pipes[0]); if (capacity) output[0] = 0; return -1; }
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    size_t used = 0;
    int status = 0, finished = 0, eof = 0;
    gint64 end = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    while (!finished || !eof) {
        char buffer[2048];
        ssize_t n;
        while ((n = read(pipes[0], buffer, sizeof(buffer))) > 0) {
            size_t copy = capacity > used + 1 ? MIN((size_t)n, capacity - used - 1) : 0;
            if (copy) { memcpy(output + used, buffer, copy); used += copy; }
        }
        if (n == 0) eof = 1;
        if (!finished && waitpid(pid, &status, WNOHANG) == pid) finished = 1;
        if (finished && eof) break;
        if (g_get_monotonic_time() >= end) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            result = -1; break;
        }
        struct pollfd fd = {pipes[0], POLLIN, 0};
        poll(&fd, 1, 20);
    }
    close(pipes[0]);
    if (capacity) output[used] = 0;
    return result == -1 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int ddc_read(SbDevice *device, char *error, size_t size)
{
    char bus[24], output[4096];
    snprintf(bus, sizeof(bus), "%d", device->bus);
    const char *argv[] = {"ddcutil", "--bus", bus, "--terse", "getvcp", "10", NULL};
    if (command(argv, output, sizeof(output), 5000)) goto failed;
    for (char *line = output; line && *line;) {
        unsigned current, maximum;
        if (sscanf(line, "VCP 10 C %u %u", &current, &maximum) == 2 && maximum && current <= maximum) {
            device->current = current; device->maximum = maximum;
            device->percent = sb_percent(current, maximum); return 0;
        }
        line = strchr(line, '\n'); if (line) line++;
    }
failed:
    snprintf(error, size, "Cannot read %s. Enable DDC/CI in the monitor and check I2C permissions.", device->name);
    return -1;
}

int sb_read(SbDevice *device, char *error, size_t size)
{
    if (device->bus >= 0) return ddc_read(device, error, size);
    if (number_file(device->id, "max_brightness", &device->maximum) || !device->maximum ||
        number_file(device->id, "brightness", &device->current) || device->current > device->maximum) {
        snprintf(error, size, "Backlight %s is unavailable; press R to rescan.", device->id); return -1;
    }
    device->percent = sb_percent(device->current, device->maximum);
    return 0;
}

int sb_scan(SbDevice devices[SB_DEVICES], char *error, size_t size)
{
    int count = 0;
    struct dirent **entries = NULL;
    int n = scandir(SIMPLEBRIGHT_BACKLIGHT_DIR, &entries, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        const char *name = entries[i]->d_name;
        if (name[0] != '.' && strlen(name) < sizeof(devices[0].id) && count < SB_DEVICES) {
            SbDevice *d = &devices[count]; memset(d, 0, sizeof(*d));
            g_strlcpy(d->id, name, sizeof(d->id));
            g_strlcpy(d->name, name, sizeof(d->name));
            g_strlcpy(d->backend, "Backlight", sizeof(d->backend)); d->bus = -1;
            char ignored[SB_TEXT];
            if (!sb_read(d, ignored, sizeof(ignored))) count++;
        }
        free(entries[i]);
    }
    free(entries);
    char output[16384];
    const char *argv[] = {"ddcutil", "detect", "--brief", NULL};
    command(argv, output, sizeof(output), 8000);
    int valid = 0, bus = -1;
    for (char *save = NULL, *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "Display ", 8)) { valid = 1; bus = -1; }
        else if (strstr(line, "Invalid display") || strstr(line, "Phantom display")) { valid = 0; bus = -1; }
        else if (valid && sscanf(line, "I2C bus: /dev/i2c-%d", &bus) == 1) {}
        else if (valid && bus >= 0 && !strncmp(line, "Monitor:", 8) && count < SB_DEVICES) {
            SbDevice *d = &devices[count]; memset(d, 0, sizeof(*d));
            snprintf(d->id, sizeof(d->id), "ddc:%d", bus); d->bus = bus;
            char *name = line + 8; while (*name == ' ') name++;
            g_strlcpy(d->name, name, sizeof(d->name)); g_strlcpy(d->backend, "DDC/CI", sizeof(d->backend));
            char ignored[SB_TEXT]; if (!sb_read(d, ignored, sizeof(ignored))) count++;
            valid = 0;
        }
    }
    if (!count) snprintf(error, size, "No brightness device found. Press ? for setup help or R to rescan.");
    return count;
}

static GVariant *bus_call(GDBusConnection *bus, const char *name, const char *path, const char *interface,
                          const char *method, GVariant *args, const GVariantType *reply)
{
    if (!bus) return NULL;
    return g_dbus_connection_call_sync(bus, name, path, interface, method, args, reply,
                                      G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, NULL, NULL);
}

static int logind_set(const char *device, unsigned raw)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!bus) return -1;
    GVariant *reply = bus_call(bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                              "GetSessionByPID", g_variant_new("(u)", (guint)getpid()), G_VARIANT_TYPE("(o)"));
    /* Applications launched by the user service manager may be outside the
     * login session's cgroup. Resolve that user's active local seat as well. */
    if (!reply) {
        GVariant *list = bus_call(bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                 "ListSessions", NULL, G_VARIANT_TYPE("(a(susso))"));
        if (list) {
            GVariantIter *iter; g_variant_get(list, "(a(susso))", &iter);
            const char *id, *user, *seat, *path; guint uid;
            while (g_variant_iter_loop(iter, "(&su&s&s&o)", &id, &uid, &user, &seat, &path)) {
                if (uid != getuid() || !*seat) continue;
                GVariant *properties = bus_call(bus, "org.freedesktop.login1", path, "org.freedesktop.DBus.Properties", "GetAll",
                    g_variant_new("(s)", "org.freedesktop.login1.Session"), G_VARIANT_TYPE("(a{sv})"));
                if (!properties) continue;
                GVariant *map; gboolean active = FALSE, remote = TRUE;
                g_variant_get(properties, "(@a{sv})", &map);
                g_variant_lookup(map, "Active", "b", &active); g_variant_lookup(map, "Remote", "b", &remote);
                if (active && !remote) reply = g_variant_ref_sink(g_variant_new("(o)", path));
                g_variant_unref(map); g_variant_unref(properties);
                if (reply) break;
            }
            g_variant_iter_free(iter); g_variant_unref(list);
        }
    }
    int ok = 0;
    if (reply) {
        const char *path; g_variant_get(reply, "(&o)", &path);
        GVariant *set = bus_call(bus, "org.freedesktop.login1", path, "org.freedesktop.login1.Session", "SetBrightness",
                                g_variant_new("(ssu)", "backlight", device, raw), NULL);
        ok = set != NULL;
        if (set) g_variant_unref(set);
        g_variant_unref(reply);
    }
    g_object_unref(bus); return ok ? 0 : -1;
}

int sb_set(SbDevice *device, int percent, char *error, size_t size)
{
    if (sb_read(device, error, size)) return -1;
    unsigned raw = sb_raw(percent, device->maximum);
    char output[1024], value[32]; snprintf(value, sizeof(value), "%u", raw);
    if (device->bus >= 0) {
        char bus[24]; snprintf(bus, sizeof(bus), "%d", device->bus);
        const char *argv[] = {"ddcutil", "--bus", bus, "setvcp", "10", value, NULL};
        if (command(argv, output, sizeof(output), 5000)) {
            snprintf(error, size, "Monitor rejected brightness. Check its DDC/CI setting and I2C access."); return -1;
        }
    } else {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s/brightness", SIMPLEBRIGHT_BACKLIGHT_DIR, device->id);
        int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
        int written = fd >= 0 && write(fd, value, strlen(value)) == (ssize_t)strlen(value);
        if (fd >= 0) close(fd);
        if (!written && logind_set(device->id, raw)) {
            const char *argv[] = {"brightnessctl", "--class=backlight", "--device", device->id, "set", value, NULL};
            if (command(argv, output, sizeof(output), 3000)) {
                snprintf(error, size, "No permission for %s. Use an active local session or brightnessctl's udev rules.", device->id); return -1;
            }
        }
    }
    return sb_read(device, error, size);
}

static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "simplebright", "settings.ini", NULL);
}
static GKeyFile *settings_load(void)
{
    GKeyFile *file = g_key_file_new(); char *path = settings_path();
    g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL); g_free(path); return file;
}
static void settings_save(GKeyFile *file)
{
    char *path = settings_path(), *dir = g_path_get_dirname(path);
    if (!g_mkdir_with_parents(dir, 0700)) {
        gsize length; char *data = g_key_file_to_data(file, &length, NULL);
        g_file_set_contents(path, data, (gssize)length, NULL); g_free(data);
    }
    g_free(dir); g_free(path);
}
int sb_saved_temperature(void)
{
    GKeyFile *file = settings_load(); int n = g_key_file_get_integer(file, "Simplebright", "temperature", NULL);
    g_key_file_unref(file); return n >= 1000 && n <= 6500 ? n : 4000;
}
void sb_save_temperature(int kelvin)
{
    GKeyFile *file = settings_load(); g_key_file_set_integer(file, "Simplebright", "temperature", kelvin);
    settings_save(file); g_key_file_unref(file);
}

static int method_is(GDBusInterfaceInfo *info, const char *name, const char *input, const char *output)
{
    GDBusMethodInfo *method = g_dbus_interface_info_lookup_method(info, name);
    if (!method) return 0;
    char in[128] = "", out[128] = "";
    for (int i = 0; method->in_args && method->in_args[i]; i++) g_strlcat(in, method->in_args[i]->signature, sizeof(in));
    for (int i = 0; method->out_args && method->out_args[i]; i++) g_strlcat(out, method->out_args[i]->signature, sizeof(out));
    return !strcmp(input, in) && !strcmp(output, out);
}
void sb_dbus_capabilities(const char *xml, const char *interface, int *toggle, int *temp)
{
    *toggle = *temp = 0;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    if (!node) return;
    GDBusInterfaceInfo *info = g_dbus_node_info_lookup_interface(node, interface);
    if (info) {
        *toggle = method_is(info, "inhibit", "", "u") && method_is(info, "uninhibit", "u", "");
        *temp = method_is(info, "preview", "u", "") && method_is(info, "stopPreview", "", "");
        /* Older, public KWin API exposes configuration through a typed map. */
        if (method_is(info, "changeConfiguration", "a{sv}", "b") && method_is(info, "nightColorInfo", "", "a{sv}"))
            *toggle = *temp = 2;
    }
    g_dbus_node_info_unref(node);
}

typedef struct {
    const char *name, *path, *interface, *schema, *label;
} Desktop;
static const Desktop desktops[] = {
    {"org.gnome.SettingsDaemon.Color", "/org/gnome/SettingsDaemon/Color", "org.gnome.SettingsDaemon.Color", "org.gnome.settings-daemon.plugins.color", "GNOME Night Light"},
    {"org.cinnamon.SettingsDaemon.Color", "/org/cinnamon/SettingsDaemon/Color", "org.cinnamon.SettingsDaemon.Color", "org.cinnamon.settings-daemon.plugins.color", "Cinnamon Night Light"},
    {"org.kde.KWin.NightLight", "/org/kde/KWin/NightLight", "org.kde.KWin.NightLight", NULL, "KDE Night Color"},
    {"org.kde.KWin", "/org/kde/KWin/NightLight", "org.kde.KWin.NightLight", NULL, "KDE Night Color"},
    {"org.kde.KWin", "/ColorCorrect", "org.kde.kwin.ColorCorrect", NULL, "KDE Night Color"},
};
static GDBusConnection *session_bus;
static GSettings *desktop_settings;
static const Desktop *desktop;
static int direct_available, direct_open, kde_legacy, preview_supported;
static guint inhibit_cookie;
static int has_inhibit;
static SbNight daemon_night;

static GVariant *desktop_call(const char *method, GVariant *args)
{
    return bus_call(session_bus, desktop->name, desktop->path, desktop->interface, method, args, NULL);
}
static GVariant *property(const char *name)
{
    GVariant *reply = bus_call(session_bus, desktop->name, desktop->path, "org.freedesktop.DBus.Properties", "Get",
                              g_variant_new("(ss)", desktop->interface, name), G_VARIANT_TYPE("(v)"));
    if (!reply) return NULL;
    GVariant *value; g_variant_get(reply, "(v)", &value); g_variant_unref(reply); return value;
}

static void desktop_detect(void)
{
    if (desktop_settings) g_clear_object(&desktop_settings);
    desktop = NULL; kde_legacy = preview_supported = 0;
    daemon_night.can_toggle = daemon_night.can_temperature = 0;
    if (!session_bus) session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(desktops); i++) {
        const Desktop *d = &desktops[i];
        GVariant *reply = bus_call(session_bus, d->name, d->path, "org.freedesktop.DBus.Introspectable", "Introspect", NULL, G_VARIANT_TYPE("(s)"));
        if (!reply) continue;
        const char *xml; g_variant_get(reply, "(&s)", &xml);
        if (d->schema) {
            GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
            int interface_exists = node && g_dbus_node_info_lookup_interface(node, d->interface);
            if (node) g_dbus_node_info_unref(node);
            if (!interface_exists) { g_variant_unref(reply); continue; }
            GSettingsSchemaSource *source = g_settings_schema_source_get_default();
            GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source, d->schema, TRUE) : NULL;
            if (schema) {
                int toggle = g_settings_schema_has_key(schema, "night-light-enabled");
                int temp = g_settings_schema_has_key(schema, "night-light-temperature");
                GSettings *settings = g_settings_new_full(schema, NULL, NULL);
                daemon_night.can_toggle = toggle && g_settings_is_writable(settings, "night-light-enabled");
                daemon_night.can_temperature = temp && g_settings_is_writable(settings, "night-light-temperature");
                const char *schedule[] = {"night-light-schedule-automatic", "night-light-schedule-from", "night-light-schedule-to"};
                for (int k = 0; k < 3; k++) if (!g_settings_schema_has_key(schema, schedule[k]) || !g_settings_is_writable(settings, schedule[k]))
                    daemon_night.can_toggle = 0;
                if (daemon_night.can_toggle || daemon_night.can_temperature) {
                    desktop_settings = settings; desktop = d;
                    if (toggle) daemon_night.enabled = g_settings_get_boolean(settings, "night-light-enabled");
                    if (temp) daemon_night.kelvin = (int)g_settings_get_uint(settings, "night-light-temperature");
                } else g_object_unref(settings);
                g_settings_schema_unref(schema);
            }
        } else {
            int toggle, temp; sb_dbus_capabilities(xml, d->interface, &toggle, &temp);
            if (toggle || temp) {
                desktop = d; kde_legacy = toggle == 2; preview_supported = temp == 1;
                daemon_night.can_toggle = toggle != 0;
                daemon_night.can_temperature = temp != 0;
                if (kde_legacy) {
                    GVariant *configuration = desktop_call("nightColorInfo", NULL), *map = NULL;
                    if (configuration) g_variant_get(configuration, "(@a{sv})", &map);
                    gboolean toggle_ok = FALSE, temp_ok = FALSE, mode_ok = FALSE;
                    gint32 kelvin;
                    if (map) {
                        g_variant_lookup(map, "ActiveEnabled", "b", &toggle_ok);
                        g_variant_lookup(map, "ModeEnabled", "b", &mode_ok);
                        g_variant_lookup(map, "NightTemperatureEnabled", "b", &temp_ok);
                        if (g_variant_lookup(map, "NightTemperature", "i", &kelvin)) daemon_night.kelvin = kelvin;
                        g_variant_unref(map);
                    }
                    if (configuration) g_variant_unref(configuration);
                    daemon_night.can_toggle = toggle_ok && mode_ok;
                    daemon_night.can_temperature = temp_ok;
                } else {
                    GVariant *available = property("available");
                    if (available && g_variant_is_of_type(available, G_VARIANT_TYPE_BOOLEAN) && !g_variant_get_boolean(available))
                        daemon_night.can_toggle = daemon_night.can_temperature = 0;
                    if (available) g_variant_unref(available);
                    if (!preview_supported) {
                        GVariant *configured = property("enabled");
                        if (!configured || !g_variant_is_of_type(configured, G_VARIANT_TYPE_BOOLEAN) || !g_variant_get_boolean(configured)) daemon_night.can_toggle = 0;
                        if (configured) g_variant_unref(configured);
                    }
                }
                GVariant *active = property("running");
                if (active && g_variant_is_of_type(active, G_VARIANT_TYPE_BOOLEAN)) daemon_night.enabled = g_variant_get_boolean(active);
                if (active) g_variant_unref(active);
            }
        }
        g_variant_unref(reply);
        if (desktop) { g_strlcpy(daemon_night.backend, d->label, sizeof(daemon_night.backend)); return; }
    }
    g_strlcpy(daemon_night.backend, "Unavailable", sizeof(daemon_night.backend));
}

static int settings_key(const char *key)
{
    GSettingsSchema *schema = NULL;
    g_object_get(desktop_settings, "settings-schema", &schema, NULL);
    int ok = schema && g_settings_schema_has_key(schema, key);
    if (schema) g_settings_schema_unref(schema);
    return ok && g_settings_is_writable(desktop_settings, key);
}

static int settings_temperature(int kelvin)
{
    if (!daemon_night.can_temperature) return -1;
    g_settings_delay(desktop_settings);
    if (!g_settings_set_uint(desktop_settings, "night-light-temperature", (guint)kelvin)) return -1;
    g_settings_apply(desktop_settings); g_settings_sync(); return 0;
}

static int settings_apply(int enabled, int kelvin, char *error, size_t size)
{
    const char *schedule[] = {"night-light-schedule-automatic", "night-light-schedule-from", "night-light-schedule-to"};
    GKeyFile *file = settings_load();
    if (enabled) {
        /* The documented schedule keys provide manual all-day activation.
         * Save their typed values and restore them when N turns the filter off. */
        int schedule_ok = 1;
        for (int i = 0; i < 3; i++) if (!settings_key(schedule[i])) schedule_ok = 0;
        if (!schedule_ok) {
            snprintf(error, size, "This service allows configuration but cannot activate night light immediately.");
            g_key_file_unref(file); return -1;
        }
        if (!g_key_file_has_group(file, desktop->schema)) {
            for (int i = 0; i < 3; i++) {
                GVariant *value = g_settings_get_value(desktop_settings, schedule[i]);
                char *text = g_variant_print(value, TRUE);
                g_key_file_set_string(file, desktop->schema, schedule[i], text);
                g_free(text); g_variant_unref(value);
            }
            settings_save(file);
        }
    }
    g_settings_delay(desktop_settings);
    if (daemon_night.can_temperature) g_settings_set_uint(desktop_settings, "night-light-temperature", (guint)kelvin);
    if (daemon_night.can_toggle) g_settings_set_boolean(desktop_settings, "night-light-enabled", enabled);
    if (enabled) {
        g_settings_set_boolean(desktop_settings, schedule[0], FALSE);
        g_settings_set_double(desktop_settings, schedule[1], 0.0);
        g_settings_set_double(desktop_settings, schedule[2], 24.0);
    } else if (g_key_file_has_group(file, desktop->schema)) {
        for (int i = 0; i < 3; i++) {
            char *text = g_key_file_get_string(file, desktop->schema, schedule[i], NULL);
            GVariant *value = text ? g_variant_parse(NULL, text, NULL, NULL, NULL) : NULL;
            if (value && settings_key(schedule[i])) g_settings_set_value(desktop_settings, schedule[i], value);
            if (value) g_variant_unref(value);
            g_free(text);
        }
        g_key_file_remove_group(file, desktop->schema, NULL);
    }
    g_settings_apply(desktop_settings); g_settings_sync();
    if (enabled) {
        GVariant *reply = bus_call(session_bus, desktop->name, desktop->path, "org.freedesktop.DBus.Properties", "Set",
                                  g_variant_new("(ssv)", desktop->interface, "DisabledUntilTomorrow", g_variant_new_boolean(FALSE)), NULL);
        if (reply) g_variant_unref(reply);
    }
    settings_save(file); g_key_file_unref(file);
    daemon_night.enabled = enabled; daemon_night.kelvin = kelvin;
    return 0;
}

static int kde_apply(int enabled, int kelvin, char *error, size_t size)
{
    GVariant *reply = NULL;
    if (kde_legacy) {
        GKeyFile *saved = settings_load();
        const char *group = "KWin schedule";
        if (enabled && !g_key_file_has_key(saved, group, "Mode", NULL)) {
            GVariant *configuration = desktop_call("nightColorInfo", NULL), *map = NULL;
            gint32 mode;
            if (configuration) {
                g_variant_get(configuration, "(@a{sv})", &map);
                if (g_variant_lookup(map, "Mode", "i", &mode)) {
                    g_key_file_set_integer(saved, group, "Mode", mode); settings_save(saved);
                }
                g_variant_unref(map); g_variant_unref(configuration);
            }
        }
        GVariantBuilder builder; g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&builder, "{sv}", "Active", g_variant_new_boolean(enabled));
        if (daemon_night.can_temperature) g_variant_builder_add(&builder, "{sv}", "NightTemperature", g_variant_new_int32(kelvin));
        if (enabled) g_variant_builder_add(&builder, "{sv}", "Mode", g_variant_new_int32(3));
        else if (g_key_file_has_key(saved, group, "Mode", NULL))
            g_variant_builder_add(&builder, "{sv}", "Mode", g_variant_new_int32(g_key_file_get_integer(saved, group, "Mode", NULL)));
        reply = desktop_call("changeConfiguration", g_variant_new("(a{sv})", &builder));
        gboolean ok = FALSE; if (reply) g_variant_get(reply, "(b)", &ok);
        if (reply) g_variant_unref(reply);
        if (ok && !enabled) { g_key_file_remove_group(saved, group, NULL); settings_save(saved); }
        g_key_file_unref(saved);
        if (!ok) goto failed;
    } else if (enabled) {
        if (has_inhibit) {
            reply = desktop_call("uninhibit", g_variant_new("(u)", inhibit_cookie));
            if (!reply) goto failed;
            g_variant_unref(reply); has_inhibit = 0;
        }
        if (preview_supported) reply = desktop_call("preview", g_variant_new("(u)", (guint)kelvin));
        else {
            GVariant *active = property("running");
            int running = active && g_variant_is_of_type(active, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(active);
            if (active) g_variant_unref(active);
            if (!running) goto failed;
        }
        if (preview_supported && !reply) goto failed;
        if (reply) g_variant_unref(reply);
    } else {
        if (preview_supported) { reply = desktop_call("stopPreview", NULL); if (reply) g_variant_unref(reply); }
        if (!has_inhibit) {
            reply = desktop_call("inhibit", NULL);
            if (!reply) goto failed;
            g_variant_get(reply, "(u)", &inhibit_cookie); g_variant_unref(reply); has_inhibit = 1;
        }
    }
    daemon_night.enabled = enabled; daemon_night.kelvin = kelvin; return 0;
failed:
    snprintf(error, size, "KWin rejected this operation. Its available controls may have changed; press R to rescan."); return -1;
}

typedef struct { uint32_t magic; int command, kelvin; } Request;
#define SB_MAGIC 0x53425231u
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static volatile sig_atomic_t daemon_stop;
static void daemon_signal(int number) { (void)number; daemon_stop = 1; }

static int socket_location(void)
{
    const char *runtime = g_get_user_runtime_dir();
    char *dir = g_build_filename(runtime, "simplebright", NULL);
    struct stat st;
    if (g_mkdir_with_parents(dir, 0700) || lstat(dir, &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0077)) { g_free(dir); return -1; }
    const char *wl = g_getenv("WAYLAND_DISPLAY"), *x = g_getenv("DISPLAY");
    char *display = g_strdup_printf("%s|%s", wl ? wl : "", x ? x : "");
    int n = snprintf(socket_path, sizeof(socket_path), "%s/night-%08x.sock", dir, g_str_hash(display));
    g_free(display); g_free(dir);
    return n < (int)sizeof(socket_path) ? 0 : -1;
}
static int connect_daemon(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    g_strlcpy(address.sun_path, socket_path, sizeof(address.sun_path));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { close(fd); return -1; }
    return fd;
}
static void daemon_session(gpointer unused) { (void)unused; setsid(); }
int sb_night_request(int command_id, int kelvin, SbNight *night)
{
    if (socket_location()) {
        g_strlcpy(night->error, "Cannot access a private runtime directory for night filtering.", sizeof(night->error)); return -1;
    }
    int fd = connect_daemon();
    if (fd < 0 && command_id == 5) return 0;
    if (fd < 0) {
        char exe[PATH_MAX]; ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (length <= 0) return -1;
        exe[length] = 0;
        char *argv[] = {exe, "--night-daemon", NULL};
        if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                           daemon_session, NULL, NULL, NULL)) return -1;
        for (int i = 0; i < 100 && fd < 0; i++) { g_usleep(20000); fd = connect_daemon(); }
        if (fd < 0) { g_strlcpy(night->error, "Night-filter worker could not start.", sizeof(night->error)); return -1; }
    }
    struct timeval timeout = {.tv_sec = 12};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    Request request = {SB_MAGIC, command_id, kelvin};
    int ok = send(fd, &request, sizeof(request), MSG_NOSIGNAL) == (ssize_t)sizeof(request) &&
             recv(fd, night, sizeof(*night), MSG_WAITALL) == (ssize_t)sizeof(*night);
    close(fd);
    if (!ok) { g_strlcpy(night->error, "Night-filter worker did not respond.", sizeof(night->error)); return -1; }
    return *night->error ? -1 : 0;
}

int sb_night_daemon(void)
{
    if (socket_location()) return 1;
    char lock_path[PATH_MAX]; snprintf(lock_path, sizeof(lock_path), "%s.lock", socket_path);
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { if (lock >= 0) close(lock); return 1; }
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) { close(lock); return 1; }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    g_strlcpy(address.sun_path, socket_path, sizeof(address.sun_path));
    unlink(socket_path); mode_t mask = umask(0077);
    int bound = bind(server, (struct sockaddr *)&address, sizeof(address)); umask(mask);
    if (bound || listen(server, 8)) { close(server); close(lock); return 1; }
    signal(SIGINT, daemon_signal); signal(SIGTERM, daemon_signal); signal(SIGHUP, daemon_signal);
    daemon_night.kelvin = sb_saved_temperature();
    desktop_detect();
    direct_available = sb_filter_probe();
    if (direct_available) {
        daemon_night.can_toggle = daemon_night.can_temperature = 1;
        g_strlcpy(daemon_night.backend, "Simplebright gamma", sizeof(daemon_night.backend));
        daemon_night.enabled = 0;
    }
    gint64 last_request = g_get_monotonic_time(), next_preview = last_request;
    while (!daemon_stop) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        struct pollfd fds[2] = {{server, POLLIN, 0}, {direct_open ? sb_filter_fd() : -1, POLLIN, 0}};
        int ready = poll(fds, 2, 100);
        if (ready < 0 && errno != EINTR) break;
        if (fds[1].revents) {
            if (sb_filter_dispatch(daemon_night.error, sizeof(daemon_night.error))) {
                sb_filter_close(); direct_open = 0; daemon_night.enabled = 0;
            }
        }
        if (fds[0].revents & POLLIN) {
            int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
            if (client < 0) continue;
            struct ucred credentials; socklen_t length = sizeof(credentials);
            struct timeval timeout = {.tv_sec = 2};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            Request request;
            if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) || credentials.uid != getuid() ||
                recv(client, &request, sizeof(request), MSG_WAITALL) != (ssize_t)sizeof(request) || request.magic != SB_MAGIC) {
                close(client); continue;
            }
            last_request = g_get_monotonic_time();
            daemon_night.error[0] = 0;
            if (request.command == 5) daemon_stop = 1;
            if (request.command == 3 && !direct_open && !has_inhibit) {
                desktop_detect(); direct_available = sb_filter_probe();
                if (direct_available) {
                    daemon_night.can_toggle = daemon_night.can_temperature = 1;
                    g_strlcpy(daemon_night.backend, "Simplebright gamma", sizeof(daemon_night.backend));
                }
            }
            if (request.command == 4) {
                int kelvin = CLAMP(request.kelvin, 1000, 6500), result = 0;
                if (!daemon_night.can_temperature) result = -1;
                else if (direct_available && daemon_night.enabled && direct_open)
                {
                    result = sb_filter_apply(kelvin, daemon_night.error, sizeof(daemon_night.error));
                    if (result) { sb_filter_close(); direct_open = 0; daemon_night.enabled = 0; }
                }
                else if (!direct_available && desktop_settings) result = settings_temperature(kelvin);
                else if (!direct_available && desktop && daemon_night.enabled)
                    result = kde_apply(1, kelvin, daemon_night.error, sizeof(daemon_night.error));
                else if (!direct_available && desktop && kde_legacy) {
                    GVariantBuilder builder; g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
                    g_variant_builder_add(&builder, "{sv}", "NightTemperature", g_variant_new_int32(kelvin));
                    GVariant *reply = desktop_call("changeConfiguration", g_variant_new("(a{sv})", &builder));
                    gboolean ok = FALSE; if (reply) { g_variant_get(reply, "(b)", &ok); g_variant_unref(reply); }
                    result = ok ? 0 : -1;
                }
                if (!result) { daemon_night.kelvin = kelvin; sb_save_temperature(kelvin); }
                else if (!*daemon_night.error) g_strlcpy(daemon_night.error, "Temperature control was rejected by this session.", sizeof(daemon_night.error));
            }
            if (request.command == 1 || request.command == 2) {
                int enabled = request.command == 1;
                int kelvin = CLAMP(request.kelvin, 1000, 6500), result = -1;
                if (!daemon_night.can_toggle) {
                    g_strlcpy(daemon_night.error, "Night-mode toggle is unavailable in this session.", sizeof(daemon_night.error));
                } else if (direct_available) {
                    if (!enabled) { sb_filter_close(); direct_open = 0; result = 0; }
                    else {
                        if (!direct_open && !sb_filter_open(daemon_night.error, sizeof(daemon_night.error))) direct_open = 1;
                        if (direct_open) result = sb_filter_apply(kelvin, daemon_night.error, sizeof(daemon_night.error));
                    }
                    if (result) {
                        sb_filter_close(); direct_open = 0;
                        if (desktop) {
                            direct_available = 0; desktop_detect(); daemon_night.error[0] = 0;
                        }
                    }
                }
                if (result && daemon_night.can_toggle && !direct_available && desktop_settings)
                    result = settings_apply(enabled, kelvin, daemon_night.error, sizeof(daemon_night.error));
                else if (result && daemon_night.can_toggle && !direct_available && desktop)
                    result = kde_apply(enabled, kelvin, daemon_night.error, sizeof(daemon_night.error));
                if (!result) {
                    daemon_night.enabled = enabled; daemon_night.kelvin = kelvin;
                    daemon_night.error[0] = 0; sb_save_temperature(kelvin);
                } else if (!*daemon_night.error) {
                    g_strlcpy(daemon_night.error, "No supported night-filter control is available in this session.", sizeof(daemon_night.error));
                }
                next_preview = last_request + 8000000;
            }
            if (!direct_available && desktop_settings) {
                if (daemon_night.can_toggle) daemon_night.enabled = g_settings_get_boolean(desktop_settings, "night-light-enabled");
                if (daemon_night.can_temperature) daemon_night.kelvin = (int)g_settings_get_uint(desktop_settings, "night-light-temperature");
            }
            send(client, &daemon_night, sizeof(daemon_night), MSG_NOSIGNAL); close(client);
        }
        gint64 now = g_get_monotonic_time();
        if (!direct_available && desktop && !desktop_settings && preview_supported && daemon_night.enabled && now >= next_preview) {
            GVariant *reply = desktop_call("preview", g_variant_new("(u)", (guint)daemon_night.kelvin));
            if (reply) g_variant_unref(reply); else daemon_night.enabled = 0;
            next_preview = now + 8000000;
        }
        if (!direct_open && !has_inhibit && !(preview_supported && daemon_night.enabled) && now - last_request > 30000000) break;
    }
    if (direct_open) sb_filter_close();
    if (has_inhibit) { GVariant *reply = desktop_call("uninhibit", g_variant_new("(u)", inhibit_cookie)); if (reply) g_variant_unref(reply); }
    if (desktop && preview_supported && daemon_night.enabled) { GVariant *reply = desktop_call("stopPreview", NULL); if (reply) g_variant_unref(reply); }
    g_clear_object(&desktop_settings); g_clear_object(&session_bus);
    close(server); unlink(socket_path); close(lock); return 0;
}
