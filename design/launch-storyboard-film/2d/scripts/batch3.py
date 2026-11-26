"""Batch 3 - Act II, chapter 02 "Bring your documents" (0:25-0:34): S07 pages shred into the disc, S08 knowledge footage.
Pages are paper, not orange; they only turn orange once they are shredded into the logo's dots."""
import math, random
from kit import *
from batch2 import glass_panel, panel_anim, ken_burns, light_hole, caption, LIT_REST

PAGE_W, PAGE_H = 0.78, 1.0
BAR = hexlin("#A7A49B")


# ------------------------------------------------------------------ S07
def s07():
    N = 120
    sc, coll, cam = new_shot("S07", N)
    rng = random.Random(707)
    sd = symbol_data()
    holes = sd["holes"]

    # the disc lies low in frame; the camera orbits (disc turns) and rises (disc opens up)
    d = build_disc(coll, "disc", 0.0, lights=True)
    dv = disc_anim(d, N, lambda f: dict(x=0.0, y=-2.1, size=4.4, rot=lerp(8, 34, ease_io(f / (N - 1))),
                                        squash=lerp(0.30, 0.40, smooth(f / (N - 1))), thick=0.16))
    d["centre_light"].color = with_alpha(LIT_REST, 1.0)
    d["centre_glow"].color = hexlin(GLOW, 0.6)
    fill = lambda f: smooth(lin01(f, 40, 108))
    fg = glow("floorglow", 13, coll, hexlin(GLOW, 0.3), -0.5, loc=(0, -2.2), sy=0.35)
    animate(fg, N, lambda f: {"color": hexlin(GLOW, 0.26 + 0.24 * fill(f))})

    # vortex: centre above the disc, tightening and dropping as it feeds the disc
    VX, VY = 0.0, 1.0
    def vortex_c(f):
        return VX + 0.25 * math.sin(f / 23), VY - 0.6 * smooth(lin01(f, 30, 110))

    # faint wind arcs so the spiral reads before the pages start to tear
    for k in range(5):
        a0, r0, ln = rng.uniform(0, 6.28), rng.uniform(1.2, 3.6), rng.uniform(1.4, 2.4)
        arc = nurbs(f"wind{k}", [(0, 0)] * 7, coll, hexlin(PAPER, 0.0), 0.022, 0.8)
        def pts(f, a0=a0, r0=r0, ln=ln):
            cx, cy = vortex_c(f)
            w = a0 + f * 0.085 * (1 + 0.6 * lin01(f, 20, 90))
            r = r0 * lerp(1.0, 0.55, smooth(lin01(f, 20, 110)))
            return [(cx + r * (1 - 0.05 * j) * math.cos(w - ln * j / 6),
                     cy + 0.36 * r * (1 - 0.05 * j) * math.sin(w - ln * j / 6)) for j in range(7)]
        animate_points(arc, N, pts)
        animate(arc, N, lambda f: {"color": hexlin(PAPER, 0.07 * smooth(lin01(f, 0, 14)) * (1 - smooth(lin01(f, 92, 112))))})

    # pages ---------------------------------------------------------
    NP = 16
    pages = []
    for i in range(NP):
        root = empty(f"page{i:02d}", coll, (0, 0, 2.0))
        rect(f"page{i:02d}_p", PAGE_W, PAGE_H, coll, hexlin(PAPER, 0.96), 0, parent=root)
        for j in range(7):                                   # abstract text bars, no words
            w = PAGE_W * (0.72 if j % 3 else 0.48) * rng.uniform(0.8, 1.0)
            rect(f"page{i:02d}_b{j}", w, 0.035, coll, hexlin("#A7A49B"), 0.001, parent=root,
                 loc=(-PAGE_W * 0.36 + w / 2, PAGE_H * 0.33 - j * 0.1))
        a0 = 2 * math.pi * i / NP + rng.uniform(-0.2, 0.2)
        r0 = rng.uniform(2.6, 4.6)
        ts = int(lerp(34, 74, i / (NP - 1)) + rng.uniform(-3, 3))   # shred frame
        ph, spin = rng.uniform(0, 6.28), rng.uniform(0.12, 0.22)
        pages.append(dict(root=root, a0=a0, r0=r0, ts=ts, ph=ph, spin=spin, sz=rng.uniform(0.85, 1.1)))

    def page_state(p, f):
        cx, cy = vortex_c(f)
        t = f / 30
        w = p["a0"] + t * (1.3 + 1.8 * ease_in(lin01(f, 0, p["ts"]), 2))       # spins faster as it's pulled in
        r = p["r0"] * lerp(1.0, 0.42, ease_in(lin01(f, 0, p["ts"]), 1.6))
        x = cx + r * math.cos(w)
        y = cy + 0.36 * r * math.sin(w) + 0.25 * math.sin(t * 2 + p["ph"])
        return x, y, w

    for p in pages:
        def fn(f, p=p):
            x, y, w = page_state(p, f)
            flip = abs(math.cos(p["ph"] + f * p["spin"]))
            gone = lin01(f, p["ts"], p["ts"] + 4)
            s = p["sz"] * (1 - 0.15 * gone)
            return {"location": (x, y, 2.0 - 0.5 * math.sin(w)),     # near side draws over far side
                    "rotation_euler": (0, 0, w + math.pi / 2 + 0.6 * math.sin(p["ph"] + f * 0.07)),
                    "scale": (s * (0.25 + 0.75 * flip) if gone < 1 else 1e-4, s if gone < 1 else 1e-4, 1)}
        animate(p["root"], N, fn)
        for ch in p["root"].children:
            base = ch.color[:]
            animate(ch, N, lambda f, p=p, base=base: {"color": (base[0], base[1], base[2],
                                                                 base[3] * (1 - lin01(f, p["ts"], p["ts"] + 4)))})

    # dots: every page shreds into ~18 logo dots that rain into the holes ------------
    order = list(range(len(holes)))
    rng.shuffle(order)
    first_land = {}
    k = 0
    for pi, p in enumerate(pages):
        for j in range(18):
            hi = order[k % len(order)]; k += 1
            hx, hy, hr = holes[hi]
            # starting point on the page's surface at the shred frame
            px, py, pw = page_state(p, p["ts"])
            u, v = rng.uniform(-0.4, 0.4) * PAGE_W, rng.uniform(-0.45, 0.45) * PAGE_H
            ca, sa = math.cos(pw + math.pi / 2), math.sin(pw + math.pi / 2)
            sx, sy = px + u * ca * 0.6 - v * sa, py + u * sa * 0.6 + v * ca
            t0 = p["ts"] + rng.randint(0, 3)
            t1 = min(t0 + rng.randint(16, 30), N - 8)
            first_land[hi] = min(first_land.get(hi, 10 ** 6), t1)
            dot = circle(f"dot{pi:02d}_{j:02d}", 1.0, coll, hexlin(PAPER, 0), 2.6, seg=14)
            swirl, sgn = rng.uniform(0.5, 1.2), rng.choice((-1, 1))
            def dfn(f, sx=sx, sy=sy, t0=t0, t1=t1, hx=hx, hy=hy, hr=hr, swirl=swirl, sgn=sgn):
                if f < t0:
                    return {"location": (sx, sy, 2.6), "scale": (1e-4, 1e-4, 1), "color": hexlin(PAPER, 0)}
                tx, ty = disc_world(dv[min(f, N - 1)], hx, hy)
                p_ = lin01(f, t0, t1)
                ang = sgn * swirl * math.pi * (1 - p_)            # keep circling as they fall
                ex, ey = sx - tx, sy - ty
                rx = ex * math.cos(ang) - ey * math.sin(ang) * 0.5
                ry = ex * math.sin(ang) * 0.5 + ey * math.cos(ang)
                fall = ease_in(p_, 1.8)
                x = tx + rx * (1 - smooth(p_))
                y = ty + ry * (1 - fall)
                r_world = hr * dv[min(f, N - 1)]["size"]
                pop = lerp(0.35, 1.0, ease_out(lin01(f, t0, t0 + 5)))
                landed = f >= t1
                grow = lerp(0.6, 1.0, smooth(lin01(p_, 0.7, 1.0)))          # small dots, sized to their hole on landing
                sq = lerp(1.0, dv[min(f, N - 1)]["squash"], smooth(lin01(p_, 0.6, 1.0)))
                col = mix(hexlin(PAPER), hexlin(ORANGE), smooth(lin01(f, t0, t0 + 7)))
                return {"location": (x, y, 2.6 if p_ < 0.85 else 0.01),
                        "scale": ((r_world * pop * grow, r_world * pop * grow * sq, 1) if not landed else (1e-4, 1e-4, 1)),
                        "color": with_alpha(col, 1.0)}
            animate(dot, N, dfn)

    # each hole fills and glows the moment its first dot lands
    for hi, t in first_land.items():
        light_hole(d["lights"][hi], d["glows"][hi], N, t, 0.95, 0.5)
    animate(d["body"], N, lambda f: {"color": mix(hexlin(ORANGE), hexlin("#FF6A30"), 0.35 * fill(f))})

    # shred flashes: a brief warm puff where each page tears
    for i, p in enumerate(pages):
        px, py, _ = page_state(p, p["ts"])
        g = glow(f"tear{i:02d}", 1.6, coll, hexlin(GLOW, 0), 2.5, loc=(px, py))
        animate(g, N, lambda f, p=p: {"color": hexlin(GLOW, 0.35 * (lin01(f, p["ts"], p["ts"] + 2) - lin01(f, p["ts"] + 2, p["ts"] + 12)))})

    # camera: slow rise (frame drifts up then settles on the disc) with a slight roll for the orbit
    camv = cam_anim(cam, N, lambda f: (lerp(0.35, -0.2, ease_io(f / (N - 1))),
                                       lerp(0.7, 0.3, ease_io(f / (N - 1))),
                                       lerp(12.8, 12.2, smooth(f / (N - 1))),
                                       lerp(1.5, -1.0, ease_io(f / (N - 1)))))
    chapter(cam, camv, coll, 2, "Bring your documents", 4, max(first_land.values()))


# ------------------------------------------------------------------ S08
def s08():
    """Same set as S06: disc low, glass panel above, now with the Knowledge footage."""
    N = 150
    sc, coll, cam = new_shot("S08", N)
    d = build_disc(coll, "disc", 0.0, lights=True)
    disc_anim(d, N, lambda f: dict(x=0.0, y=-3.3, size=4.6, rot=34 + 4 * f / (N - 1), squash=0.32, thick=0.16))
    d["centre_light"].color = with_alpha(LIT_REST, 1.0)
    d["centre_glow"].color = hexlin(GLOW, 0.7)
    for i, (L, G) in enumerate(zip(d["lights"], d["glows"])):   # every hole is filled after S07
        L.color = with_alpha(LIT_REST, 0.45)
        animate(G, N, lambda f, i=i: {"color": hexlin(GLOW, 0.45 + 0.08 * math.sin(f / 11 + i))})
    glow("floorglow", 14, coll, hexlin(GLOW, 0.42), -0.5, loc=(0, -3.4), sy=0.35)
    glow("uplight", 12, coll, hexlin(GLOW, 0.13), -0.6, loc=(0, -1.0), sy=0.6)
    p = glass_panel(coll, "panel", "F2_knowledge_PLACEHOLDER.png")
    # the panel is already up as the shot opens (S07 cuts on the dots landing); it settles in
    def pfn(f):
        t = ease_out(lin01(f, 0, 18))
        return (0.25 * f / (N - 1), lerp(1.15, 1.35, t), lerp(0.94, 1.0, t), lerp(0.94, 1.0, t), smooth(lin01(f, 0, 5)))
    panel_anim(p, N, pfn)
    # slow push-in from the document list to the question and its answer
    ken_burns(p, [(0, 1.0, (0.5, 0.5)), (25, 1.0, (0.5, 0.5)), (149, 1.25, (0.02, 0.45))])
    camv = cam_anim(cam, N, lambda f: (0.1 - 0.2 * f / (N - 1), 1.25, lerp(12.0, 11.6, smooth(f / (N - 1))), 0.0))
    chapter(cam, camv, coll, 2, "Bring your documents", 0, 0, small=True)
    caption(cam, camv, coll, "Ask your files anything. Answers show their sources.", 20)


def build():
    s07()
    s08()
