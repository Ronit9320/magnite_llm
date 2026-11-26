"""Batch 1 - Act I, the noise (0:00-0:12): S01 tether, S02 friction, S03 the cut.
Cold, flat, no orange except the few sparks at the severed cable ends."""
import math, random
from kit import *

FLOOR_Y = -2.0
SLAB_TOP = -1.28
CABLE_BODY = hexlin("#4A4B53")
CABLE_RIM = hexlin(COLD, 0.30)

MSGS = ["Rate limit reached", "Sign in to continue", "Upgrade to continue", "Connecting…",
        "$20 / month", "Session expired", "Usage cap reached", "Try again later",
        "Network unavailable", "Verify your account", "Quota exceeded", "Server busy",
        "You are in a queue", "Payment required", "Reconnecting…", "Terms updated",
        "Usage: 98%", "Request failed", "Upgrade to Pro", "Too many requests"]


def floor_and_slab(coll):
    rect("floor", 200, 40, coll, hexlin("#0C0C0F"), -0.5, loc=(0, FLOOR_Y - 20))
    rect("floor_line", 200, 0.025, coll, hexlin(LINE), -0.4, loc=(0, FLOOR_Y))
    rrect("slab_edge", 3.66, 0.76, 0.09, coll, hexlin(GREY), 0.20, loc=(0, FLOOR_Y + 0.36))
    rrect("slab", 3.6, 0.70, 0.07, coll, hexlin(SURFACE), 0.21, loc=(0, FLOOR_Y + 0.36))
    rect("slab_lip", 3.3, 0.02, coll, hexlin("#2A2A30"), 0.22, loc=(0, FLOOR_Y + 0.58))


def cable_defs(n=9, seed=7, tall=False):
    rng = random.Random(seed)
    out = []
    for i in range(n):
        bx = -1.3 + 2.6 * i / (n - 1)
        ys = [-1.2, -0.2, 1.4, 3.4, 5.8, 9.0]
        xs = [bx, bx + rng.uniform(-.35, .35), bx * 0.6 + rng.uniform(-.7, .7),
              rng.uniform(-1.6, 1.6), rng.uniform(-2.5, 2.5), rng.uniform(-3.5, 3.5)]
        if tall:   # S02: keep climbing into the card storm
            ys += [14.0, 20.0, 27.0]
            xs += [rng.uniform(-5, 5), rng.uniform(-8, 8), rng.uniform(-11, 11)]
        out.append(dict(pts=list(zip(xs, ys)), w=rng.uniform(.05, .09), phase=rng.uniform(0, 6.3)))
    return out


def cable(coll, name, pts, w, z=0.1):
    body = nurbs(name, pts, coll, CABLE_BODY, w, z)
    rim = nurbs(name + "_rim", pts, coll, CABLE_RIM, w * 0.28, z + 0.01)
    return body, rim


def animate_cable(body, rim, w, n, fn):
    """fn(f) -> list of points; the rim is offset to the right edge of the body."""
    vals = [fn(f) for f in range(n)]
    animate_points(body, n, lambda f: vals[f])
    animate_points(rim, n, lambda f: [(x + w * 0.33, y) for x, y in vals[f]])


def make_card(coll, name, msg, z, rng):
    w = max(rng.uniform(2.2, 3.3), len(msg) * 0.125 + 0.55)
    h = rng.uniform(0.66, 0.92)
    root = empty(name, coll, (0, 0, z))
    parts = [
        (rrect(name + "_f", w, h, 0.09, coll, hexlin(CARD), 0.0, parent=root), hexlin(CARD), 0.86),
        (rrect_outline(name + "_e", w, h, 0.09, 0.035, coll, hexlin(COLD), 0.002, parent=root), hexlin(COLD), 0.8),
        (text(name + "_t", msg, 0.21, coll, hexlin(COLDTXT), 0.004, fnt=font_mono(), align="LEFT",
              parent=root, loc=(-w / 2 + 0.2, 0.1)), hexlin(COLDTXT), 1.0),
        (rect(name + "_b", w * 0.36, 0.05, coll, hexlin(MUTE), 0.004, parent=root,
              loc=(-w / 2 + 0.2 + w * 0.18, -0.19)), hexlin(MUTE), 0.8),
    ]
    return root, parts


def animate_card(root, parts, n, fn):
    """fn(f) -> (x, y, rot_deg, scale, alpha)."""
    vals = [fn(f) for f in range(n)]
    animate(root, n, lambda f: {"location": (vals[f][0], vals[f][1], root.location.z),
                                "rotation_euler": (0, 0, math.radians(vals[f][2])),
                                "scale": (vals[f][3], vals[f][3], 1)})
    for ob, col, a in parts:
        animate(ob, n, lambda f, col=col, a=a: {"color": with_alpha(col, a * vals[f][4])})


# ------------------------------------------------------------------ S01
def s01():
    N = 90
    sc, coll, cam = new_shot("S01", N)
    floor_and_slab(coll)
    # cables fade into the dark above
    rect("fade", 200, 3.2, coll, hexlin(GRAPHITE), 0.5, mat_fadeup(), loc=(0, 3.6))
    rect("dark", 200, 30, coll, hexlin(GRAPHITE), 0.5, loc=(0, 5.2 + 15))
    for i, d in enumerate(cable_defs()):
        body, rim = cable(coll, f"cable{i}", d["pts"], d["w"])
        def pts(f, d=d):
            amp = 0.05 * (1 + f / N)
            return [(x + (amp * k * math.sin(f / 30 * 1.6 + d["phase"] + k * 0.8) if k else 0), y)
                    for k, (x, y) in enumerate(d["pts"])]
        animate_cable(body, rim, d["w"], N, pts)
    # notification pings start to build near the end
    rng = random.Random(11)
    for i in range(9):
        b = 6 + int(78 * (i / 9) ** 0.8)            # pings from the first second: the hook starts at once
        x0, y0 = rng.choice([-1, 1]) * rng.uniform(2.2, 5.5), rng.uniform(0.3, 1.7)   # below the opening line
        root = empty(f"ping{i}", coll, (x0, y0, 0.6))
        fl = rrect(f"ping{i}_f", 0.98, 0.28, 0.13, coll, hexlin(CARD), 0.0, parent=root)
        e = rrect_outline(f"ping{i}_e", 0.98, 0.28, 0.13, 0.025, coll, hexlin(COLD), 0.002, parent=root)
        dot = circle(f"ping{i}_d", 0.045, coll, hexlin(COLD), 0.004, parent=root, loc=(-0.33, 0))
        bar = rect(f"ping{i}_b", 0.45, 0.04, coll, hexlin(MUTE), 0.004, parent=root, loc=(0.05, 0))
        def fn(f, b=b, x0=x0, y0=y0):
            t = lin01(f, b, b + 6)
            return {"location": (x0, y0 + 0.004 * max(0, f - b), 0.6),
                    "scale": (lerp(0.6, 1, ease_out_back(t)),) * 2 + (1,)}
        animate(root, N, fn)
        for ob, col, a in ((e, COLD, .5), (fl, CARD, .9), (dot, COLD, .7), (bar, MUTE, .6)):
            animate(ob, N, lambda f, b=b, col=col, a=a: {"color": hexlin(col, a * lin01(f, b, b + 4))})
    camv = cam_anim(cam, N, lambda f: (0.0, lerp(0.6, 0.15, smooth(f / (N - 1))),
                                       lerp(16.0, 12.6, ease_out(f / (N - 1), 1.6)), 0.0))
    # the opening line: what the cables mean, said before the viewer has to guess
    screen_text(cam, camv, coll, "line", "Most AI lives on someone else's computer.", 0.6, 0, 2.9,
                hexlin(COLDTXT), 4, 80, align="CENTER", backing=0.6)


# ------------------------------------------------------------------ S02
def s02():
    N = 180
    sc, coll, cam = new_shot("S02", N)
    floor_and_slab(coll)
    defs = cable_defs(tall=True)
    for i, d in enumerate(defs):
        body, rim = cable(coll, f"cable{i}", d["pts"], d["w"])
        def pts(f, d=d):
            amp = 0.1 + 0.25 * (f / N) ** 2
            return [(x + (amp * min(k, 4) * math.sin(f / 30 * 3.2 + d["phase"] + k) if k else 0), y)
                    for k, (x, y) in enumerate(d["pts"])]
        animate_cable(body, rim, d["w"], N, pts)
    fade = rect("fade", 200, 3.2, coll, hexlin(GRAPHITE), 0.5, mat_fadeup(), loc=(0, 3.6))
    dark = rect("dark", 200, 4, coll, hexlin(GRAPHITE), 0.5, loc=(0, 7.2))
    for ob in (fade, dark):
        animate(ob, N, lambda f: {"color": hexlin(GRAPHITE, 1 - smooth(lin01(f, 0, 9)))})

    # camera: whip up the cables, then jumpy handheld cuts that get shakier
    shots = [(15, 0.0, 16.0, 14.0, 0, .3, .2), (46, -5.0, 22.0, 12.0, -3, -.2, .3),
             (78, 6.0, 13.5, 13.0, 4, .25, -.2), (104, -2.0, 26.0, 11.0, 2, -.3, -.1),
             (126, 4.5, 19.0, 11.5, -5, .2, .25), (144, -6.0, 15.5, 10.0, 3, -.15, .2),
             (160, 1.5, 21.0, 14.5, -2, .1, -.25)]
    def cam_fn(f):
        if f < 15:
            t = ease_io(f / 14)
            return (0.0, lerp(0.15, 16.0, t), lerp(12.6, 14.0, t), 0.0)
        s = max(s for s in shots if s[0] <= f)
        dt = (f - s[0]) / 30
        A = 0.04 + 0.30 * (f / N) ** 2
        return (s[1] + s[5] * dt + A * wobble(f * 0.55, 1.3), s[2] + s[6] * dt + A * wobble(f * 0.6, 4.1),
                s[3] * (1 - 0.04 * dt), s[4] + A * 7 * wobble(f * 0.4, 2.2))
    camv = cam_anim(cam, N, cam_fn)

    def G(f):  # speed ramp: time integral of an accelerating speed
        return (f + 2.4 * f ** 3 / N ** 2) / 30

    rng = random.Random(21)
    for i in range(150):
        if i < 45:
            b = 0
            x0, y0 = rng.uniform(-13, 13), rng.uniform(9, 31)
        else:
            b = int(15 + 155 * math.sqrt(rng.random()))
            cx, cy, s_, _ = camv[b]
            x0, y0 = cx + rng.uniform(-0.48, 0.48) * s_, cy + rng.uniform(-0.3, 0.3) * s_
        z = rng.uniform(1, 4)
        root, parts = make_card(coll, f"card{i:03d}", rng.choice(MSGS), z, rng)
        vx, vy = rng.uniform(-.5, .5), rng.uniform(-.35, .35)
        r0, om = rng.uniform(-24, 24), rng.uniform(-22, 22)
        a0 = rng.uniform(0.55, 1.0)
        sz = 0.75 + 0.12 * z
        def fn(f, b=b, x0=x0, y0=y0, vx=vx, vy=vy, r0=r0, om=om, a0=a0, sz=sz):
            if f < b:
                return (x0, y0, r0, 0.6 * sz, 0.0)
            g = G(f) - G(b)
            t = lin01(f, b, b + 6)
            fl = 0.0 if 170 <= f <= 171 else 0.5 if f == 172 else 1.0
            return (x0 + vx * g, y0 + vy * g, r0 + om * g, sz * lerp(0.6, 1, ease_out_back(t)),
                    a0 * min(1, lin01(f, b, b + 4)) * fl)
        animate_card(root, parts, N, fn)
    # one flicker of the whole frame at the very end
    ov = overlay("flicker", cam, coll, hexlin(GRAPHITE, 0))
    animate(ov, N, lambda f: {"color": hexlin(GRAPHITE, 1.0 if 170 <= f <= 171 else 0.55 if f == 172 else 0.0)})
    # what it costs you, one beat per camera cut, until the storm drowns it out
    for k, (t0, t1, w) in enumerate([(17, 42, "Sign in."), (47, 73, "Stay online."),
                                     (79, 101, "Pay monthly."), (105, 122, "Please wait.")]):
        screen_text(cam, camv, coll, f"beat{k}", w, 1.15, 0, 0.1, hexlin(COLDTXT), t0, t1, align="CENTER",
                    rise=0.0, pop=0.22, fade_in=3, fade_out=3, backing=1.0)


# ------------------------------------------------------------------ S03
def s03():
    N = 90
    sc, coll, cam = new_shot("S03", N)
    floor_and_slab(coll)
    cam_anim(cam, N, lambda f: (0.0, 1.6, 17.0, 0.0))
    rng = random.Random(31)
    # the storm, frozen mid-air
    for i in range(26):
        x0, y0 = rng.uniform(-10, 10), rng.uniform(3.9, 7.4)
        z = rng.uniform(1, 3)
        root, parts = make_card(coll, f"card{i:02d}", rng.choice(MSGS), z, rng)
        vx, vy, r0, om = rng.uniform(-3, 3), rng.uniform(-2, 2), rng.uniform(-22, 22), rng.uniform(-60, 60)
        a0 = rng.uniform(0.4, 0.8)
        def fn(f, x0=x0, y0=y0, vx=vx, vy=vy, r0=r0, om=om, a0=a0):
            t = min(f, 4) / 30
            flash = 1.25 if 4 <= f <= 5 else 1.0
            return (x0 + vx * t, y0 + vy * t, r0 + om * t, 0.85, min(1, a0 * flash) * lerp(1, 0.7, lin01(f, 6, 55)))
        animate_card(root, parts, N, fn)

    CUT_Y = 3.4
    order = [3, 0, 5, 1, 6, 2, 4]
    T = [6, 11, 17, 24, 32, 41, 50]                     # each thump a little later than the last
    for i in range(7):
        bx = -1.25 + 2.5 * i / 6
        topx = bx * 1.6 + rng.uniform(-0.6, 0.6)
        hang = [(bx, -1.2), (bx + rng.uniform(-.2, .2), -0.3), (lerp(bx, topx, .35) + rng.uniform(-.3, .3), 0.8),
                (lerp(bx, topx, .6) + rng.uniform(-.3, .3), 1.9), (lerp(bx, topx, .85), 2.7), (topx, CUT_Y)]
        s = -1 if bx < 0 else 1 if bx > 0 else rng.choice([-1, 1])
        L = rng.uniform(3.0, 4.2)
        w = rng.uniform(.06, .1)
        rest = [(bx, -1.2), (bx + s * 0.3, FLOOR_Y + 0.3)] + \
               [(bx + s * (0.45 + L * (k - 1) / 4), FLOOR_Y + w / 2 + (0.03 if k % 2 else 0)) for k in range(2, 6)]
        Ti = T[order.index(i)]
        body, rim = cable(coll, f"cable{i}", hang, w)
        def pts(f, hang=hang, rest=rest, Ti=Ti, s=s):
            out = []
            for k, ((hx, hy), (rx, ry)) in enumerate(zip(hang, rest)):
                if k == 0:
                    out.append((hx, hy)); continue
                if f < Ti:
                    sl = smooth(lin01(f, Ti - 4, Ti)) * 0.18 * math.sin(math.pi * k / 5)
                    out.append((hx + s * sl, hy)); continue
                d = 3 + 7 * math.sqrt(max(hy - ry, 0) / 5.6)
                p = clamp((f - Ti) / d)
                y = hy + (ry - hy) * p * p
                if f > Ti + d:
                    e = f - Ti - d
                    y += 0.09 * math.exp(-e / 2.5) * abs(math.sin(e * 0.9))
                out.append((hx + (rx - hx) * smooth(p) + s * 0.18 * math.sin(math.pi * k / 5) * (1 - p), y))
            return out
        animate_cable(body, rim, w, N, pts)
        # the part above the cut snaps up out of frame
        stub_pts = [(topx, CUT_Y + 0.05), (topx + rng.uniform(-.4, .4), 5.0), (topx * 1.2, 7.5), (topx * 1.4, 10)]
        sb, sr = cable(coll, f"stub{i}", stub_pts, w, z=0.05)
        animate_cable(sb, sr, w, N, lambda f, sp=stub_pts, Ti=Ti: [(x, y + 14 * ease_in(lin01(f, Ti, Ti + 7), 2)) for x, y in sp])
        # a few small sparks at the severed end
        for j in range(6):
            vx, vy = rng.uniform(-3, 3), rng.uniform(0.5, 4)
            life = rng.randint(9, 15)
            sp = circle(f"spark{i}_{j}", rng.uniform(.025, .045), coll, hexlin(GLOW, 0), 0.8, loc=(topx, CUT_Y))
            def sfn(f, Ti=Ti, vx=vx, vy=vy, life=life, x0=topx):
                t = max(0, f - Ti) / 30
                a = 0.0 if f < Ti or f > Ti + life else 1 - (f - Ti) / life
                return {"location": (x0 + vx * t, CUT_Y + vy * t - 9 * t * t, 0.8),
                        "color": with_alpha(mix(hexlin("#FFD2B8"), hexlin(ORANGE), lin01(f, Ti, Ti + life)), a)}
            animate(sp, N, sfn)
    # hard black after the last cable lands: one full second of nothing
    ov = overlay("black", cam, coll, hexlin(GRAPHITE, 0))
    animate(ov, N, lambda f: {"color": hexlin(GRAPHITE, 1.0 if f >= 60 else 0.0)})
    # the question, asked in the silence
    screen_text(cam, [(0.0, 1.6, 17.0, 0.0)] * N, coll, "question", "What if it lived on yours?", 0.72, 0, 0.0,
                hexlin(PAPER), 66, None, align="CENTER", fade_in=10)


def build():
    s01()
    s02()
    s03()
