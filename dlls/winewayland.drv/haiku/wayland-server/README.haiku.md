# Haiku in-process Wayland server (HaiWay), vendored for Guitarix

Source: https://github.com/X547/wayland-server, commit 8ede02e ("implement drag-and-drop and image clipboard
integration", 2026-08-04). MIT licensed (see License.md).

Why it is here: the `wayland_server` package in HaikuPorts (0.1.20251004) predates drag and drop, so GTK 3 drags
(Guitarix's rack) start but never complete. Guitarix loads this newer copy at startup (see ../haiku_wayland.cpp),
from `lib/guitarix/wayland-server-inproc.so`; the system's copy is not touched.

Changes against upstream (kept as small as possible):
- `HaikuDataOffer::HandleSetActions` sends the action events only when the chosen action changes. Without this GTK
  answered every acknowledgement with another motion event, thousands a second during a drag.

Build: `meson setup build && ninja -C build` (needs the wayland, wayland_server_devel? no: wayland_devel and
wayland_protocols packages); src/haiku/make-package.sh does it.
