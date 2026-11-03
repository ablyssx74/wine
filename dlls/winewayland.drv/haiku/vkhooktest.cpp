/* A test of the Vulkan hook of the bridge (hglshim.cpp) without a GPU: a stand-in for the layer's surface hands bitmaps to the hook
 * the way the layer does. Build: g++ -o vkhooktest vkhooktest.cpp -lbe   Run with libwinehglshim.so in the same folder. */
#include <Bitmap.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

class BitmapHook {
public:
    virtual ~BitmapHook() {}
    virtual void GetSize(uint32_t &width, uint32_t &height) = 0;
    virtual BBitmap *SetBitmap(BBitmap *bmp) = 0;
};

class VKLayerSurfaceBase {
public:
    virtual ~VKLayerSurfaceBase() {}
    virtual void SetBitmapHook(BitmapHook *hook) = 0;
};

class MockSurface : public VKLayerSurfaceBase {
public:
    BitmapHook *hook = NULL;
    void SetBitmapHook(BitmapHook *h) override { hook = h; }
};

static int frames, last_w, last_h, last_stride;
static unsigned first_pixel;

static void size_cb(void *, unsigned *w, unsigned *h) { *w = 320; *h = 200; }
static void frame_cb(void *, const void *bits, int w, int h, int stride)
{
    frames++;
    last_w = w; last_h = h; last_stride = stride;
    first_pixel = *(const unsigned *)bits;
}

typedef void *(*attach_t)(void *, void (*)(void *, unsigned *, unsigned *), void (*)(void *, const void *, int, int, int), void *);
typedef void (*detach_t)(void *, void *);

#define CHECK(c) do { if (!(c)) { printf("FAILED: %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

int main()
{
    int fails = 0;
    void *lib = dlopen("./libwinehglshim.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { printf("no shim: %s\n", dlerror()); return 1; }
    attach_t attach = (attach_t)dlsym(lib, "hvk_surface_attach");
    detach_t detach = (detach_t)dlsym(lib, "hvk_surface_detach");
    if (!attach || !detach) { printf("no hvk functions\n"); return 1; }

    MockSurface surface;
    void *hook = attach(&surface, size_cb, frame_cb, NULL);
    CHECK(hook != NULL);
    CHECK(surface.hook != NULL);

    uint32_t w = 0, h = 0;
    surface.hook->GetSize(w, h);
    CHECK(w == 320 && h == 200);            /* no frame yet: the window's size */

    BBitmap *first = new BBitmap(BRect(0, 0, 63, 31), B_RGB32);
    memset(first->Bits(), 0, first->BitsLength());
    *(unsigned *)first->Bits() = 0xff112233;
    BBitmap *ret = surface.hook->SetBitmap(first);      /* the layer: delete hook->SetBitmap(bitmap.Detach()) */
    CHECK(ret == NULL);
    CHECK(frames == 1 && last_w == 64 && last_h == 32 && last_stride == (int)first->BytesPerRow());
    CHECK(first_pixel == 0xff112233);
    surface.hook->GetSize(w, h);
    CHECK(w == 64 && h == 32);              /* the size of the frames, not the window's */

    *(unsigned *)first->Bits() = 0xff445566;
    ret = surface.hook->SetBitmap(first);               /* the same bitmap again for every next frame */
    CHECK(ret == NULL);
    CHECK(frames == 2 && first_pixel == 0xff445566);

    BBitmap *second = new BBitmap(BRect(0, 0, 127, 63), B_RGB32);    /* a new swapchain */
    memset(second->Bits(), 0, second->BitsLength());
    ret = surface.hook->SetBitmap(second);
    CHECK(ret == first);                    /* the old bitmap goes back to the layer to be deleted */
    delete ret;
    CHECK(frames == 3 && last_w == 128 && last_h == 64);

    detach(&surface, hook);
    CHECK(surface.hook == NULL);
    printf(fails ? "vkhooktest: %d FAILED\n" : "vkhooktest: all passed\n", fails);
    return fails != 0;
}
