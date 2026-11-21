"""Batch 2 - Act II "Choose" (0:12-0:25): S04 the disc wakes, S05 model pills, S06 library footage.
Warm and quiet. Orange appears only when something is done."""
import math, random
from kit import *

LIT_PEAK = hexlin("#FFF4EC")
LIT_REST = hexlin("#FFB48C")
BODY_DARK = hexlin("#1E0E09")


def floor(coll, horizon_y, z=-1.0):
    rect("floor", 300, 60, coll, hexlin("#0C0C0F"), z, loc=(0, horizon_y - 30))
    rect("horizon", 300, 0.02, coll, hexlin("#1C1C21"), z + 0.01, loc=(0, horizon_y))


def light_hole(ob, gl, n, t0, peak_a=0.9, rest_a=0.45):
    """Hole ignites: alpha snaps in, colour runs from near-white to warm, glow pulses then settles."""
    def fn(f):
        if f < t0:
            return {"color": with_alpha(LIT_PEAK, 0.0)}
        a = lin01(f, t0, t0 + 2)
        return {"color": with_alpha(mix(LIT_PEAK, LIT_REST, smooth(lin01(f, t0 + 3, t0 + 16))), a)}
    animate(ob, n, fn)
    def gfn(f):
        if f < t0:
            return {"color": hexlin(GLOW, 0.0)}
        up = lin01(f, t0, t0 + 3)
        return {"color": hexlin(GLOW, lerp(0, peak_a, up) - (peak_a - rest_a) * smooth(lin01(f, t0 + 3, t0 + 18)))}
    animate(gl, n, gfn)


# ------------------------------------------------------------------ S04
def s04():
    N = 150
    sc, coll, cam = new_shot("S04", N)
    floor(coll, 2.6)
    hz = bpy.data.objects["horizon"]                      # fades as the camera lifts, so it never cuts the name
    animate(hz, N, lambda f: {"color": hexlin("#1C1C21", 1 - smooth(lin01(f, 80, 110)))})
    d = build_disc(coll, "disc", 0.0, lights=True)
    dv = disc_anim(d, N, lambda f: dict(x=0.0, y=-0.3, size=6.0, rot=15 * ease_io(f / (N - 1)),
                                        squash=lerp(0.52, 0.62, smooth(f / (N - 1))), thick=0.22))
    # ignition: centre first, then every hole outward
    order = hole_order()
    T0 = 10
    light_hole(d["centre_light"], d["centre_glow"], N, T0, 1.0, 0.6)
    times = {}
    for rank, i in enumerate(order):
        t = int(16 + 100 * (rank / (len(order) - 1)) ** 0.85)
        times[i] = t
        light_hole(d["lights"][i], d["glows"][i], N, t)
    warmth = lambda f: smooth(lin01(f, 6, 120))
    animate(d["body"], N, lambda f: {"color": mix(BODY_DARK, hexlin(ORANGE), warmth(f))})
    animate(d["side"], N, lambda f: {"color": mix(hexlin("#1A0C07"), hexlin(DISC_SIDE), warmth(f))})
    # the disc lights its own floor
    g1 = glow("floorglow", 16, coll, hexlin(GLOW, 0), -0.5, loc=(0, -0.6), sy=0.45)
    g2 = glow("halo", 9, coll, hexlin("#FF7A45", 0), -0.4, loc=(0, -0.3), sy=0.55)
    animate(g1, N, lambda f: {"color": hexlin(GLOW, 0.45 * warmth(f))})
    animate(g2, N, lambda f: {"color": hexlin("#FF7A45", 0.35 * warmth(f) * (1 + 0.06 * math.sin(f / 9)))})
    # focus drifts toward the centre hole while it lights, then the camera lifts so the disc
    # becomes a rising sun at the bottom of frame and the name appears above it
    cc = symbol_data()["centre_c"]
    def cam_fn(f):
        t = ease_io(lin01(f, 0, 85))
        u = ease_io(lin01(f, 78, 128))
        cx, cy = disc_world(dv[f], *cc)
        return (lerp(0, cx, 0.6 * t) * (1 - u), lerp(lerp(-0.1, cy, 0.6 * t), 1.75, u),
                lerp(lerp(10.5, 8.6, t), 10.2, u), lerp(0, -3, ease_io(f / (N - 1))))
    camv = cam_anim(cam, N, cam_fn)
    # the name, from wordmark.svg, then the promise that sets up the three chapters
    wm = wordmark(coll, "wordmark", 6.4, parent=cam)
    T = 100
    def wfn(f):
        k = camv[f][2] / VIEW_W
        return {"location": (0, (2.35 - 0.18 * (1 - ease_out(lin01(f, T, T + 16)))) * k, -0.5), "scale": (k, k, 1),
                "color": (1, 1, 1, smooth(lin01(f, T, T + 12)))}
    animate(wm, N, wfn)
    screen_text(cam, camv, coll, "promise", "Your own AI, in three steps.", 0.42, 0, 1.15, hexlin(PAPER), 116, None,
                align="CENTER")


# ------------------------------------------------------------------ S05
def s05():
    N = 90
    sc, coll, cam = new_shot("S05", N)
    d = build_disc(coll, "disc", 0.0, lights=True)
    dv = disc_anim(d, N, lambda f: dict(x=0.0, y=-2.4, size=4.4 * (1 + 0.035 * math.sin(math.pi * lin01(f, 56, 64))),
                                        rot=6 * f / (N - 1), squash=0.34, thick=0.16))
    g = glow("floorglow", 13, coll, hexlin(GLOW, 0.32), -0.5, loc=(0, -2.5), sy=0.35)
    animate(g, N, lambda f: {"color": hexlin(GLOW, 0.3 + 0.2 * smooth(lin01(f, 56, 66)) - 0.08 * lin01(f, 66, 89))})
    DOCK = 56
    light_hole(d["centre_light"], d["centre_glow"], N, DOCK, 1.0, 0.7)
    # a ring of light leaves the centre on the dock
    rg = ring("dockring", 0.95, 1.0, coll, hexlin(GLOW, 0), 0.3, loc=(0, -2.4))
    animate(rg, N, lambda f: {"scale": (0.3 + 2.6 * ease_out(lin01(f, DOCK, DOCK + 22)), (0.3 + 2.6 * ease_out(lin01(f, DOCK, DOCK + 22))) * 0.34, 1),
                              "color": hexlin(GLOW, 0.0 if f < DOCK else 0.8 * (1 - lin01(f, DOCK, DOCK + 22)))})

    # the school of models
    rng = random.Random(51)
    def pill(name, edge):
        root = empty(name, coll, (0, 0, 1.0))
        rrect(name + "_f", 0.56, 0.21, 0.105, coll, hexlin("#24242A"), 0, parent=root)
        rrect_outline(name + "_e", 0.56, 0.21, 0.105, 0.02, coll, hexlin(edge), 0.002, parent=root)
        rect(name + "_l", 0.22, 0.025, coll, hexlin("#55555D"), 0.003, parent=root, loc=(-0.06, 0.0))
        return root
    for i in range(44):
        root = pill(f"pill{i:02d}", "#46464E")
        x0, y0 = rng.uniform(-10, 10), rng.uniform(0.2, 4.2)
        sp, ph, sz = rng.uniform(0.35, 0.6), rng.uniform(0, 6.3), rng.uniform(0.75, 1.15)
        def fn(f, x0=x0, y0=y0, sp=sp, ph=ph, sz=sz):
            t = f / 30
            x = (x0 + sp * t + 10) % 20 - 10
            y = y0 + 0.28 * math.sin(t * 0.9 + ph + x * 0.3)
            vy = 0.28 * math.cos(t * 0.9 + ph + x * 0.3) * (0.9 + 0.3 * sp)
            return {"location": (x, y, 1.0 + sz * 0.01), "rotation_euler": (0, 0, math.atan2(vy, sp) * 0.8),
                    "scale": (sz, sz, 1)}
        animate(root, N, fn)
    # the hero: slows, drops, docks with a clean tock
    hero = pill("hero", "#7A7A84")
    cc = symbol_data()["centre_c"]
    def hero_pos(f):
        t = f / 30
        if f <= 26:
            sp = lerp(0.5, 0.0, smooth(lin01(f, 8, 26)))
            x = -2.6 + 0.5 * min(t, 8 / 30) + sum(lerp(0.5, 0.0, smooth(lin01(k, 8, 26))) for k in range(8, f)) / 30
            return x, 2.3 + 0.12 * math.sin(t * 2), 0.0, 1.05
        x1, y1 = hero_pos(26)[:2]
        tx, ty = disc_world(dv[min(f, N - 1)], *cc)
        p = ease_in(lin01(f, 26, DOCK), 2.2)
        x = lerp(x1, tx, smooth(lin01(f, 26, DOCK)))
        y = lerp(y1, ty, p) + 0.5 * math.sin(math.pi * lin01(f, 26, DOCK)) * (1 - p)
        return x, y, 90 * smooth(lin01(f, 30, DOCK - 4)), lerp(1.05, 0.55, ease_in(lin01(f, DOCK - 8, DOCK)))
    def hfn(f):
        x, y, r, s = hero_pos(f)
        return {"location": (x, y, 1.2 if f < DOCK - 6 else 0.01), "rotation_euler": (0, 0, math.radians(r)),
                "scale": (s if f < DOCK else 0.0001, s if f < DOCK else 0.0001, 1)}
    animate(hero, N, hfn)

    def cam_fn(f):
        t = smooth(f / (N - 1))
        hx = hero_pos(f)[0]
        return (lerp(-0.6, 0.0, t) + 0.15 * hx * (1 - t), lerp(0.7, -0.2, t), lerp(14.0, 12.4, t), 0.0)
    camv = cam_anim(cam, N, cam_fn)
    chapter(cam, camv, coll, 1, "Choose a model", 4, DOCK)


# ------------------------------------------------------------------ glass panel (S06, S08, S10)
PANEL_W = 7.6
PANEL_ASPECT = 1104 / 760

def glass_panel(coll, name, image, z=2.0):
    """A frameless glass panel: footage, thin edge highlight, soft shadow, warm reflection of the disc."""
    h = PANEL_W / PANEL_ASPECT
    root = empty(name, coll, (0, 0, z))
    shadow = glow(name + "_shadow", PANEL_W * 1.5, coll, hexlin("#000000", 0.0), -0.02, root, (0.15, -0.35), sy=h / PANEL_W * 1.45)
    plate = rect(name + "_img", PANEL_W, h, coll, (1, 1, 1, 1), 0.0, mat_image("M_" + os.path.splitext(image)[0], os.path.join(FOOTAGE, image)), root)  # one material per clip
    edge = rrect_outline(name + "_edge", PANEL_W, h, 0.06, 0.025, coll, hexlin(PAPER, 0.0), 0.002, root)
    warm = glow(name + "_warm", PANEL_W * 0.9, coll, hexlin(GLOW, 0.0), 0.004, root, (0, -h / 2), sy=0.12)
    return dict(root=root, parts=[(shadow, "#000000", 0.55), (plate, None, 1.0), (edge, PAPER, 0.22), (warm, GLOW, 0.16)],
                mat=plate.data.materials[0], h=h)

def panel_anim(p, n, fn):
    """fn(f) -> (x, y, sx, sy, alpha)."""
    vals = [fn(f) for f in range(n)]
    animate(p["root"], n, lambda f: {"location": (vals[f][0], vals[f][1], p["root"].location.z),
                                     "scale": (vals[f][2], vals[f][3], 1)})
    for ob, col, a in p["parts"]:
        if col is None:
            animate(ob, n, lambda f, a=a: {"color": (1, 1, 1, a * vals[f][4])})
        else:
            animate(ob, n, lambda f, col=col, a=a: {"color": hexlin(col, a * vals[f][4])})

def caption(cam, camv, coll, body, t_in, t_out=None):
    """One plain sentence under the footage panel: what the viewer is looking at."""
    return screen_text(cam, camv, coll, "caption", body, 0.4, 0, -4.0, hexlin(PAPER), t_in, t_out, align="CENTER")

def ken_burns(p, keys):
    """keys: [(frame, zoom, (u, v))] - zoom toward image point (u, v), v measured from the bottom."""
    mp = p["mat"].node_tree.nodes["Map"]
    for f, z, (u, v) in keys:
        mp.inputs["Scale"].default_value = (1 / z, 1 / z, 1)
        mp.inputs["Location"].default_value = (u * (1 - 1 / z), v * (1 - 1 / z), 0)
        mp.inputs["Scale"].keyframe_insert("default_value", frame=f + 1)
        mp.inputs["Location"].keyframe_insert("default_value", frame=f + 1)


# ------------------------------------------------------------------ S06
def s06():
    N = 150
    sc, coll, cam = new_shot("S06", N)
    d = build_disc(coll, "disc", 0.0, lights=True)
    disc_anim(d, N, lambda f: dict(x=0.0, y=-3.3, size=4.6, rot=6 + 4 * f / (N - 1), squash=0.32, thick=0.16))
    for ob in (d["centre_light"],):
        ob.color = with_alpha(LIT_REST, 1.0)
    d["centre_glow"].color = hexlin(GLOW, 0.7)
    glow("floorglow", 14, coll, hexlin(GLOW, 0.32), -0.5, loc=(0, -3.4), sy=0.35)
    glow("uplight", 12, coll, hexlin(GLOW, 0.10), -0.6, loc=(0, -1.0), sy=0.6)
    p = glass_panel(coll, "panel", "F1_library_PLACEHOLDER.png")
    def pfn(f):
        t = ease_out_back(lin01(f, 12, 40), 1.2)
        return (0.25 * f / (N - 1), 1.35, lerp(0.72, 1.0, t), max(0.02, lerp(0.02, 1.0, t)), smooth(lin01(f, 12, 22)))
    panel_anim(p, N, pfn)
    # guided read of the footage: the search, then the model card and its Load button
    ken_burns(p, [(0, 1.0, (0.5, 0.5)), (45, 1.0, (0.5, 0.5)), (95, 1.18, (0.42, 0.62)), (149, 1.24, (0.3, 0.46))])
    camv = cam_anim(cam, N, lambda f: (-0.15 * f / (N - 1), lerp(-2.4, 1.25, ease_io(lin01(f, 0, 42))),
                                       lerp(12.6, 11.8, smooth(f / (N - 1))), 0.0))
    chapter(cam, camv, coll, 1, "Choose a model", 30, 0, small=True)
    caption(cam, camv, coll, "Download open models. Run them on your machine.", 44)


def build():
    s04()
    s05()
    s06()
