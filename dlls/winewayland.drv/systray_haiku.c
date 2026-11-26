/*
 * Haiku tray icons: each tray icon of a Windows program becomes an item in the Deskbar's tray
 *
 * Wayland has no system tray. The driver hands the icons to haiku/haikutray.cpp (libwinehaikutray.so), which puts an item
 * for each into the Deskbar (haiku/trayitem.cpp), and tells the program about the clicks that come back through a pipe.
 * WINE_HAIKU_TRAY=0 turns this off (the icons are then Wine's own business, which shows none).
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

#ifdef __HAIKU__

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "waylanddrv.h"
#include "shellapi.h"
#include "haiku/haikutray.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(systray);

struct tray_icon
{
    struct wl_list link;
    uint32_t serial;
    HWND owner;
    UINT id;
    UINT callback;
    UINT version;
    BOOL hidden;
    uint32_t *pixels;
    int width, height;
    char tip[512];
};

static struct
{
    int (*init)(const char *, int);
    int (*set)(uint32_t, const uint32_t *, int, int, const char *);
    void (*remove)(uint32_t);
    uint32_t *(*decode)(const void *, size_t, int *, int *);
} bridge;

/* The large icon in the program's own file (up to 256 x 256), straight pixels; read once. */
static struct
{
    BOOL looked;
    uint32_t *pixels;
    int width, height;
} program;

static pthread_mutex_t tray_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct wl_list tray_icons = { &tray_icons, &tray_icons };
static uint32_t tray_serial;
static int tray_pipe[2] = { -1, -1 };

/* The end of the pipe that the event loop waits on; -1 when there is none. */
int wayland_haiku_tray_fd(void)
{
    if (tray_pipe[0] == -1 && pipe(tray_pipe) == -1) tray_pipe[0] = tray_pipe[1] = -1;
    return tray_pipe[0];
}

/* The path of a file in the folder of this driver. */
static BOOL driver_folder_path(const char *name, char *path, size_t size)
{
    const char *slash;
    Dl_info info;

    if (!dladdr((void *)driver_folder_path, &info) || !info.dli_fname || !(slash = strrchr(info.dli_fname, '/')))
        return FALSE;
    snprintf(path, size, "%.*s/%s", (int)(slash - info.dli_fname), info.dli_fname, name);
    return TRUE;
}

/* Loads the bridge library from the folder of this driver, once. */
static BOOL load_library(void)
{
    static int state;   /* 0 not tried, 1 loaded, -1 not there */
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    char path[1024];
    void *handle;

    pthread_mutex_lock(&mutex);
    if (!state)
    {
        state = -1;
        if (driver_folder_path("libwinehaikutray.so", path, sizeof(path)))
        {
            if (!(handle = dlopen(path, RTLD_NOW))) WARN("Failed to load %s: %s\n", path, dlerror());
            else
            {
                bridge.init = dlsym(handle, "wine_tray_init");
                bridge.set = dlsym(handle, "wine_tray_set");
                bridge.remove = dlsym(handle, "wine_tray_remove");
                bridge.decode = dlsym(handle, "wine_tray_decode");
                if (bridge.init && bridge.set && bridge.remove && bridge.decode) state = 1;
            }
        }
    }
    pthread_mutex_unlock(&mutex);
    return state > 0;
}

/* Starts the bridge to the Deskbar, once. */
static BOOL load_bridge(void)
{
    static int state;   /* 0 not tried, 1 works, -1 does not */
    const char *env = getenv("WINE_HAIKU_TRAY");
    char item[1024];

    if (state) return state > 0;
    state = -1;
    if (env && !strcmp(env, "0")) return FALSE;
    if (wayland_haiku_tray_fd() == -1 || !load_library()) return FALSE;
    if (!driver_folder_path("winetrayitem", item, sizeof(item)) || !bridge.init(item, tray_pipe[1]))
    {
        WARN("The tray bridge could not start\n");
        return FALSE;
    }
    state = 1;
    return TRUE;
}

/* Reading the program's file: the icon is among its resources (a PE file). */
static BOOL read_at(HANDLE file, DWORD offset, void *data, DWORD size)
{
    LARGE_INTEGER at;
    IO_STATUS_BLOCK io;

    at.QuadPart = offset;
    return !NtReadFile(file, 0, NULL, NULL, &io, data, size, &at, NULL) && io.Information == size;
}

struct pe_file
{
    HANDLE file;
    IMAGE_SECTION_HEADER sections[96];
    int count;
    DWORD resources;     /* the file offset of the resource directory */
};

static DWORD pe_offset(const struct pe_file *pe, DWORD rva)
{
    int i;

    for (i = 0; i < pe->count; i++)
        if (rva >= pe->sections[i].VirtualAddress &&
            rva - pe->sections[i].VirtualAddress < pe->sections[i].SizeOfRawData)
            return rva - pe->sections[i].VirtualAddress + pe->sections[i].PointerToRawData;
    return 0;
}

/* The entry `id` of a resource directory (the first one when id is 0): its offset field, 0 when there is none. */
static DWORD resource_entry(const struct pe_file *pe, DWORD directory, DWORD id)
{
    IMAGE_RESOURCE_DIRECTORY head;
    IMAGE_RESOURCE_DIRECTORY_ENTRY entry;
    DWORD offset = pe->resources + (directory & 0x7fffffff);
    int i, count;

    if (!read_at(pe->file, offset, &head, sizeof(head))) return 0;
    count = head.NumberOfNamedEntries + head.NumberOfIdEntries;
    for (i = 0; i < count && i < 4096; i++)
    {
        if (!read_at(pe->file, offset + sizeof(head) + i * sizeof(entry), &entry, sizeof(entry))) return 0;
        if (!id || (!entry.NameIsString && entry.Id == id)) return entry.OffsetToData;
    }
    return 0;
}

/* The data of resource `name` (0: the first) of a type, to be freed. */
static void *resource_data(const struct pe_file *pe, DWORD type, DWORD name, DWORD *size)
{
    IMAGE_RESOURCE_DATA_ENTRY entry;
    DWORD offset, found;
    void *data;

    if (!(found = resource_entry(pe, 0, type)) || !(found = resource_entry(pe, found, name)) ||
        !(found = resource_entry(pe, found, 0)))
        return NULL;
    if (!read_at(pe->file, pe->resources + (found & 0x7fffffff), &entry, sizeof(entry))) return NULL;
    if (!(offset = pe_offset(pe, entry.OffsetToData)) || !entry.Size || entry.Size > 4 * 1024 * 1024) return NULL;
    if (!(data = malloc(entry.Size))) return NULL;
    if (!read_at(pe->file, offset, data, entry.Size))
    {
        free(data);
        return NULL;
    }
    *size = entry.Size;
    return data;
}

#include "pshpack2.h"
struct group_icon_entry
{
    BYTE width, height, colors, reserved;
    WORD planes, bits;
    DWORD size;
    WORD id;
};
#include "poppack.h"

/* One picture of an icon: a PNG, or a 32-bit bitmap without its file header. */
static uint32_t *decode_icon(const void *data, DWORD size, int *width, int *height)
{
    const BITMAPINFOHEADER *header = data;
    uint32_t *pixels;
    int w, h, y;

    if (size > 8 && !memcmp(data, "\x89PNG", 4))
        return load_library() ? bridge.decode(data, size, width, height) : NULL;
    if (size < sizeof(*header) || header->biBitCount != 32 || header->biCompression != BI_RGB) return NULL;
    w = header->biWidth;
    h = header->biHeight / 2;     /* the picture and its mask */
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || header->biSize + (DWORD)w * h * 4 > size) return NULL;
    if (!(pixels = malloc(w * h * 4))) return NULL;
    for (y = 0; y < h; y++)       /* the rows run from the bottom */
        memcpy(pixels + y * w, (const char *)data + header->biSize + (h - 1 - y) * w * 4, w * 4);
    *width = w;
    *height = h;
    return pixels;
}

static void read_program_icon(HANDLE file)
{
    struct pe_file pe = { .file = file };
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    const IMAGE_DATA_DIRECTORY *directory;
    DWORD size, group_size, best_size = 0;
    struct group_icon_entry *entries;
    WORD *group, i, count;
    int best = -1, best_width = 0;
    void *data;

    if (!read_at(file, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return;
    if (!read_at(file, dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return;
    if (nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        directory = &nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    else
        directory = &((IMAGE_NT_HEADERS32 *)&nt)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    pe.count = min(nt.FileHeader.NumberOfSections, ARRAY_SIZE(pe.sections));
    if (!read_at(file, dos.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader,
                 pe.sections, pe.count * sizeof(pe.sections[0])))
        return;
    if (!directory->VirtualAddress || !(pe.resources = pe_offset(&pe, directory->VirtualAddress))) return;

    /* the first icon of the file is the program's: of its pictures, the largest with the most colors */
    if (!(group = resource_data(&pe, 14 /* RT_GROUP_ICON */, 0, &group_size))) return;
    count = group_size >= 6 ? group[2] : 0;
    if (6 + count * sizeof(*entries) > group_size) count = 0;
    entries = (struct group_icon_entry *)(group + 3);
    for (i = 0; i < count; i++)
    {
        int width = entries[i].width ? entries[i].width : 256;
        if (entries[i].bits < 32) continue;
        if (width > best_width || (width == best_width && entries[i].size > best_size))
        {
            best = i;
            best_width = width;
            best_size = entries[i].size;
        }
    }
    if (best >= 0 && (data = resource_data(&pe, 3 /* RT_ICON */, entries[best].id, &size)))
    {
        program.pixels = decode_icon(data, size, &program.width, &program.height);
        free(data);
    }
    free(group);
}

/* The large icon of the program that runs in this process; NULL when its file has none that can be used. */
static const uint32_t *program_icon(int *width, int *height)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static const WCHAR prefix[] = {'\\','?','?','\\'};
    const UNICODE_STRING *image;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    WCHAR *path;
    HANDLE file;

    pthread_mutex_lock(&mutex);
    if (!program.looked)
    {
        program.looked = TRUE;
        image = &NtCurrentTeb()->Peb->ProcessParameters->ImagePathName;
        if (image->Length && (path = malloc(sizeof(prefix) + image->Length)))
        {
            memcpy(path, prefix, sizeof(prefix));
            memcpy(path + ARRAY_SIZE(prefix), image->Buffer, image->Length);
            name.Buffer = path;
            name.Length = name.MaximumLength = sizeof(prefix) + image->Length;
            InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, 0, NULL);
            if (!NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &io, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE))
            {
                read_program_icon(file);
                NtClose(file);
            }
            free(path);
        }
        TRACE("program icon %d x %d\n", program.width, program.height);
    }
    pthread_mutex_unlock(&mutex);
    *width = program.width;
    *height = program.height;
    return program.pixels;
}

/* Reduces straight pixels to a smaller square by averaging, weighting the colors by their alpha. */
static uint32_t *reduce_pixels(const uint32_t *from, int width, int height, int to_width, int to_height)
{
    uint32_t *to = malloc(to_width * to_height * 4);
    int x, y, sx, sy;

    if (!to) return NULL;
    for (y = 0; y < to_height; y++)
        for (x = 0; x < to_width; x++)
        {
            unsigned int a = 0, r = 0, g = 0, b = 0, n = 0;
            for (sy = y * height / to_height; sy < max((y + 1) * height / to_height, y * height / to_height + 1); sy++)
                for (sx = x * width / to_width; sx < max((x + 1) * width / to_width, x * width / to_width + 1); sx++, n++)
                {
                    uint32_t p = from[sy * width + sx], pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 0xff) * pa;
                    g += ((p >> 8) & 0xff) * pa;
                    b += (p & 0xff) * pa;
                }
            to[y * to_width + x] = a ? ((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a) : 0;
        }
    return to;
}

/* Whether a small picture is the program's icon: the two, over gray, differ little as a whole. */
static BOOL is_program_icon(const uint32_t *pixels, int width, int height)
{
    const uint32_t *large;
    uint32_t *small;
    int large_width, large_height, i, c;
    unsigned long difference = 0;

    if (!(large = program_icon(&large_width, &large_height)) || large_width <= width) return FALSE;
    if (!(small = reduce_pixels(large, large_width, large_height, width, height))) return FALSE;
    for (i = 0; i < width * height; i++)
        for (c = 0; c < 24; c += 8)
        {
            int a = pixels[i] >> 24, b = small[i] >> 24;
            int one = (((pixels[i] >> c) & 0xff) * a + 128 * (255 - a)) / 255;
            int other = (((small[i] >> c) & 0xff) * b + 128 * (255 - b)) / 255;
            difference += abs(one - other);
        }
    free(small);
    TRACE("difference to the program's icon %lu\n", difference / (width * height * 3));
    return difference <= (unsigned long)width * height * 3 * 10;
}

/* Hands an item's picture to the Deskbar: the program's large icon when the small one the program set is that icon. */
static BOOL show_icon_item(struct tray_icon *icon)
{
    const uint32_t *large;
    uint32_t *better = NULL;
    int width, height, side = 128;
    BOOL ret;

    if (icon->pixels && icon->width <= 64 && is_program_icon(icon->pixels, icon->width, icon->height) &&
        (large = program_icon(&width, &height)))
    {
        if (side > width) side = width;
        better = reduce_pixels(large, width, height, side, side);
    }
    ret = bridge.set(icon->serial, better ? better : icon->pixels, better ? side : icon->width,
                     better ? side : icon->height, icon->tip) != 0;
    free(better);
    return ret;
}

static struct tray_icon *find_icon(HWND owner, UINT id)
{
    struct tray_icon *icon;

    wl_list_for_each(icon, &tray_icons, link)
        if (icon->owner == owner && icon->id == id) return icon;
    return NULL;
}

static void delete_icon(struct tray_icon *icon)
{
    bridge.remove(icon->serial);
    wl_list_remove(&icon->link);
    free(icon->pixels);
    free(icon);
}

/* The picture of an icon's bitmaps as straight (not premultiplied) pixels, to be freed; NULL for an icon without color. */
static uint32_t *icon_pixels(const ICONINFO *info, int *width, int *height)
{
    struct wayland_shm_buffer *buffer;
    uint32_t *pixels = NULL;
    HDC hdc;
    int i;

    if (!info->hbmColor) return NULL;
    hdc = NtGdiCreateCompatibleDC(0);
    buffer = wayland_shm_buffer_from_color_bitmaps(hdc, info->hbmColor, info->hbmMask, TRUE);
    NtGdiDeleteObjectApp(hdc);
    if (!buffer) return NULL;
    if ((pixels = malloc(buffer->width * buffer->height * 4)))
    {
        const uint32_t *from = buffer->map_data;
        for (i = 0; i < buffer->width * buffer->height; i++)
        {
            uint32_t p = from[i], a = p >> 24;
            if (a && a != 255)
            {
                uint32_t r = ((p >> 16) & 0xff) * 255 / a, g = ((p >> 8) & 0xff) * 255 / a, b = (p & 0xff) * 255 / a;
                p = (a << 24) | (min(r, 255) << 16) | (min(g, 255) << 8) | min(b, 255);
            }
            pixels[i] = p;
        }
        *width = buffer->width;
        *height = buffer->height;
    }
    wayland_shm_buffer_unref(buffer);
    return pixels;
}

static void set_icon_image(struct tray_icon *icon, HICON handle)
{
    ICONINFO info;
    uint32_t *pixels;
    int width, height;

    if (!handle || !NtUserGetIconInfo(handle, &info, NULL, NULL, NULL, 0)) return;
    if ((pixels = icon_pixels(&info, &width, &height)))
    {
        free(icon->pixels);
        icon->pixels = pixels;
        icon->width = width;
        icon->height = height;
        TRACE("tray icon picture %d x %d\n", width, height);
    }
    if (info.hbmColor) NtGdiDeleteObjectApp(info.hbmColor);
    if (info.hbmMask) NtGdiDeleteObjectApp(info.hbmMask);
}

/* Saves the icon of a program's window as ~/config/cache/wine-icons/<team>.wicn ("WICN", width, height, straight pixels),
 * for a dock to show instead of Wine's icon: every Windows program is a team of Wine's one program file. */
void wayland_haiku_save_icon(const ICONINFO *info)
{
    const char *home = getenv("HOME");
    char folder[1024], path[1088], temporary[1100];
    const uint32_t *large;
    uint32_t *own = NULL;
    const uint32_t *pixels;
    int width, height, large_width, large_height, size[2];
    FILE *file;

    if (!home) return;
    /* the large icon in the program's file, else the window's icon */
    if ((large = program_icon(&large_width, &large_height)) && large_width >= 48)
    {
        pixels = large;
        width = large_width;
        height = large_height;
    }
    else if (info && (own = icon_pixels(info, &width, &height))) pixels = own;
    else return;
    snprintf(folder, sizeof(folder), "%s/config/cache/wine-icons", home);
    mkdir(folder, 0755);
    snprintf(path, sizeof(path), "%s/%d.wicn", folder, (int)getpid());
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    if ((file = fopen(temporary, "wb")))
    {
        size[0] = width;
        size[1] = height;
        if (fwrite("WICN", 1, 4, file) == 4 && fwrite(size, 4, 2, file) == 2 &&
            fwrite(pixels, 4, width * height, file) == (size_t)width * height && !fclose(file))
            rename(temporary, path);
        else
            unlink(temporary);
    }
    free(own);
}

static void set_icon_tip(struct tray_icon *icon, const WCHAR *tip, unsigned int max)
{
    DWORD length = 0, size = 0;

    while (length < max && tip[length]) length++;
    RtlUnicodeToUTF8N(icon->tip, sizeof(icon->tip) - 1, &size, tip, length * sizeof(WCHAR));
    icon->tip[min(size, sizeof(icon->tip) - 1)] = 0;
}

/* Applies what a request says to the icon, and shows the result. */
static BOOL update_icon(struct tray_icon *icon, NOTIFYICONDATAW *data)
{
    if (data->uFlags & NIF_STATE)
        if (data->dwStateMask & NIS_HIDDEN) icon->hidden = !!(data->dwState & NIS_HIDDEN);
    if (data->uFlags & NIF_ICON) set_icon_image(icon, data->hIcon);
    if (data->uFlags & NIF_MESSAGE) icon->callback = data->uCallbackMessage;
    if (data->uFlags & NIF_TIP) set_icon_tip(icon, data->szTip, ARRAY_SIZE(data->szTip));

    if (icon->hidden)
    {
        bridge.remove(icon->serial);
        return TRUE;
    }
    if (!(data->uFlags & (NIF_STATE | NIF_ICON | NIF_TIP))) return TRUE;
    return show_icon_item(icon);
}

/***********************************************************************
 *           WAYLAND_NotifyIcon
 */
LRESULT WAYLAND_NotifyIcon(HWND hwnd, UINT msg, NOTIFYICONDATAW *data)
{
    struct tray_icon *icon;
    LRESULT ret = FALSE;

    TRACE("msg %u owner %p id %u flags %#x\n", msg, data->hWnd, data->uID, data->uFlags);

    pthread_mutex_lock(&tray_mutex);
    if (!load_bridge())
    {
        pthread_mutex_unlock(&tray_mutex);
        return -1;     /* Wine handles the icon itself */
    }
    icon = find_icon(data->hWnd, data->uID);
    switch (msg)
    {
    case NIM_ADD:
        if (icon || !(icon = calloc(1, sizeof(*icon)))) break;
        icon->serial = ++tray_serial;
        icon->owner = data->hWnd;
        icon->id = data->uID;
        wl_list_insert(tray_icons.prev, &icon->link);
        if (!(ret = update_icon(icon, data)))
        {
            delete_icon(icon);
            ret = -1;     /* no Deskbar: Wine handles it */
        }
        break;
    case NIM_MODIFY:
        if (icon) ret = update_icon(icon, data);
        else ret = -1;
        break;
    case NIM_DELETE:
        if (icon)
        {
            delete_icon(icon);
            ret = TRUE;
        }
        else ret = -1;
        break;
    case NIM_SETVERSION:
        if (icon)
        {
            icon->version = data->uVersion;
            ret = TRUE;
        }
        else ret = -1;
        break;
    default:
        ret = -1;
        break;
    }
    pthread_mutex_unlock(&tray_mutex);
    return ret;
}

/***********************************************************************
 *           WAYLAND_CleanupIcons
 */
void WAYLAND_CleanupIcons(HWND hwnd)
{
    struct tray_icon *icon, *next;

    pthread_mutex_lock(&tray_mutex);
    wl_list_for_each_safe(icon, next, &tray_icons, link)
        if (icon->owner == hwnd) delete_icon(icon);
    pthread_mutex_unlock(&tray_mutex);
}

/* Tells the owner of an icon about the mouse, as the Windows tray does. */
static BOOL notify_owner(const struct tray_icon *icon, UINT msg, int x, int y)
{
    WPARAM wp = icon->id;
    LPARAM lp = msg;

    if (icon->version >= NOTIFYICON_VERSION_4)
    {
        wp = MAKEWPARAM(x, y);
        lp = MAKELPARAM(msg, icon->id);
    }
    TRACE("message %#x to %p, icon %u\n", msg, icon->owner, icon->id);
    return NtUserMessageCall(icon->owner, icon->callback, wp, lp, 0, NtUserSendNotifyMessage, FALSE) ||
           RtlGetLastWin32Error() != ERROR_INVALID_WINDOW_HANDLE;
}

/* A click on an item in the Deskbar: called by the thread that reads the events, when the pipe has something. */
void wayland_haiku_tray_dispatch(void)
{
    static const UINT downs[] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
    static const UINT ups[] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
    static const UINT doubles[] = { WM_LBUTTONDBLCLK, WM_RBUTTONDBLCLK, WM_MBUTTONDBLCLK };
    struct wine_tray_event event;
    struct tray_icon *found, copy;
    int button;

    if (read(tray_pipe[0], &event, sizeof(event)) != sizeof(event)) return;
    button = event.button >= 1 && event.button <= 3 ? event.button - 1 : 0;

    pthread_mutex_lock(&tray_mutex);
    copy.serial = 0;
    wl_list_for_each(found, &tray_icons, link)
        if (found->serial == event.serial) copy = *found;
    pthread_mutex_unlock(&tray_mutex);
    if (!copy.serial || !copy.callback) return;

    /* the program opens its menu at the cursor */
    NtUserSetCursorPos(event.x, event.y);

    if (!notify_owner(&copy, event.clicks == 2 ? doubles[button] : downs[button], event.x, event.y) ||
        !notify_owner(&copy, ups[button], event.x, event.y))
    {
        WAYLAND_CleanupIcons(copy.owner);     /* the window is gone */
        return;
    }
    if (copy.version > 0)
    {
        if (button == 0 && event.clicks != 2) notify_owner(&copy, NIN_SELECT, event.x, event.y);
        if (button == 1) notify_owner(&copy, WM_CONTEXTMENU, event.x, event.y);
    }
}

#endif /* __HAIKU__ */
