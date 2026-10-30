#!/bin/sh
# Builds libwinehaikushim.so, the bridge to BSoundPlayer (see haikushim.cpp). Needs g++ and libbe; run it on Haiku.
set -e
cd "$(dirname "$0")"
g++ -O2 -fPIC -shared -o libwinehaikushim.so haikushim.cpp -lbe -lmedia
echo built libwinehaikushim.so
