# Wine on Haiku

This is Wine 11.20 with a Haiku port: windows are drawn through Haiku's Wayland compositor, sound goes through `BSoundPlayer`,
OpenGL through `BGLView`, and Vulkan (DXVK) through the NVK driver of Nebula. Both 64-bit and 32-bit (WoW64) programs run.

## What's new in 11.20-44

Steam's window resizes from every corner, and Wine no longer opens a separate tray window.

- **Resizing from every edge and corner.** Haiku's compositor only followed the bottom-right corner (and its top-left code moved the window by a
  delta that added up), so Steam's window, which draws handles on all four corners, only resized from the bottom-right one. The size now follows the
  pointer for all eight edges, and the left and top edges also move the window so that the opposite edge stays where it was.
- **No separate tray window.** Wine's explorer showed the tray icons (Steam's, for one) in a window of its own. New prefixes get
  `HKCU\Software\Wine\Explorer\ShowSystray = 0`; for an existing prefix run `wineboot -u`. Set the value to 1 to get the window back. (The icons of
  other programs are hidden too.)

Note: clicking the X button of Steam's window does not quit Steam. As on Windows, it minimizes Steam to the tray and keeps it running, with no
window; use Steam > Exit to quit it. The overlay problem described in 11.20-43 is still there.

## What's new in 11.20-43

Steam no longer hangs at start-up or locks up when you click its menus, and its menus open where they should.

- **Steam's browser no longer hangs at start-up.** Haiku's `posix_spawn()` with file actions forks and then calls `malloc()` in the child; when
  another thread of the parent was inside `malloc()` at that moment, the child waited for ever for a lock nobody could release, and the thread
  that was starting it waited for the child (no update window, one CPU at 100%, menus and games queued behind it). Wine now starts its loader without
  file actions, which Haiku does not fork for, and the loader does what the actions did (standard input and output, directory, new session) as its
  first step.
- **Steam's menus do not lock it up.** A flush of a window's picture asked Windows for the window stacking while it held the picture's lock, and
  `SetParent()` (which a menu click does) holds the other lock and wants the picture's: two threads waited for each other. The stacking is now read first.
- **Menus open under their titles and stay open.** A menu is an undecorated popup that Steam activates; an activated window became a top-level window,
  which Haiku's compositor places in the middle of the screen while Wine believed it was where it had asked, so the pointer position Wine worked out for
  the menu was wrong and Steam closed it as the mouse entered it. An undecorated popup with an owner is now drawn inside its owner, where it was asked to be.

Known problem: with the Steam overlay turned on, Steam keeps showing a game it started as running after the game has quit (the game and the overlay
process have ended; the cause is in Steam, not found yet). Turn the overlay off in Steam's settings (In Game) as a workaround.

## What's new in 11.20-42

Unreal Tournament 3 comes up full-screen, and games lag much less with Mesa's shader cache on.

- **Unreal Tournament 3 starts at full size.** When it lost activation at start-up (Steam's window or its own splash took the focus) it minimized
  itself, and the compositor's reply to that, a configure for the old full-screen size, was applied to the minimized window. The window moved
  back to full size but stayed minimized as far as the game knew, so it never got a real size again and drew into a 160x31 strip. A configure that
  arrives right after the minimize is now ignored, and the one the compositor sends when you click the window's tab restores it.
- **Mesa's shader cache is much smoother.** Its default stores one file per shader, and on Haiku's file system that made Unreal Tournament 4 lag
  all the time. The `wine` loader now selects Mesa's single-file cache (`MESA_DISK_CACHE_SINGLE_FILE=true`), which was the smoothest of the three
  kinds in tests (single file, database `MESA_DISK_CACHE_DATABASE=true`, one file per shader). Set any of those variables yourself to choose
  another, or `MESA_SHADER_CACHE_DISABLE=true` for no cache at all. A new cache still costs some stutter while it fills during the first
  hours of play.

## What's new in 11.20-41

Steam starts reliably, 32-bit DirectX games draw again, Unreal Tournament 3 gets past its start-up, and Mesa's shader cache finally
stores what the games compile.

- **Steam starts reliably.** About every other start its browser helper hung (no window, "RetryCreateBrowser" in the logs). Cause: Chromium's
  profiler suspends other threads all the time, and Wine's suspend (a signal) can park a thread inside `dlopen()` while it holds Haiku's
  runtime-loader lock; the thread that starts the helper processes then waits for that lock for ever. The `wine` loader now blocks the suspend
  signal while `dlopen()` runs. 8 starts in a row: all 8 up in 12 seconds.
- **Programs that look up host names no longer crash.** Wine's `gethostbyname()` crashed on Haiku (it used a `gethostbyname_r()` that Haiku does not
  have in that form), so a program that asks again at once looped for ever. Unreal Tournament 3 did this at start-up (about 240 lookups a second,
  a white window). It now starts, and shows its screens at full size.
- **32-bit DXVK games draw.** NVK on Haiku ignores the address that a 32-bit program's memory mapping asks for, so the game wrote into pages the
  GPU never saw and every frame was black. The mapping is now moved to the requested address.
- **Mesa's shader cache works.** On Haiku's file system Mesa could not set up its disk cache (`posix_fallocate()` is not supported there and Mesa
  gives up), so nothing was ever stored and every shader was compiled again at every start, which showed as pauses of seconds. The `wine` loader now
  provides a `posix_fallocate()` that falls back to `ftruncate()`. A first start of a big game still compiles for a while (UT3: about 10,000 shaders,
  31 MB stored); later starts reuse them. The real fix belongs into Nebula's Mesa: `dxvk-tools.zip` has the small patch
  (`mesa-disk-cache-posix-fallocate.patch`); with it the loader's version does nothing.
- **`wine steam.exe` starts Steam with the switches it needs** (`-cef-disable-gpu -cef-disable-gpu-compositing -no-cef-sandbox`), unless the command
  line has them or `WINE_HAIKU_STEAM_GPU` / `WINE_HAIKU_STEAM_SANDBOX` is set. A buffer overflow in building that command line, which could crash
  Steam, is fixed.
- **Restoring a window that was saved maximized** (Steam) gives a window of 3/4 of the work area instead of the whole screen.
- Debug aids: `WINE_HAIKU_FRAME_GAP=<ms>` logs pauses between presented frames, `WINE_HAIKU_VK_DEBUG=1` the first Vulkan calls, and
  `WINE_HAIKU_GL_DEBUG=1` now also counts the pixels of presented frames.

Wine does not switch the display mode: set the fullscreen resolution of a game to your screen's own in its settings file, or it shows a small
picture in a white window. Unreal Tournament 2004: `FullscreenViewportX/Y` in `System/UT2004.ini`. Unreal Tournament 3: `ResX`/`ResY` in the
`[SystemSettings]` section of `Documents/My Games/Unreal Tournament 3/UTGame/Config/UTEngine.ini` (the file has Windows line endings).

## What's new in 11.20-40

11.20-39 could not start 32-bit Windows programs (the error was "ShellExecuteEx failed: File not found", or "Missing game executable" in
Steam). Install 11.20-40 if you installed 11.20-39.

- **32-bit programs start again.** The new way of starting programs (`posix_spawn()`) asked for a separate 32-bit loader that a WoW64
  build does not have, and gave up. It now falls back to the normal loader, as the old code did.
- **Steam's Library page shows.** Steam keeps the page it left in a window that it hides; Wine put the picture of that hidden window over
  the page that replaced it, so the Library never appeared under the old Store page. Hidden windows are no longer painted.
- **Sockets** receive and send without blocking (`MSG_DONTWAIT`), as the build that ran Steam reliably did.
- **Maximize and restore:** the compositor no longer returns a window to a 1x1 or full-screen-sized frame that it had recorded when a
  window was created full-screen (Steam opens maximized from its saved settings). This stops a loop of 60 state changes a second, but
  Steam's own restore still stays at full size, because the size it restores to is the whole screen. Resizing it smaller does not work yet.
- Debug aids: `WINE_HAIKU_WIN_TRACE=1` prints the window state requests and replies of the compositor, and `WINE_HAIKU_DUMP_DIR=<dir>` (then
  `touch <dir>/trigger`) saves what each window surface has committed.

## What's new in 11.20-39

Steam now starts, signs in and shows the store, the library and your profile. Windows apps get smoother, and several network and font
problems that broke Steam and the browsers are fixed.

- **Steam signs in.** The network fixes below remove the "Unexpected Transport Error" and the failing sign-in polling; QR-code sign-in works.
- **Steam draws better, but not perfectly.** Child surfaces are now stacked in the order the application asks for, and a page Steam switched
  away from no longer shows through behind the current one. The tab layout is still not right in places and some glitches remain.
- **Readable text.** The usual Windows font names (Arial, Tahoma, Segoe UI, Verdana, Times New Roman, Courier New, Consolas and others) fall
  back to Noto Sans / Noto Sans Mono, so text in Steam and other apps is no longer blank. Existing prefixes get this after `wineboot -u`.
- **Smoother window dragging**, close to a native window.
- **Child processes start without `fork()`.** On Haiku `fork()` copies page data in proportion to the reserved address space, which could use
  up all the memory of the machine when a program like Steam's browser started its helpers. Wine now uses `posix_spawn()`
  (`WINE_HAIKU_FORK_SPAWN=1` goes back to `fork()`).
- **Network:** UDP sockets no longer fail every send (this was a runaway kernel memory growth while Steam ran); `netstat` and the TCP
  connection table list the real connections; a crash in the network driver on interfaces without an address is fixed; a failing
  `IPV6_V6ONLY` query is answered with success; `getaddrinfo()` calls are serialized.
- **Browsers:** Chromium-based browsers (including Edge) start with `--no-sandbox` and `--disable-gpu`, hardware acceleration is off by default
  in Firefox and Chromium browsers, `WINE_HAIKU_NO_KQUEUE=1` selects the `poll()` loop in the Wine server, and a crash that read a damaged TEB
  page in the SIGSEGV handler is fixed.

## Downloads in this release

| file | what it is |
|---|---|
| `wine11-11.20-44-x86_64.hpkg` | Wine, install for the whole system |
| `wine_compat-ds-es.zip` | kernel add-on for 32-bit programs |
| `wine_int3.zip` | kernel add-on for programs that execute `int3` (Chromium, WebView2, the Firefox sandbox) |
| `dxvk-tools.zip` | DXVK 3.1.1 (64 and 32 bit), the install script, `vkprobe`, test programs, the optional VideoStreams layer build and the optional Mesa/layer patches (now with the shader cache fix) |
| `ut4-launcher.zip` | the UT4 Community Launcher 1.9.0 built to run under Wine (needs the WebView2 runtime and `wine_int3`; steps in its README) |

## Requirements

- Haiku x86_64, a recent nightly (hrev60299 or newer was tested), with the `wayland_server` compositor running.
- For 3D: an NVIDIA GPU with the **Nebula** driver package installed. It brings the NVK Vulkan driver and Zink OpenGL.
  Without it, 64-bit programs still run and OpenGL uses Haiku's software renderer; DXVK needs Nebula.
- For 32-bit programs: the `wine_compat` kernel add-on (see below).
- Optional: the `wine_int3` kernel add-on, for programs that execute `int3` (Chromium, WebView2, the Firefox sandbox; see below).

## Install

Install `wine11-11.20-44-x86_64.hpkg` for the whole system, then make a prefix:

    wineboot -u
    wineserver -w

`wineboot` returns before it has finished; `wineserver -w` waits until the prefix is complete. Do this once, and before any
32-bit program. Install the `wine_compat` add-on (below) **before** you make the prefix: without it the 32-bit part
of the prefix (`syswow64`) is not created, and 32-bit programs, Steam among them, do not start.

### 32-bit programs (`wine_compat`)

Haiku cannot run 32-bit code in a 64-bit process on a stock kernel. `wine_compat` is a small kernel add-on that makes it possible.
It is **not part of the package**: download `wine_compat-ds-es.zip` from the release, unzip it, and in a Terminal in that folder:

    wineserver -k
    mkdir -p ~/config/non-packaged/add-ons/kernel/drivers/bin
    mkdir -p ~/config/non-packaged/add-ons/kernel/drivers/dev/misc
    cp wine_compat ~/config/non-packaged/add-ons/kernel/drivers/bin/wine_compat
    ln -sf ~/config/non-packaged/add-ons/kernel/drivers/bin/wine_compat ~/config/non-packaged/add-ons/kernel/drivers/dev/misc/wine_compat

Check that it is active (the device can take a few seconds to appear, no reboot is needed):

    ls /dev/misc/wine_compat
    grep wine_compat /var/log/syslog | tail -5

"patched the call at ..." means it is active. To update an older `wine_compat`, or to remove it, run `wineserver -k`, remove
`~/config/non-packaged/add-ons/kernel/drivers/dev/misc/wine_compat` and `.../drivers/bin/wine_compat`, wait about 10 seconds, and
install again (`INSTALL.txt` in the zip has the exact commands). Without the add-on a 32-bit program stops with a message and 64-bit
programs are not affected.

### `int3` and `DebugBreak()` (`wine_int3`)

Haiku's kernel does not turn an `int3` into `SIGTRAP`, so Chromium, WebView2 and the Firefox sandbox die when they execute one. `wine_int3`
is a second small kernel add-on, only for Wine processes. Download `wine_int3.zip` from the release and install it the same way as
`wine_compat` (its `INSTALL.txt` has the commands). It is optional: without it a program that executes `int3` is killed.

## Graphics

| Direct3D / OpenGL | what Wine uses on Haiku |
|---|---|
| OpenGL | Haiku's `BGLView` (Nebula: Zink on NVK) |
| Direct3D 8/9/10/11 | Wine's own translation to OpenGL, or **DXVK** (Vulkan) if you install it in the prefix |
| Vulkan programs | the Vulkan loader with Nebula's NVK driver and the VideoStreams WSI layer |

### Checking that Vulkan works

`vkprobe` and `vkclear.exe` are in the `tests` folder of `dxvk-tools.zip`. `vkprobe` lists the devices and whether DXVK's needs are met; `vkclear.exe` opens a window with a red/blue picture, a moving
white bar and the frame rate (`wine vkclear.exe -t 0` runs until you close it). If `vkCreateInstance` fails with `-9`, the
Vulkan loader found no driver: install Nebula.

### DXVK

DXVK is opt-in, per prefix. It comes in its own download, `dxvk-tools.zip` (DXVK 3.1.1 for 64 and 32 bits, the install script, the
test programs and a README). Unzip it, open a Terminal in its folder, and:

    ./dxvk-install.sh create mygame            # a new prefix ~/.wine_mygame with DXVK in it
    ./dxvk-install.sh run -p ~/.wine_mygame game.exe

    ./dxvk-install.sh install -p ~/.wine     # or put it in an existing prefix (close its programs first)
    ./dxvk-install.sh uninstall -p ~/.wine   # takes it out again
    ./dxvk-install.sh status -p ~/.wine

Install also sets two variables in the prefix: `DXVK_CONFIG = dxvk.enableDescriptorBuffer = False; dxvk.numCompilerThreads = 0`
(NVK's descriptor buffer path fails for Unreal Engine 4) and `MESA_SHADER_CACHE_MAX_SIZE = 100G` (a stopgap for a Mesa shader cache
bug on Haiku that makes games hang at exit and in their first frames).

Only Direct3D 9 and 11 programs use DXVK; an OpenGL renderer does not.

### Environment switches

| variable | effect |
|---|---|
| `WINE_HAIKU_VK_VSYNC=0` | do not pace Vulkan frames to the display (for benchmarks) |
| `WINE_HAIKU_VK=wayland` | the Vulkan driver's own Wayland surface (does not work on Haiku yet) |
| `WINE_HAIKU_SYSTEM_WAYLAND=1` | use the system Wayland server instead of Wine's own copy |
| `WINE_HAIKU_GL_DEBUG=1` | print timing and diagnostics of the OpenGL and Vulkan bridge |

## Firefox

The standard Firefox install and the portable one both render pages, with Firefox's own sandbox on (a Haiku kernel bug that left the
sandboxed content processes with unreadable code was worked around in Wine, so `MOZ_DISABLE_CONTENT_SANDBOX=1` is no longer needed).
New prefixes get a policy that turns hardware acceleration off; for an existing prefix run `wineboot -u`.

To use the GPU for rendering anyway, put this in the profile's `user.js`:

    user_pref("gfx.webrender.all", true);
    user_pref("gfx.webrender.software", false);
    user_pref("gfx.webrender.compositor", false);
    user_pref("layers.acceleration.force-enabled", true);
    user_pref("layers.gpu-process.enabled", false);

Video with DRM does not play, and `about:support` crashed an earlier build (not rechecked).

## Known problems

- On one test PC the whole system sometimes froze for good, usually during Steam or game activity. The kernel log of several of these freezes
  ends with `NVRM: Assertion failed: _rmGpuAllocLockIsOwner()` from Nebula's kernel driver; the cause has not been found.
- There is no vertical-blank sync: Haiku's graphics driver for NVIDIA (Nebula) does not offer a retrace signal (`BScreen::WaitForRetrace()`
  answers "not supported"), so full-screen games can show a horizontal tear line. Wine's frame pacing follows the compositor, not the display.
- Wine does not change the display mode (see the note under 11.20-41).
- A first start of a big game compiles its shaders and can take minutes with a white or black window; Mesa's cache then keeps them.
- Steam runs with its GPU path turned off (`-cef-disable-gpu -cef-disable-gpu-compositing`) and some pages still draw with small glitches
  while they load. Steam streaming has not been fully tested yet.
- Programs that need Wine's sandbox features (Firefox's content and DRM processes) do not start.
- 32-bit programs need the `wine_compat` add-on. It depends on kernel internals and was tested on hrev60299 and hrev60382 (`wine_int3` on hrev60377); on another
  build it reports that it could not install its hook and Wine disables 32-bit programs instead of crashing.
