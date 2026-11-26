#!/bin/sh
# Builds libwinehglshim.so, the bridge to Haiku's BGLView (see hglshim.cpp). Needs g++, libbe and libGL; run it on Haiku.
set -e
cd "$(dirname "$0")"
g++ -O2 -fPIC -shared -o libwinehglshim.so hglshim.cpp -lbe -lGL
echo built libwinehglshim.so
# the tray bridge: the library Wine's driver loads and the item it puts into the Deskbar
g++ -O2 -fPIC -shared -fvisibility=hidden -o libwinehaikutray.so haikutray.cpp -lbe -ltranslation
g++ -O2 -o winetrayitem trayitem.cpp -lbe
rc -o trayitem.rsrc trayitem.rdef
xres -o winetrayitem trayitem.rsrc
mimeset -f winetrayitem
rm trayitem.rsrc
echo built libwinehaikutray.so winetrayitem
