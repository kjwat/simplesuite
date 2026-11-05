/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "simplebright-filter.h"
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <wayland-client.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The two small interfaces below implement wlr-gamma-control-v1 directly.
 * Protocol copyright and permission are in protocols/wlr-gamma-control-NOTICE.
 * No desktop night-light service or third-party filter program is used. */
static const struct wl_interface gamma_interface, manager_interface;
static const struct wl_interface *control_types[] = {&gamma_interface, &wl_output_interface};
static const struct wl_message manager_requests[] = {
    {"get_gamma_control", "no", control_types}, {"destroy", "", NULL}
};
static const struct wl_message gamma_requests[] = {{"set_gamma", "h", NULL}, {"destroy", "", NULL}};
static const struct wl_message gamma_events[] = {{"gamma_size", "u", NULL}, {"failed", "", NULL}};
static const struct wl_interface manager_interface = {"zwlr_gamma_control_manager_v1", 1, 2, manager_requests, 0, NULL};
static const struct wl_interface gamma_interface = {"zwlr_gamma_control_v1", 1, 2, gamma_requests, 2, gamma_events};

#define OUTPUTS 64
static struct wl_display *wayland;
static struct wl_registry *registry;
static struct wl_proxy *manager;
static Display *xdisplay;
static int temperature = 6500;
static struct Output {
    uint32_t name, size;
    struct wl_output *output;
    struct wl_proxy *control;
    int failed;
} outputs[OUTPUTS];
static struct { RRCrtc crtc; XRRCrtcGamma *original; } crtcs[OUTPUTS];
static int ncrtcs, xerror, probing, applied;

static double clamp(double value) { return fmax(0.0, fmin(1.0, value)); }

void sb_temperature_rgb(int kelvin, double rgb[3])
{
    /* Planckian-locus approximation normalized to identity at 6500 K. */
    double t = fmax(1000, fmin(6500, kelvin)) / 100.0;
    rgb[0] = 1.0;
    rgb[1] = clamp((99.4708025861 * log(t) - 161.1195681661) /
                   (99.4708025861 * log(65.0) - 161.1195681661));
    rgb[2] = t <= 19 ? 0 : clamp((138.5177312231 * log(t - 10) - 305.0447927307) /
                                  (138.5177312231 * log(55.0) - 305.0447927307));
}

static void sync_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)callback; (void)serial; *(int *)data = 1;
}
static int wl_sync(void)
{
    int done = 0;
    struct wl_callback *callback = wl_display_sync(wayland);
    static const struct wl_callback_listener listener = {sync_done};
    if (!callback) return -1;
    wl_callback_add_listener(callback, &listener, &done);
    int result = -1;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t end = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 2500;
    while (!done) {
        while (wl_display_prepare_read(wayland) != 0)
            if (wl_display_dispatch_pending(wayland) < 0) goto finish;
        if (done) { wl_display_cancel_read(wayland); break; }
        if (wl_display_flush(wayland) < 0 && errno != EAGAIN) { wl_display_cancel_read(wayland); goto finish; }
        clock_gettime(CLOCK_MONOTONIC, &now);
        int remaining = (int)(end - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000));
        struct pollfd fd = {wl_display_get_fd(wayland), POLLIN, 0};
        if (remaining <= 0 || poll(&fd, 1, remaining) <= 0) { wl_display_cancel_read(wayland); goto finish; }
        if (wl_display_read_events(wayland) < 0 || wl_display_dispatch_pending(wayland) < 0) goto finish;
    }
    result = 0;
finish:
    wl_callback_destroy(callback); return result;
}

static int wl_ramp(struct Output *output)
{
    if (!output->size || output->failed || !output->control) return -1;
    if (output->size > 1048576) { output->failed = 1; return -1; }
    size_t bytes = (size_t)output->size * 3 * sizeof(uint16_t);
    int fd = memfd_create("simplebright-gamma", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) < 0) { if (fd >= 0) close(fd); return -1; }
    uint16_t *ramp = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ramp == MAP_FAILED) { close(fd); return -1; }
    double rgb[3];
    sb_temperature_rgb(temperature, rgb);
    for (int c = 0; c < 3; c++)
        for (uint32_t i = 0; i < output->size; i++)
            ramp[c * output->size + i] = (uint16_t)(65535.0 * i / (output->size > 1 ? output->size - 1 : 1) * rgb[c]);
    munmap(ramp, bytes);
    wl_proxy_marshal(output->control, 0, fd);
    close(fd);
    return 0;
}

static void gamma_size(void *data, struct wl_proxy *proxy, uint32_t size)
{
    (void)proxy;
    struct Output *output = data;
    output->size = size;
    /* Applying identity at discovery would overwrite another client's ramps.
     * Only write after the user has explicitly enabled the filter. */
    if (temperature != 6500 && wl_ramp(output) < 0) output->failed = 1;
}
static void gamma_failed(void *data, struct wl_proxy *proxy)
{
    (void)proxy;
    ((struct Output *)data)->failed = 1;
}
static void (*gamma_listener[])(void) = {(void (*)(void))gamma_size, (void (*)(void))gamma_failed};

static void make_control(struct Output *output)
{
    output->control = wl_proxy_marshal_constructor(manager, 0, &gamma_interface, NULL, output->output);
    if (output->control) wl_proxy_add_listener(output->control, gamma_listener, output);
}
static void remove_output(struct Output *output)
{
    if (output->control) { wl_proxy_marshal(output->control, 1); wl_proxy_destroy(output->control); }
    if (output->output) wl_output_destroy(output->output);
    memset(output, 0, sizeof(*output));
}
static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t width, int32_t height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)width; (void)height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}
static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{ (void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh; }
static const struct wl_output_listener output_listener = {.geometry=output_geometry, .mode=output_mode};
static void global_add(void *data, struct wl_registry *reg, uint32_t name, const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(interface, manager_interface.name) && !manager)
        manager = wl_registry_bind(reg, name, &manager_interface, 1);
    if (!strcmp(interface, "wl_output")) {
        for (int i = 0; i < OUTPUTS; i++) if (!outputs[i].output) {
            outputs[i].name = name;
            outputs[i].output = wl_registry_bind(reg, name, &wl_output_interface, 1);
            wl_output_add_listener(outputs[i].output, &output_listener, NULL);
            if (manager && !probing) make_control(&outputs[i]);
            break;
        }
    }
}
static void global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg;
    for (int i = 0; i < OUTPUTS; i++) if (outputs[i].name == name) remove_output(&outputs[i]);
}
static const struct wl_registry_listener registry_listener = {global_add, global_remove};
static int x_error(Display *display, XErrorEvent *event) { (void)display; (void)event; xerror = 1; return 0; }

static int x_discover(void)
{
    XRRScreenResources *resources = XRRGetScreenResourcesCurrent(xdisplay, DefaultRootWindow(xdisplay));
    if (!resources) return -1;
    for (int i = 0; i < resources->ncrtc && ncrtcs < OUTPUTS; i++) {
        XRRCrtcInfo *info = XRRGetCrtcInfo(xdisplay, resources, resources->crtcs[i]);
        int active = info && info->mode && info->noutput;
        if (info) XRRFreeCrtcInfo(info);
        if (!active) continue;
        int found = 0;
        for (int j = 0; j < ncrtcs; j++) if (crtcs[j].crtc == resources->crtcs[i]) found = 1;
        if (found) continue;
        XRRCrtcGamma *gamma = XRRGetCrtcGamma(xdisplay, resources->crtcs[i]);
        if (!gamma || gamma->size < 2) { if (gamma) XRRFreeGamma(gamma); continue; }
        crtcs[ncrtcs].crtc = resources->crtcs[i];
        crtcs[ncrtcs++].original = gamma;
    }
    XRRFreeScreenResources(resources);
    XSync(xdisplay, False);
    return ncrtcs ? 0 : -1;
}

int sb_filter_open(char *error, size_t size)
{
    const char *session = getenv("XDG_SESSION_TYPE"), *wl = getenv("WAYLAND_DISPLAY");
    temperature = 6500;
    /* Never mistake Xwayland's emulated gamma for the physical display. */
    if ((wl && *wl) || (session && !strcmp(session, "wayland"))) {
        wayland = wl_display_connect(NULL);
        if (wayland) {
            registry = wl_display_get_registry(wayland);
            wl_registry_add_listener(registry, &registry_listener, NULL);
            if (wl_sync() >= 0 && manager) {
                if (probing) return 0;
                for (int i = 0; i < OUTPUTS; i++)
                    if (outputs[i].output && !outputs[i].control) make_control(&outputs[i]);
                if (wl_sync() >= 0) {
                    for (int i = 0; i < OUTPUTS; i++)
                        if (outputs[i].size && !outputs[i].failed) return 0;
                }
            }
        }
        snprintf(error, size, "This Wayland desktop blocks direct night filtering. Brightness still works. See --setup-help.");
    } else {
        xdisplay = XOpenDisplay(NULL);
        if (xdisplay) {
            XSetErrorHandler(x_error);
            int event, err;
            if (XRRQueryExtension(xdisplay, &event, &err) && !x_discover()) {
                XRRSelectInput(xdisplay, DefaultRootWindow(xdisplay), RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
                return 0;
            }
        }
        snprintf(error, size, "No usable X11 gamma controls. Night filtering needs X11 or wlr-gamma-control on Wayland.");
    }
    sb_filter_close();
    return -1;
}

int sb_filter_apply(int kelvin, char *error, size_t size)
{
    temperature = kelvin;
    applied = 1;
    int count = 0;
    if (wayland) {
        for (int i = 0; i < OUTPUTS; i++) if (!wl_ramp(&outputs[i])) count++;
        if (count && wl_sync() >= 0) {
            for (int i = 0; i < OUTPUTS; i++) if (outputs[i].output && outputs[i].failed) {
                snprintf(error, size, "A display rejected gamma control; another filter may own it."); return -1;
            }
            return 0;
        }
    }
    if (xdisplay) {
        double rgb[3]; sb_temperature_rgb(kelvin, rgb);
        xerror = 0;
        for (int i = 0; i < ncrtcs; i++) {
            XRRCrtcGamma *original = crtcs[i].original, *gamma = XRRAllocGamma(original->size);
            if (!gamma) continue;
            for (int j = 0; j < original->size; j++) {
                gamma->red[j] = (unsigned short)(original->red[j] * rgb[0]);
                gamma->green[j] = (unsigned short)(original->green[j] * rgb[1]);
                gamma->blue[j] = (unsigned short)(original->blue[j] * rgb[2]);
            }
            XRRSetCrtcGamma(xdisplay, crtcs[i].crtc, gamma);
            XRRFreeGamma(gamma); count++;
        }
        XSync(xdisplay, False);
        if (count && !xerror) return 0;
    }
    snprintf(error, size, "The display rejected the night filter.");
    return -1;
}

int sb_filter_fd(void) { return wayland ? wl_display_get_fd(wayland) : xdisplay ? ConnectionNumber(xdisplay) : -1; }
int sb_filter_dispatch(char *error, size_t size)
{
    if (wayland) {
        if (wl_display_dispatch(wayland) < 0) return -1;
        if (wl_display_flush(wayland) < 0 && errno != EAGAIN) return -1;
        for (int i = 0; i < OUTPUTS; i++) if (outputs[i].failed) {
            snprintf(error, size, "Gamma control was revoked by the compositor or another filter."); return -1;
        }
    } else if (xdisplay) {
        while (XPending(xdisplay)) { XEvent event; XNextEvent(xdisplay, &event); }
        x_discover();
        return sb_filter_apply(temperature, error, size);
    }
    return 0;
}
void sb_filter_close(void)
{
    if (wayland) {
        for (int i = 0; i < OUTPUTS; i++) remove_output(&outputs[i]);
        if (manager) { wl_proxy_marshal(manager, 1); wl_proxy_destroy(manager); manager = NULL; }
        if (registry) { wl_registry_destroy(registry); registry = NULL; }
        wl_display_flush(wayland);
        wl_display_disconnect(wayland); wayland = NULL;
    }
    if (xdisplay) {
        for (int i = 0; i < ncrtcs; i++) {
            if (applied) XRRSetCrtcGamma(xdisplay, crtcs[i].crtc, crtcs[i].original);
            XRRFreeGamma(crtcs[i].original);
        }
        XSync(xdisplay, False); XCloseDisplay(xdisplay); xdisplay = NULL; ncrtcs = 0;
    }
    temperature = 6500;
    applied = 0;
}

int sb_filter_probe(void)
{
    char error[256];
    probing = 1;
    int result = sb_filter_open(error, sizeof(error));
    sb_filter_close();
    probing = 0;
    return result == 0;
}
