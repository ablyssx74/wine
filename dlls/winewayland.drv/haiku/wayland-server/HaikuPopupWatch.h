#pragma once
// A popup that is a window of its own (a menu) takes no focus, so nothing told Wine when the user clicked somewhere else
// and the menu stayed. This view, added to such a window, sees every click; one that is on none of the program's windows is
// reported to the client as a keyboard "leave" with serial 0, which Wine's driver takes as "close the popups".
#include <View.h>

class BWindow;
class HaikuSurface;

// The caller holds the window's lock.
void HaikuWatchPopup(BWindow *window, HaikuSurface *surface);
// The surface is going away: the view must not use it any more.
void HaikuForgetPopup(BWindow *window);
