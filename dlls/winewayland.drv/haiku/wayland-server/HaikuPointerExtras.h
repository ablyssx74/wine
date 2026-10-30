#pragma once
#include "Wayland.h"
#include "WlGlobal.h"
#include <Point.h>
#include <OS.h>

class HaikuSurface;


/*
 * Pointer features the seat itself does not have: relative motion (zwp_relative_pointer_v1), locking and confining the pointer
 * (zwp_pointer_constraints_v1) and moving it (wp_pointer_warp_v1). Programs that look around with the mouse, hide the cursor or
 * place it themselves need them. The seat tells this code where the pointer is and which surface has it.
 */

class HaikuRelativePointerManagerGlobal: public WlGlocal {
public:
	static HaikuRelativePointerManagerGlobal *Create(struct wl_display *display);
	virtual ~HaikuRelativePointerManagerGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};

class HaikuPointerConstraintsGlobal: public WlGlocal {
public:
	static HaikuPointerConstraintsGlobal *Create(struct wl_display *display);
	virtual ~HaikuPointerConstraintsGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};

class HaikuPointerWarpGlobal: public WlGlocal {
public:
	static HaikuPointerWarpGlobal *Create(struct wl_display *display);
	virtual ~HaikuPointerWarpGlobal() = default;
	void Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) override;
};

// The surface is going away: the constraints that were on it are over.
void HaikuPointerSurfaceDestroyed(HaikuSurface *surface);

// The pointer now belongs to newSurface (either may be NULL).
void HaikuPointerFocusChanged(HaikuSurface *oldSurface, HaikuSurface *newSurface);

// The pointer moved to `where` (in the surface's coordinates). Sends the relative motion, keeps a lock or a confinement. `where` is
// moved back inside a confinement. Returns true if the motion is not to be reported as a position (the pointer is locked).
bool HaikuPointerMotion(HaikuSurface *surface, BPoint &where, bigtime_t when);
