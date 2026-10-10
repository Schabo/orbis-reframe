// Drive a virtual pointer on a (headless) wlroots compositor:
//   vp move X Y | down | up | click | wheel N | sleep MS ...   (screen 1920x1080)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client.h"

static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct wl_seat *seat;
static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener rl = {global, global_remove};
static uint32_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
int main(int argc, char **argv)
{
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &rl, NULL);
    wl_display_roundtrip(dpy);
    if (!mgr) { fprintf(stderr, "no virtual pointer manager\n"); return 1; }
    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    wl_display_roundtrip(dpy);
    for (int i = 1; i < argc; ++i) {
        const char *c = argv[i];
        if (!strcmp(c, "move") && i + 2 < argc) {
            zwlr_virtual_pointer_v1_motion_absolute(p, now(), atoi(argv[i + 1]), atoi(argv[i + 2]), 1920, 1080);
            i += 2;
        } else if (!strcmp(c, "down")) {
            zwlr_virtual_pointer_v1_button(p, now(), 0x110, 1);
        } else if (!strcmp(c, "up")) {
            zwlr_virtual_pointer_v1_button(p, now(), 0x110, 0);
        } else if (!strcmp(c, "click")) {
            zwlr_virtual_pointer_v1_button(p, now(), 0x110, 1);
            zwlr_virtual_pointer_v1_frame(p);
            wl_display_roundtrip(dpy);
            usleep(50000);
            zwlr_virtual_pointer_v1_button(p, now(), 0x110, 0);
        } else if (!strcmp(c, "rclick")) {
            zwlr_virtual_pointer_v1_button(p, now(), 0x111, 1);
            zwlr_virtual_pointer_v1_frame(p);
            wl_display_roundtrip(dpy);
            usleep(50000);
            zwlr_virtual_pointer_v1_button(p, now(), 0x111, 0);
        } else if (!strcmp(c, "sleep") && i + 1 < argc) {
            usleep(atoi(argv[++i]) * 1000);
            continue;
        }
        zwlr_virtual_pointer_v1_frame(p);
        wl_display_roundtrip(dpy);
        usleep(30000);
    }
    zwlr_virtual_pointer_v1_destroy(p);
    wl_display_roundtrip(dpy);
    return 0;
}
