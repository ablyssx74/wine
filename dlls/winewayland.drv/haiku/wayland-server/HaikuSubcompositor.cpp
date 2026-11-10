#include "HaikuSubcompositor.h"
#include "HaikuCompositor.h"

#include "AppKitPtrs.h"
#include <View.h>
#include <Bitmap.h>

#include <stdio.h>
#include <algorithm>
#include <vector>

extern const struct wl_interface wl_subcompositor_interface;


#define SUBCOMPOSITOR_VERSION 1


//#pragma mark - HaikuSubcompositor

class HaikuSubcompositor: public WlSubcompositor {
protected:
	virtual ~HaikuSubcompositor() = default;

public:
	void HandleGetSubsurface(uint32_t id, struct wl_resource *surface, struct wl_resource *parent) final;
};


HaikuSubcompositorGlobal *HaikuSubcompositorGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuSubcompositorGlobal> global(new(std::nothrow) HaikuSubcompositorGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &wl_subcompositor_interface, SUBCOMPOSITOR_VERSION)) return NULL;
	return global.Detach();
}

void HaikuSubcompositorGlobal::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	HaikuSubcompositor *manager = new(std::nothrow) HaikuSubcompositor();
	if (manager == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	if (!manager->Init(wl_client, version, id)) {
		return;
	}
}


void HaikuSubcompositor::HandleGetSubsurface(uint32_t id, struct wl_resource *surface, struct wl_resource *parent)
{
	HaikuSubsurface *subsurface = HaikuSubsurface::Create(Client(), Version(), id, surface, parent);
}


//#pragma mark - HaikuSubsurface

HaikuSubsurface *HaikuSubsurface::Create(struct wl_client *client, uint32_t version, uint32_t id, struct wl_resource *surface, struct wl_resource *parent)
{
	HaikuSubsurface *subsurface = new(std::nothrow) HaikuSubsurface();
	if (!subsurface->Init(client, version, id)) {
		return NULL;
	}
	HaikuSurface *haikuSurface = HaikuSurface::FromResource(surface);
	subsurface->fSurface = haikuSurface;
	haikuSurface->fSubsurface = subsurface;
	subsurface->fParent = HaikuSurface::FromResource(parent);
	subsurface->fParent->SurfaceList().Insert(subsurface);

	BView *parentView = subsurface->fParent->View();
	if (parentView) {
		haikuSurface->AttachView(parentView);
		haikuSurface->AttachViewsToEarlierSubsurfaces();
	}
	return subsurface;
}

HaikuSubsurface::~HaikuSubsurface()
{
	fSurface->Detach();
	if (fParent != NULL) {
		fParent->SurfaceList().Remove(this);
	}
	fSurface->fSubsurface = NULL;
}


HaikuSurface *HaikuSubsurface::Root()
{
	HaikuSurface *s = fParent;
	while (s->Subsurface() != NULL) {
		s = s->Subsurface()->fParent;
	}
	return s;
}

void HaikuSubsurface::GetOffset(int32_t &x, int32 &y)
{
	x = fState.x;
	y = fState.y;
	HaikuSubsurface *s = fParent->Subsurface();
	while (s != NULL) {
		x += s->fState.x;
		y += s->fState.y;
		s = s->fParent->Subsurface();
	}
}


void HaikuSubsurface::HandleSetPosition(int32_t x, int32_t y)
{
	fState.x = x;
	fState.y = y;
}

// Moves this subsurface in its parent's stacking order (the list runs from the bottom to the top) and in the parent's view
// (later children are drawn on top). Placing a surface above or below the parent itself puts it at the bottom of the
// children: a BView cannot be drawn behind the view it is in.
void HaikuSubsurface::Restack(struct wl_resource *siblingResource, bool above)
{
	if (fParent == NULL || siblingResource == NULL) return;
	HaikuSurface *sibling = HaikuSurface::FromResource(siblingResource);
	if (sibling == NULL) return;
	HaikuSubsurface *siblingSub = sibling->Subsurface();

	// (DoublyLinkedList's insert-before/after need an ASSERT macro this build does not have: rebuild the list instead.)
	SurfaceList &list = fParent->SurfaceList();
	if (sibling != fParent && (siblingSub == NULL || siblingSub == this || siblingSub->fParent != fParent)) return;
	std::vector<HaikuSubsurface*> order;
	for (HaikuSubsurface *s = list.First(); s != NULL; s = list.GetNext(s))
		if (s != this) order.push_back(s);
	auto at = order.begin();
	if (sibling != fParent) {
		at = std::find(order.begin(), order.end(), siblingSub);
		if (above && at != order.end()) ++at;
	}
	order.insert(at, this);
	while (list.First() != NULL) list.Remove(list.First());
	for (HaikuSubsurface *s: order) list.Add(s);

	BView *parentView = fParent->View();
	BView *view = fSurface->View();
	if (parentView == NULL || view == NULL || view->Parent() != parentView) return;

	BView *nextView = NULL;
	for (HaikuSubsurface *s = list.GetNext(this); s != NULL && nextView == NULL; s = list.GetNext(s)) {
		BView *v = s->Surface()->View();
		if (v != NULL && v->Parent() == parentView) nextView = v;
	}
	auto parentLocked = AppKitPtrs::LockedPtr(parentView);
	if (view->NextSibling() == nextView) return;
	view->RemoveSelf();
	parentView->AddChild(view, nextView);
	parentView->Invalidate();
}

void HaikuSubsurface::HandlePlaceAbove(struct wl_resource *sibling)
{
	Restack(sibling, true);
}

void HaikuSubsurface::HandlePlaceBelow(struct wl_resource *sibling)
{
	Restack(sibling, false);
}

void HaikuSubsurface::HandleSetSync()
{
}

void HaikuSubsurface::HandleSetDesync()
{
}
