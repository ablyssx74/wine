# Wine on Haiku

This is Wine 11.20 with a Haiku port: windows are drawn through Haiku's Wayland compositor, sound goes through `BSoundPlayer`,
OpenGL through `BGLView`, and Vulkan (DXVK) through the NVK driver of Nebula. Both 64-bit and 32-bit (WoW64) programs run.

## Requirements

- Haiku x86_64, a recent nightly (hrev60299 or newer was tested), with the `wayland_server` compositor running.
- For 3D: an NVIDIA GPU with the **Nebula** driver package installed. It brings the NVK Vulkan driver and Zink OpenGL.
  Without it, 64-bit programs still run and OpenGL uses Haiku's software renderer; DXVK needs Nebula.
- For 32-bit programs: the `wine_compat` kernel add-on (see below).

## Install

Install `wine11-11.20-32-x86_64.hpkg` for the whole system, then make a prefix:

    wineboot -u
    wineserver -w

`wineboot` returns before it has finished; `wineserver -w` waits until the prefix is complete. Do this once, and before any
32-bit program.

### 32-bit programs (`wine_compat`)

Haiku cannot run 32-bit code in a 64-bit process on a stock kernel. `wine_compat` is a small kernel add-on that makes it possible.
It is **not part of the package**: unzip `wine_compat-ds-es.zip` and follow its `INSTALL.txt`. Check with
`grep wine_compat /var/log/syslog | tail -5` ("patched the call at ..." means it is active). Without the add-on a 32-bit program
stops with a message and 64-bit programs are not affected.

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

The portable Firefox works. In its profile's `user.js`, to use the GPU for rendering:

    user_pref("gfx.webrender.all", true);
    user_pref("gfx.webrender.software", false);
    user_pref("gfx.webrender.compositor", false);
    user_pref("layers.acceleration.force-enabled", true);
    user_pref("layers.gpu-process.enabled", false);

Keep `browser.tabs.remote.autostart` set to `false`. The standard Firefox install shows a blank page unless
`MOZ_DISABLE_CONTENT_SANDBOX=1` is set, video with DRM does not play, and `about:support` crashes it.

## Known problems

- Programs that need Wine's sandbox features (Firefox's content and DRM processes, Steam's web helper) do not start.
- 32-bit programs need the `wine_compat` add-on, which was tested on hrev60299 only.
