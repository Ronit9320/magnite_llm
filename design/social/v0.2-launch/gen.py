import re, sys
R, OUT = sys.argv[1], sys.argv[2]
ORANGE, INK, BG, CREAM, MUTED = "#FF4A0E", "#201411", "#09090B", "#F1EFE7", "#B6BAC2"
src = open(f"{R}/app/resources/brand/symbol.svg").read()
inner = re.search(r"<svg[^>]*>(.*)</svg>", src, re.S).group(1)
inner = inner.replace("@brandInk", INK).replace("@brandOrange", ORANGE)
# symbol viewBox 140..424 (284 units), centre 282
def symbol(cx, cy, size, ring=False):
    s = size / 284.0
    g = f'<g transform="translate({cx - 282*s:.2f},{cy - 282*s:.2f}) scale({s:.4f})">{inner}</g>'
    if ring:
        g = f'<circle cx="{cx}" cy="{cy}" r="{size*0.5*1.13:.1f}" fill="{INK}"/>' + g
    return g
MONO = 'font-family="JetBrains Mono" font-weight="500" letter-spacing="0.18em"'
HEAD = 'font-family="Space Grotesk" font-weight="600" letter-spacing="-0.035em"'
BODY = 'font-family="DM Sans" font-weight="500"'

# Instagram 1080x1350 — orange, continues the teaser carousel's orange slides
ig = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1080" height="1350" viewBox="0 0 1080 1350">
<rect width="1080" height="1350" fill="{ORANGE}"/>
<text x="72" y="100" {MONO} font-size="27" fill="{INK}">MAGNITE SPACE · v0.2 PREVIEW</text>
<text x="1008" y="100" {MONO} font-size="27" fill="{INK}" text-anchor="end">20.11</text>
<text x="60" y="400" {HEAD} font-size="270" fill="{INK}">Live</text>
<text x="60" y="640" {HEAD} font-size="270" fill="{INK}">tonight.</text>
<text x="72" y="770" {BODY} font-size="44" fill="{INK}">Train, run and chat with models</text>
<text x="72" y="826" {BODY} font-size="44" fill="{INK}">on your own machine.</text>
{symbol(860, 1150, 420, ring=True)}
<text x="72" y="1272" {MONO} font-size="27" fill="{INK}">LINK IN BIO</text>
<path d="M318 1263 H 378 M 362 1249 L 380 1263 L 362 1277" stroke="{INK}" stroke-width="3" fill="none"/>
</svg>'''

# X 1600x900 — dark, mirrors teaser slide 1
x = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1600" height="900" viewBox="0 0 1600 900">
<rect width="1600" height="900" fill="{BG}"/>
<circle cx="88" cy="91" r="8" fill="{ORANGE}"/>
<text x="110" y="100" {MONO} font-size="26" fill="{ORANGE}">MAGNITE SPACE · 20 NOVEMBER</text>
<text x="1528" y="100" {MONO} font-size="26" fill="{MUTED}" text-anchor="end">v0.2 PREVIEW</text>
<text x="66" y="430" {HEAD} font-size="210" fill="{CREAM}">Launching</text>
<text x="66" y="620" {HEAD} font-size="210" fill="{ORANGE}">tonight.</text>
<text x="80" y="720" {BODY} font-size="42" fill="{CREAM}">Intelligence on your terms.</text>
<text x="80" y="826" {MONO} font-size="26" fill="{ORANGE}">LOCAL · TRAIN · RUN · CHAT</text>
{symbol(1420, 760, 620)}
</svg>'''
for name, svg in (("instagram-1080x1350", ig), ("x-1600x900", x)):
    open(f"{OUT}/{name}.svg", "w").write(svg)
