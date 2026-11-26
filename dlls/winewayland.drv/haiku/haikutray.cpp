/* libwinehaikutray.so: puts the tray icons of Windows programs into the Deskbar's tray, one item for each (see
 * trayitem.cpp), and passes the clicks on them back to Wine's driver through a pipe. It runs in the process that has the
 * Windows tray (explorer.exe); the application object is the one the Wayland compositor made there. */
#include <Application.h>
#include <Bitmap.h>
#include <DataIO.h>
#include <TranslationUtils.h>
#include <Deskbar.h>
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <Mime.h>
#include <OS.h>
#include <View.h>

#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "haikutray.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {

struct Icon {
    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    std::string tip;
    BMessenger item;      // set once the item in the Deskbar has said hello
};

/* The item as it is handed to the Deskbar: the Deskbar archives it and makes the real one from the add-on. */
class Stub : public BView {
public:
    Stub(const char *name, float size, uint32_t serial, const BMessenger &target, const Icon &icon)
        : BView(BRect(0, 0, size - 1, size - 1), name, B_FOLLOW_NONE, B_WILL_DRAW), fSerial(serial), fTarget(target),
          fIcon(icon) {}

    status_t Archive(BMessage *archive, bool deep) const override
    {
        status_t status = BView::Archive(archive, deep);
        if (status != B_OK) return status;
        archive->RemoveName("class");
        archive->AddString("class", WINE_TRAY_ITEM_CLASS);
        archive->AddString("add_on", WINE_TRAY_ITEM_SIGNATURE);
        archive->AddInt32("wine:serial", fSerial);
        archive->AddMessenger("wine:target", fTarget);
        archive->AddInt32("wine:width", fIcon.width);
        archive->AddInt32("wine:height", fIcon.height);
        if (!fIcon.pixels.empty())
            archive->AddData("wine:pixels", B_RAW_TYPE, fIcon.pixels.data(), fIcon.pixels.size() * 4);
        return archive->AddString("wine:tip", fIcon.tip.c_str());
    }

private:
    uint32_t fSerial;
    BMessenger fTarget;
    const Icon &fIcon;
};

static std::string ItemName(uint32_t serial)
{
    char name[64];
    snprintf(name, sizeof(name), "WineTray-%d-%u", (int)getpid(), (unsigned)serial);
    return name;
}

static void SendIcon(const Icon &icon)
{
    BMessage update(WINE_TRAY_MSG_UPDATE);
    update.AddInt32("width", icon.width);
    update.AddInt32("height", icon.height);
    if (!icon.pixels.empty())
        update.AddData("pixels", B_RAW_TYPE, icon.pixels.data(), icon.pixels.size() * 4);
    update.AddString("tip", icon.tip.c_str());
    icon.item.SendMessage(&update, (BHandler *)NULL, 500000);
}

class Bridge : public BLooper {
public:
    Bridge(int fd) : BLooper("wine tray bridge"), fEventFd(fd) {}

    std::map<uint32_t, Icon> fIcons;     // under the looper's lock

    void MessageReceived(BMessage *message) override
    {
        uint32_t serial = message->GetInt32("serial", 0);
        std::map<uint32_t, Icon>::iterator found = fIcons.find(serial);

        switch (message->what) {
        case WINE_TRAY_MSG_HELLO:
            if (found != fIcons.end() && message->FindMessenger("item", &found->second.item) == B_OK)
                SendIcon(found->second);     // it may have changed since it was added
            break;
        case WINE_TRAY_MSG_CLICK: {
            BPoint where;
            if (found == fIcons.end() || message->FindPoint("where", &where) != B_OK) break;
            wine_tray_event event;
            event.serial = serial;
            event.button = message->GetInt32("button", 1);
            event.clicks = message->GetInt32("clicks", 1);
            event.x = (int32_t)where.x;
            event.y = (int32_t)where.y;
            if (write(fEventFd, &event, sizeof(event)) != sizeof(event))
                fprintf(stderr, "wine tray: a click was lost\n");
            break;
        }
        default:
            BLooper::MessageReceived(message);
        }
    }

private:
    int fEventFd;
};

static Bridge *gBridge;

}   // namespace

EXPORT uint32_t *wine_tray_decode(const void *data, size_t size, int *width, int *height)
{
    BMemoryIO io(data, size);
    BBitmap *bitmap = BTranslationUtils::GetBitmap(&io);
    uint32_t *pixels = NULL;
    if (bitmap != NULL && bitmap->InitCheck() == B_OK
        && (bitmap->ColorSpace() == B_RGBA32 || bitmap->ColorSpace() == B_RGB32)) {
        const int w = bitmap->Bounds().IntegerWidth() + 1, h = bitmap->Bounds().IntegerHeight() + 1;
        if (w > 0 && h > 0 && w <= 512 && h <= 512 && (pixels = (uint32_t *)malloc((size_t)w * h * 4)) != NULL) {
            for (int y = 0; y < h; y++)
                memcpy(pixels + (size_t)y * w, (const uint8 *)bitmap->Bits() + y * bitmap->BytesPerRow(), (size_t)w * 4);
            if (bitmap->ColorSpace() == B_RGB32)
                for (int i = 0; i < w * h; i++) pixels[i] |= 0xff000000;
            *width = w;
            *height = h;
        }
    }
    delete bitmap;
    return pixels;
}

EXPORT int wine_tray_init(const char *item_path, int event_fd)
{
    if (gBridge != NULL) return 1;
    if (be_app == NULL || access(item_path, R_OK) != 0) return 0;
    // the Deskbar finds an item's code by its signature, so the system has to know the file
    create_app_meta_mime(item_path, false, true, false);
    gBridge = new Bridge(event_fd);
    if (gBridge->Run() < B_OK) {
        gBridge = NULL;
        return 0;
    }
    return 1;
}

EXPORT int wine_tray_set(uint32_t serial, const uint32_t *pixels, int width, int height, const char *tip)
{
    if (gBridge == NULL || !gBridge->Lock()) return 0;
    Icon &icon = gBridge->fIcons[serial];
    if (pixels != NULL && width > 0 && height > 0) {
        icon.pixels.assign(pixels, pixels + (size_t)width * height);
        icon.width = width;
        icon.height = height;
    }
    if (tip != NULL) icon.tip = tip;

    BDeskbar deskbar;
    int result = 1;
    const std::string name = ItemName(serial);
    if (!deskbar.HasItem(name.c_str())) {
        icon.item = BMessenger();
        float size = deskbar.MaxItemHeight();
        if (size < 16) size = 16;
        Stub stub(name.c_str(), size, serial, BMessenger(gBridge), icon);
        status_t status = deskbar.AddItem(&stub);
        if (status != B_OK) {
            fprintf(stderr, "wine tray: the Deskbar did not take the item: %s\n", strerror(status));
            result = 0;
        }
    } else if (icon.item.IsValid())
        SendIcon(icon);
    gBridge->Unlock();
    return result;
}

EXPORT void wine_tray_remove(uint32_t serial)
{
    if (gBridge == NULL || !gBridge->Lock()) return;
    gBridge->fIcons.erase(serial);
    BDeskbar deskbar;
    const std::string name = ItemName(serial);
    if (deskbar.HasItem(name.c_str())) deskbar.RemoveItem(name.c_str());
    gBridge->Unlock();
}
