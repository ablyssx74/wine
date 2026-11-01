/*
 * C bridge to Haiku's native OpenGL for the Wayland driver
 *
 * Haiku's EGL cannot open a display and its OpenGL is reached through BGLView only: a context lives inside a view in a window.
 * Each context made here owns a window that is never shown and a view in it, big enough for any window the program draws to;
 * the driver renders there and copies the pixels out with hgl_read_pixels(). BGLView has no way to share objects between
 * contexts, so contexts made here are independent.
 *
 * This is C++ against <GLView.h>, so it is built apart from Wine (see build.sh) into libwinehglshim.so, which the driver loads
 * with dlopen() when Haiku's EGL is no use.
 */

#include <Application.h>
#include <GLView.h>
#include <OS.h>
#include <Screen.h>
#include <Window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <GL/gl.h>


struct hgl_context
{
    BWindow *window;
    BGLView *view;
    int width, height;          /* size of the view */
    thread_id owner;            /* thread the context is current in, or -1 */
};

static pthread_mutex_t app_lock = PTHREAD_MUTEX_INITIALIZER;
static thread_id app_thread = -1;
static sem_id app_ready = -1;
static std::vector<hgl_context *> live_contexts;     /* every context that has not been destroyed (guarded by app_lock) */
static bool exit_hook_set = false;

static bool debug_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("WINE_HAIKU_GL_DEBUG") != NULL;
    return enabled == 1;
}

/* When the process ends, it leaves at once, without the clean-up of the libraries. Haiku's OpenGL stack (libglvnd, Mesa, Zink on
 * the GPU) cannot be taken down while the process is ending: the windows and views of the contexts that were left (Wine does not
 * delete the contexts of a program that exits without doing it) hold references to objects of those libraries, other threads of
 * the stack were stopped by Wine in the middle of what they were doing, and the clean-up either finds a reference count that is
 * not zero (Haiku's BReferenceable calls the debugger and the process stays on the screen, stopped) or a lock that its owner
 * never gave back (an abort). A program that restarts itself, which has to end the old process, ended this way and froze the
 * system. Wine itself ends a process without this clean-up when it has to (abort_process() uses _exit()), and the server has the
 * exit code already, so nothing is lost; the system frees the process's windows, ports and memory. This handler was registered
 * after the libraries were loaded, so it runs before their own. */
static void exit_cleanup(void)
{
    if (debug_enabled())
    {
        pthread_mutex_lock(&app_lock);
        const size_t count = live_contexts.size();
        pthread_mutex_unlock(&app_lock);
        fprintf(stderr, "hgl: ending, %d context(s) left; leaving without taking them down\n", (int)count);
    }
    fflush(NULL);
    _exit(0);
}

static int32 app_main(void *)
{
    BApplication *app = new BApplication("application/x-vnd.wine-hglshim");
    release_sem(app_ready);
    app->Run();
    delete app;
    return 0;
}

/* make sure the process has a BApplication (Wine processes that show windows already have one) */
extern "C" int hgl_init(void)
{
    int ok = 1;

    pthread_mutex_lock(&app_lock);
    if (be_app == NULL && app_thread < 0)
    {
        app_ready = create_sem(0, "hgl app ready");
        app_thread = spawn_thread(app_main, "hgl app", B_NORMAL_PRIORITY, NULL);
        if (app_thread < 0 || resume_thread(app_thread) != B_OK) ok = 0;
        else acquire_sem_etc(app_ready, 1, B_RELATIVE_TIMEOUT, 5000000);
        delete_sem(app_ready);
    }
    if (be_app == NULL) ok = 0;
    if (ok && !exit_hook_set)
    {
        exit_hook_set = true;
        atexit(exit_cleanup);
    }
    pthread_mutex_unlock(&app_lock);
    return ok;
}

extern "C" struct hgl_context *hgl_context_create(int width, int height, int alpha, int doublebuffer, int depth, int stencil)
{
    uint32 options = BGL_RGB;
    hgl_context *ctx;

    if (!hgl_init()) return NULL;
    if (alpha) options |= BGL_ALPHA;
    if (doublebuffer) options |= BGL_DOUBLE;
    if (depth) options |= BGL_DEPTH;
    if (stencil) options |= BGL_STENCIL;

    /* With Mesa's software renderer a surface can be grown after it was first used; Zink on a GPU (Nebula, through libglvnd) keeps
     * the size its swapchain was made with, so a surface that starts small stays small and only its corner is ever drawn. A
     * program does not tell its window's size before the first use, so there the surface starts as large as the screen. Mesa's
     * libglapi is what tells the two apart. */
    if (!dlsym(RTLD_DEFAULT, "_glapi_get_proc_address"))
    {
        BScreen screen(B_MAIN_SCREEN_ID);
        if (screen.IsValid())
        {
            BRect frame = screen.Frame();
            if ((int)frame.Width() + 1 > width) width = (int)frame.Width() + 1;
            if ((int)frame.Height() + 1 > height) height = (int)frame.Height() + 1;
        }
    }

    if (!(ctx = (hgl_context *)calloc(1, sizeof(*ctx)))) return NULL;
    ctx->width = width;
    ctx->height = height;
    ctx->owner = -1;
    ctx->window = new BWindow(BRect(0, 0, width - 1, height - 1), "wine gl", B_NO_BORDER_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
                              B_NOT_RESIZABLE | B_AVOID_FOCUS | B_NO_WORKSPACE_ACTIVATION);
    ctx->view = new BGLView(BRect(0, 0, width - 1, height - 1), "gl", B_FOLLOW_ALL, 0, options);
    ctx->window->Lock();
    ctx->window->AddChild(ctx->view);
    ctx->window->Unlock();
    ctx->window->Run();     /* starts the window's thread; the window is never shown */
    pthread_mutex_lock(&app_lock);
    live_contexts.push_back(ctx);
    pthread_mutex_unlock(&app_lock);
    if (debug_enabled()) fprintf(stderr, "hgl: context %p made, %dx%d\n", (void *)ctx, ctx->width, ctx->height);
    return ctx;
}

extern "C" void hgl_context_destroy(struct hgl_context *ctx)
{
    if (!ctx) return;
    pthread_mutex_lock(&app_lock);
    for (size_t i = 0; i < live_contexts.size(); i++)
    {
        if (live_contexts[i] == ctx) { live_contexts.erase(live_contexts.begin() + i); break; }
    }
    pthread_mutex_unlock(&app_lock);
    if (debug_enabled()) fprintf(stderr, "hgl: context %p destroyed\n", (void *)ctx);
    if (ctx->owner >= 0) ctx->view->UnlockGL();
    if (ctx->window->Lock()) ctx->window->Quit();
    free(ctx);
}

extern "C" int hgl_make_current(struct hgl_context *ctx)
{
    if (ctx->owner == find_thread(NULL)) return 1;
    ctx->view->LockGL();
    ctx->owner = find_thread(NULL);
    return 1;
}

extern "C" void hgl_release_current(struct hgl_context *ctx)
{
    if (ctx->owner != find_thread(NULL)) return;
    ctx->owner = -1;
    ctx->view->UnlockGL();
}

/* Make the context's surface at least width x height pixels (it only grows). The context must not be current in any thread;
 * the new size is in effect when this returns. */
extern "C" int hgl_context_resize(struct hgl_context *ctx, int width, int height)
{
    bigtime_t deadline = system_time() + 2000000;

    if (width <= ctx->width && height <= ctx->height) return 1;
    if (width < ctx->width) width = ctx->width;
    if (height < ctx->height) height = ctx->height;
    if (ctx->owner >= 0) return 0;

    ctx->window->Lock();
    ctx->window->ResizeTo(width - 1, height - 1);
    ctx->window->Unlock();

    /* the window's own thread applies the new size and tells the view (and so its renderer) */
    for (;;)
    {
        BRect bounds;
        ctx->window->Lock();
        bounds = ctx->view->Bounds();
        ctx->window->Unlock();
        if ((int)bounds.Width() + 1 == width && (int)bounds.Height() + 1 == height) break;
        if (system_time() > deadline) return 0;
        snooze(2000);
    }
    ctx->window->Lock();    /* one more round trip, so the resize hook has run */
    ctx->window->Unlock();
    ctx->width = width;
    ctx->height = height;
    return 1;
}

extern "C" int hgl_context_size(struct hgl_context *ctx, int *width, int *height)
{
    *width = ctx->width;
    *height = ctx->height;
    return 1;
}

extern "C" void *hgl_proc_address(const char *name);

/* Copy the lower left width x height pixels of the current back (or only) buffer to out as top-down BGRA rows. The context
 * must be current in the calling thread. */
extern "C" int hgl_read_pixels(struct hgl_context *ctx, int width, int height, void *out, int stride)
{
    unsigned char *tmp;
    int y;

    if (ctx->owner != find_thread(NULL)) return 0;
    if (width > ctx->width || height > ctx->height) return 0;
    if (!(tmp = (unsigned char *)malloc((size_t)width * height * 4))) return 0;

    /* The window is the default framebuffer: a program (wined3d under Direct3D 10/11) may have left one of its own bound for
     * reading, holding its picture upside down, so read from the default one and put the binding back */
    typedef void (*bindfb_t)(unsigned, unsigned);
    static bindfb_t bind_fb;
    static int looked;
    int old_read = 0;
    if (!looked) { bind_fb = (bindfb_t)hgl_proc_address("glBindFramebuffer"); looked = 1; }
    if (bind_fb)
    {
        glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_read);
        if (old_read) bind_fb(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, width, height, 0x80E1 /* GL_BGRA */, GL_UNSIGNED_BYTE, tmp);
    if (bind_fb && old_read) bind_fb(0x8CA8, (unsigned)old_read);
    for (y = 0; y < height; y++)
        memcpy((char *)out + (size_t)y * stride, tmp + (size_t)(height - 1 - y) * width * 4, (size_t)width * 4);
    free(tmp);
    return 1;
}

/* The address of a GL function. Mesa's libGL has the table of them in libglapi; libglvnd's (Nebula, which replaces Mesa and has no
 * libglapi) answers through libEGL's eglGetProcAddress, or exports the function itself. Nothing here needs libglapi to be there. */
extern "C" void *hgl_proc_address(const char *name)
{
    typedef void *(*getproc_t)(const char *);
    static getproc_t glapi_get, egl_get;
    static int looked;
    void *p = NULL;

    pthread_mutex_lock(&app_lock);
    if (!looked)
    {
        void *egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
        glapi_get = (getproc_t)dlsym(RTLD_DEFAULT, "_glapi_get_proc_address");
        if (egl) egl_get = (getproc_t)dlsym(egl, "eglGetProcAddress");
        looked = 1;
    }
    pthread_mutex_unlock(&app_lock);

    if (glapi_get) p = glapi_get(name);
    if (!p && egl_get) p = egl_get(name);
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}
