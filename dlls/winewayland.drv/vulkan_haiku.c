/*
 * Haiku Vulkan driver for the Wayland driver
 *
 * Haiku's Vulkan (NVK on Nebula) has no Wayland surface that works with the Wayland server of this driver, but it has the
 * VideoStreamsWsi layer: a VK_EXT_headless_surface whose swapchain copies every presented image into a bitmap and hands it to a
 * hook (see haiku/hglshim.cpp). A program's VK_KHR_win32_surface is made a headless surface of the layer, and the hook copies
 * each frame into a shared memory buffer of the window's client surface, as the OpenGL driver does when the program swaps.
 *
 * Copyright 2026 the Wine Haiku port
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifdef __HAIKU__

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#include "waylanddrv.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#define BUFFER_POOL 4
#define LAYER_NAME "VK_LAYER_window_system_integration"

static struct
{
    void *handle;
    int (*init)(void);
    void *(*surface_attach)(void *surface, void (*size)(void *, unsigned *, unsigned *),
                            void (*frame)(void *, const void *, int, int, int), void *user);
} shim;

/* what the window shows of one headless surface */
struct haiku_vk_surface
{
    struct wayland_client_surface *client;
    struct wayland_shm_buffer *pool[BUFFER_POOL];
};

static BOOL load_shim(void)
{
    Dl_info info;
    char path[1024];
    void *handle = NULL;

    if (shim.handle) return TRUE;

    if (dladdr((void *)load_shim, &info) && info.dli_fname)
    {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash)
        {
            snprintf(path, sizeof(path), "%.*s/libwinehglshim.so", (int)(slash - info.dli_fname), info.dli_fname);
            handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        }
    }
    if (!handle) handle = dlopen("libwinehglshim.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        WARN("Failed to load libwinehglshim.so: %s\n", dlerror());
        return FALSE;
    }

    if (!(shim.init = dlsym(handle, "hgl_init")) || !(shim.surface_attach = dlsym(handle, "hvk_surface_attach")))
    {
        WARN("libwinehglshim.so has no Vulkan support\n");
        dlclose(handle);
        return FALSE;
    }
    if (!shim.init())
    {
        WARN("The Haiku bridge could not start an application connection\n");
        dlclose(handle);
        return FALSE;
    }
    shim.handle = handle;
    return TRUE;
}

/* whether the loader has the layer: its surfaces are the only ones that take the hook */
static BOOL layer_present(void *vulkan_handle)
{
    VkResult (*enumerate)(uint32_t *, VkLayerProperties *);
    VkLayerProperties *layers;
    uint32_t count = 0, i;
    BOOL found = FALSE;

    if (!(enumerate = dlsym(vulkan_handle, "vkEnumerateInstanceLayerProperties"))) return FALSE;
    if (enumerate(&count, NULL) || !count) return FALSE;
    if (!(layers = calloc(count, sizeof(*layers)))) return FALSE;
    if (!enumerate(&count, layers))
    {
        for (i = 0; i < count; i++)
        {
            if (!strcmp(layers[i].layerName, LAYER_NAME)) { found = TRUE; break; }
        }
    }
    free(layers);
    return found;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;
    shm_buffer->busy = FALSE;
    wayland_shm_buffer_unref(shm_buffer);
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

/* a buffer of the wanted size that the compositor is not holding */
static struct wayland_shm_buffer *get_buffer(struct haiku_vk_surface *surface, int width, int height)
{
    struct wayland_shm_buffer *buffer;
    int i, free_slot = -1;

    for (i = 0; i < BUFFER_POOL; i++)
    {
        buffer = surface->pool[i];
        if (!buffer) { if (free_slot < 0) free_slot = i; continue; }
        if (buffer->busy) continue;
        if (buffer->width == width && buffer->height == height) return buffer;
        /* the swapchain changed size: let the old buffer go (the compositor does not hold it) */
        wayland_shm_buffer_unref(buffer);
        surface->pool[i] = NULL;
        if (free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;

    if (!(buffer = wayland_shm_buffer_create(width, height, WL_SHM_FORMAT_XRGB8888))) return NULL;
    wl_proxy_set_queue((struct wl_proxy *)buffer->wl_buffer, process_wayland.wl_event_queue);
    wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
    surface->pool[free_slot] = buffer;
    return buffer;
}

/* the size of the window's client area, until the first frame tells the size of the swapchain */
static void surface_size(void *user, unsigned *width, unsigned *height)
{
    struct haiku_vk_surface *surface = user;
    const struct client_surface *client = &surface->client->client;

    *width = max(0, client->virtual_rect.right - client->virtual_rect.left);
    *height = max(0, client->virtual_rect.bottom - client->virtual_rect.top);
}

/* a presented image: called by the layer on the thread that presents, with the pixels as B8G8R8A8 rows */
static void surface_frame(void *user, const void *bits, int width, int height, int bytes_per_row)
{
    struct haiku_vk_surface *surface = user;
    struct wayland_shm_buffer *buffer;
    int y;

    if (width <= 0 || height <= 0) return;
    if (!(buffer = get_buffer(surface, width, height)))
    {
        WARN("No buffer available, dropping the frame\n");
        return;
    }

    if (bytes_per_row == width * 4)
        memcpy(buffer->map_data, bits, (size_t)width * height * 4);
    else
        for (y = 0; y < height; y++)
            memcpy((char *)buffer->map_data + (size_t)y * width * 4, (const char *)bits + (size_t)y * bytes_per_row, (size_t)width * 4);

    buffer->busy = TRUE;
    wayland_shm_buffer_ref(buffer);
    wl_surface_attach(surface->client->wl_surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(surface->client->wl_surface, 0, 0, width, height);
    wl_surface_commit(surface->client->wl_surface);
    wl_display_flush(process_wayland.wl_display);
}

static VkResult haiku_vulkan_surface_create(struct client_surface *client, const struct vulkan_instance *instance, VkSurfaceKHR *handle)
{
    VkHeadlessSurfaceCreateInfoEXT create_info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    struct wayland_client_surface *wayland_client = impl_from_client_surface(client);
    struct haiku_vk_surface *surface;
    VkResult res;

    TRACE("%s %p %p\n", debugstr_client_surface(client), instance, handle);

    if (!(surface = calloc(1, sizeof(*surface)))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    surface->client = wayland_client;

    if ((res = instance->p_vkCreateHeadlessSurfaceEXT(instance->host.instance, &create_info, NULL /* allocator */, handle)))
    {
        free(surface);
        return res;
    }
    /* (the surface, its hook and the buffers live as long as the process: the layer tells nothing when a surface goes) */
    if (!shim.surface_attach((void *)(ULONG_PTR)*handle, surface_size, surface_frame, surface))
    {
        WARN("The layer's surface took no hook, nothing will be shown\n");
        free(surface);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    set_client_surface(client->hwnd, wayland_client);

    TRACE("Created surface=0x%s\n", wine_dbgstr_longlong(*handle));
    return VK_SUCCESS;
}

static VkBool32 haiku_get_physical_device_presentation_support(struct vulkan_physical_device *physical_device, uint32_t index)
{
    return VK_TRUE;
}

/* The layer answers the base surface functions (capabilities, formats, present modes) but only has stubs that fail with VK_NOT_READY for
 * the ones of VK_KHR_get_surface_capabilities2, and nothing of VK_EXT_surface_maintenance1 / VK_EXT_swapchain_maintenance1 /
 * VK_KHR_present_id / VK_KHR_present_wait, which the driver below it (NVK) offers: a program that used them (DXVK does) got no
 * surface capabilities and so no swapchain. They are not offered to programs, which then use the base functions. */
static void haiku_map_instance_extensions(struct vulkan_instance_extensions *extensions)
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_headless_surface = 1;
    if (extensions->has_VK_EXT_headless_surface) extensions->has_VK_KHR_win32_surface = 1;
    extensions->has_VK_KHR_get_surface_capabilities2 = 0;
    extensions->has_VK_EXT_surface_maintenance1 = 0;
}

static void haiku_map_device_extensions(struct vulkan_device_extensions *extensions)
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
    extensions->has_VK_EXT_swapchain_maintenance1 = 0;
    extensions->has_VK_KHR_present_id = 0;
    extensions->has_VK_KHR_present_wait = 0;
    /* the layer makes its swapchain images from the swapchain's format only, without the mutable-format flag and the list of formats:
     * a view of another format (DXVK makes sRGB ones) is then not allowed for them, and NVK stops the program on an assertion */
    extensions->has_VK_KHR_swapchain_mutable_format = 0;
}

static const struct vulkan_driver_funcs haiku_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = haiku_vulkan_surface_create,
    .p_get_physical_device_presentation_support = haiku_get_physical_device_presentation_support,
    .p_map_instance_extensions = haiku_map_instance_extensions,
    .p_map_device_extensions = haiku_map_device_extensions,
};

/* Use the layer's headless surfaces when the loader has the layer and the bridge library loads. WINE_HAIKU_VK=wayland keeps the
 * Wayland surface of the Vulkan driver. */
BOOL wayland_haiku_vulkan_init(void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    const char *choice = getenv("WINE_HAIKU_VK");

    if (choice && !strcmp(choice, "wayland")) return FALSE;
    if (!layer_present(vulkan_handle))
    {
        TRACE("The loader has no %s\n", LAYER_NAME);
        return FALSE;
    }
    if (!load_shim()) return FALSE;

    *driver_funcs = &haiku_vulkan_driver_funcs;
    TRACE("Using the Haiku Vulkan layer for window surfaces\n");
    return TRUE;
}

#endif /* __HAIKU__ */
