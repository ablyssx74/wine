/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
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
#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "objidl.h"
#include "dxgi.h"
#include "initguid.h"
#include "dcomp.h"
#include "dxgi1_2.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

/* A minimal DirectComposition: the device, the targets and the visuals exist and remember their tree (the root of a target,
 * the children and the content of a visual), but nothing is composited yet. It is enough for Chromium (Edge, WebView2),
 * which treats a missing DirectComposition as a bug and crashes on purpose, to find a device. Everything else the interfaces
 * have (transforms, clips, effects, surfaces) is accepted and ignored, or reported as not implemented where an object would
 * have to be handed back. */

/* Methods that are accepted and ignored: one function per argument count (the object included), so the stdcall stack is
 * cleaned right on x86. */
static HRESULT WINAPI nop1(void *iface)
{
    TRACE("%p ignored.\n", iface);
    return S_OK;
}

static HRESULT WINAPI nop2(void *iface, UINT_PTR a)
{
    TRACE("%p ignored.\n", iface);
    return S_OK;
}

#define NOP1 ((void *)nop1)
#define NOP2 ((void *)nop2)

/* Methods that would hand an object back: not implemented (the out pointer is cleared). */
static HRESULT WINAPI nil2(void *iface, void **out)
{
    FIXME("%p: not implemented.\n", iface);
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI nil3(void *iface, UINT_PTR a, void **out)
{
    FIXME("%p: not implemented.\n", iface);
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI nil4(void *iface, UINT_PTR a, UINT_PTR b, void **out)
{
    FIXME("%p: not implemented.\n", iface);
    if (out) *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI nil6(void *iface, UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d, void **out)
{
    FIXME("%p: not implemented.\n", iface);
    if (out) *out = NULL;
    return E_NOTIMPL;
}

#define NIL2 ((void *)nil2)
#define NIL3 ((void *)nil3)
#define NIL4 ((void *)nil4)
#define NIL6 ((void *)nil6)

struct dcomp_visual
{
    IDCompositionVisual2 IDCompositionVisual2_iface;
    LONG refcount;
    IUnknown *content;
    struct dcomp_visual **children;
    unsigned int child_count, children_size;
    float offset_x, offset_y;
    float transform_dx, transform_dy;
};

struct dcomp_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    LONG refcount;
    HWND hwnd;
    struct dcomp_visual *root;
    struct dcomp_device *device;
    struct list entry;
};

struct dcomp_device
{
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDevice3 IDCompositionDevice3_iface;
    IDCompositionDesktopDevice IDCompositionDesktopDevice_iface;
    LONG refcount;
    struct list targets;
};

static ULONG device_release(struct dcomp_device *device);

static inline struct dcomp_visual *impl_from_IDCompositionVisual2(IDCompositionVisual2 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_visual, IDCompositionVisual2_iface);
}

static inline struct dcomp_target *impl_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_target, IDCompositionTarget_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDevice(IDCompositionDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDevice3(IDCompositionDevice3 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice3_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDesktopDevice(IDCompositionDesktopDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDesktopDevice_iface);
}

/* The visuals are only used from the one thread that composes, as in Windows; no locking. */

static struct dcomp_visual *visual_from_interface(IDCompositionVisual *visual)
{
    /* A visual of this device has IDCompositionVisual2's table, which starts with IDCompositionVisual's. */
    return visual ? impl_from_IDCompositionVisual2((IDCompositionVisual2 *)visual) : NULL;
}

static HRESULT WINAPI visual_QueryInterface(IDCompositionVisual2 *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualIID(iid, &IID_IDCompositionVisual2) || IsEqualIID(iid, &IID_IDCompositionVisual)
            || IsEqualIID(iid, &IID_IUnknown))
    {
        IDCompositionVisual2_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI visual_AddRef(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    return InterlockedIncrement(&visual->refcount);
}

static ULONG WINAPI visual_Release(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG refcount = InterlockedDecrement(&visual->refcount);
    unsigned int i;

    if (!refcount)
    {
        for (i = 0; i < visual->child_count; ++i)
            IDCompositionVisual2_Release(&visual->children[i]->IDCompositionVisual2_iface);
        free(visual->children);
        if (visual->content) IUnknown_Release(visual->content);
        free(visual);
    }
    return refcount;
}

static HRESULT WINAPI visual_SetOffsetX(IDCompositionVisual2 *iface, float x)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, x %f.\n", iface, x);
    visual->offset_x = x;
    return S_OK;
}

static HRESULT WINAPI visual_SetOffsetY(IDCompositionVisual2 *iface, float y)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    TRACE("iface %p, y %f.\n", iface, y);
    visual->offset_y = y;
    return S_OK;
}

static HRESULT WINAPI visual_SetTransform(IDCompositionVisual2 *iface, const D2D_MATRIX_3X2_F *matrix)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, matrix %p.\n", iface, matrix);

    /* Only the translation is applied. */
    visual->transform_dx = matrix ? matrix->dx : 0.0f;
    visual->transform_dy = matrix ? matrix->dy : 0.0f;
    return S_OK;
}

static HRESULT WINAPI visual_SetContent(IDCompositionVisual2 *iface, IUnknown *content)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, content %p.\n", iface, content);

    if (content) IUnknown_AddRef(content);
    if (visual->content) IUnknown_Release(visual->content);
    visual->content = content;
    return S_OK;
}

static void visual_remove_child(struct dcomp_visual *visual, struct dcomp_visual *child)
{
    unsigned int i;

    for (i = 0; i < visual->child_count; ++i)
    {
        if (visual->children[i] != child) continue;
        memmove(&visual->children[i], &visual->children[i + 1], (visual->child_count - i - 1) * sizeof(*visual->children));
        --visual->child_count;
        IDCompositionVisual2_Release(&child->IDCompositionVisual2_iface);
        return;
    }
}

static HRESULT WINAPI visual_AddVisual(IDCompositionVisual2 *iface, IDCompositionVisual *child_iface, BOOL above,
        IDCompositionVisual *reference_iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);
    struct dcomp_visual *child = visual_from_interface(child_iface), *reference = visual_from_interface(reference_iface);
    unsigned int pos;

    TRACE("iface %p, child %p, above %d, reference %p.\n", iface, child_iface, above, reference_iface);

    if (!child) return E_INVALIDARG;

    visual_remove_child(visual, child);

    if (visual->child_count == visual->children_size)
    {
        unsigned int size = visual->children_size ? visual->children_size * 2 : 4;
        struct dcomp_visual **children = realloc(visual->children, size * sizeof(*children));
        if (!children) return E_OUTOFMEMORY;
        visual->children = children;
        visual->children_size = size;
    }

    /* Above the reference (or on top of everything), or below it (or at the bottom). */
    pos = above ? visual->child_count : 0;
    if (reference)
    {
        for (pos = 0; pos < visual->child_count && visual->children[pos] != reference; ++pos) {}
        if (pos == visual->child_count) pos = above ? pos : 0;
        else if (above) ++pos;
    }
    memmove(&visual->children[pos + 1], &visual->children[pos], (visual->child_count - pos) * sizeof(*visual->children));
    visual->children[pos] = child;
    ++visual->child_count;
    IDCompositionVisual2_AddRef(&child->IDCompositionVisual2_iface);
    return S_OK;
}

static HRESULT WINAPI visual_RemoveVisual(IDCompositionVisual2 *iface, IDCompositionVisual *child)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, child %p.\n", iface, child);

    if (child) visual_remove_child(visual, visual_from_interface(child));
    return S_OK;
}

static HRESULT WINAPI visual_RemoveAllVisuals(IDCompositionVisual2 *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p.\n", iface);

    while (visual->child_count)
        visual_remove_child(visual, visual->children[visual->child_count - 1]);
    return S_OK;
}

static const IDCompositionVisual2Vtbl visual_vtbl =
{
    visual_QueryInterface,
    visual_AddRef,
    visual_Release,
    NOP2,               /* SetOffsetXAnimation */
    (void *)visual_SetOffsetX,
    NOP2,               /* SetOffsetYAnimation */
    (void *)visual_SetOffsetY,
    NOP2,               /* SetTransformObject */
    (void *)visual_SetTransform,
    NOP2,               /* SetTransformParent */
    NOP2,               /* SetEffect */
    NOP2,               /* SetBitmapInterpolationMode */
    NOP2,               /* SetBorderMode */
    NOP2,               /* SetClipObject */
    NOP2,               /* SetClip */
    visual_SetContent,
    visual_AddVisual,
    visual_RemoveVisual,
    visual_RemoveAllVisuals,
    NOP2,               /* SetCompositeMode */
    NOP2,               /* SetOpacityMode */
    NOP2,               /* SetBackFaceVisibility */
};

static HRESULT WINAPI target_QueryInterface(IDCompositionTarget *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualIID(iid, &IID_IDCompositionTarget) || IsEqualIID(iid, &IID_IUnknown))
    {
        IDCompositionTarget_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI target_AddRef(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    return InterlockedIncrement(&target->refcount);
}

static ULONG WINAPI target_Release(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    ULONG refcount = InterlockedDecrement(&target->refcount);

    if (!refcount)
    {
        list_remove(&target->entry);
        if (target->root) IDCompositionVisual2_Release(&target->root->IDCompositionVisual2_iface);
        device_release(target->device);
        free(target);
    }
    return refcount;
}

static HRESULT WINAPI target_SetRoot(IDCompositionTarget *iface, IDCompositionVisual *root)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    struct dcomp_visual *visual = visual_from_interface(root);

    TRACE("iface %p, root %p.\n", iface, root);

    if (visual) IDCompositionVisual2_AddRef(&visual->IDCompositionVisual2_iface);
    if (target->root) IDCompositionVisual2_Release(&target->root->IDCompositionVisual2_iface);
    target->root = visual;
    return S_OK;
}

static const IDCompositionTargetVtbl target_vtbl =
{
    target_QueryInterface,
    target_AddRef,
    target_Release,
    target_SetRoot,
};

static HRESULT device_create_visual(void **out)
{
    struct dcomp_visual *visual;

    if (!out) return E_INVALIDARG;
    if (!(visual = calloc(1, sizeof(*visual)))) return E_OUTOFMEMORY;
    visual->IDCompositionVisual2_iface.lpVtbl = &visual_vtbl;
    visual->refcount = 1;
    *out = &visual->IDCompositionVisual2_iface;
    return S_OK;
}

static HRESULT device_create_target(struct dcomp_device *device, HWND hwnd, IDCompositionTarget **out)
{
    struct dcomp_target *target;

    if (!out) return E_INVALIDARG;
    if (!IsWindow(hwnd)) return E_INVALIDARG;
    if (!(target = calloc(1, sizeof(*target)))) return E_OUTOFMEMORY;
    target->IDCompositionTarget_iface.lpVtbl = &target_vtbl;
    target->refcount = 1;
    target->hwnd = hwnd;
    target->device = device;
    InterlockedIncrement(&device->refcount);
    list_add_tail(&device->targets, &target->entry);
    *out = &target->IDCompositionTarget_iface;
    return S_OK;
}

static HRESULT device_query_interface(struct dcomp_device *device, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IDCompositionDevice))
        *out = &device->IDCompositionDevice_iface;
    else if (IsEqualIID(iid, &IID_IDCompositionDevice2) || IsEqualIID(iid, &IID_IDCompositionDevice3))
        *out = &device->IDCompositionDevice3_iface;
    else if (IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        *out = &device->IDCompositionDesktopDevice_iface;
    else if (IsEqualIID(iid, &IID_IUnknown))
        *out = &device->IDCompositionDevice_iface;
    else
    {
        WARN("Unsupported interface %s.\n", debugstr_guid(iid));
        *out = NULL;
        return E_NOINTERFACE;
    }

    InterlockedIncrement(&device->refcount);
    return S_OK;
}

static ULONG device_release(struct dcomp_device *device)
{
    ULONG refcount = InterlockedDecrement(&device->refcount);
    if (!refcount) free(device);
    return refcount;
}

/* IDCompositionDevice */

/* Commit: the swapchains that visuals show are windows (see dxgi's CreateSwapChainForComposition); put each into the window of
 * the target, at the place of its visual, and above the windows of the visuals below it. */
static void visual_place(struct dcomp_visual *visual, HWND parent, float x, float y)
{
    unsigned int i;

    x += visual->offset_x + visual->transform_dx;
    y += visual->offset_y + visual->transform_dy;

    if (visual->content)
    {
        IDXGISwapChain1 *swapchain;

        if (SUCCEEDED(IUnknown_QueryInterface(visual->content, &IID_IDXGISwapChain1, (void **)&swapchain)))
        {
            DXGI_SWAP_CHAIN_DESC1 desc;
            HWND window;

            if (SUCCEEDED(IDXGISwapChain1_GetHwnd(swapchain, &window)) && IsWindow(window)
                    && SUCCEEDED(IDXGISwapChain1_GetDesc1(swapchain, &desc)))
            {
                WCHAR name[32];

                if (GetClassNameW(window, name, ARRAY_SIZE(name)) && !wcscmp(name, L"WineDXGIComposition"))
                {
                    /* The swapchain draws into the window of the target by itself, when it presents (dxgi); it is told where. */
                    SetPropW(window, L"WineDCompTarget", parent);
                    /* (win32u puts the window that the visuals are shown in over the others of the same top-level window) */
                    SetPropW(parent, L"WineDCompContent", (HANDLE)1);
                    SetPropW(window, L"WineDCompX", (HANDLE)(INT_PTR)(int)x);
                    SetPropW(window, L"WineDCompY", (HANDLE)(INT_PTR)(int)y);
                    TRACE("visual %p: window %p into %p at %.0f,%.0f, %ux%u.\n", visual, window, parent, x, y, desc.Width, desc.Height);
                }
            }
            IDXGISwapChain1_Release(swapchain);
        }
    }

    for (i = 0; i < visual->child_count; ++i)
        visual_place(visual->children[i], parent, x, y);
}

static HRESULT device_commit(struct dcomp_device *device)
{
    struct dcomp_target *target;

    TRACE("device %p.\n", device);

    LIST_FOR_EACH_ENTRY(target, &device->targets, struct dcomp_target, entry)
    {
        if (target->root && IsWindow(target->hwnd))
            visual_place(target->root, target->hwnd, 0.0f, 0.0f);
    }
    return S_OK;
}

static HRESULT WINAPI device1_Commit(IDCompositionDevice *iface)
{
    return device_commit(impl_from_IDCompositionDevice(iface));
}

static HRESULT WINAPI device3_Commit(IDCompositionDevice3 *iface)
{
    return device_commit(impl_from_IDCompositionDevice3(iface));
}

static HRESULT WINAPI desktop_Commit(IDCompositionDesktopDevice *iface)
{
    return device_commit(impl_from_IDCompositionDesktopDevice(iface));
}

static HRESULT WINAPI device1_QueryInterface(IDCompositionDevice *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);
    return device_query_interface(impl_from_IDCompositionDevice(iface), iid, out);
}

static ULONG WINAPI device1_AddRef(IDCompositionDevice *iface)
{
    return InterlockedIncrement(&impl_from_IDCompositionDevice(iface)->refcount);
}

static ULONG WINAPI device1_Release(IDCompositionDevice *iface)
{
    return device_release(impl_from_IDCompositionDevice(iface));
}

static HRESULT WINAPI device1_CreateTargetForHwnd(IDCompositionDevice *iface, HWND hwnd, BOOL topmost,
        IDCompositionTarget **target)
{
    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);
    return device_create_target(impl_from_IDCompositionDevice(iface), hwnd, target);
}

static HRESULT WINAPI device1_CreateVisual(IDCompositionDevice *iface, IDCompositionVisual **visual)
{
    TRACE("iface %p, visual %p.\n", iface, visual);
    return device_create_visual((void **)visual);
}

static HRESULT WINAPI check_device_state(void *iface, BOOL *valid)
{
    TRACE("iface %p, valid %p.\n", iface, valid);
    if (valid) *valid = TRUE;
    return S_OK;
}

static const IDCompositionDeviceVtbl device1_vtbl =
{
    device1_QueryInterface,
    device1_AddRef,
    device1_Release,
    device1_Commit,
    NOP1,               /* WaitForCommitCompletion */
    NIL2,               /* GetFrameStatistics */
    device1_CreateTargetForHwnd,
    device1_CreateVisual,
    NIL6,               /* CreateSurface */
    NIL6,               /* CreateVirtualSurface */
    NIL3,               /* CreateSurfaceFromHandle */
    NIL3,               /* CreateSurfaceFromHwnd */
    NIL2,               /* CreateTranslateTransform */
    NIL2,               /* CreateScaleTransform */
    NIL2,               /* CreateRotateTransform */
    NIL2,               /* CreateSkewTransform */
    NIL2,               /* CreateMatrixTransform */
    NIL4,               /* CreateTransformGroup */
    NIL2,               /* CreateTranslateTransform3D */
    NIL2,               /* CreateScaleTransform3D */
    NIL2,               /* CreateRotateTransform3D */
    NIL2,               /* CreateMatrixTransform3D */
    NIL4,               /* CreateTransform3DGroup */
    NIL2,               /* CreateEffectGroup */
    NIL2,               /* CreateRectangleClip */
    NIL2,               /* CreateAnimation */
    (void *)check_device_state,
};

/* IDCompositionDevice3 (and 2, which is its beginning) */

static HRESULT WINAPI device3_QueryInterface(IDCompositionDevice3 *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);
    return device_query_interface(impl_from_IDCompositionDevice3(iface), iid, out);
}

static ULONG WINAPI device3_AddRef(IDCompositionDevice3 *iface)
{
    return InterlockedIncrement(&impl_from_IDCompositionDevice3(iface)->refcount);
}

static ULONG WINAPI device3_Release(IDCompositionDevice3 *iface)
{
    return device_release(impl_from_IDCompositionDevice3(iface));
}

static HRESULT WINAPI device3_CreateVisual(IDCompositionDevice3 *iface, IDCompositionVisual2 **visual)
{
    TRACE("iface %p, visual %p.\n", iface, visual);
    return device_create_visual((void **)visual);
}

static const IDCompositionDevice3Vtbl device3_vtbl =
{
    device3_QueryInterface,
    device3_AddRef,
    device3_Release,
    device3_Commit,
    NOP1,               /* WaitForCommitCompletion */
    NIL2,               /* GetFrameStatistics */
    device3_CreateVisual,
    NIL3,               /* CreateSurfaceFactory */
    NIL6,               /* CreateSurface */
    NIL6,               /* CreateVirtualSurface */
    NIL2,               /* CreateTranslateTransform */
    NIL2,               /* CreateScaleTransform */
    NIL2,               /* CreateRotateTransform */
    NIL2,               /* CreateSkewTransform */
    NIL2,               /* CreateMatrixTransform */
    NIL4,               /* CreateTransformGroup */
    NIL2,               /* CreateTranslateTransform3D */
    NIL2,               /* CreateScaleTransform3D */
    NIL2,               /* CreateRotateTransform3D */
    NIL2,               /* CreateMatrixTransform3D */
    NIL4,               /* CreateTransform3DGroup */
    NIL2,               /* CreateEffectGroup */
    NIL2,               /* CreateRectangleClip */
    NIL2,               /* CreateAnimation */
    NIL2,               /* CreateGaussianBlurEffect */
    NIL2,               /* CreateBrightnessEffect */
    NIL2,               /* CreateColorMatrixEffect */
    NIL2,               /* CreateShadowEffect */
    NIL2,               /* CreateHueRotationEffect */
    NIL2,               /* CreateSaturationEffect */
    NIL2,               /* CreateTurbulenceEffect */
    NIL2,               /* CreateLinearTransferEffect */
    NIL2,               /* CreateTableTransferEffect */
    NIL2,               /* CreateCompositeEffect */
    NIL2,               /* CreateBlendEffect */
    NIL2,               /* CreateArithmeticCompositeEffect */
    NIL2,               /* CreateAffineTransform2DEffect */
};

/* IDCompositionDesktopDevice */

static HRESULT WINAPI desktop_QueryInterface(IDCompositionDesktopDevice *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);
    return device_query_interface(impl_from_IDCompositionDesktopDevice(iface), iid, out);
}

static ULONG WINAPI desktop_AddRef(IDCompositionDesktopDevice *iface)
{
    return InterlockedIncrement(&impl_from_IDCompositionDesktopDevice(iface)->refcount);
}

static ULONG WINAPI desktop_Release(IDCompositionDesktopDevice *iface)
{
    return device_release(impl_from_IDCompositionDesktopDevice(iface));
}

static HRESULT WINAPI desktop_CreateVisual(IDCompositionDesktopDevice *iface, IDCompositionVisual2 **visual)
{
    TRACE("iface %p, visual %p.\n", iface, visual);
    return device_create_visual((void **)visual);
}

static HRESULT WINAPI desktop_CreateTargetForHwnd(IDCompositionDesktopDevice *iface, HWND hwnd, BOOL topmost,
        IDCompositionTarget **target)
{
    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);
    return device_create_target(impl_from_IDCompositionDesktopDevice(iface), hwnd, target);
}

static const IDCompositionDesktopDeviceVtbl desktop_vtbl =
{
    desktop_QueryInterface,
    desktop_AddRef,
    desktop_Release,
    desktop_Commit,
    NOP1,               /* WaitForCommitCompletion */
    NIL2,               /* GetFrameStatistics */
    desktop_CreateVisual,
    NIL3,               /* CreateSurfaceFactory */
    NIL6,               /* CreateSurface */
    NIL6,               /* CreateVirtualSurface */
    NIL2,               /* CreateTranslateTransform */
    NIL2,               /* CreateScaleTransform */
    NIL2,               /* CreateRotateTransform */
    NIL2,               /* CreateSkewTransform */
    NIL2,               /* CreateMatrixTransform */
    NIL4,               /* CreateTransformGroup */
    NIL2,               /* CreateTranslateTransform3D */
    NIL2,               /* CreateScaleTransform3D */
    NIL2,               /* CreateRotateTransform3D */
    NIL2,               /* CreateMatrixTransform3D */
    NIL4,               /* CreateTransform3DGroup */
    NIL2,               /* CreateEffectGroup */
    NIL2,               /* CreateRectangleClip */
    NIL2,               /* CreateAnimation */
    desktop_CreateTargetForHwnd,
    NIL3,               /* CreateSurfaceFromHandle */
    NIL3,               /* CreateSurfaceFromHwnd */
};

static HRESULT create_device(REFIID iid, void **out)
{
    struct dcomp_device *device;
    HRESULT hr;

    if (!out) return E_INVALIDARG;
    *out = NULL;

    if (!(device = calloc(1, sizeof(*device)))) return E_OUTOFMEMORY;
    device->IDCompositionDevice_iface.lpVtbl = &device1_vtbl;
    device->IDCompositionDevice3_iface.lpVtbl = &device3_vtbl;
    device->IDCompositionDesktopDevice_iface.lpVtbl = &desktop_vtbl;
    device->refcount = 1;
    list_init(&device->targets);

    hr = device_query_interface(device, iid, out);
    device_release(device);
    return hr;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", dxgi_device, debugstr_guid(iid), device);

    return create_device(iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return create_device(iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return create_device(iid, device);
}
