/* winetrayitem: the item in the Deskbar's tray that stands for one tray icon of a Windows program. It draws the icon, and
 * sends clicks to the bridge library in Wine. It takes itself out of the tray when that program is gone. */
#include <Bitmap.h>
#include <Deskbar.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "haikutray.h"

class WineTrayView : public BView {
public:
    WineTrayView(BMessage *archive)
        : BView(archive), fBitmap(NULL), fRunner(NULL), fSerial(archive->GetInt32("wine:serial", 0)), fButton(0), fClicks(1)
    {
        archive->FindMessenger("wine:target", &fTarget);
        SetIcon(*archive, "wine:width", "wine:height", "wine:pixels", "wine:tip");
    }

    ~WineTrayView() override
    {
        delete fBitmap;
        delete fRunner;
    }

    // (defined outside the class: the Deskbar looks the symbol up, and an inline function nobody calls is not made)
    static BArchivable *Instantiate(BMessage *archive);

    status_t Archive(BMessage *archive, bool deep) const override
    {
        status_t status = BView::Archive(archive, deep);
        if (status != B_OK) return status;
        archive->AddString("add_on", WINE_TRAY_ITEM_SIGNATURE);
        archive->AddString("class", WINE_TRAY_ITEM_CLASS);
        archive->AddInt32("wine:serial", fSerial);
        archive->AddMessenger("wine:target", fTarget);
        archive->AddString("wine:tip", fTip);
        if (fBitmap != NULL) {
            archive->AddInt32("wine:width", fBitmap->Bounds().IntegerWidth() + 1);
            archive->AddInt32("wine:height", fBitmap->Bounds().IntegerHeight() + 1);
            archive->AddData("wine:pixels", B_RAW_TYPE, fBitmap->Bits(), fBitmap->BitsLength());
        }
        return B_OK;
    }

    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        if (Parent() != NULL) SetViewColor(Parent()->ViewColor());
        SetLowColor(ViewColor());
        SetToolTip(fTip.Length() > 0 ? fTip.String() : NULL);
        BMessage hello(WINE_TRAY_MSG_HELLO);
        hello.AddInt32("serial", fSerial);
        hello.AddMessenger("item", BMessenger(this));
        fTarget.SendMessage(&hello, (BHandler *)NULL, 500000);
        BMessage check(WINE_TRAY_MSG_CHECK);
        fRunner = new BMessageRunner(BMessenger(this), &check, 2000000);
    }

    void DetachedFromWindow() override
    {
        delete fRunner;
        fRunner = NULL;
        BView::DetachedFromWindow();
    }

    void Draw(BRect) override
    {
        if (fBitmap == NULL) return;
        const BRect bounds = Bounds();
        const float side = min_c(bounds.Width(), bounds.Height()) + 1;
        const float left = floorf((bounds.Width() + 1 - side) / 2), top = floorf((bounds.Height() + 1 - side) / 2);
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        DrawBitmap(fBitmap, fBitmap->Bounds(), BRect(left, top, left + side - 1, top + side - 1), B_FILTER_BITMAP_BILINEAR);
        SetDrawingMode(B_OP_COPY);
    }

    // The click is passed on when the button is let go: a menu that opened under a pointer whose button is still down
    // would get the release.
    void MouseDown(BPoint where) override
    {
        uint32 buttons = 0;
        BMessage *message = Window() != NULL ? Window()->CurrentMessage() : NULL;
        if (message != NULL) {
            buttons = message->GetInt32("buttons", 0);
            fClicks = message->GetInt32("clicks", 1);
        }
        fButton = (buttons & B_SECONDARY_MOUSE_BUTTON) ? 2 : (buttons & B_TERTIARY_MOUSE_BUTTON) ? 3 : 1;
        fWhere = ConvertToScreen(where);
        SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
        // (a click that a dock passes on may come without a release: then it is passed on a little later)
        BMessage later('wTlt');
        BMessageRunner::StartSending(BMessenger(this), &later, 300000, 1);
    }

    void MouseUp(BPoint) override
    {
        SendClick();
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
        case WINE_TRAY_MSG_UPDATE:
            SetIcon(*message, "width", "height", "pixels", "tip");
            SetToolTip(fTip.Length() > 0 ? fTip.String() : NULL);
            Invalidate();
            break;
        case WINE_TRAY_MSG_CHECK:
            if (!fTarget.IsValid()) RemoveSelfFromTray();
            break;
        case 'wTlt': {
            BPoint where;
            uint32 buttons = 0;
            GetMouse(&where, &buttons, false);
            if (buttons == 0) SendClick();
            break;
        }
        default:
            BView::MessageReceived(message);
        }
    }

private:
    void SetIcon(const BMessage &from, const char *widthName, const char *heightName, const char *dataName, const char *tipName)
    {
        const char *tip = NULL;
        if (from.FindString(tipName, &tip) == B_OK) fTip = tip;
        const int32 width = from.GetInt32(widthName, 0), height = from.GetInt32(heightName, 0);
        const void *data = NULL;
        ssize_t size = 0;
        if (width <= 0 || height <= 0 || width > 512 || height > 512
            || from.FindData(dataName, B_RAW_TYPE, &data, &size) != B_OK || size != (ssize_t)width * height * 4)
            return;
        BBitmap *bitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), 0, B_RGBA32);
        if (bitmap->InitCheck() != B_OK) {
            delete bitmap;
            return;
        }
        for (int32 y = 0; y < height; y++)
            memcpy((uint8 *)bitmap->Bits() + y * bitmap->BytesPerRow(), (const uint8 *)data + y * width * 4, width * 4);
        delete fBitmap;
        fBitmap = bitmap;
    }

    void SendClick()
    {
        if (fButton == 0) return;
        BMessage click(WINE_TRAY_MSG_CLICK);
        click.AddInt32("serial", fSerial);
        click.AddInt32("button", fButton);
        click.AddInt32("clicks", fClicks);
        click.AddPoint("where", fWhere);
        fButton = 0;
        fTarget.SendMessage(&click, (BHandler *)NULL, 500000);
    }

    static int32 Remove(void *name)
    {
        BDeskbar().RemoveItem((const char *)name);
        free(name);
        return 0;
    }

    // (asking the Deskbar from its own window's thread would wait for itself: another thread asks)
    void RemoveSelfFromTray()
    {
        delete fRunner;
        fRunner = NULL;
        thread_id thread = spawn_thread(Remove, "remove wine tray item", B_NORMAL_PRIORITY, strdup(Name()));
        if (thread >= 0) resume_thread(thread);
    }

    BBitmap *fBitmap;
    BMessageRunner *fRunner;
    BMessenger fTarget;
    BString fTip;
    uint32 fSerial;
    int32 fButton, fClicks;
    BPoint fWhere;
};

BArchivable *WineTrayView::Instantiate(BMessage *archive)
{
    return validate_instantiation(archive, WINE_TRAY_ITEM_CLASS) ? new WineTrayView(archive) : NULL;
}

int main()
{
    return 0;
}
