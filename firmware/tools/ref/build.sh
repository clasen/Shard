#!/bin/zsh
# build.sh <out dir>: builds the NS2 comparison tools (outside the repo) and copies rec.sh next to them.
#   midid   virtual MIDI source that plays seq.h to NS2      arec    records a Core Audio input device
#   render  renders a content-image preset with the port      cmpref  compares an NS2 take with a render
set -e
D=${0:a:h}; O=${1:?out dir}
mkdir -p "$O"
clang -O2 "$D/midid.c" -I"$D" -framework CoreMIDI -framework CoreFoundation -o "$O/midid"
clang -O2 "$D/arec.c" -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -o "$O/arec"
clang++ -std=c++17 -O2 -w -I"$D" -I"$D/host" -I"$D/../../obsidian_esp32" "$D/render.cpp" -o "$O/render"
clang++ -std=c++17 -O2 "$D/cmpref.cpp" -o "$O/cmpref"
cp "$D/rec.sh" "$O/rec.sh"
