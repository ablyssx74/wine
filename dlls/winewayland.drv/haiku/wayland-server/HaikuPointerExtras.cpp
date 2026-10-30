#include "HaikuPointerExtras.h"
#include "HaikuCompositor.h"
#include "HaikuSeat.h"
#include "AppKitPtrs.h"
#include "RelativePointerUnstableV1.h"
#include "PointerConstraintsUnstableV1.h"
#include "PointerWarpV1.h"
#include <AutoDeleter.h>
#include <Region.h>
#include <Screen.h>
#include <Rect.h>
#include <View.h>
#include <vector>
#include <algorithm>
#include <new>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct wl_interface zwp_relative_pointer_manager_v1_interface;
extern const struct wl_interface zwp_pointer_constraints_v1_interface;
extern const struct wl_interface wp_pointer_warp_v1_interface;

// libgame
extern void set_mouse_position(int32 x, int32 y);

bool HaikuGetRegionBounds(struct wl_resource *region, BRect &bounds);


namespace {

HaikuSurface *sFocus = NULL;
BPoint sPrev;
bool sHavePrev = false;

// A pointing device that reports absolute positions (a virtual machine's tablet) puts the pointer back where it is whatever
// the lock does, so the lock cannot hold it at a point: movement is then measured between one event and the next. It is told
// from a mouse by the first position after a warp, which is where the pointer was and not near the lock point.
bool sAbsoluteDevice = false;
int sAbsoluteVotes = 0;
bool sAfterWarp = false;
bigtime_t sWarpWhen = 0;    // when the pointer was last put back
BPoint sLastReal;
bool sHaveLastReal = false;

// A lock does not put the pointer back after every move (each put-back is a round trip to the input server, and a mouse
// reports hundreds of moves a second: the queue behind them is the stutter a player sees when first moving the mouse).
// Movement is measured between one event and the next, and the pointer is put back to the hold point only once it has
// strayed from it by kHoldRadius. Events that were already on their way when it was put back are dropped until one arrives
// near the hold point.
const double kHoldRadius = 100;
const bigtime_t kSettleTimeout = 100000;
BPoint sHold;
BPoint sHeld;
bool sHaveHold = false;
bool sSettling = false;
bigtime_t sSettleSince = 0;

// WINE_HAIKU_POINTER_TRACE=1: once a second, what the lock did (events, put-backs, the longest put-back, the longest gap)
struct PointerTrace {
	bigtime_t windowStart = 0, lastEvent = 0, longestWarp = 0, longestGap = 0;
	int events = 0, warps = 0, dropped = 0;
};
PointerTrace sTrace;
double sSentX = 0, sSentY = 0;
bool sTraceOn = false;
bool sTraceChecked = false;

bool
TraceWanted()
{
	if (!sTraceChecked) {
		sTraceChecked = true;
		sTraceOn = getenv("WINE_HAIKU_POINTER_TRACE") != NULL;
	}
	return sTraceOn;
}


// a point of the surface in whole pixels of the screen
BPoint
ToScreen(HaikuSurface *surface, BPoint where)
{
	BView *view = surface->View();
	if (view == NULL) return where;
	AppKitPtrs::LockedPtr(view)->ConvertToScreen(&where);
	return BPoint(floorf(where.x + 0.5f), floorf(where.y + 0.5f));
}


void
WarpToScreen(BPoint screen)
{
	bigtime_t start = sTraceOn ? system_time() : 0;
	set_mouse_position((int32)screen.x, (int32)screen.y);
	if (sTraceOn) {
		sTrace.warps++;
		sTrace.longestWarp = std::max(sTrace.longestWarp, system_time() - start);
	}
}

void
TraceEvent(bool dropped)
{
	if (!sTraceChecked) {
		sTraceChecked = true;
		sTraceOn = getenv("WINE_HAIKU_POINTER_TRACE") != NULL;
	}
	if (!sTraceOn) return;
	bigtime_t now = system_time();
	if (sTrace.windowStart == 0) sTrace.windowStart = now;
	if (sTrace.lastEvent != 0) sTrace.longestGap = std::max(sTrace.longestGap, now - sTrace.lastEvent);
	sTrace.lastEvent = now;
	sTrace.events++;
	if (dropped) sTrace.dropped++;
	if (now - sTrace.windowStart >= 1000000) {
		fprintf(stderr, "pointer: %d events, %d put back, %d dropped, longest put-back %ld us, longest gap %ld us, sent total %.0f,%.0f\n",
			sTrace.events, sTrace.warps, sTrace.dropped, (long)sTrace.longestWarp, (long)sTrace.longestGap, sSentX, sSentY);
		sTrace = PointerTrace();
	}
}


void
WarpTo(HaikuSurface *surface, BPoint where)
{
	if (surface->View() == NULL) return;
	WarpToScreen(ToScreen(surface, where));
}


#pragma mark - relative pointers

class HaikuRelativePointer;
std::vector<HaikuRelativePointer*> sRelativePointers;

class HaikuRelativePointer: public ZwpRelativePointerV1 {
public:
	HaikuRelativePointer() { sRelativePointers.push_back(this); }
	virtual ~HaikuRelativePointer()
	{
		sRelativePointers.erase(std::remove(sRelativePointers.begin(), sRelativePointers.end(), this), sRelativePointers.end());
	}
};

class HaikuRelativePointerManager: public ZwpRelativePointerManagerV1 {
public:
	void HandleGetRelativePointer(uint32_t id, struct wl_resource *pointer) final
	{
		HaikuRelativePointer *relative = new(std::nothrow) HaikuRelativePointer();
		if (relative == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		relative->Init(Client(), Version(), id);
	}
};


void
SendRelative(HaikuSurface *surface, double dx, double dy, bigtime_t when)
{
	sSentX += dx;
	sSentY += dy;
	for (HaikuRelativePointer *relative : sRelativePointers) {
		if (relative->Client() != surface->Client()) continue;
		relative->SendRelativeMotion((uint32_t)((uint64_t)when >> 32), (uint32_t)when, wl_fixed_from_double(dx),
			wl_fixed_from_double(dy), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	}
}


#pragma mark - constraints

class Constraint {
public:
	HaikuSurface *surface = NULL;
	bool persistent = true;
	bool active = false;
	bool dead = false;       // a oneshot constraint that has been broken
	bool hasRegion = false;
	BRect region;
	BPoint lockScreen;       // where the pointer is held, in whole pixels of the screen
	bool hasHint = false;
	BPoint hint;

	virtual ~Constraint() = default;
	virtual bool IsLock() const = 0;
	virtual void SendActive(bool active) = 0;
};

std::vector<Constraint*> sConstraints;

// a program locks the surface of its window, and the pointer is over a subsurface of it when a window draws with OpenGL
bool
InTree(HaikuSurface *surface, HaikuSurface *ancestor)
{
	while (surface != NULL) {
		if (surface == ancestor) return true;
		HaikuSubsurface *sub = surface->Subsurface();
		surface = sub != NULL ? sub->Parent() : NULL;
	}
	return false;
}

Constraint *
ActiveConstraint(HaikuSurface *surface)
{
	for (Constraint *constraint : sConstraints) {
		if (constraint->active && InTree(surface, constraint->surface)) return constraint;
	}
	return NULL;
}

void
Activate(Constraint *constraint)
{
	if (constraint->active || constraint->dead || constraint->surface == NULL) return;
	constraint->active = true;
	if (TraceWanted()) fprintf(stderr, "pointer: %s activated on %p (focus %p)\n", constraint->IsLock() ? "lock" : "confine",
		(void*)constraint->surface, (void*)sFocus);
	sAfterWarp = false;
	sHaveLastReal = false;
	sAbsoluteVotes = 0;
	sHaveHold = false;
	sSettling = false;
	if (constraint->IsLock()) {
		HaikuSurface *on = sFocus != NULL ? sFocus : constraint->surface;
		BSize size = on->Size();
		constraint->lockScreen = ToScreen(on, sHavePrev ? sPrev : BPoint(size.width / 2, size.height / 2));
	}
	constraint->SendActive(true);
}

void
Deactivate(Constraint *constraint)
{
	if (!constraint->active) return;
	constraint->active = false;
	if (TraceWanted()) fprintf(stderr, "pointer: %s deactivated on %p (focus %p)\n", constraint->IsLock() ? "lock" : "confine",
		(void*)constraint->surface, (void*)sFocus);
	constraint->SendActive(false);
	if (!constraint->persistent) constraint->dead = true;
}

void
ActivateFor(HaikuSurface *surface)
{
	for (Constraint *constraint : sConstraints) {
		if (InTree(surface, constraint->surface)) {
			Activate(constraint);
			break;    // one at a time
		}
	}
}

void
Forget(Constraint *constraint)
{
	sConstraints.erase(std::remove(sConstraints.begin(), sConstraints.end(), constraint), sConstraints.end());
}

bool
HasConstraint(HaikuSurface *surface)
{
	for (Constraint *constraint : sConstraints) {
		if (surface != NULL && constraint->surface == surface && !constraint->dead) return true;
	}
	return false;
}


class HaikuLockedPointer: public ZwpLockedPointerV1, public Constraint {
public:
	virtual ~HaikuLockedPointer()
	{
		bool wasActive = active;
		Forget(this);
		// the pointer goes where the program said it wants it once the lock is over
		if (hasHint && wasActive && surface != NULL && InTree(sFocus, surface)) {
			WarpTo(surface, hint);
			sPrev = hint;
			sHavePrev = true;
		}
	}
	bool IsLock() const final { return true; }
	void SendActive(bool on) final { if (on) SendLocked(); else SendUnlocked(); }
	void HandleSetCursorPositionHint(wl_fixed_t x, wl_fixed_t y) final
	{
		hint = BPoint(wl_fixed_to_double(x), wl_fixed_to_double(y));
		hasHint = true;
	}
	void HandleSetRegion(struct wl_resource *) final {}
};

class HaikuConfinedPointer: public ZwpConfinedPointerV1, public Constraint {
public:
	virtual ~HaikuConfinedPointer()
	{
		bool wasActive = active;
		Forget(this);
	}
	bool IsLock() const final { return false; }
	void SendActive(bool on) final { if (on) SendConfined(); else SendUnconfined(); }
	void HandleSetRegion(struct wl_resource *newRegion) final
	{
		hasRegion = newRegion != NULL && HaikuGetRegionBounds(newRegion, region);
	}
};

class HaikuPointerConstraints: public ZwpPointerConstraintsV1 {
	template<class T> void Create(uint32_t id, struct wl_resource *surfaceResource, struct wl_resource *region, uint32_t lifetime)
	{
		HaikuSurface *surface = HaikuSurface::FromResource(surfaceResource);
		if (surface == NULL || HasConstraint(surface)) {
			wl_resource_post_error(ToResource(), errorAlreadyConstrained, "the surface is already constrained");
			return;
		}
		T *constraint = new(std::nothrow) T();
		if (constraint == NULL) {
			wl_client_post_no_memory(Client());
			return;
		}
		constraint->surface = surface;
		constraint->persistent = lifetime == lifetimePersistent;
		if (region != NULL) constraint->hasRegion = HaikuGetRegionBounds(region, constraint->region);
		if (!constraint->Init(Client(), Version(), id)) return;
		sConstraints.push_back(constraint);
		if (TraceWanted()) fprintf(stderr, "pointer: %s asked for on %p (focus %p, %s)\n", constraint->IsLock() ? "lock" : "confine",
			(void*)surface, (void*)sFocus, InTree(sFocus, surface) ? "inside" : "outside");
		if (InTree(sFocus, surface)) Activate(constraint);
	}

public:
	void HandleLockPointer(uint32_t id, struct wl_resource *surface, struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime) final
	{
		Create<HaikuLockedPointer>(id, surface, region, lifetime);
	}
	void HandleConfinePointer(uint32_t id, struct wl_resource *surface, struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime) final
	{
		Create<HaikuConfinedPointer>(id, surface, region, lifetime);
	}
};


#pragma mark - warp

class HaikuPointerWarp: public WpPointerWarpV1 {
public:
	void HandleWarpPointer(struct wl_resource *surfaceResource, struct wl_resource *pointer, wl_fixed_t x, wl_fixed_t y, uint32_t serial) final
	{
		HaikuSurface *surface = HaikuSurface::FromResource(surfaceResource);
		if (surface == NULL || !InTree(sFocus, surface)) return;
		BPoint where(wl_fixed_to_double(x), wl_fixed_to_double(y));
		WarpTo(surface, where);
		sPrev = where;
		sHavePrev = true;
	}
};

}


#pragma mark - globals

template<class Global, class Object>
static void
BindObject(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	Object *object = new(std::nothrow) Object();
	if (object == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	object->Init(wl_client, version, id);
}

#define GLOBAL(Name, Object, Interface, Version) \
	Name *Name::Create(struct wl_display *display) \
	{ \
		ObjectDeleter<Name> global(new(std::nothrow) Name()); \
		if (!global.IsSet()) return NULL; \
		if (!global->Init(display, &Interface, Version)) return NULL; \
		return global.Detach(); \
	} \
	void Name::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id) \
	{ \
		BindObject<Name, Object>(wl_client, version, id); \
	}

GLOBAL(HaikuRelativePointerManagerGlobal, HaikuRelativePointerManager, zwp_relative_pointer_manager_v1_interface, 1)
GLOBAL(HaikuPointerConstraintsGlobal, HaikuPointerConstraints, zwp_pointer_constraints_v1_interface, 1)
GLOBAL(HaikuPointerWarpGlobal, HaikuPointerWarp, wp_pointer_warp_v1_interface, 1)


#pragma mark - what the seat reports

void
HaikuPointerSurfaceDestroyed(HaikuSurface *surface)
{
	// a constraint outlives its surface until the program destroys it: it must not attach itself to a new surface that is
	// given the same address
	for (Constraint *constraint : sConstraints) {
		if (constraint->surface == surface) {
			constraint->surface = NULL;
			constraint->active = false;
			constraint->dead = true;
		}
	}
	if (sFocus == surface) {
		sFocus = NULL;
		sHavePrev = false;
	}
}

void
HaikuPointerFocusChanged(HaikuSurface *oldSurface, HaikuSurface *newSurface)
{
	if (oldSurface == newSurface) return;
	if (TraceWanted()) fprintf(stderr, "pointer: focus %p -> %p (%zu constraints)\n", (void*)oldSurface, (void*)newSurface,
		sConstraints.size());
	sHavePrev = false;
	if (oldSurface != NULL) {
		for (size_t i = 0; i < sConstraints.size(); i++) {
			// moving between a window and its subsurfaces does not end its lock
			if (InTree(oldSurface, sConstraints[i]->surface) && !InTree(newSurface, sConstraints[i]->surface))
				Deactivate(sConstraints[i]);
		}
	}
	sFocus = newSurface;
	if (newSurface != NULL) ActivateFor(newSurface);
}

bool
HaikuPointerMotion(HaikuSurface *surface, BPoint &where, bigtime_t when)
{
	Constraint *constraint = surface == sFocus ? ActiveConstraint(surface) : NULL;    // (also a subsurface of the locked one)

	if (constraint != NULL && constraint->IsLock()) {
		// motion is measured from the point the pointer is held at, which it is put back to every time
		BPoint now = ToScreen(surface, where);
		static const char *absoluteEnv = getenv("WINE_HAIKU_ABSOLUTE_POINTER");
		bool absoluteOff = absoluteEnv != NULL && strcmp(absoluteEnv, "0") == 0;
		if (!sAbsoluteDevice && absoluteEnv != NULL && !absoluteOff) sAbsoluteDevice = true;
		// events that were already on their way when the pointer was put back arrive within a few milliseconds, near where
		// the pointer was: only an event after that says that the device put the pointer back by itself
		if (!absoluteOff && !sAbsoluteDevice && sAfterWarp && sHaveLastReal && !(now == constraint->lockScreen)
			&& when - sWarpWhen >= 30000) {
			BPoint hold = sHaveHold ? sHold : constraint->lockScreen;
			bool nearLock = fabs(now.x - hold.x) <= 40 && fabs(now.y - hold.y) <= 40;
			bool nearLast = fabs(now.x - sLastReal.x) <= 12 && fabs(now.y - sLastReal.y) <= 12;
			if (!nearLock && nearLast) {
				if (++sAbsoluteVotes >= 4) sAbsoluteDevice = true;
			} else {
				sAbsoluteVotes = 0;
			}
		}
		if (sAbsoluteDevice) {
			if (sHaveLastReal && !(now == sLastReal) && !(now == constraint->lockScreen))
				SendRelative(surface, now.x - sLastReal.x, now.y - sLastReal.y, when);
			if (!(now == constraint->lockScreen)) { sLastReal = now; sHaveLastReal = true; }
			sAfterWarp = false;
			return true;
		}
		// WINE_HAIKU_POINTER_LOCK: "perevent" puts the pointer back after every move (how it was before), "off" never puts it
		// back (relative motion is only the change of position), anything else holds it as described above
		static const char *lockMode = getenv("WINE_HAIKU_POINTER_LOCK");
		if (lockMode != NULL && strcmp(lockMode, "off") == 0) {
			TraceEvent(false);
			if (sHaveLastReal && !(now == sLastReal)) SendRelative(surface, now.x - sLastReal.x, now.y - sLastReal.y, when);
			sLastReal = now;
			sHaveLastReal = true;
			return true;
		}
		if (lockMode != NULL && strcmp(lockMode, "perevent") == 0) {
			TraceEvent(false);
			double dx = now.x - constraint->lockScreen.x, dy = now.y - constraint->lockScreen.y;
			if (dx == 0 && dy == 0) return true;
			SendRelative(surface, dx, dy, when);
			sLastReal = now;
			sHaveLastReal = true;
			WarpToScreen(constraint->lockScreen);
			sAfterWarp = true;
			sWarpWhen = when;
			return true;
		}
		if (!sHaveHold) {
			// hold the pointer away from the edges of the screen, so that it has room to move before it is put back
			BRect screen = BScreen().Frame();
			float margin = kHoldRadius + 20;
			sHold = BPoint(std::min<float>(std::max<float>(constraint->lockScreen.x, screen.left + margin), screen.right - margin),
				std::min<float>(std::max<float>(constraint->lockScreen.y, screen.top + margin), screen.bottom - margin));
			sHeld = constraint->lockScreen;
			sHaveHold = true;
			sSettling = false;
			if (!(sHold == constraint->lockScreen)) {
				WarpToScreen(sHold);
				sHeld = sHold;
				sSettling = true;
				sSettleSince = when;
				sAfterWarp = true;
				sWarpWhen = when;
			}
		}
		bool far = fabs(now.x - sHold.x) > kHoldRadius || fabs(now.y - sHold.y) > kHoldRadius;
		if (sSettling) {
			if (far && when - sSettleSince < kSettleTimeout) {
				TraceEvent(true);
				return true;    // on its way before the pointer was put back
			}
			sSettling = false;
			if (far) sHeld = now;    // the pointer did not come back in time: measure from here
			else sAfterWarp = false;    // it came back: a mouse, not a device that puts the pointer back by itself
		}
		TraceEvent(false);
		double dx = now.x - sHeld.x, dy = now.y - sHeld.y;
		sHeld = now;
		if (dx != 0 || dy != 0) SendRelative(surface, dx, dy, when);
		sLastReal = now;
		sHaveLastReal = true;
		if (far) {
			WarpToScreen(sHold);
			sHeld = sHold;
			sSettling = true;
			sSettleSince = when;
			sAfterWarp = true;
			sWarpWhen = when;
		}
		return true;
	}

	if (sHavePrev && (where.x != sPrev.x || where.y != sPrev.y))
		SendRelative(surface, where.x - sPrev.x, where.y - sPrev.y, when);

	if (constraint != NULL) {
		BRect bounds(0, 0, surface->Size().width - 1, surface->Size().height - 1);
		if (constraint->hasRegion) bounds = bounds & constraint->region;
		BPoint clamped(std::min<float>(std::max<float>(where.x, bounds.left), bounds.right),
			std::min<float>(std::max<float>(where.y, bounds.top), bounds.bottom));
		if (clamped != where) {
			WarpTo(surface, clamped);
			where = clamped;
		}
	}
	sPrev = where;
	sHavePrev = true;
	return false;
}
