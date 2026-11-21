#!/usr/bin/env bash
# build <batch>              rebuild a batch's scenes into film2d.blend
# render <S01> [S02 ...]     render shots to renders/<shot>/####.png
# still <S01> <frame>        render one frame to renders/stills/
# preview <name> <S01> ...   encode shots into renders/<name>.mp4 plus a contact sheet
set -euo pipefail
cd "$(dirname "$0")"
BLEND=film2d.blend
cmd=$1; shift
case "$cmd" in
  build)
    if [ -f "$BLEND" ]; then blender -b "$BLEND" -P scripts/build.py -- "$1"
    else blender -b --factory-startup -P scripts/build.py -- "$1"; fi ;;
  render)
    for s in "$@"; do rm -rf "renders/$s"; blender -b "$BLEND" -S "$s" -a 2>&1 | grep -E "Saved|Error" | tail -n 1 || true; done ;;
  still)
    mkdir -p renders/stills
    blender -b "$BLEND" -S "$1" -o "//renders/stills/$1_####" -F PNG -f "$2" 2>&1 | grep -E "Saved|Error" ;;
  preview)
    name=$1; shift
    list=$(mktemp)
    for s in "$@"; do
      ffmpeg -v error -y -framerate 30 -i "renders/$s/%04d.png" -c:v libx264 -pix_fmt yuv420p -crf 18 "renders/$s.mp4"
      echo "file '$PWD/renders/$s.mp4'" >> "$list"
    done
    ffmpeg -v error -y -f concat -safe 0 -i "$list" -c copy "renders/$name.mp4"
    ffmpeg -v error -y -i "renders/$name.mp4" -vf "fps=2,scale=384:-1,tile=6x$(( ( $(ls renders/$1 | wc -l) * $# / 15 + 5 ) / 6 ))" -frames:v 1 "renders/${name}_contact.jpg" || true
    rm -f "$list"; ls -la "renders/$name.mp4" ;;
esac
