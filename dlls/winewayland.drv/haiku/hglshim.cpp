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
#include <Window.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>

extern "C" void *_glapi_get_proc_address(const char *name);

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
    return ctx;
}

extern "C" void hgl_context_destroy(struct hgl_context *ctx)
{
    if (!ctx) return;
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

extern "C" int hgl_context_size(struct hgl_context *ctx, int *width, int *height)
{
    *width = ctx->width;
    *height = ctx->height;
    return 1;
}

/* Copy the lower left width x height pixels of the current back (or only) buffer to out as top-down BGRA rows. The context
 * must be current in the calling thread. */
extern "C" int hgl_read_pixels(struct hgl_context *ctx, int width, int height, void *out, int stride)
{
    unsigned char *tmp;
    int y;

    if (ctx->owner != find_thread(NULL)) return 0;
    if (width > ctx->width || height > ctx->height) return 0;
    if (!(tmp = (unsigned char *)malloc((size_t)width * height * 4))) return 0;

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, width, height, 0x80E1 /* GL_BGRA */, GL_UNSIGNED_BYTE, tmp);
    for (y = 0; y < height; y++)
        memcpy((char *)out + (size_t)y * stride, tmp + (size_t)(height - 1 - y) * width * 4, (size_t)width * 4);
    free(tmp);
    return 1;
}

extern "C" void *hgl_proc_address(const char *name)
{
    return _glapi_get_proc_address(name);
}
