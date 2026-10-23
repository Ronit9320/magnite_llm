#!/usr/bin/env bash
# Build build/packaging/macos/magnite.icns from the brand symbol.
#
# app/resources/brand/symbol.svg is not a standalone SVG: its fills are @tokens
# that the theme substitutes at runtime (rasterized as-is it renders solid black).
# So this script resolves them from app/src/theme.h — the single source of truth,
# no colour is duplicated here — and places the symbol on a rounded paper tile,
# because macOS does not mask app icons for you.
#
# Needs only macOS tools (qlmanage, sips, iconutil) and the system python3 (stdlib).
# Idempotent. Usage: bash packaging/macos/make_icon.sh [output.icns]
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-$repo_dir/build/packaging/macos/magnite.icns}"
work="$repo_dir/build/packaging/macos/icon"
rm -rf "$work"; mkdir -p "$work" "$(dirname "$out")"

for tool in qlmanage sips iconutil python3; do
    command -v "$tool" >/dev/null || { echo "make_icon.sh: missing $tool" >&2; exit 2; }
done

python3 - "$repo_dir" "$work/icon.svg" <<'PY'
import re, sys
from pathlib import Path
repo, out = Path(sys.argv[1]), Path(sys.argv[2])
theme = (repo / "app/src/theme.h").read_text()

def token(name):
    m = re.search(rf'inline QString {name}\(\)[^\n]*', theme)
    hexes = re.findall(r'#[0-9A-Fa-f]{6}', m.group(0)) if m else []
    if not hexes:
        sys.exit(f"make_icon.sh: cannot read {name}() from theme.h")
    return hexes[-1]          # for dark?light ternaries the last value is the light theme

ink, orange, paper = token("brandInk"), token("brandOrange"), token("canvasBackground")
symbol = (repo / "app/resources/brand/symbol.svg").read_text()
symbol = symbol.replace("@brandInk", ink).replace("@brandOrange", orange)
unresolved = set(re.findall(r'@\w+', symbol))
if unresolved:
    sys.exit(f"make_icon.sh: unresolved tokens in symbol.svg: {sorted(unresolved)}")
view = re.search(r'viewBox="([^"]+)"', symbol).group(1)
body = re.search(r'<svg[^>]*>(.*)</svg>', symbol, re.S).group(1)

# Apple icon grid: an 824-px rounded square centred on a 1024 canvas (transparent margin).
size, margin, radius, glyph = 824, 100, 185, 560
pos = (1024 - glyph) // 2
out.write_text(f'''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
<rect x="{margin}" y="{margin}" width="{size}" height="{size}" rx="{radius}" fill="{paper}"/>
<svg x="{pos}" y="{pos}" width="{glyph}" height="{glyph}" viewBox="{view}">{body}</svg>
</svg>
''')
print(f"tokens: ink={ink} orange={orange} paper={paper}")
PY

qlmanage -t -s 1024 -o "$work" "$work/icon.svg" >/dev/null 2>&1 || true
master="$work/icon.svg.png"
[[ -s "$master" ]] || { echo "make_icon.sh: qlmanage produced no PNG" >&2; exit 1; }
[[ "$(sips -g pixelWidth "$master" | awk '/pixelWidth/{print $2}')" == 1024 ]] \
    || { echo "make_icon.sh: master is not 1024 px" >&2; exit 1; }
[[ "$(sips -g hasAlpha "$master" | awk '/hasAlpha/{print $2}')" == yes ]] \
    || { echo "make_icon.sh: master lost its alpha channel" >&2; exit 1; }

iconset="$work/magnite.iconset"; mkdir -p "$iconset"
for base in 16 32 128 256 512; do
    sips -z "$base" "$base" "$master" --out "$iconset/icon_${base}x${base}.png" >/dev/null
    sips -z "$((base * 2))" "$((base * 2))" "$master" --out "$iconset/icon_${base}x${base}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$out"
echo "wrote $out ($(stat -f %z "$out") bytes)"
