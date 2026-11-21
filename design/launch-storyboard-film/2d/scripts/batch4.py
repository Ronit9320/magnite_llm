"""Batch 4 - Act II, chapter 03 "Make it yours" (0:34-0:46):
S09 the loss ribbon and the marble, S10 training then adapters footage (cut on Activate), S11 the switch."""
import csv, math, random, statistics
from kit import *
from batch2 import glass_panel, panel_anim, ken_burns, caption, LIT_REST, PANEL_W


# ------------------------------------------------------------------ loss data
def loss_trend(window=15, keep_noise=0.10):
    """The run's per-step loss is very noisy (batch size 1), so show its centred rolling median
    plus a little of the raw noise, as the storyboard asks. Swap loss.csv for the final run."""
    rows = list(csv.reader(open(os.path.join(DATA, "loss.csv"))))[1:]
    raw = [float(r[1]) for r in rows]
    med = [statistics.median(raw[max(0, i - window):i + window + 1]) for i in range(len(raw))]
    out = [m + keep_noise * max(-1.0, min(1.0, r - m)) for m, r in zip(med, raw)]
    # light EMA so the ribbon reads as one smooth stroke
    sm, e = [], out[0]
    for v in out:
        e = 0.75 * e + 0.25 * v
        sm.append(e)
    return sm


# ------------------------------------------------------------------ S09
def s09():
    N = 120
    sc, coll, cam = new_shot("S09", N)
    L = loss_trend()
    X0, X1 = -13.0, 13.0
    lo, hi = min(L), max(L)
    ymap = lambda v: lerp(-2.5, 1.7, (v - lo) / (hi - lo))
    n = len(L)
    pts = [(lerp(X0, X1, i / (n - 1)), ymap(v)) for i, v in enumerate(L)]
    step = 3
    cpts = pts[::step] + [pts[-1]]
    vi = min(range(n), key=lambda i: L[i])                  # the valley
    xv, yv = pts[vi]
    def ribbon_y(x):
        t = clamp((x - X0) / (X1 - X0)) * (n - 1)
        i = min(int(t), n - 2)
        return lerp(pts[i][1], pts[i + 1][1], t - i)

    # the ribbon draws on from the left; a soft wide copy is its bloom
    reveal = lambda f: ease_out(lin01(f, 0, 78), 2.2)
    for name, w, col, z in (("bloom", 0.32, hexlin(GLOW, 0.13), 0.9), ("ribbon", 0.10, hexlin(ORANGE), 1.0)):
        rb = nurbs(name, cpts, coll, col, w, z)
        rb.data.bevel_factor_mapping_end = "SPLINE"
        animate(rb.data, N, lambda f: {"bevel_factor_end": max(0.002, reveal(f))})
    head = glow("head", 1.4, coll, hexlin(GLOW, 0.0), 1.1)
    def hfn(f):
        x = lerp(X0, X1, reveal(f))
        return {"location": (x, ribbon_y(x), 1.1), "color": hexlin(GLOW, 0.7 * (1 - lin01(f, 72, 84)))}
    animate(head, N, hfn)

    # the marble rolls down and settles in the valley
    T0, TS = 12, 92
    def marble_x(f):
        x = lerp(X0 + 1.2, xv, ease_io(lin01(f, T0, TS)) ** 1.15)
        if f > TS:
            x += 0.45 * math.exp(-(f - TS) / 7) * math.sin((f - TS) / 2.6)
        return x
    R = 0.2
    mroot = empty("marble", coll, (0, 0, 1.4))
    glow("marble_shadow", 0.9, coll, hexlin("#000000", 0.5), -0.02, mroot, (0.05, -0.18), sy=0.35)
    circle("marble_body", R, coll, hexlin(PAPER), 0, parent=mroot, seg=40)
    circle("marble_shade", R * 0.92, coll, hexlin("#CFCBC0"), 0.001, parent=mroot, loc=(0.03, -0.04), seg=40)
    circle("marble_hi", R * 0.28, coll, hexlin("#FFFFFF"), 0.002, parent=mroot, loc=(-0.07, 0.08), seg=20)
    animate(mroot, N, lambda f: {"location": (marble_x(f), ribbon_y(marble_x(f)) + R + 0.04, 1.4),
                                 "scale": ((1.0,) * 2 if f >= T0 - 4 else (1e-4,) * 2) + (1,)})
    # settle: a warm pulse leaves the valley (done = orange)
    pr = ring("settle", 0.92, 1.0, coll, hexlin(GLOW, 0), 1.2, loc=(xv, yv + R))
    animate(pr, N, lambda f: {"scale": (0.3 + 2.2 * ease_out(lin01(f, TS + 4, TS + 24)),
                                        (0.3 + 2.2 * ease_out(lin01(f, TS + 4, TS + 24))) * 0.4, 1),
                              "color": hexlin(GLOW, 0 if f < TS + 4 else 0.7 * (1 - lin01(f, TS + 4, TS + 24)))})
    vg = glow("valleyglow", 5, coll, hexlin(GLOW, 0), 0.5, loc=(xv, yv - 0.2), sy=0.4)
    animate(vg, N, lambda f: {"color": hexlin(GLOW, 0.3 * smooth(lin01(f, TS, TS + 14)))})

    # camera flies alongside the marble, then holds on it in the valley
    def cam_fn(f):
        mx = marble_x(max(f, T0))
        x = lerp(mx + 2.2, xv + 0.6, smooth(lin01(f, 78, 108)))
        return (x, lerp(0.5, yv + 2.0, smooth(f / (N - 1))), lerp(12.6, 10.8, smooth(f / (N - 1))), 0.0)
    camv = cam_anim(cam, N, cam_fn)

    # dark grid landscape, screen-locked with the lines sliding past as the camera travels
    HZ = 0.9                      # horizon height on screen
    for k in range(11):           # receding cross lines, closer together near the horizon
        y = HZ - 5.6 * (k / 10) ** 1.8
        a = 0.85 * (1 - 0.7 * (1 - k / 10) ** 3)
        ln = rect(f"gridh{k}", 18, 0.018 + 0.012 * k / 10, coll, hexlin(LINE, a), 0, None, cam, (0, 0))
        animate(ln, N, lambda f, y=y: {"location": (0, y * camv[f][2] / VIEW_W, -100.5),
                                       "scale": (camv[f][2] / VIEW_W,) * 2 + (1,)})
    for j in range(24):           # lines to the vanishing point
        ln = nurbs(f"gridv{j}", [(0, 0), (0, 0)], coll, hexlin(LINE, 0.8), 0.02, 0, cam, order=2)
        ln.location = (0, 0, -100.5)
        def lp(f, j=j):
            k = camv[f][2] / VIEW_W
            bx = ((j * 2.0 - camv[f][0] * 1.4) % 48) - 24
            return [(bx * 0.04 * k, HZ * k), (bx * k, -4.7 * k)]
        animate_points(ln, N, lp)
    fd = rect("horizon_fog", 18, 1.4, coll, hexlin(GRAPHITE), 0, mat_fadedown(), cam, (0, 0))
    animate(fd, N, lambda f: {"location": (0, (HZ - 0.3) * camv[f][2] / VIEW_W, -100.4),
                              "scale": (camv[f][2] / VIEW_W,) * 2 + (1,)})
    sky = rect("sky", 18, 6, coll, hexlin(GRAPHITE), 0, None, cam, (0, 0))
    animate(sky, N, lambda f: {"location": (0, (HZ + 3.0) * camv[f][2] / VIEW_W, -100.4),
                               "scale": (camv[f][2] / VIEW_W,) * 2 + (1,)})

    chapter(cam, camv, coll, 3, "Make it yours", 4, TS + 4)


# ------------------------------------------------------------------ S10
def s10():
    """Training footage, then the adapters footage; the shot cuts on the click of Activate."""
    N = 150
    sc, coll, cam = new_shot("S10", N)
    d = build_disc(coll, "disc", 0.0, lights=True)
    disc_anim(d, N, lambda f: dict(x=0.0, y=-3.3, size=4.6, rot=40 + 4 * f / (N - 1), squash=0.32, thick=0.16))
    d["centre_light"].color = with_alpha(LIT_REST, 1.0)
    d["centre_glow"].color = hexlin(GLOW, 0.7)
    for i, (Lo, G) in enumerate(zip(d["lights"], d["glows"])):
        Lo.color = with_alpha(LIT_REST, 0.45)
        G.color = hexlin(GLOW, 0.45)
    glow("floorglow", 14, coll, hexlin(GLOW, 0.42), -0.5, loc=(0, -3.4), sy=0.35)
    glow("uplight", 12, coll, hexlin(GLOW, 0.13), -0.6, loc=(0, -1.0), sy=0.6)

    SW = 76                                                    # the flip from F3 to F4
    p3 = glass_panel(coll, "panelA", "F3_training_PLACEHOLDER.png")
    p4 = glass_panel(coll, "panelB", "F4_adapters_PLACEHOLDER.png", z=2.05)
    def base(f):
        return 0.25 * f / (N - 1), lerp(1.15, 1.35, ease_out(lin01(f, 0, 18)))
    def f3(f):
        x, y = base(f)
        s = lerp(0.94, 1.0, ease_out(lin01(f, 0, 18)))
        sx = s * (1 - ease_in(lin01(f, SW - 6, SW)))
        return (x, y, max(sx, 0.002), s, smooth(lin01(f, 0, 5)) * (1 if f < SW else 0))
    def f4(f):
        x, y = base(f)
        sx = ease_out(lin01(f, SW, SW + 8))
        return (x, y, max(sx, 0.002), 1.0, 1.0 if f >= SW else 0.0)
    panel_anim(p3, N, f3)
    panel_anim(p4, N, f4)
    ken_burns(p3, [(0, 1.0, (0.5, 0.5)), (16, 1.0, (0.5, 0.5)), (SW, 1.32, (0.2, 0.3))])
    KZ, KU, KV = 1.45, 0.07, 0.36
    ken_burns(p4, [(SW, 1.0, (0.5, 0.5)), (SW + 18, 1.0, (0.5, 0.5)), (124, KZ, (KU, KV)), (149, KZ, (KU, KV))])

    # the click: a ring and a brief press flash on the second Activate button (about 80, 487 px)
    CLICK = 132
    W, H = PANEL_W, p4["h"]
    U, V = 80 / 1104, 1 - 487 / 760
    u0, v0 = KU * (1 - 1 / KZ), KV * (1 - 1 / KZ)
    px = ((U - u0) * KZ - 0.5) * W
    py = ((V - v0) * KZ - 0.5) * H
    bx, by = base(CLICK)
    cx, cy = bx + px, by + py
    rg = ring("click", 0.8, 1.0, coll, hexlin(ORANGE, 0), 2.3, loc=(cx, cy))
    animate(rg, N, lambda f: {"scale": ((0.2 + 1.1 * ease_out(lin01(f, CLICK, CLICK + 14))),) * 2 + (1,),
                              "color": hexlin(GLOW, 0 if f < CLICK else 0.95 * (1 - lin01(f, CLICK + 2, CLICK + 14)))})
    bw, bh = 85 / 1104 * W * KZ, 34 / 760 * H * KZ
    fl = rrect("press", bw, bh, bh * 0.2, coll, hexlin(PAPER, 0), 2.25, loc=(cx, cy))
    animate(fl, N, lambda f: {"color": hexlin(PAPER, 0.35 * (lin01(f, CLICK, CLICK + 1) - lin01(f, CLICK + 2, CLICK + 8)))})

    camv = cam_anim(cam, N, lambda f: (0.1 - 0.2 * f / (N - 1), 1.25, lerp(12.0, 11.6, smooth(f / (N - 1))), 0.0))
    chapter(cam, camv, coll, 3, "Make it yours", 0, 0, small=True)
    caption(cam, camv, coll, "Train it on your own examples.", 14, SW - 8)
    screen_text(cam, camv, coll, "caption2", "Then switch it on.", 0.4, 0, -4.0, hexlin(PAPER), SW + 10, None,
                align="CENTER")


# ------------------------------------------------------------------ S11
def s11():
    """Top-down: a ripple runs out from the centre and every dot lifts and re-seats in a new pattern."""
    N = 90
    sc, coll, cam = new_shot("S11", N)
    sd = symbol_data()
    cxc, cyc = sd["centre_c"]
    d = build_disc(coll, "disc", 0.0, lights=True)
    dv = disc_anim(d, N, lambda f: dict(x=0.0, y=0.55, size=5.3, rot=10 * ease_io(f / (N - 1)), squash=1.0, thick=0.0))
    d["side"].color = hexlin(DISC_SIDE, 0.0)
    d["centre_light"].color = with_alpha(LIT_REST, 1.0)
    d["centre_glow"].color = hexlin(GLOW, 0.7)
    rng = random.Random(1111)
    T0, SPEED = 12, 95.0               # wave leaves the centre at T0, travels SPEED frames per unit radius

    # the new pattern: a swirl that is strongest near the centre, plus a little size change per dot
    def target(x, y, r):
        dx, dy = x - cxc, y - cyc
        rad = math.hypot(dx, dy)
        a = math.radians(38 * (1 - min(rad / 0.46, 1)) ** 1.2 + rng.uniform(-6, 6) * (rad < 0.4))
        nx, ny = cxc + dx * math.cos(a) - dy * math.sin(a), cyc + dx * math.sin(a) + dy * math.cos(a)
        return nx, ny, r * rng.uniform(0.8, 1.2), rad
    for i, (x, y, r) in enumerate(sd["holes"]):
        nx, ny, nr, rad = target(x, y, r)
        t = T0 + rad * SPEED
        def fn(f, x=x, y=y, nx=nx, ny=ny, s1=nr / r, t=t):
            p = smooth(lin01(f, t, t + 14))
            lift = math.sin(math.pi * lin01(f, t, t + 14))
            s = lerp(1.0, s1, p) * (1 + 0.3 * lift)
            return {"location": (lerp(x, nx, p), lerp(y, ny, p), 0.002), "scale": (s, s, 1)}
        vals = [fn(f) for f in range(N)]
        for ob, z in ((d["holes"][i], 0.002), (d["lights"][i], 0.004), (d["glows"][i], 0.006)):
            animate(ob, N, lambda f, z=z: {"location": vals[f]["location"][:2] + (z,), "scale": vals[f]["scale"]})
        animate(d["lights"][i], N, lambda f, t=t: {"color": with_alpha(mix(LIT_REST, hexlin("#FFF4EC"),
                                                   math.sin(math.pi * lin01(f, t, t + 14))), 0.45 + 0.5 * math.sin(math.pi * lin01(f, t, t + 14)))})
        animate(d["glows"][i], N, lambda f, t=t: {"color": hexlin(GLOW, 0.45 + 0.45 * math.sin(math.pi * lin01(f, t, t + 14)))})
    # the centre: the logo's crisp ink shape with its lit core set just inside it, so it keeps the
    # symbol's outline. On the switch it turns a quarter and breathes once, evenly (no stretching).
    d["centre_glow"].color = hexlin(GLOW, 0.0)            # the glow sprite read as a red smudge here
    d["centre"].color = hexlin(INK, 0.0)                 # no dark border: the lit core is the whole centre
    turn = lambda f: math.radians(-28 * smooth(lin01(f, T0 - 6, T0 + 22)))
    pulse = lambda f: 1 + 0.08 * math.sin(math.pi * lin01(f, T0 - 6, T0 + 22))
    animate(d["centre"], N, lambda f: {"scale": (pulse(f), pulse(f), 1), "rotation_euler": (0, 0, turn(f))})
    flash = lambda f: math.sin(math.pi * lin01(f, T0 - 2, T0 + 16))
    animate(d["centre_light"], N, lambda f: {"scale": (pulse(f), pulse(f), 1),
                                             "rotation_euler": (0, 0, turn(f)),
                                             "color": with_alpha(mix(LIT_REST, hexlin("#FFF4EC"), flash(f)), 1.0)})
    # the ripple itself: two thin rings of light leaving the centre like ink in water
    cw = disc_world(dv[0], cxc, cyc)
    for k, (dl, a) in enumerate(((0, 0.55), (7, 0.3))):
        rg = ring(f"ripple{k}", 0.97, 1.0, coll, hexlin(PAPER, 0), 0.05, loc=cw)
        def rfn(f, dl=dl, a=a):
            p = lin01(f, T0 + dl, T0 + dl + 0.55 * SPEED)
            s = 0.05 + 0.55 * 5.3 * p
            return {"scale": (s, s, 1), "color": hexlin("#FFD9C4", a * math.sin(math.pi * p))}
        animate(rg, N, rfn)
    glow("halo", 11, coll, hexlin(GLOW, 0.28), -0.5, loc=(0, 0.55))
    camv = cam_anim(cam, N, lambda f: (0.0, 0.0, lerp(11.6, 11.0, smooth(f / (N - 1))), 0.0))
    caption(cam, camv, coll, "Switch its style in one click.", 6)


def build():
    s09()
    s10()
    s11()
