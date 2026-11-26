/* The tray bridge: what Wine's driver (systray_haiku.c), the bridge library (haikutray.cpp, libwinehaikutray.so) and the
 * item in the Deskbar's tray (trayitem.cpp, winetrayitem) have in common. */
#ifndef WINE_HAIKU_TRAY_H
#define WINE_HAIKU_TRAY_H

#include <stddef.h>
#include <stdint.h>

#define WINE_TRAY_ITEM_SIGNATURE "application/x-vnd.winehaiku-trayitem"
#define WINE_TRAY_ITEM_CLASS "WineTrayView"

/* messages between the library and an item */
#define WINE_TRAY_MSG_HELLO 'wThi'     /* item -> library: "serial", "item" (messenger) */
#define WINE_TRAY_MSG_UPDATE 'wTup'    /* library -> item: "width", "height", "pixels", "tip" */
#define WINE_TRAY_MSG_CLICK 'wTck'     /* item -> library: "serial", "button" (1, 2, 3), "clicks", "where" (screen) */
#define WINE_TRAY_MSG_CHECK 'wTcr'     /* item -> itself: is the program still there */

/* a click, as the library writes it to the driver's pipe */
struct wine_tray_event
{
    uint32_t serial;
    int32_t button;     /* 1 left, 2 right, 3 middle */
    int32_t clicks;
    int32_t x, y;       /* screen */
};

#ifdef __cplusplus
extern "C" {
#endif

/* item_path: the winetrayitem file; event_fd: where clicks are written. 0 when the bridge cannot be used. */
int wine_tray_init(const char *item_path, int event_fd);
/* Adds or changes the item `serial`; pixels are width x height B_RGBA32 (not premultiplied). 0 on failure. */
int wine_tray_set(uint32_t serial, const uint32_t *pixels, int width, int height, const char *tip);
void wine_tray_remove(uint32_t serial);
/* Decodes a picture (a PNG) into straight B_RGBA32 pixels, to be freed with free(). Needs no wine_tray_init(). */
uint32_t *wine_tray_decode(const void *data, size_t size, int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif
