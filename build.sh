#!/bin/bash
# Builds GW2WvWPresence.dll from Linux/macOS/Windows with Zig as the cross compiler:
#   pip install ziglang    (or install zig and replace "python3 -m ziglang" with "zig")
#   ./build.sh
set -e
cd "$(dirname "$0")"
Z="python3 -m ziglang"
FLAGS="-target x86_64-windows-gnu -O2 -std=c++17 -DNDEBUG -Ithird_party -Ithird_party/imgui -Wno-everything"
mkdir -p build/obj
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  [ build/obj/$f.o -nt third_party/imgui/$f.cpp ] || $Z c++ $FLAGS -c third_party/imgui/$f.cpp -o build/obj/$f.o &
done
wait
$Z c++ $FLAGS -shared -s -o build/GW2WvWPresence.dll src/presence.cpp build/obj/*.o -luser32 -lwinhttp -lcrypt32
ls -la build/GW2WvWPresence.dll
