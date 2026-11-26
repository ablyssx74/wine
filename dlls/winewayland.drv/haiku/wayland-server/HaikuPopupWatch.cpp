#include "HaikuPopupWatch.h"
#include "HaikuCompositor.h"
#include "HaikuSeat.h"
#include "WaylandEnv.h"

#include <OS.h>
#include <Window.h>
#include <WindowInfo.h>

#include <stdlib.h>

static const char* const kName = "wine popup watch";

class PopupWatch: public BView {
public:
	PopupWatch(HaikuSurface *surface):
		BView(BRect(0, 0, 0, 0), kName, B_FOLLOW_NONE, 0),
		fSurface(surface)
	{
	}

	void AttachedToWindow() override
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
	}

	void MouseDown(BPoint where) override
	{
		ConvertToScreen(&where);
		if (fSurface == NULL || OnOwnWindow(where))
			return;
		WaylandEnv env(Window());
		if (fSurface == NULL)
			return;
		HaikuSeatGlobal *seat = HaikuGetSeat(fSurface->Client());
		if (seat != NULL)
			seat->DismissPopup(fSurface);
	}

	HaikuSurface *fSurface;

private:
	// whether the point is on a window of this program that is shown (the menu itself, its submenu, the program's window)
	static bool OnOwnWindow(BPoint where)
	{
		int32 count = 0;
		int32 *tokens = get_token_list(getpid(), &count);
		bool found = false;
		for (int32 i = 0; i < count && !found; i++) {
			client_window_info *info = get_window_info(tokens[i]);
			if (info == NULL)
				continue;
			found = info->show_hide_level <= 0 && !info->is_mini
				&& where.x >= info->window_left && where.x <= info->window_right
				&& where.y >= info->window_top && where.y <= info->window_bottom;
			free(info);
		}
		free(tokens);
		return found;
	}
};

void HaikuWatchPopup(BWindow *window, HaikuSurface *surface)
{
	if (window->FindView(kName) == NULL)
		window->AddChild(new PopupWatch(surface));
}

void HaikuForgetPopup(BWindow *window)
{
	if (window == NULL || !window->Lock())
		return;
	PopupWatch *watch = dynamic_cast<PopupWatch*>(window->FindView(kName));
	if (watch != NULL)
		watch->fSurface = NULL;
	window->Unlock();
}
