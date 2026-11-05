/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the native client against an isolated, real Wayland connection. */
#include "../simplebright-filter.c"
#include <wayland-server.h>
#include <wayland-server-protocol.h>
#include <glib.h>
#include <assert.h>
#include <sys/stat.h>

static struct wl_display *test_server;
static gint controls, updates, destroyed, last_hash, fail_control, server_stop;
static void resource_destroy(struct wl_client *client, struct wl_resource *resource)
{ (void)client; wl_resource_destroy(resource); }
static void control_destroyed(struct wl_resource *resource)
{ (void)resource; g_atomic_int_inc(&destroyed); }
static void set_gamma(struct wl_client *client, struct wl_resource *resource, int32_t fd)
{
    (void)client; (void)resource;
    struct stat st; assert(!fstat(fd, &st) && st.st_size == 256 * 3 * 2);
    uint16_t values[256 * 3]; assert(read(fd, values, sizeof(values)) == sizeof(values)); close(fd);
    assert(values[0] == 0 && values[255] == 65535);
    assert(values[511] <= values[255] && values[767] <= values[511]);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < G_N_ELEMENTS(values); i++) hash = (hash ^ values[i]) * 16777619u;
    g_atomic_int_set(&last_hash, (gint)hash); g_atomic_int_inc(&updates);
}
static const struct {
    void (*set_gamma)(struct wl_client *, struct wl_resource *, int32_t);
    void (*destroy)(struct wl_client *, struct wl_resource *);
} gamma_impl = {set_gamma, resource_destroy};
static void get_control(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *output)
{
    (void)resource; assert(output);
    struct wl_resource *control = wl_resource_create(client, &gamma_interface, 1, id); assert(control);
    wl_resource_set_implementation(control, &gamma_impl, NULL, control_destroyed);
    g_atomic_int_inc(&controls);
    if (g_atomic_int_get(&fail_control)) wl_resource_post_event(control, 1);
    else wl_resource_post_event(control, 0, 256u);
}
static const struct {
    void (*get_control)(struct wl_client *, struct wl_resource *, uint32_t, struct wl_resource *);
    void (*destroy)(struct wl_client *, struct wl_resource *);
} manager_impl = {get_control, resource_destroy};
static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data; (void)version;
    struct wl_resource *resource = wl_resource_create(client, &manager_interface, 1, id); assert(resource);
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}
static void bind_output(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data; (void)version;
    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, 1, id); assert(resource);
    wl_resource_set_implementation(resource, NULL, NULL, NULL);
    wl_output_send_geometry(resource, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Test", "Virtual display", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, 1280, 720, 60000);
}
static gpointer run_server(gpointer unused)
{
    (void)unused;
    while (!g_atomic_int_get(&server_stop)) {
        wl_event_loop_dispatch(wl_display_get_event_loop(test_server), 50);
        wl_display_flush_clients(test_server);
    }
    return NULL;
}

static void x11_test(void)
{
    char error[256]; assert(sb_filter_probe()); assert(!sb_filter_open(error, sizeof(error)));
    assert(xdisplay && ncrtcs);
    XRRCrtcGamma *before = XRRGetCrtcGamma(xdisplay, crtcs[0].crtc);
    assert(!sb_filter_apply(3500, error, sizeof(error)));
    XRRCrtcGamma *first = XRRGetCrtcGamma(xdisplay, crtcs[0].crtc);
    assert(!sb_filter_apply(3500, error, sizeof(error)));
    XRRCrtcGamma *second = XRRGetCrtcGamma(xdisplay, crtcs[0].crtc);
    assert(!memcmp(first->green, second->green, (size_t)first->size * 2));
    int readable = before->green[before->size - 1] != 0;
    if (readable) assert(first->green[first->size - 1] < before->green[before->size - 1]);
    assert(!sb_filter_apply(6500, error, sizeof(error)));
    XRRCrtcGamma *neutral = XRRGetCrtcGamma(xdisplay, crtcs[0].crtc);
    assert(!memcmp(before->red, neutral->red, (size_t)before->size * 2));
    assert(!memcmp(before->green, neutral->green, (size_t)before->size * 2));
    assert(!memcmp(before->blue, neutral->blue, (size_t)before->size * 2));
    XRRFreeGamma(before); XRRFreeGamma(first); XRRFreeGamma(second); XRRFreeGamma(neutral);
    sb_filter_close();
    puts(readable ? "OK X11: gamma writes, noncumulative adjustment and neutral restoration" :
                    "OK X11: protocol requests accepted; virtual server provides no readable gamma ramps");
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--x11")) { x11_test(); return 0; }
    char *runtime = g_dir_make_tmp("simplebright-wayland-XXXXXX", NULL); assert(runtime);
    g_setenv("XDG_RUNTIME_DIR", runtime, TRUE); g_setenv("XDG_SESSION_TYPE", "wayland", TRUE);
    test_server = wl_display_create(); assert(test_server);
    const char *socket = wl_display_add_socket_auto(test_server); assert(socket);
    g_setenv("WAYLAND_DISPLAY", socket, TRUE);
    assert(wl_global_create(test_server, &wl_output_interface, 1, NULL, bind_output));
    assert(wl_global_create(test_server, &manager_interface, 1, NULL, bind_manager));
    GThread *thread = g_thread_new("gamma-test-server", run_server, NULL);
    assert(sb_filter_probe());
    assert(g_atomic_int_get(&controls) == 0 && g_atomic_int_get(&updates) == 0);
    char error[256]; assert(!sb_filter_open(error, sizeof(error)));
    assert(g_atomic_int_get(&controls) == 1 && g_atomic_int_get(&updates) == 0);
    assert(!sb_filter_apply(3500, error, sizeof(error)));
    gint first = g_atomic_int_get(&last_hash);
    assert(!sb_filter_apply(3500, error, sizeof(error)));
    assert(first == g_atomic_int_get(&last_hash) && g_atomic_int_get(&updates) == 2);
    assert(!sb_filter_apply(6500, error, sizeof(error)));
    assert(first != g_atomic_int_get(&last_hash));
    sb_filter_close();
    g_atomic_int_set(&fail_control, 1);
    assert(sb_filter_open(error, sizeof(error)) < 0);
    assert(g_atomic_int_get(&updates) == 3);
    g_atomic_int_set(&server_stop, 1); g_thread_join(thread);
    wl_display_destroy_clients(test_server); wl_display_destroy(test_server);
    rmdir(runtime); g_free(runtime);
    puts("OK Wayland: real protocol transport, read-only discovery, ramp fd data, repeat stability, neutral, ownership and rejection");
    return 0;
}
