#!/bin/sh
# Builds Tracy's viewer (tracy-profiler), capture tool (tracy-capture) and zone statistics exporter (tracy-csvexport) from the Tracy sources xprofile uses - the same version as
# the client, which is what a viewer must match. Their CMake downloads their own libraries (CPM: glfw or wayland, capstone, freetype,
# ...) into a cache next to the output, once. Linux needs the viewer's system packages (Tracy's manual: e.g. libdbus-1-dev, libxkbcommon-dev,
# libwayland-dev or the X11/glfw ones, libtbb-dev).
#   BuildTracyTools.sh <tracy source folder> <output folder>
# The executables: <output>/profiler/tracy-profiler, <output>/capture/tracy-capture, <output>/csvexport/tracy-csvexport
set -e
if [ -z "$2" ]; then echo "usage: BuildTracyTools.sh <tracy source folder> <output folder>"; exit 1; fi
TRACY=$(cd "$1" && pwd)
mkdir -p "$2"; OUT=$(cd "$2" && pwd)
export CPM_SOURCE_CACHE="$OUT/cpm"
for T in capture csvexport profiler; do
    cmake -S "$TRACY/$T" -B "$OUT/$T" -DCMAKE_BUILD_TYPE=Release -DNO_CCACHE=ON
    cmake --build "$OUT/$T" --config Release --parallel
done
echo "Tracy tools built in $OUT"
