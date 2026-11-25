#pragma once

#include <SupportDefs.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

// B_TRANSPARENT_WINDOW exists on RenkuOS only: app_server then blends the window with what is behind it by the alpha channel of its
// pixels. A popup that Wine draws with rounded corners and a shadow (a menu of Telegram) has transparent pixels around it, which show as
// a white box in a window that is not transparent. The flag is set when the desktop is composited (is_composited_desktop() is looked up
// by name, Haiku does not have it); HAIWAY_TRANSPARENT_POPUPS=0 or =1 decides it either way.
static const uint32 kHaiwayTransparentWindow = 0x80000000;   // B_TRANSPARENT_WINDOW of RenkuOS (an enum there, not on Haiku)

static inline uint32 HaiwayTransparentWindowFlag()
{
	static const uint32 flag = []() -> uint32 {
		const char *env = getenv("HAIWAY_TRANSPARENT_POPUPS");
		if (env != NULL)
			return strcmp(env, "0") != 0 ? kHaiwayTransparentWindow : 0;
		bool (*isComposited)() = (bool (*)())dlsym(RTLD_DEFAULT, "is_composited_desktop");
		return (isComposited != NULL && isComposited()) ? kHaiwayTransparentWindow : 0;
	}();
	return flag;
}
