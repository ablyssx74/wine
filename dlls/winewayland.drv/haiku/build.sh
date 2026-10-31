#!/bin/sh
# Builds libwinehglshim.so, the bridge to Haiku's BGLView (see hglshim.cpp). Needs g++, libbe and libGL; run it on Haiku.
set -e
cd "$(dirname "$0")"
g++ -O2 -fPIC -shared -o libwinehglshim.so hglshim.cpp -lbe -lGL
echo built libwinehglshim.so
