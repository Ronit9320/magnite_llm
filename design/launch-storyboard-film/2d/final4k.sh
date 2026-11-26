#!/usr/bin/env bash
# Render every shot at 3840x2160 and encode the 60 s master. Frames go to renders_4k/<shot>/.
set -uo pipefail
cd "$(dirname "$0")"
SHOTS="S01 S02 S03 S04 S05 S06 S07 S08 S09 S10 S11 S12 S13 S14"
for s in $SHOTS; do
  rm -rf "renders_4k/$s"
  start=$(date +%s)
  blender -b film2d.blend -S "$s" -P scripts/set4k.py -a 2>&1 | grep -E "Error" | tail -n 3
  echo "$s: $(ls renders_4k/$s | wc -l) frames in $(( $(date +%s) - start )) s"
done
list=$(mktemp)
for s in $SHOTS; do echo "file '$PWD/renders_4k/$s/%04d.png'" > /dev/null; done
# one continuous image sequence in shot order, then encode once
seq=renders_4k/_seq; rm -rf "$seq"; mkdir -p "$seq"; n=0
for s in $SHOTS; do for f in renders_4k/$s/*.png; do n=$((n+1)); ln -s "$PWD/$f" "$seq/$(printf %05d $n).png"; done; done
ffmpeg -v error -y -framerate 30 -i "$seq/%05d.png" -c:v libx264 -preset slow -crf 14 -pix_fmt yuv420p -tag:v avc1 -movflags +faststart renders/magnite_launch_2d_4k.mp4
rm -rf "$seq" "$list"
echo "frames: $n"; ls -la renders/magnite_launch_2d_4k.mp4
