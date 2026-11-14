#include "HaikuPointerExtras.h"
#include "HaikuCompositor.h"
#include "HaikuPointerExtras.h"
#include "HaikuSubcompositor.h"
#include "HaikuShm.h"
#include "HaikuXdgSurface.h"
#include "HaikuXdgToplevel.h"
#include "HaikuXdgPopup.h"
#include "HaikuSeat.h"
#include "HaikuDataDeviceManager.h"
#include "Wayland.h"
#include "WaylandEnv.h"
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <stdio.h>
#include <bit>
#include <sys/stat.h>
#include <time.h>
#include <map>

#include "AppKitPtrs.h"
#include <Application.h>
#include <InterfaceDefs.h>
#include <View.h>
#include <Window.h>
#include <Bitmap.h>
#include <Region.h>
#include <Cursor.h>
#include <Screen.h>
#include <Messenger.h>
#include <pthread.h>
#include <vector>
#include <map>

extern const struct wl_interface wl_compositor_interface;


#define COMPOSITOR_VERSION 4

static void Assert(bool cond) {if (!cond) abort();}


// Drawing in step with the vertical retrace (V-sync). A surface that asks for it (its client requested a frame callback with the commit, which
// Wine does only when the program presents with the FIFO mode) and is big enough is not drawn at once: it is registered with the vsync thread
// below, which waits for the retrace and then tells the window, in a message, to draw. The copy into Haiku's single frame buffer then starts
// right after the vblank and stays ahead of the scan-out (no tear line). Nothing waits in the thread that handles the commit: it holds the
// global Wayland lock that the windows' input handling needs too (waiting there made the games lag and stopped the mouse look short).
// The frame callbacks of such a surface are sent after it was drawn, not by the pulse timer. It needs a display driver with a retrace
// semaphore (BScreen::WaitForRetrace() works) and turns itself off with a message otherwise.
// HAIWAY_VSYNC: "off" (never), "all" (every surface that requested a frame callback), anything else or not set (on): the ones that cover
// at least 60 percent of the screen.
static const uint32 kVSyncMessage = 'HVsy';

static bool VSyncWanted(const BBitmap *bitmap, bool isSubsurface)
{
	static int state = 0;   // 0: not decided, 1: on, -1: off
	static int onlyBig = 1;
	static float screenArea = 0;

	if (state < 0 || isSubsurface || bitmap == NULL) {
		return false;
	}
	if (state == 0) {
		const char *env = getenv("HAIWAY_VSYNC");
		if (env != NULL && strcmp(env, "off") == 0) {
			state = -1;
			return false;
		}
		onlyBig = !(env != NULL && strcmp(env, "all") == 0);
		BScreen screen;
		if (!screen.IsValid() || screen.WaitForRetrace(100000) != B_OK) {
			fprintf(stderr, "HAIWAY: no vertical retrace from the display driver, not waiting for it\n");
			state = -1;
			return false;
		}
		BRect frame = screen.Frame();
		screenArea = (frame.Width() + 1) * (frame.Height() + 1);
		state = 1;
		fprintf(stderr, "HAIWAY: drawing %s in step with the vertical retrace when they ask for it\n", onlyBig ? "big surfaces" : "surfaces");
	}
	if (onlyBig) {
		BRect b = bitmap->Bounds();
		if ((b.Width() + 1) * (b.Height() + 1) < 0.6f * screenArea) {
			return false;
		}
	}
	return true;
}

// Where in the refresh cycle the draws of big surfaces start (HAIWAY_VSYNC_STATS=<file>, for testing): the vsync thread then follows every
// retrace and notes when it came; each draw adds its delay after the last retrace to a histogram, and a line per 5 seconds is appended to the
// file. With V-sync working nearly all draws start in the first milliseconds of the cycle; without it they are spread evenly over it.
static const char *StatsFile()
{
	static const char *path = getenv("HAIWAY_VSYNC_STATS");
	return path != NULL && *path != '\0' ? path : NULL;
}

static volatile bigtime_t sLastRetrace = 0;
static const bigtime_t kRefreshPeriod = 16650;   // 60.06 Hz

class VSyncStats {
private:
	// no lock: a window thread that stopped inside one would stop every other draw (a draw only adds to the counters)
	volatile uint32 fBucket[17] = {};
	volatile uint32 fDraws = 0;
	volatile int64 fDrawTime = 0, fDrawMax = 0;
	volatile bigtime_t fSince = 0;
	volatile int32 fReporting = 0;
	float fScreenArea = 0;

public:
	static VSyncStats &Instance()
	{
		static VSyncStats instance;
		return instance;
	}

	bool Big(const BBitmap *bitmap)
	{
		if (fScreenArea == 0) {
			BScreen screen;
			BRect frame = screen.Frame();
			fScreenArea = (frame.Width() + 1) * (frame.Height() + 1);
		}
		BRect b = bitmap->Bounds();
		return (b.Width() + 1) * (b.Height() + 1) >= 0.6f * fScreenArea;
	}

	bigtime_t DrawStart(const BBitmap *bitmap)
	{
		if (sLastRetrace == 0 || !Big(bitmap)) {
			return 0;
		}
		return system_time();
	}

	void DrawEnd(bigtime_t start)
	{
		if (start == 0) {
			return;
		}
		const bigtime_t now = system_time();
		const bigtime_t phase = (start - sLastRetrace) % kRefreshPeriod;
		if (phase < 0) {
			return;
		}
		__atomic_add_fetch(&fBucket[phase * 17 / kRefreshPeriod], 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&fDraws, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&fDrawTime, now - start, __ATOMIC_RELAXED);
		if (now - start > fDrawMax) {
			fDrawMax = now - start;
		}
		if (fSince == 0) {
			fSince = now;
		}
		if (now - fSince >= 5000000 && __atomic_exchange_n(&fReporting, 1, __ATOMIC_ACQUIRE) == 0) {
			uint32 bucket[17];
			for (int i = 0; i < 17; i++) {
				bucket[i] = __atomic_exchange_n(&fBucket[i], 0, __ATOMIC_RELAXED);
			}
			const uint32 draws = __atomic_exchange_n(&fDraws, 0, __ATOMIC_RELAXED);
			const int64 drawTime = __atomic_exchange_n(&fDrawTime, 0, __ATOMIC_RELAXED);
			const int64 drawMax = __atomic_exchange_n(&fDrawMax, 0, __ATOMIC_RELAXED);
			const bigtime_t span = now - fSince;
			fSince = now;
			if (draws > 0) {
				if (FILE *f = fopen(StatsFile(), "a")) {
					uint32 early = bucket[0] + bucket[1] + bucket[2];   // the first ~2.9 ms of the cycle
					fprintf(f, "%.1f s: %u draws (%.1f/s), %.0f%% start in the first 3 ms of the cycle, draw call avg %lld max %lld us; start by ms:",
						span / 1e6, draws, draws * 1e6 / span, early * 100.0 / draws, (long long)(drawTime / draws), (long long)drawMax);
					for (int i = 0; i < 17; i++) {
						fprintf(f, " %u", bucket[i]);
					}
					fprintf(f, "\n");
					fclose(f);
				}
			}
			__atomic_store_n(&fReporting, 0, __ATOMIC_RELEASE);
		}
	}
};

// The vsync thread. It holds no lock but its own, and takes none of the others.
class VSyncDispatcher {
private:
	pthread_mutex_t fLock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t fCond = PTHREAD_COND_INITIALIZER;
	std::map<const void*, BMessenger> fPending;   // keyed by the surface only to take it out again; never used as a pointer
	bool fStarted = false;

	static void *ThreadEntry(void *arg) {((VSyncDispatcher*)arg)->Loop(); return NULL;}

	void Loop()
	{
		BScreen screen;
		for (;;) {
			pthread_mutex_lock(&fLock);
			while (fPending.empty() && StatsFile() == NULL) {
				pthread_cond_wait(&fCond, &fLock);
			}
			pthread_mutex_unlock(&fLock);

			// a retrace that came while nobody waited stays counted: use it up, then wait for the next one
			screen.WaitForRetrace(0);
			status_t result = screen.WaitForRetrace(40000);
			if (result == B_OK) {
				sLastRetrace = system_time();
			}
			if (result != B_OK && result != B_TIMED_OUT) {
				snooze(16000);   // no retrace after all: do not spin
			}

			std::vector<BMessenger> targets;
			pthread_mutex_lock(&fLock);
			for (auto &entry: fPending) {
				targets.push_back(entry.second);
			}
			fPending.clear();
			pthread_mutex_unlock(&fLock);

			for (BMessenger &target: targets) {
				BMessage message(kVSyncMessage);
				target.SendMessage(&message, (BHandler*)NULL, 0);   // a window that is gone just does not get it
			}
		}
	}

public:
	static VSyncDispatcher &Instance()
	{
		static VSyncDispatcher instance;
		return instance;
	}

	void Request(const void *key, const BMessenger &target)
	{
		pthread_mutex_lock(&fLock);
		fPending[key] = target;
		StartLocked();
		pthread_cond_signal(&fCond);
		pthread_mutex_unlock(&fLock);
	}

	void EnsureStarted()
	{
		pthread_mutex_lock(&fLock);
		StartLocked();
		pthread_cond_signal(&fCond);
		pthread_mutex_unlock(&fLock);
	}

	void StartLocked()
	{
		if (!fStarted) {
			pthread_t thread;
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			if (pthread_create(&thread, &attr, ThreadEntry, this) == 0) {
				fStarted = true;
			}
			pthread_attr_destroy(&attr);
		}
	}

	void Cancel(const void *key)
	{
		pthread_mutex_lock(&fLock);
		fPending.erase(key);
		pthread_mutex_unlock(&fLock);
	}
};



//#pragma mark - HaikuRegion

class HaikuRegion: public WlRegion {
private:
	BRegion fRegion;

public:
	virtual ~HaikuRegion() = default;
	static HaikuRegion *FromResource(struct wl_resource *resource) {return (HaikuRegion*)WlResource::FromResource(resource);}

	const BRegion &Region() {return fRegion;}

	void HandleAdd(int32_t x, int32_t y, int32_t width, int32_t height) final;
	void HandleSubtract(int32_t x, int32_t y, int32_t width, int32_t height) final;
};

void HaikuRegion::HandleAdd(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fRegion.Include(BRect(x, y, x + width - 1, y + height - 1));
}

void HaikuRegion::HandleSubtract(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fRegion.Exclude(BRect(x, y, x + width - 1, y + height - 1));
}


//#pragma mark - HaikuCompositor

class HaikuCompositor: public WlCompositor {
protected:
	virtual ~HaikuCompositor() = default;

public:
	void HandleCreateSurface(uint32_t id) override;
	void HandleCreateRegion(uint32_t id) override;
};


HaikuCompositorGlobal *HaikuCompositorGlobal::Create(struct wl_display *display)
{
	ObjectDeleter<HaikuCompositorGlobal> global(new(std::nothrow) HaikuCompositorGlobal());
	if (!global.IsSet()) return NULL;
	if (!global->Init(display, &wl_compositor_interface, COMPOSITOR_VERSION)) return NULL;
	return global.Detach();
}

void HaikuCompositorGlobal::Bind(struct wl_client *wl_client, uint32_t version, uint32_t id)
{
	HaikuCompositor *manager = new(std::nothrow) HaikuCompositor();
	if (manager == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	if (!manager->Init(wl_client, version, id)) {
		return;
	}
}


void HaikuCompositor::HandleCreateSurface(uint32_t id)
{
	HaikuSurface *surface = HaikuSurface::Create(Client(), Version(), id);
}

// the extent of a wl_region (for the pointer constraints)
bool HaikuGetRegionBounds(struct wl_resource *region, BRect &bounds)
{
	HaikuRegion *haikuRegion = HaikuRegion::FromResource(region);
	if (haikuRegion == NULL) return false;
	bounds = haikuRegion->Region().Frame();
	return bounds.IsValid();
}

void HaikuCompositor::HandleCreateRegion(uint32_t id)
{
	HaikuRegion *region = new(std::nothrow) HaikuRegion();
	if (region == NULL) {
		wl_client_post_no_memory(Client());
		return;
	}
	if (!region->Init(Client(), Version(), id)) {
		return;
	}
}


//#pragma mark - WaylandView

class WaylandView: public BView {
private:
	friend class HaikuSurface;

	HaikuSurface *fSurface;
	uint32 fOldMouseBtns = 0;
	bool fFramerateLimitDisabled = false;
	WaylandEnv *fActiveWlEnv {};

public:
	WaylandView(HaikuSurface *surface);
	virtual ~WaylandView();

	HaikuSurface *Surface() {return fSurface;}

	void RemoveDescendantsAndSelf();

	void WindowActivated(bool active) final;
	void MessageReceived(BMessage *msg) final;
	void Draw(BRect dirty);
	void Pulse(void);
};


WaylandView::WaylandView(HaikuSurface *surface):
	BView(BRect(), "WaylandView", B_FOLLOW_NONE, B_WILL_DRAW | B_TRANSPARENT_BACKGROUND | B_INPUT_METHOD_AWARE | B_PULSE_NEEDED),
	fSurface(surface)
{
	SetViewColor(B_TRANSPARENT_COLOR);

	char *envValue = getenv("HAIWAY_FRAMERATE_LIMIT");
	if (envValue != NULL) {
		fFramerateLimitDisabled = (strcmp(envValue, "disabled") == 0);
		if (fFramerateLimitDisabled) {
			SetFlags(Flags() & ~B_PULSE_NEEDED);
			fprintf(stderr, "Frame rate limiting disabled\n");
		}
	}
}

WaylandView::~WaylandView()
{
	if (fSurface != NULL) {
		debugger("[!] ~WaylandView: bad deletion");
	}
}

void WaylandView::RemoveDescendantsAndSelf()
{
	while (BView* child = ChildAt(0)) {
		WaylandView* view = dynamic_cast<WaylandView*>(child);
		view->RemoveDescendantsAndSelf();
	}

	if (fActiveWlEnv != NULL)
		WaylandEnv::Wait(&fActiveWlEnv, Looper());
	RemoveSelf();
}

void WaylandView::WindowActivated(bool active)
{
	WaylandEnv wlEnv(this, &fActiveWlEnv);
	HaikuSeatGlobal *seat = HaikuGetSeat(fSurface->Client());
	if (seat == NULL) return;

	if (fSurface->Subsurface() != NULL) {
		return;
	}

	seat->SetKeyboardFocus(fSurface, active);
}

void WaylandView::MessageReceived(BMessage *msg)
{
	if (msg->what == kVSyncMessage) {
		// the retrace came: draw what was committed
		WaylandEnv wlEnv(this, &fActiveWlEnv);
		if (fSurface != NULL) {
			fSurface->InvalidateNow();
		}
		return;
	}
	{
		WaylandEnv wlEnv(this, &fActiveWlEnv);

		HaikuSeatGlobal *seat = HaikuGetSeat(fSurface->Client());
		if (seat != NULL) {
			if (msg->what == B_COPY_TARGET) {
				entry_ref dirRef;
				BString name;
				if (msg->FindRef("directory", &dirRef) == B_OK && msg->FindString("name", &name) == B_OK) {
					if (seat->DataDevice() != NULL) {
						seat->DataDevice()->HandleCopyTargetReply(msg, dirRef, name);
						return;
					}
				}
			}

			bool isPointerMessage = true;
			BPoint where;
			if (msg->WasDropped()) {
				where = msg->DropPoint();
				AppKitPtrs::LockedPtr(this)->ConvertFromScreen(&where);
			} else if (msg->FindPoint("be:view_where", &where) < B_OK) {
				isPointerMessage = false;
			}
			HaikuSurface *surface = fSurface;
			while (isPointerMessage && !surface->InputRgnContains(where) && surface->Subsurface() != NULL) {
				surface = surface->Subsurface()->Parent();
			}
			if (surface != fSurface) {
				BPoint offset = AppKitPtrs::LockedPtr(fSurface->View())->Frame().LeftTop() - AppKitPtrs::LockedPtr(surface->View())->Frame().LeftTop();
				msg->ReplacePoint("be:view_where", where + offset);
			}
			if (seat->MessageReceived(surface, msg)) {
				return;
			}
		}
	}
	BView::MessageReceived(msg);
}

void WaylandView::Draw(BRect dirty)
{
	WaylandEnv wlEnv(this, &fActiveWlEnv);

	BBitmap *bmp = fSurface->Bitmap();
	bigtime_t statsStart = 0;
	if (StatsFile() != NULL && bmp != NULL) {
		VSyncDispatcher::Instance().EnsureStarted();
		statsStart = VSyncStats::Instance().DrawStart(bmp);
	}
	if (bmp != NULL) {
		auto viewLocked = AppKitPtrs::LockedPtr(this);
		if (fSurface->fBackdrop != NULL) {
			// a surface in a window of its own (a menu that sticks out of its owner): what was under the window first
			viewLocked->SetDrawingMode(B_OP_COPY);
			viewLocked->DrawBitmap(fSurface->fBackdrop, BPoint(0, 0));
		}
		drawing_mode mode;
		switch (bmp->ColorSpace()) {
			case B_RGBA64:
			case B_RGBA32:
			case B_RGBA15:
			case B_RGBA64_BIG:
			case B_RGBA32_BIG:
			case B_RGBA15_BIG:
				mode = B_OP_ALPHA;
				break;
			default:
				mode = B_OP_COPY;
				break;
		}
		if (mode == B_OP_ALPHA && fSurface->fState.opaqueRgn.has_value()) {
			viewLocked->ConstrainClippingRegion(&fSurface->fState.opaqueRgn.value());
			viewLocked->SetDrawingMode(B_OP_COPY);
			viewLocked->DrawBitmap(bmp);
			BRegion remaining = viewLocked->Bounds();
			remaining.Exclude(&fSurface->fState.opaqueRgn.value());
			viewLocked->ConstrainClippingRegion(&remaining);
		}
		viewLocked->SetDrawingMode(mode);
		const auto &viewportSrc = fSurface->fState.viewportSrc;
		const auto &viewportDst = fSurface->fState.viewportDst;
		if (viewportSrc.IsValid() && viewportDst.IsValid()) {
			BRect bitmapRect(viewportSrc.x, viewportSrc.y, viewportSrc.x + viewportSrc.width - 1, viewportSrc.y + viewportSrc.height - 1);
			BRect viewRect(0, 0, viewportDst.width - 1, viewportDst.height - 1);
			viewLocked->DrawBitmap(bmp, bitmapRect, viewRect);
		} else {
			viewLocked->DrawBitmap(bmp);
		}
	}

	if (statsStart != 0)
		VSyncStats::Instance().DrawEnd(statsStart);
	if (fSurface)
		fSurface->NoteDrawn();
	if (fSurface && (fFramerateLimitDisabled || fSurface->VSyncActive()))
		fSurface->CallFrameCallbacks();
}

void WaylandView::Pulse(void)
{
	if (fSurface && !fFramerateLimitDisabled && (!fSurface->VSyncActive() || fSurface->VSyncIdle()))
		fSurface->CallFrameCallbacks();
}

//#pragma mark - HaikuSurface

HaikuSurface::FrameCallback *HaikuSurface::FrameCallback::Create(struct wl_client *client, uint32_t version, uint32_t id)
{
	FrameCallback *callback = new(std::nothrow) FrameCallback();
	if (!callback->Init(client, version, id)) {
		return NULL;
	}
	return callback;
}

HaikuSurface *HaikuSurface::Create(struct wl_client *client, uint32_t version, uint32_t id)
{
	HaikuSurface *surface = new(std::nothrow) HaikuSurface();
	if (!surface->Init(client, version, id)) {
		return NULL;
	}
	return surface;
}

HaikuSurface::~HaikuSurface()
{
	VSyncDispatcher::Instance().Cancel(this);
	HaikuPointerSurfaceDestroyed(this);
	HaikuSeatGlobal *seat = HaikuGetSeat(Client());
	if (seat != NULL) {
		seat->SetPointerFocus(this, false, BMessage());
		seat->SetKeyboardFocus(this, false);
	}

	if (fView != NULL) {
		Detach();
	}

	for (HaikuSubsurface *subsurface = SurfaceList().First(); subsurface != NULL; subsurface = SurfaceList().GetNext(subsurface)) {
		subsurface->fParent = NULL;
	}
}

BSize HaikuSurface::Size() const
{
	if (fState.viewportDst.IsValid()) {
		return BSize(fState.viewportDst.width - 1, fState.viewportDst.height - 1);
	}
	if (Bitmap() != NULL) {
		return BSize(Bitmap()->Bounds().Width(), Bitmap()->Bounds().Height());
	}
	return BSize(-1, -1);
}

// A subsurface is a view in the window of its root surface, so it is cut off at that window's edge. Wine draws owned popups, Chromium's menus
// for one, as subsurfaces of their owner, and a menu or submenu that opens beyond the browser window was cut off or not drawn at all. A
// subsurface that sticks out of the root window is therefore shown in a window of its own (like an xdg_popup): borderless, without focus,
// in the subset of the root window, at the screen position of the root window plus the offset of the subsurface.
class WaylandSubsurfaceWindow: public BWindow {
public:
	WaylandSubsurfaceWindow(BRect frame):
		BWindow(frame, "", B_NO_BORDER_WINDOW_LOOK, B_FLOATING_SUBSET_WINDOW_FEEL, B_AVOID_FOCUS | B_NOT_RESIZABLE | B_NOT_ZOOMABLE)
	{
	}
	bool QuitRequested() override {return true;}
};

bool HaikuSurface::NeedsOwnWindow()
{
	if (fSubsurface == NULL || fSubWindow != NULL || Bitmap() == NULL) {
		return false;
	}
	HaikuSurface *root = fSubsurface->Root();
	if (root == NULL || root->View() == NULL || root->View()->Window() == NULL) {
		return false;
	}
	int32_t ox = 0, oy = 0;
	fSubsurface->GetOffset(ox, oy);
	const BSize size = Size();
	if (size.width < 8 || size.height < 8) {
		return false;
	}
	auto rootLocked = AppKitPtrs::LockedPtr(root->View());
	const BRect bounds = root->View()->Bounds();
	const float slack = 4;
	const float rootWidth = bounds.Width(), rootHeight = bounds.Height();
	// not the client area of the window itself (a subsurface that covers most of the root, which may be bigger than the root for a
	// moment while the window is resized), and it has to overlap the root (it opens from something in the window)
	if (size.width >= 0.9f * rootWidth && size.height >= 0.9f * rootHeight) {
		return false;
	}
	if (ox + size.width < 0 || oy + size.height < 0 || ox > rootWidth || oy > rootHeight) {
		return false;
	}
	return ox < -slack || oy < -slack || ox + size.width > rootWidth + slack || oy + size.height > rootHeight + slack;
}

void HaikuSurface::MoveToOwnWindow()
{
	HaikuSurface *root = fSubsurface->Root();
	int32_t ox = 0, oy = 0;
	fSubsurface->GetOffset(ox, oy);
	const BSize size = Size();

	BWindow *rootWindow;
	BPoint screenPoint;
	{
		auto rootLocked = AppKitPtrs::LockedPtr(root->View());
		rootWindow = root->View()->Window();
		screenPoint = root->View()->ConvertToScreen(BPoint(ox, oy));
	}

	WaylandSubsurfaceWindow *window = new(std::nothrow) WaylandSubsurfaceWindow(
		BRect(screenPoint.x, screenPoint.y, screenPoint.x + size.width, screenPoint.y + size.height));
	if (window == NULL) {
		return;
	}

	// take the view out of the window of its parent
	BWindow *parentWindow = fView->Window();
	if (parentWindow != NULL && parentWindow->Lock()) {
		fView->RemoveSelf();
		parentWindow->Unlock();
	}

	// the transparent pixels of the surface (the soft shadow and the corners of a menu) show what is under the window: take that from the
	// screen before the window is there
	{
		BScreen screen;
		BRect area = window->Frame();
		if (screen.IsValid() && area.IsValid()) {
			BBitmap *backdrop = new(std::nothrow) BBitmap(BRect(0, 0, area.Width(), area.Height()), B_RGB32);
			if (backdrop != NULL && backdrop->IsValid() && screen.ReadBitmap(backdrop, false, &area) == B_OK) {
				fBackdrop = backdrop;
			} else {
				delete backdrop;
			}
		}
	}

	window->Lock();
	window->AddChild(fView);
	fView->MoveTo(0, 0);
	fView->ResizeTo(size.width, size.height);
	if (rootWindow != NULL) {
		window->AddToSubset(rootWindow);
	}
	fSubWindow = window;
	window->Show();
	window->Unlock();
}

void HaikuSurface::PlaceOwnWindow()
{
	HaikuSurface *root = fSubsurface != NULL ? fSubsurface->Root() : NULL;
	if (fSubWindow == NULL || root == NULL || root->View() == NULL || root->View()->Window() == NULL) {
		return;
	}
	int32_t ox = 0, oy = 0;
	fSubsurface->GetOffset(ox, oy);
	const BSize size = Size();

	BPoint screenPoint;
	{
		auto rootLocked = AppKitPtrs::LockedPtr(root->View());
		screenPoint = root->View()->ConvertToScreen(BPoint(ox, oy));
	}

	if (fSubWindow->Lock()) {
		fSubWindow->MoveTo(screenPoint.x, screenPoint.y);
		fSubWindow->ResizeTo(size.width, size.height);
		if (fView != NULL) {
			fView->MoveTo(0, 0);
			fView->ResizeTo(size.width, size.height);
		}
		if (Bitmap() == NULL) {
			if (!fSubWindow->IsHidden()) fSubWindow->Hide();
		} else if (fSubWindow->IsHidden()) {
			fSubWindow->Show();
		}
		fSubWindow->Unlock();
	}
}

void HaikuSurface::AttachWindow(BWindow *window)
{
	Assert(fView == NULL);

	fView = new WaylandView(this);
	window->AddChild(fView);
	fView->MakeFocus();
}

void HaikuSurface::AttachView(BView *view)
{
	if (view == NULL) {
		fprintf(stderr, "[!] HaikuSurface::AttachView(): view == NULL\n");
		return;
	}
	fView = new WaylandView(this);
	view->AddChild(fView);
}

void HaikuSurface::AttachViewsToEarlierSubsurfaces()
{
	if (fView == NULL) {
		fprintf(stderr, "[!] HaikuSurface::AttachViewsToEarlierSubsurfaces(): fView == NULL\n");
		return;
	}
	for (HaikuSubsurface *subsurface = SurfaceList().First(); subsurface != NULL; subsurface = SurfaceList().GetNext(subsurface)) {
		subsurface->Surface()->AttachView(fView);
	}
}

void HaikuSurface::Detach()
{
	if (fView == NULL) {
		return;
	}

	fView->LockLooper();
	BLooper *looper = fView->Looper();
	fView->RemoveDescendantsAndSelf();

	fView->fSurface = NULL;
	delete fView;
	fView = NULL;

	if (looper != NULL) {
		looper->Unlock();
	}

	if (fSubWindow != NULL) {
		BWindow *window = fSubWindow;
		fSubWindow = NULL;
		if (window->Lock()) {
			window->Quit();
		}
	}
	delete fBackdrop;
	fBackdrop = NULL;
}

void HaikuSurface::Invalidate()
{
	if (fView == NULL) {
		return;
	}
	const bool sync = !fState.frameCallbacks.IsEmpty() && VSyncWanted(Bitmap(), fSubsurface != NULL);
	if (sync) {
		fVSyncActive = true;
		BMessenger target;
		{
			auto viewLocked = AppKitPtrs::LockedPtr(fView);
			target = BMessenger(fView);
		}
		VSyncDispatcher::Instance().Request(this, target);
		return;
	}
	if (fVSyncActive) {
		fVSyncActive = false;
		VSyncDispatcher::Instance().Cancel(this);
	}
	InvalidateNow();
}

// Draw what was committed: at once, or when the vsync thread says so (called in the window's thread then)
void HaikuSurface::InvalidateNow()
{
	if (fView == NULL) {
		return;
	}
	auto viewLocked = AppKitPtrs::LockedPtr(fView);
	if (fSubsurface != NULL) {
		viewLocked->Invalidate();
	} else {
		viewLocked->Invalidate(&fDirty);
	}
	fDirty.MakeEmpty();
}

void HaikuSurface::CallFrameCallbacks()
{
	while (!fState.frameCallbacks.IsEmpty()) {
		FrameCallback *callback = fState.frameCallbacks.RemoveHead();
		callback->SendDone(system_time()/1000);
		callback->Destroy();
	}
}

void HaikuSurface::SetHook(Hook *hook)
{
	if (hook != NULL) {hook->fBase = this;}
	fHook.SetTo(hook);
}


void HaikuSurface::SetViewportSrc(float x, float y, float width, float height)
{
	fPendingState.viewportSrc = {x, y, width, height};
	fPendingFields |= (1 << fieldViewport);
}

void HaikuSurface::SetViewportDst(int32_t width, int32_t height)
{
	fPendingState.viewportDst = {width, height};
	fPendingFields |= (1 << fieldViewport);
}

void HaikuSurface::HandleAttach(struct wl_resource *buffer_resource, int32_t dx, int32_t dy)
{
	fPendingState.buffer = HaikuShmBuffer::FromResource(buffer_resource);
	fPendingState.dx = dx;
	fPendingState.dy = dy;
	fPendingFields |= (1 << fieldBuffer) | (1 << fieldOffset);
}

void HaikuSurface::HandleDamage(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fDirty.Include(BRect(x, y, x + width - 1, y + height - 1));
}

void HaikuSurface::HandleFrame(uint32_t callback_id)
{
	fPendingState.frameCallbacks.Insert(FrameCallback::Create(Client(), 1, callback_id));
	fPendingFields |= (1 << fieldFrameCallbacks);
}

void HaikuSurface::HandleSetOpaqueRegion(struct wl_resource *region_resource)
{
	if (region_resource == NULL) {
		fPendingState.opaqueRgn.reset();
	} else {
		fPendingState.opaqueRgn.emplace(HaikuRegion::FromResource(region_resource)->Region());
	}
	fPendingFields |= (1 << fieldOpaqueRgn);
}

void HaikuSurface::HandleSetInputRegion(struct wl_resource *region_resource)
{
	if (region_resource == NULL) {
		fPendingState.inputRgn.reset();
	} else {
		fPendingState.inputRgn.emplace(HaikuRegion::FromResource(region_resource)->Region());
	}
	fPendingFields |= (1 << fieldInputRgn);
}


// Debug aid: with WINE_HAIKU_DUMP_DIR set, touching <dir>/trigger makes every surface that commits in the next 3 seconds
// write its buffer as <dir>/<surface>-<w>x<h>[-sub].ppm (once per trigger), to see what a client really sent.
static void DumpSurfaceIfRequested(HaikuSurface *surface, BBitmap *bmp, bool isSub)
{
	static const char *dir = getenv("WINE_HAIKU_DUMP_DIR");
	static std::map<HaikuSurface*, time_t> done;
	if (dir == NULL || bmp == NULL) return;
	char path[1024];
	snprintf(path, sizeof(path), "%s/trigger", dir);
	struct stat st;
	if (stat(path, &st) != 0 || time(NULL) - st.st_mtime > 3) return;
	auto it = done.find(surface);
	if (it != done.end() && it->second == st.st_mtime) return;
	done[surface] = st.st_mtime;
	BRect b = bmp->Bounds();
	int w = (int)b.Width() + 1, h = (int)b.Height() + 1;
	snprintf(path, sizeof(path), "%s/%p-%dx%d%s.ppm", dir, (void*)surface, w, h, isSub ? "-sub" : "");
	FILE *f = fopen(path, "wb");
	if (f == NULL) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	const uint8 *bits = (const uint8*)bmp->Bits();
	for (int y = 0; y < h; y++) {
		const uint8 *row = bits + (size_t)y * bmp->BytesPerRow();
		for (int x = 0; x < w; x++) {
			uint8 rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4]};
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

void HaikuSurface::HandleCommit()
{
	//printf("HaikuSurface::HandleCommit()\n");

	for (;;) {
		uint32 field = std::countr_zero(fPendingFields);
		if (field >= 32) {
			break;
		}
		fPendingFields &= ~(1U << field);
		switch (field) {
			case fieldBuffer:
				if (fState.buffer != NULL && fState.buffer != fPendingState.buffer) {
					fState.buffer->SendRelease();
				}
				fState.buffer = fPendingState.buffer;
				break;
			case fieldOffset:
				fState.dx = fPendingState.dx;
				fState.dy = fPendingState.dy;
				break;
			case fieldTransform:
				fState.transform = fPendingState.transform;
				break;
			case fieldScale:
				fState.scale = fPendingState.scale;
				break;
			case fieldOpaqueRgn:
				fState.opaqueRgn = std::move(fPendingState.opaqueRgn);
				break;
			case fieldInputRgn:
				fState.inputRgn = std::move(fPendingState.inputRgn);
				break;
			case fieldFrameCallbacks:
				fState.frameCallbacks.TakeFrom(&fPendingState.frameCallbacks);
				break;
			case fieldViewport:
				fState.viewportSrc = fPendingState.viewportSrc;
				fState.viewportDst = fPendingState.viewportDst;
				break;
		}
	}

	if (fSubsurface != NULL && View() != NULL && View()->Window() != NULL && NeedsOwnWindow()) {
		MoveToOwnWindow();
	}
	if (fSubWindow != NULL && View() != NULL) {
		PlaceOwnWindow();
		Invalidate();
	} else if (View() != NULL && View()->Window() != NULL) {
		auto viewLocked = AppKitPtrs::LockedPtr(View());
		if (fSubsurface != NULL) {
			viewLocked->MoveTo(fSubsurface->GetState().x, fSubsurface->GetState().y);
			// A subsurface without a buffer is unmapped: hide its view so that the parent is redrawn where it was (an
			// invalidated transparent view leaves the old pixels on the screen, and Steam's pages that were switched
			// away from showed through).
			if (fState.buffer == NULL && !viewLocked->IsHidden(viewLocked)) viewLocked->Hide();
			else if (fState.buffer != NULL && viewLocked->IsHidden(viewLocked)) viewLocked->Show();
		}
		BSize size = Size();
		viewLocked->ResizeTo(size.width, size.height);
		Invalidate();
	}
	DumpSurfaceIfRequested(this, Bitmap(), fSubsurface != NULL);
	if (fHook.IsSet()) {
		fHook->HandleCommit();
	}
}

void HaikuSurface::HandleSetBufferTransform(int32_t transform)
{
	fPendingState.transform = transform;
	fPendingFields |= (1 << fieldTransform);
}

void HaikuSurface::HandleSetBufferScale(int32_t scale)
{
	fPendingState.scale = scale;
	fPendingFields |= (1 << fieldScale);
}

void HaikuSurface::HandleDamageBuffer(int32_t x, int32_t y, int32_t width, int32_t height)
{
	width = std::min(width, 1 << 24);
	height = std::min(height, 1 << 24);
	fDirty.Include(BRect(x, y, x + width - 1, y + height - 1));
}

void HaikuSurface::HandleOffset(int32_t x, int32_t y)
{
	fPendingState.dx = x;
	fPendingState.dy = y;
	fPendingFields |= (1 << fieldOffset);
}


// The user's colours of Haiku's interface, for the system colours of Wine (winewayland.drv turns them into the "Control Panel\\Colors" of the
// prefix): out[i] = 0xRRGGBB of the i-th entry of the list below, in this order. Returns how many were filled in.
extern "C" __attribute__((visibility("default"))) int haiku_get_ui_colors(uint32_t *out, int count)
{
	static const color_which which[] = {
		B_PANEL_BACKGROUND_COLOR, B_PANEL_TEXT_COLOR, B_DOCUMENT_BACKGROUND_COLOR, B_DOCUMENT_TEXT_COLOR,
		B_CONTROL_BACKGROUND_COLOR, B_CONTROL_TEXT_COLOR, B_CONTROL_BORDER_COLOR, B_CONTROL_HIGHLIGHT_COLOR,
		B_MENU_BACKGROUND_COLOR, B_MENU_ITEM_TEXT_COLOR, B_MENU_SELECTED_BACKGROUND_COLOR, B_MENU_SELECTED_ITEM_TEXT_COLOR,
		B_LIST_SELECTED_BACKGROUND_COLOR, B_LIST_SELECTED_ITEM_TEXT_COLOR,
		B_TOOL_TIP_BACKGROUND_COLOR, B_TOOL_TIP_TEXT_COLOR,
		B_WINDOW_TAB_COLOR, B_WINDOW_TEXT_COLOR, B_WINDOW_INACTIVE_TAB_COLOR, B_WINDOW_INACTIVE_TEXT_COLOR,
		B_SHINE_COLOR, B_SHADOW_COLOR, B_NAVIGATION_BASE_COLOR,
	};
	const int n = (int)(sizeof(which) / sizeof(which[0]));
	int i;
	for (i = 0; i < n && i < count; i++) {
		rgb_color c = ui_color(which[i]);
		out[i] = ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
	}
	return i;
}
