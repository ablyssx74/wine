/*
 * Haiku OpenGL driver for the Wayland driver
 *
 * Haiku's EGL cannot open a display, so OpenGL is reached through BGLView by libwinehglshim.so (see haiku/hglshim.cpp). Every
 * Wine context owns one hidden BGLView; its pixels are copied into a shared memory buffer of the window's client surface when
 * the program swaps. Contexts cannot share objects, so wglShareLists fails.
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

#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#include "waylanddrv.h"
#include "wine/debug.h"
#include "wine/opengl_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#define HGL_INITIAL_SIZE 64    /* a context starts tiny and grows to the windows it is used with */
#define HGL_SIZE_STEP    128
#define BUFFER_POOL     4

struct hgl_context;

static struct
{
    void *handle;
    int (*init)(void);
    struct hgl_context *(*context_create)(int width, int height, int alpha, int doublebuffer, int depth, int stencil);
    void (*context_destroy)(struct hgl_context *ctx);
    int (*make_current)(struct hgl_context *ctx);
    void (*release_current)(struct hgl_context *ctx);
    int (*context_size)(struct hgl_context *ctx, int *width, int *height);
    int (*context_resize)(struct hgl_context *ctx, int width, int height);
    int (*read_pixels)(struct hgl_context *ctx, int width, int height, void *out, int stride);
    void *(*proc_address)(const char *name);
} hgl;

static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs haiku_drawable_funcs;
static __thread struct hgl_context *thread_current;

static const struct
{
    BOOL alpha;
} formats[] =
{
    { TRUE },
    { FALSE },
};

struct haiku_context
{
    struct opengl_context base;
    struct hgl_context *hgl;
    BOOL alpha;
    BOOL viewport_set;
};

struct haiku_drawable
{
    struct opengl_drawable base;
    struct hgl_context *current;    /* context last made current with this drawable */
    struct haiku_context *context;
    struct wayland_shm_buffer *pool[BUFFER_POOL];
};

static struct haiku_drawable *impl_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct haiku_drawable, base);
}

static BOOL load_shim(void)
{
    Dl_info info;
    char path[1024];
    void *handle = NULL;

    if (hgl.handle) return TRUE;

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

#define LOAD(name) if (!(hgl.name = dlsym(handle, "hgl_" #name))) { WARN("Missing hgl_" #name "\n"); dlclose(handle); return FALSE; }
    LOAD(init) LOAD(context_create) LOAD(context_destroy) LOAD(make_current) LOAD(release_current)
    LOAD(context_size) LOAD(context_resize) LOAD(read_pixels) LOAD(proc_address)
#undef LOAD

    if (!hgl.init())
    {
        WARN("The Haiku OpenGL bridge could not start an application connection\n");
        dlclose(handle);
        return FALSE;
    }
    hgl.handle = handle;
    return TRUE;
}

static void *haiku_get_proc_address(const char *name)
{
    return hgl.proc_address(name);
}

static UINT haiku_init_pixel_formats(UINT *onscreen_count)
{
    *onscreen_count = ARRAY_SIZE(formats);
    return ARRAY_SIZE(formats);
}

static BOOL haiku_describe_pixel_format(int format, struct wgl_pixel_format *fmt)
{
    PIXELFORMATDESCRIPTOR *pfd = &fmt->pfd;

    if (format < 1 || format > ARRAY_SIZE(formats)) return FALSE;

    memset(fmt, 0, sizeof(*fmt));
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_SUPPORT_OPENGL | PFD_SUPPORT_COMPOSITION | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->iLayerType = PFD_MAIN_PLANE;
    pfd->cColorBits = 32;
    pfd->cRedBits = pfd->cGreenBits = pfd->cBlueBits = 8;
    pfd->cAlphaBits = formats[format - 1].alpha ? 8 : 0;
    pfd->cBlueShift = 0;
    pfd->cGreenShift = 8;
    pfd->cRedShift = 16;
    pfd->cAlphaShift = pfd->cAlphaBits ? 24 : 0;
    pfd->cDepthBits = 24;
    pfd->cStencilBits = 8;

    fmt->swap_method = WGL_SWAP_UNDEFINED_ARB;
    fmt->transparent = GL_FALSE;
    fmt->pixel_type = WGL_TYPE_RGBA_ARB;
    fmt->transparent_alpha_value_valid = GL_TRUE;
    fmt->transparent_index_value_valid = GL_TRUE;
    return TRUE;
}

static void haiku_init_extensions(struct opengl_funcs *funcs, BOOLEAN extensions[GL_EXTENSION_COUNT])
{
}

static void haiku_drawable_destroy(struct opengl_drawable *base)
{
    struct haiku_drawable *gl = impl_from_opengl_drawable(base);
    int i;

    for (i = 0; i < BUFFER_POOL; i++)
        if (gl->pool[i]) wayland_shm_buffer_unref(gl->pool[i]);
}

static void haiku_drawable_flush(struct opengl_drawable *base, UINT flags)
{
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;
    shm_buffer->busy = FALSE;
    wayland_shm_buffer_unref(shm_buffer);
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

/* a buffer of the wanted size that the compositor is not holding */
static struct wayland_shm_buffer *get_buffer(struct haiku_drawable *gl, int width, int height)
{
    struct wayland_shm_buffer *buffer;
    int i, free_slot = -1;

    for (i = 0; i < BUFFER_POOL; i++)
    {
        buffer = gl->pool[i];
        if (!buffer) { if (free_slot < 0) free_slot = i; continue; }
        if (buffer->busy) continue;
        if (buffer->width == width && buffer->height == height) return buffer;
        /* the window changed size: let the old buffer go (the compositor does not hold it) */
        wayland_shm_buffer_unref(buffer);
        gl->pool[i] = NULL;
        if (free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;

    if (!(buffer = wayland_shm_buffer_create(width, height, WL_SHM_FORMAT_XRGB8888))) return NULL;
    wl_proxy_set_queue((struct wl_proxy *)buffer->wl_buffer, process_wayland.wl_event_queue);
    wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
    gl->pool[free_slot] = buffer;
    return buffer;
}

static BOOL haiku_drawable_swap(struct opengl_drawable *base)
{
    struct haiku_drawable *gl = impl_from_opengl_drawable(base);
    struct wayland_client_surface *client;
    struct wayland_shm_buffer *buffer;
    int width, height, ctx_width, ctx_height;

    if (!gl->current || !base->client) return TRUE;

    width = base->virtual_size.cx;
    height = base->virtual_size.cy;
    hgl.context_size(gl->current, &ctx_width, &ctx_height);
    if ((width > ctx_width || height > ctx_height) && thread_current == gl->current)
    {
        /* the window outgrew the context's surface since the context was made current: grow it (what was drawn is lost) */
        int want_w = (width + HGL_SIZE_STEP - 1) / HGL_SIZE_STEP * HGL_SIZE_STEP;
        int want_h = (height + HGL_SIZE_STEP - 1) / HGL_SIZE_STEP * HGL_SIZE_STEP;

        hgl.release_current(gl->current);
        if (!hgl.context_resize(gl->current, want_w, want_h)) WARN("Growing the context to %dx%d failed\n", want_w, want_h);
        hgl.make_current(gl->current);
        hgl.context_size(gl->current, &ctx_width, &ctx_height);
    }
    if (width > ctx_width) width = ctx_width;
    if (height > ctx_height) height = ctx_height;
    if (width <= 0 || height <= 0) return TRUE;

    if (!(buffer = get_buffer(gl, width, height)))
    {
        WARN("No buffer available for %s, dropping the frame\n", debugstr_opengl_drawable(base));
        return TRUE;
    }
    if (!hgl.read_pixels(gl->current, width, height, buffer->map_data, width * 4))
    {
        WARN("Reading the pixels of %s failed\n", debugstr_opengl_drawable(base));
        return TRUE;
    }

    client = impl_from_client_surface(base->client);
    buffer->busy = TRUE;
    wayland_shm_buffer_ref(buffer);
    wl_surface_attach(client->wl_surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(client->wl_surface, 0, 0, width, height);
    wl_surface_commit(client->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    client_surface_present(base->client);
    return TRUE;
}

static const struct opengl_drawable_funcs haiku_drawable_funcs =
{
    .size = sizeof(struct haiku_drawable),
    .destroy = haiku_drawable_destroy,
    .flush = haiku_drawable_flush,
    .swap = haiku_drawable_swap,
};

static BOOL haiku_surface_create(struct client_surface *client, int format, struct opengl_drawable **drawable)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct haiku_drawable *gl;

    TRACE("client=%s format=%d\n", debugstr_client_surface(client), format);

    if (!(gl = opengl_drawable_create(&haiku_drawable_funcs, format, client, NULL))) return FALSE;
    set_client_surface(client->hwnd, surface);
    *drawable = &gl->base;
    return TRUE;
}

static BOOL haiku_null_surface_create(int format, struct opengl_drawable **drawable)
{
    struct haiku_drawable *gl;
    SIZE size = { 1, 1 };

    if (!(gl = opengl_drawable_create(&haiku_drawable_funcs, format, NULL, &size))) return FALSE;
    *drawable = &gl->base;
    return TRUE;
}

static BOOL haiku_pbuffer_create(HDC hdc, int format, SIZE size, BOOL largest, GLenum texture_format, GLenum texture_target,
                                 GLint max_level, struct opengl_drawable **drawable)
{
    FIXME("pbuffers are not supported on Haiku yet\n");
    return FALSE;
}

static struct opengl_context *haiku_context_create(const struct opengl_context_attrs *attrs, struct opengl_context *share)
{
    struct haiku_context *context;
    BOOL alpha = attrs->format >= 1 && attrs->format <= ARRAY_SIZE(formats) ? formats[attrs->format - 1].alpha : TRUE;

    TRACE("attrs %s, share %p\n", debugstr_opengl_context_attrs(attrs), share);

    if (!(context = calloc(1, sizeof(*context)))) return NULL;
    context->alpha = alpha;
    if (!(context->hgl = hgl.context_create(HGL_INITIAL_SIZE, HGL_INITIAL_SIZE, alpha, TRUE, TRUE, TRUE)))
    {
        WARN("Creating the Haiku GL context failed\n");
        free(context);
        return NULL;
    }
    context->base.host_context = context->hgl;
    TRACE("Created context %p (hgl %p)\n", context, context->hgl);
    return &context->base;
}

static BOOL haiku_context_destroy(struct opengl_context *base)
{
    struct haiku_context *context = CONTAINING_RECORD(base, struct haiku_context, base);

    TRACE("context %p\n", context);
    if (thread_current == context->hgl)
    {
        hgl.release_current(context->hgl);
        thread_current = NULL;
    }
    hgl.context_destroy(context->hgl);
    free(context);
    return TRUE;
}

static BOOL haiku_context_activate(struct opengl_context *base, struct opengl_drawable *draw, struct opengl_drawable *read)
{
    struct haiku_context *context = CONTAINING_RECORD(base, struct haiku_context, base);
    struct haiku_drawable *gl = draw && draw->funcs == &haiku_drawable_funcs ? impl_from_opengl_drawable(draw) : NULL;

    TRACE("context %p, draw %s, read %s\n", context, debugstr_opengl_drawable(draw), debugstr_opengl_drawable(read));

    if (thread_current && thread_current != context->hgl) hgl.release_current(thread_current);
    if (draw && draw->virtual_size.cx > 0 && draw->virtual_size.cy > 0)
    {
        /* grow the context's surface to the window, in steps so that small resizes do not do it every time */
        int width, height, want_w, want_h;

        hgl.context_size(context->hgl, &width, &height);
        want_w = (draw->virtual_size.cx + HGL_SIZE_STEP - 1) / HGL_SIZE_STEP * HGL_SIZE_STEP;
        want_h = (draw->virtual_size.cy + HGL_SIZE_STEP - 1) / HGL_SIZE_STEP * HGL_SIZE_STEP;
        if (want_w > width || want_h > height)
        {
            if (thread_current == context->hgl) { hgl.release_current(context->hgl); thread_current = NULL; }
            if (!hgl.context_resize(context->hgl, want_w, want_h))
                WARN("Growing the context %p to %dx%d failed\n", context, want_w, want_h);
        }
    }
    hgl.make_current(context->hgl);
    thread_current = context->hgl;

    if (gl)
    {
        gl->current = context->hgl;
        gl->context = context;
        if (!context->viewport_set && draw->virtual_size.cx > 0 && draw->virtual_size.cy > 0)
        {
            /* a new context starts with the viewport and scissor of its whole surface; here that is the window */
            funcs->p_glViewport(0, 0, draw->virtual_size.cx, draw->virtual_size.cy);
            context->viewport_set = TRUE;
        }
    }
    return TRUE;
}

static BOOL haiku_cleanup_thread(void)
{
    if (thread_current)
    {
        hgl.release_current(thread_current);
        thread_current = NULL;
    }
    return TRUE;
}

static struct opengl_driver_funcs haiku_driver_funcs =
{
    .p_get_proc_address = haiku_get_proc_address,
    .p_init_pixel_formats = haiku_init_pixel_formats,
    .p_describe_pixel_format = haiku_describe_pixel_format,
    .p_init_extensions = haiku_init_extensions,
    .p_surface_create = haiku_surface_create,
    .p_null_surface_create = haiku_null_surface_create,
    .p_pbuffer_create = haiku_pbuffer_create,
    .p_context_create = haiku_context_create,
    .p_context_destroy = haiku_context_destroy,
    .p_context_activate = haiku_context_activate,
    .p_cleanup_thread = haiku_cleanup_thread,
    .broken_sharing = TRUE,
};

/* Use the Haiku driver when EGL has no Wayland platform. Returns whether it was installed. */
BOOL wayland_haiku_opengl_init(const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    const char *exts;

    if (opengl_funcs->egl_handle && opengl_funcs->p_eglQueryString &&
        (exts = opengl_funcs->p_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)) &&
        (strstr(exts, "EGL_KHR_platform_wayland") || strstr(exts, "EGL_EXT_platform_wayland")))
        return FALSE;

    if (!load_shim()) return FALSE;
    funcs = opengl_funcs;
    *driver_funcs = &haiku_driver_funcs;
    TRACE("Using Haiku's native OpenGL\n");
    return TRUE;
}

#endif /* __HAIKU__ */
