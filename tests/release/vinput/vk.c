// A virtual keyboard for a *headless* Sway (which has no keyboard):
//   WAYLAND_DISPLAY=wayland-N ./vk key Right | key shift+Left | key ctrl+z | sleep MS ...
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *mgr;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    if (!strcmp(iface, "wl_seat"))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, "zwp_virtual_keyboard_manager_v1"))
        mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener rl = {global, global_remove};

static const struct { const char *n; int c; } keys[] = {
    {"Left", KEY_LEFT}, {"Right", KEY_RIGHT}, {"Up", KEY_UP}, {"Down", KEY_DOWN},
    {"Home", KEY_HOME}, {"End", KEY_END}, {"space", KEY_SPACE}, {"Delete", KEY_DELETE},
    {"BackSpace", KEY_BACKSPACE}, {"Escape", KEY_ESC}, {"Return", KEY_ENTER}, {"F1", KEY_F1},
    {"bracketleft", KEY_LEFTBRACE}, {"bracketright", KEY_RIGHTBRACE}, {"Tab", KEY_TAB},
    {"a", KEY_A}, {"e", KEY_E}, {"k", KEY_K}, {"q", KEY_Q}, {"s", KEY_S}, {"t", KEY_T}, {"z", KEY_Z}, {"0", KEY_0}, {"minus", KEY_MINUS},
    {"equal", KEY_EQUAL}, {"1", KEY_1}, {"2", KEY_2}, {"5", KEY_5},
};
static int code(const char *n)
{
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i)
        if (!strcmp(keys[i].n, n))
            return keys[i].c;
    fprintf(stderr, "unknown key %s\n", n);
    exit(2);
}
static uint32_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &rl, NULL);
    wl_display_roundtrip(dpy);
    if (!seat || !mgr) { fprintf(stderr, "no seat / manager\n"); return 1; }
    struct zwp_virtual_keyboard_v1 *kb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(mgr, seat);

    struct xkb_context *ctx = xkb_context_new(0);
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, 0);
    char *s = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t len = strlen(s) + 1;
    int fd = memfd_create("keymap", 0);
    write(fd, s, len);
    zwp_virtual_keyboard_v1_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
    wl_display_roundtrip(dpy);
    usleep(100000);

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "sleep") && i + 1 < argc) {
            wl_display_flush(dpy);
            usleep(atoi(argv[++i]) * 1000);
        } else if (!strcmp(argv[i], "key") && i + 1 < argc) {
            char buf[64];
            snprintf(buf, sizeof buf, "%s", argv[++i]);
            uint32_t mods = 0;
            int modcodes[4], nm = 0;
            char *k = buf, *plus;
            while ((plus = strchr(k, '+')) && plus[1]) {
                *plus = 0;
                if (!strcmp(k, "shift")) { mods |= 1; modcodes[nm++] = KEY_LEFTSHIFT; }
                else if (!strcmp(k, "ctrl")) { mods |= 4; modcodes[nm++] = KEY_LEFTCTRL; }
                else if (!strcmp(k, "alt")) { mods |= 8; modcodes[nm++] = KEY_LEFTALT; }
                k = plus + 1;
            }
            for (int m = 0; m < nm; ++m)
                zwp_virtual_keyboard_v1_key(kb, now(), modcodes[m], 1);
            if (mods)
                zwp_virtual_keyboard_v1_modifiers(kb, mods, 0, 0, 0);
            const int c = code(k);
            zwp_virtual_keyboard_v1_key(kb, now(), c, 1);
            wl_display_roundtrip(dpy);
            usleep(20000);
            zwp_virtual_keyboard_v1_key(kb, now(), c, 0);
            for (int m = nm - 1; m >= 0; --m)
                zwp_virtual_keyboard_v1_key(kb, now(), modcodes[m], 0);
            if (mods)
                zwp_virtual_keyboard_v1_modifiers(kb, 0, 0, 0, 0);
            wl_display_roundtrip(dpy);
            usleep(30000);
        }
    }
    wl_display_roundtrip(dpy);
    usleep(50000);
    return 0;
}
