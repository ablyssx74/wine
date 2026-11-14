/*
 * Wayland core handling
 *
 * Copyright (c) 2020 Alexandros Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"
#include "winternl.h"
#include "winreg.h"

#include "wine/debug.h"

#include <stdlib.h>
#include <stdint.h>
#ifdef __HAIKU__
# include <dlfcn.h>
# include <stdio.h>
# include <string.h>
# include <unistd.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct wayland process_wayland =
{
    .seat.mutex = PTHREAD_MUTEX_INITIALIZER,
    .keyboard.mutex = PTHREAD_MUTEX_INITIALIZER,
    .pointer.mutex = PTHREAD_MUTEX_INITIALIZER,
    .text_input.mutex = PTHREAD_MUTEX_INITIALIZER,
    .data_device.mutex = PTHREAD_MUTEX_INITIALIZER,
    .output_list = {&process_wayland.output_list, &process_wayland.output_list},
    .output_mutex = PTHREAD_MUTEX_INITIALIZER
};

/**********************************************************************
 *          xdg_wm_base handling
 */

static void xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *shell,
                                    uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener =
{
    xdg_wm_base_handle_ping
};

/**********************************************************************
 *          wl_seat handling
 */

static void wl_seat_handle_capabilities(void *data, struct wl_seat *seat,
                                        enum wl_seat_capability caps)
{
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !process_wayland.pointer.wl_pointer)
        wayland_pointer_init(wl_seat_get_pointer(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && process_wayland.pointer.wl_pointer)
        wayland_pointer_deinit();

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !process_wayland.keyboard.wl_keyboard)
        wayland_keyboard_init(wl_seat_get_keyboard(seat));
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && process_wayland.keyboard.wl_keyboard)
        wayland_keyboard_deinit();
}

static void wl_seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener =
{
    wl_seat_handle_capabilities,
    wl_seat_handle_name
};

/**********************************************************************
 *          Registry handling
 */

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface,
                                   uint32_t version)
{
    TRACE("interface=%s version=%u id=%u\n", interface, version, id);

    if (strcmp(interface, "wl_output") == 0)
    {
        if (!wayland_output_create(id, version))
            ERR("Failed to create wayland_output for global id=%u\n", id);
    }
    else if (strcmp(interface, "zxdg_output_manager_v1") == 0)
    {
        struct wayland_output *output;

        process_wayland.zxdg_output_manager_v1 =
            wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface,
                             version < 3 ? version : 3);

        /* Add zxdg_output_v1 to existing outputs. */
        wl_list_for_each(output, &process_wayland.output_list, link)
            wayland_output_use_xdg_extension(output);
    }
    else if (strcmp(interface, "wl_compositor") == 0)
    {
        process_wayland.wl_compositor =
            wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    }
    else if (strcmp(interface, "xdg_wm_base") == 0)
    {
        /* Bind version 2 so that compositors (e.g., sway) can properly send tiled
         * states, instead of falling back to (ab)using the maximized state. */
        process_wayland.xdg_wm_base =
            wl_registry_bind(registry, id, &xdg_wm_base_interface,
                             version < 2 ? version : 2);
        xdg_wm_base_add_listener(process_wayland.xdg_wm_base, &xdg_wm_base_listener, NULL);
    }
    else if (strcmp(interface, "wl_shm") == 0)
    {
        process_wayland.wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, "wl_seat") == 0)
    {
        struct wayland_seat *seat = &process_wayland.seat;
        if (seat->wl_seat)
        {
            WARN("Only a single seat is currently supported, ignoring additional seats.\n");
            return;
        }
        pthread_mutex_lock(&seat->mutex);
        seat->wl_seat = wl_registry_bind(registry, id, &wl_seat_interface,
                                         version < 8 ? version : 8);
        seat->global_id = id;
        wl_seat_add_listener(seat->wl_seat, &seat_listener, NULL);
        pthread_mutex_unlock(&seat->mutex);
        if (process_wayland.zwp_text_input_manager_v3) wayland_text_input_init();
        /* Recreate the data device for the new seat. */
        if (process_wayland.data_device.zwlr_data_control_device_v1 ||
            process_wayland.data_device.wl_data_device)
        {
            wayland_data_device_init();
        }
    }
    else if (strcmp(interface, "wp_viewporter") == 0)
    {
        process_wayland.wp_viewporter =
            wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    }
    else if (strcmp(interface, "wl_subcompositor") == 0)
    {
        process_wayland.wl_subcompositor =
            wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0 && !getenv("WINE_HAIKU_NO_CONSTRAINTS"))
    {
        process_wayland.zwp_pointer_constraints_v1 =
            wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0)
    {
        process_wayland.zwp_relative_pointer_manager_v1 =
            wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "zwp_text_input_manager_v3") == 0)
    {
        process_wayland.zwp_text_input_manager_v3 =
            wl_registry_bind(registry, id, &zwp_text_input_manager_v3_interface, 1);
        if (process_wayland.seat.wl_seat) wayland_text_input_init();
    }
    else if (strcmp(interface, "zwlr_data_control_manager_v1") == 0)
    {
        process_wayland.zwlr_data_control_manager_v1 =
            wl_registry_bind(registry, id, &zwlr_data_control_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wl_data_device_manager") == 0)
    {
        process_wayland.wl_data_device_manager =
            wl_registry_bind(registry, id, &wl_data_device_manager_interface, 2);
    }
    else if (strcmp(interface, "xdg_toplevel_icon_manager_v1") == 0)
    {
        process_wayland.xdg_toplevel_icon_manager_v1 =
            wl_registry_bind(registry, id, &xdg_toplevel_icon_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_cursor_shape_manager_v1") == 0)
    {
        process_wayland.wp_cursor_shape_manager_v1 =
            wl_registry_bind(registry, id, &wp_cursor_shape_manager_v1_interface,
                             version < 2 ? version : 2);
    }
    else if (strcmp(interface, "wp_pointer_warp_v1") == 0)
    {
        process_wayland.wp_pointer_warp_v1 =
            wl_registry_bind(registry, id, &wp_pointer_warp_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_alpha_modifier_v1") == 0)
    {
        process_wayland.wp_alpha_modifier_v1 =
            wl_registry_bind(registry, id, &wp_alpha_modifier_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_fractional_scale_manager_v1") == 0)
    {
        process_wayland.wp_fractional_scale_manager_v1 =
            wl_registry_bind(registry, id, &wp_fractional_scale_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "wp_color_manager_v1") == 0)
    {
        process_wayland.wp_color_manager_v1 =
            wl_registry_bind(registry, id, &wp_color_manager_v1_interface,
                             version < 3 ? version : 3);
        wp_color_manager_v1_add_listener(process_wayland.wp_color_manager_v1,
                                         &color_manager_listener, NULL);
    }
#ifdef WL_FIXES_ACK_GLOBAL_REMOVE
    else if (strcmp(interface, "wl_fixes") == 0)
    {
        if (version < 2)
            return;

        process_wayland.wl_fixes =
            wl_registry_bind(registry, id, &wl_fixes_interface, 2);
    }
#endif
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t id)
{
    struct wayland_output *output, *tmp;
    struct wayland_seat *seat;

    TRACE("id=%u\n", id);

#ifdef WL_FIXES_ACK_GLOBAL_REMOVE
    if (process_wayland.wl_fixes)
        wl_fixes_ack_global_remove(process_wayland.wl_fixes, registry, id);
#endif

    wl_list_for_each_safe(output, tmp, &process_wayland.output_list, link)
    {
        if (output->global_id == id)
        {
            TRACE("removing output->name=%s\n", output->current.name);
            wayland_output_destroy(output);
            return;
        }
    }

    seat = &process_wayland.seat;
    if (seat->wl_seat && seat->global_id == id)
    {
        TRACE("removing seat\n");
        if (process_wayland.pointer.wl_pointer) wayland_pointer_deinit();
        if (process_wayland.text_input.zwp_text_input_v3) wayland_text_input_deinit();
        pthread_mutex_lock(&seat->mutex);
        wl_seat_release(seat->wl_seat);
        seat->wl_seat = NULL;
        seat->global_id = 0;
        pthread_mutex_unlock(&seat->mutex);
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

/**********************************************************************
 *          wayland_process_init
 *
 *  Initialise the per process wayland objects.
 *
 */
#ifdef __HAIKU__
/* Haiku's libwayland-client starts an in-process Wayland server (wayland-server-inproc.so, loaded by name) for the client to
 * talk to. Wine ships its own copy of it, next to this library, with the pointer features programs need (hiding the cursor,
 * relative motion and pointer lock); loading it by its path first makes it the one that is found. WINE_HAIKU_SYSTEM_WAYLAND
 * set in the environment leaves the system's copy in use. */
static void haiku_preload_wayland_server(void)
{
    Dl_info info;
    char path[1024];
    const char *slash;

    if (getenv("WINE_HAIKU_SYSTEM_WAYLAND")) return;
    if (!dladdr((void *)haiku_preload_wayland_server, &info) || !info.dli_fname) return;
    if (!(slash = strrchr(info.dli_fname, '/'))) return;
    snprintf(path, sizeof(path), "%.*s/wayland-server-inproc.so", (int)(slash - info.dli_fname), info.dli_fname);
    if (access(path, R_OK)) return;
    if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL)) WARN("Failed to load %s: %s\n", path, dlerror());
    else TRACE("using the Wayland server %s\n", path);
}
#endif


/* System colours from Haiku's interface colours.
 *
 * Wine takes the colours of window frames, buttons, menus and dialogs from "Control Panel\Colors" of the prefix, which has the light
 * colours of Windows: the windows of a program that lets Wine draw its frame (Brave, Firefox) got a white border, and Wine's own dialogs were light
 * on a dark Haiku. The compositor library tells the colours of the user's Haiku theme (haiku_get_ui_colors(), in this order), they are turned into
 * the 31 system colours and written to the registry when they are not there yet, so that the programs that start from then on use them.
 * WINE_HAIKU_SYSTEM_COLORS=0 leaves the colours of the prefix alone. */
enum
{
    HC_PANEL_BG, HC_PANEL_TEXT, HC_DOC_BG, HC_DOC_TEXT, HC_CTRL_BG, HC_CTRL_TEXT, HC_CTRL_BORDER, HC_CTRL_HIGHLIGHT,
    HC_MENU_BG, HC_MENU_TEXT, HC_MENU_SEL_BG, HC_MENU_SEL_TEXT, HC_LIST_SEL_BG, HC_LIST_SEL_TEXT, HC_TIP_BG, HC_TIP_TEXT,
    HC_TAB, HC_TAB_TEXT, HC_TAB_INACTIVE, HC_TAB_INACTIVE_TEXT, HC_SHINE, HC_SHADOW, HC_NAV,
    HC_COUNT
};

static unsigned int haiku_mix(unsigned int a, unsigned int b, int percent_b)
{
    unsigned int shift, out = 0;
    for (shift = 0; shift <= 16; shift += 8)
    {
        int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
        out |= (unsigned int)((ca * (100 - percent_b) + cb * percent_b + 50) / 100) << shift;
    }
    return out;
}
static unsigned int haiku_lighter(unsigned int c, int percent) { return haiku_mix(c, 0xffffff, percent); }
static unsigned int haiku_darker(unsigned int c, int percent) { return haiku_mix(c, 0x000000, percent); }

/* HKEY_CURRENT_USER, as a handle (the user's key under \\Registry\\User, from the SID of the thread) */
static HANDLE haiku_open_hkcu(void)
{
    WCHAR pathW[128];
    char path[128];
    DWORD_PTR sid_data[(sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE) / sizeof(DWORD_PTR)];
    ULONG i, len = sizeof(sid_data);
    SID *sid;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    HANDLE key = 0;

    if (NtQueryInformationToken(GetCurrentThreadEffectiveToken(), TokenUser, sid_data, len, &len)) return 0;
    sid = ((TOKEN_USER *)sid_data)->User.Sid;
    len = snprintf(path, sizeof(path), "\\Registry\\User\\S-%u-%u", sid->Revision,
                   MAKELONG(MAKEWORD(sid->IdentifierAuthority.Value[5], sid->IdentifierAuthority.Value[4]),
                            MAKEWORD(sid->IdentifierAuthority.Value[3], sid->IdentifierAuthority.Value[2])));
    for (i = 0; i < sid->SubAuthorityCount; i++) len += snprintf(path + len, sizeof(path) - len, "-%u", sid->SubAuthority[i]);
    for (i = 0; i <= len; i++) pathW[i] = (unsigned char)path[i];
    name.Buffer = pathW;
    name.Length = name.MaximumLength = len * sizeof(WCHAR);
    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &name;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    return NtOpenKeyEx(&key, MAXIMUM_ALLOWED, &attr, 0) ? 0 : key;
}

static void haiku_registry_value(HANDLE key, const char *name, unsigned int rgb)
{
    WCHAR nameW[64], valueW[16];
    UNICODE_STRING nameStr;
    char buffer[16];
    KEY_VALUE_PARTIAL_INFORMATION *info;
    char info_buffer[sizeof(*info) + 64];
    ULONG size = sizeof(info_buffer), i, len;

    for (len = 0; name[len] && len < ARRAY_SIZE(nameW) - 1; len++) nameW[len] = (unsigned char)name[len];
    nameW[len] = 0;
    nameStr.Buffer = nameW;
    nameStr.Length = nameStr.MaximumLength = len * sizeof(WCHAR);

    len = snprintf(buffer, sizeof(buffer), "%u %u %u", (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    for (i = 0; i <= len; i++) valueW[i] = (unsigned char)buffer[i];

    info = (KEY_VALUE_PARTIAL_INFORMATION *)info_buffer;
    if (!NtQueryValueKey(key, &nameStr, KeyValuePartialInformation, info, size, &size) && info->Type == REG_SZ &&
        info->DataLength == (len + 1) * sizeof(WCHAR) && !memcmp(info->Data, valueW, info->DataLength))
        return;   /* already as it should be */
    NtSetValueKey(key, &nameStr, 0, REG_SZ, valueW, (len + 1) * sizeof(WCHAR));
}

static void haiku_sync_system_colors(void)
{
    static const WCHAR colors_keyW[] = {'C','o','n','t','r','o','l',' ','P','a','n','e','l','\\','C','o','l','o','r','s'};
    int (*get_colors)(uint32_t *, int);
    uint32_t c[HC_COUNT];
    HANDLE hkcu, key;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name;
    const char *env = getenv("WINE_HAIKU_SYSTEM_COLORS");
    unsigned int face, text, window, window_text, hilight, hilight_text, caption, caption_text, inactive_caption, inactive_text, i;

    if (env && !strcmp(env, "0")) return;
    if (!(get_colors = dlsym(RTLD_DEFAULT, "haiku_get_ui_colors"))) return;
    if (get_colors(c, HC_COUNT) < HC_COUNT) return;

    face = c[HC_PANEL_BG];
    text = c[HC_PANEL_TEXT];
    window = c[HC_DOC_BG];
    window_text = c[HC_DOC_TEXT];
    hilight = c[HC_LIST_SEL_BG];
    hilight_text = c[HC_LIST_SEL_TEXT];
    caption = c[HC_TAB];
    caption_text = c[HC_TAB_TEXT];
    inactive_caption = c[HC_TAB_INACTIVE];
    inactive_text = c[HC_TAB_INACTIVE_TEXT];

    {
        const struct { const char *name; unsigned int rgb; } colors[] =
        {
            { "Scrollbar", face },
            { "ActiveTitle", caption },
            { "InactiveTitle", inactive_caption },
            { "Menu", c[HC_MENU_BG] },
            { "MenuBar", c[HC_MENU_BG] },
            { "MenuText", c[HC_MENU_TEXT] },
            { "MenuHilight", c[HC_MENU_SEL_BG] },
            { "Window", window },
            { "WindowText", window_text },
            { "WindowFrame", haiku_darker(face, 55) },
            { "TitleText", caption_text },
            { "InactiveTitleText", inactive_text },
            { "GradientActiveTitle", caption },
            { "GradientInactiveTitle", inactive_caption },
            { "ActiveBorder", face },
            { "InactiveBorder", face },
            { "AppWorkSpace", haiku_darker(face, 20) },
            { "Hilight", hilight },
            { "HilightText", hilight_text },
            { "HotTrackingColor", c[HC_NAV] },
            { "ButtonFace", face },
            { "ButtonAlternateFace", haiku_lighter(face, 8) },
            { "ButtonHilight", haiku_lighter(face, 40) },
            { "ButtonLight", haiku_lighter(face, 15) },
            { "ButtonShadow", haiku_darker(face, 25) },
            { "ButtonDkShadow", haiku_darker(face, 55) },
            { "ButtonText", text },
            { "GrayText", haiku_mix(text, face, 55) },
            { "InfoText", c[HC_TIP_TEXT] },
            { "InfoWindow", c[HC_TIP_BG] },
        };

        if (!(hkcu = haiku_open_hkcu())) return;
        name.Buffer = (WCHAR *)colors_keyW;
        name.Length = name.MaximumLength = sizeof(colors_keyW);
        attr.Length = sizeof(attr);
        attr.RootDirectory = hkcu;
        attr.ObjectName = &name;
        attr.Attributes = 0;
        attr.SecurityDescriptor = NULL;
        attr.SecurityQualityOfService = NULL;
        if (!NtCreateKey(&key, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL))
        {
            for (i = 0; i < ARRAY_SIZE(colors); i++) haiku_registry_value(key, colors[i].name, colors[i].rgb);
            NtClose(key);
        }
        NtClose(hkcu);
    }
}

BOOL wayland_process_init(void)
{
    struct wl_display *wl_display_wrapper;

#ifdef __HAIKU__
    haiku_preload_wayland_server();
#endif
    process_wayland.wl_display = wl_display_connect(NULL);
    if (!process_wayland.wl_display)
        return FALSE;

    TRACE("wl_display=%p\n", process_wayland.wl_display);
#ifdef __HAIKU__
    haiku_sync_system_colors();
#endif

#if (WAYLAND_VERSION_MAJOR == 1 && WAYLAND_VERSION_MINOR >= 23)
    if (!(process_wayland.wl_event_queue =
          wl_display_create_queue_with_name(process_wayland.wl_display, process_name ? process_name : "winewayland")))
#else
    if (!(process_wayland.wl_event_queue = wl_display_create_queue(process_wayland.wl_display)))
#endif
    {
        ERR("Failed to create event queue\n");
        return FALSE;
    }

    if (!(wl_display_wrapper = wl_proxy_create_wrapper(process_wayland.wl_display)))
    {
        ERR("Failed to create proxy wrapper for wl_display\n");
        return FALSE;
    }
    wl_proxy_set_queue((struct wl_proxy *) wl_display_wrapper,
                       process_wayland.wl_event_queue);

    process_wayland.wl_registry = wl_display_get_registry(wl_display_wrapper);
    wl_proxy_wrapper_destroy(wl_display_wrapper);
    if (!process_wayland.wl_registry)
    {
        ERR("Failed to get to wayland registry\n");
        return FALSE;
    }

    wayland_window_init();

    /* Populate registry */
    wl_registry_add_listener(process_wayland.wl_registry, &registry_listener, NULL);

    /* We need two roundtrips. One to get and bind globals, one to handle all
     * initial events produced from registering the globals. */
    wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);
    wl_display_roundtrip_queue(process_wayland.wl_display, process_wayland.wl_event_queue);

    /* Check for required protocol globals. */
    if (!process_wayland.wl_compositor)
    {
        ERR("Wayland compositor doesn't support wl_compositor\n");
        return FALSE;
    }
    if (!process_wayland.xdg_wm_base)
    {
        ERR("Wayland compositor doesn't support xdg_wm_base\n");
        return FALSE;
    }
    if (!process_wayland.wl_shm)
    {
        ERR("Wayland compositor doesn't support wl_shm\n");
        return FALSE;
    }
    if (!process_wayland.wl_subcompositor)
    {
        ERR("Wayland compositor doesn't support wl_subcompositor\n");
        return FALSE;
    }
    if (!process_wayland.wp_viewporter)
    {
        ERR("Wayland compositor doesn't support wp_viewporter\n");
        return FALSE;
    }

    /* Check for optional globals. */
    if (!process_wayland.zwp_pointer_constraints_v1)
        ERR("Wayland compositor doesn't support optional zwp_pointer_constraints_v1 (pointer locking/confining won't work)\n");

    if (!process_wayland.zwp_relative_pointer_manager_v1)
        ERR("Wayland compositor doesn't support optional zwp_relative_pointer_manager_v1 (relative motion won't work)\n");

    if (!process_wayland.zwp_text_input_manager_v3)
        ERR("Wayland compositor doesn't support optional zwp_text_input_manager_v3 (host input methods won't work)\n");

    if (!process_wayland.zwlr_data_control_manager_v1)
    {
        if (!process_wayland.wl_data_device_manager)
            ERR("Wayland compositor doesn't support optional wl_data_device_manager (clipboard won't work)\n");
        else
            ERR("Wayland compositor doesn't support optional zwlr_data_control_manager_v1 (clipboard functionality will be limited)\n");
    }

    if (!process_wayland.xdg_toplevel_icon_manager_v1)
        ERR("Wayland compositor doesn't support xdg_toplevel_icon_manager_v1 (window icons will not be supported)\n");

    if (!process_wayland.wp_fractional_scale_manager_v1)
        ERR("Wayland compositor doesn't support wp_fractional_scale_manager_v1 (fractional scaling will be broken)\n");

    if (process_wayland.wp_color_manager_v1) color_management_init();
    else WARN("Wayland compositor doesn't support wp_color_manager_v1 (color management will be broken)\n");

    process_wayland.initialized = TRUE;

    return TRUE;
}
