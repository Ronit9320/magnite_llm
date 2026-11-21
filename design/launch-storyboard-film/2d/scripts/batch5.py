"""Batch 5 - Act III "The proof" and Act IV "The name" (0:46-1:00):
S12 the pulse that never leaves, S13 through the centre hole, S14 the end card."""
import math, random
from kit import *
from batch2 import LIT_REST

LAP = 45                       # one lap of the rim pulse = 1.5 s, three beats at 120 BPM


def lit_disc(coll, name="disc"):
    d = build_disc(coll, name, 0.0, lights=True)
    d["centre"].color = hexlin(INK, 0.0)              # same clean lit centre as S11
    d["centre_light"].color = with_alpha(LIT_REST, 1.0)
    d["centre_glow"].color = hexlin(GLOW, 0.0)
    for Lo, G in zip(d["lights"], d["glows"]):
        Lo.color = with_alpha(LIT_REST, 0.45)
        G.color = hexlin(GLOW, 0.45)
    return d


# ------------------------------------------------------------------ S12
def s12():
    N = 210
    sc, coll, cam = new_shot("S12", N)
    DX, DY, SIZE, SQ, TH = 3.0, -0.6, 4.4, 0.30, 0.16
    rect("floor", 400, 60, coll, hexlin("#0C0C0F"), -1.0, loc=(0, 6.0 - 30))
    rect("horizon", 400, 0.03, coll, hexlin("#17171B"), -0.99, loc=(0, 6.0))
    d = lit_disc(coll)
    disc_anim(d, N, lambda f: dict(x=DX, y=DY, size=SIZE, rot=12 + 6 * f / (N - 1), squash=SQ, thick=TH))
    glow("floorglow", 11, coll, hexlin(GLOW, 0.3), -0.5, loc=(DX, DY - 0.15), sy=0.33)

    # the fallen cables from Act I, dark now, lying where they dropped
    rng = random.Random(1212)
    for i in range(7):
        x0 = -16 + i * 1.1 + rng.uniform(-0.3, 0.3)
        y0 = -2.2 + rng.uniform(-0.5, 0.4)
        pts = [(x0 + k * 1.7 + rng.uniform(-0.3, 0.3), y0 + 0.35 * math.sin(k * 1.3 + i) + rng.uniform(-0.15, 0.15))
               for k in range(6)]
        nurbs(f"deadcable{i}", pts, coll, hexlin("#232328"), rng.uniform(0.07, 0.11), 0.1)

    # the pulse: a short bright arc with a fading trail that circles the rim and never leaves it
    RX = SIZE * 0.5 * 1.04
    RY = RX * SQ
    ang = lambda f: 2 * math.pi * f / LAP + 1.2
    def rim(a):
        return DX + RX * math.cos(a), DY + RY * math.sin(a) - 0.02
    TRAIL = 1.25
    arc = nurbs("pulse_trail", [(0, 0)] * 9, coll, hexlin("#FFD9C4", 0.9), 0.05, 0.3)
    animate_points(arc, N, lambda f: [rim(ang(f) - TRAIL * (1 - k / 8)) for k in range(9)])
    arc.data.bevel_factor_mapping_start = "SPLINE"
    arcg = nurbs("pulse_bloom", [(0, 0)] * 9, coll, hexlin(GLOW, 0.35), 0.18, 0.29)
    animate_points(arcg, N, lambda f: [rim(ang(f) - TRAIL * 0.6 * (1 - k / 8)) for k in range(9)])
    head = glow("pulse_head", 0.9, coll, hexlin(GLOW, 0.9), 0.31)
    def hfn(f):
        x, y = rim(ang(f))
        front = math.sin(ang(f)) < 0               # the near half of the rim draws over the disc
        return {"location": (x, y, 0.31 if front else -0.05)}
    animate(head, N, hfn)
    for ob, zf, zb in ((arc, 0.3, -0.06), (arcg, 0.29, -0.07)):
        animate(ob, N, lambda f, zf=zf, zb=zb: {"location": (0, 0, zf if math.sin(ang(f) - 0.3) < 0 else zb)})
    refl = glow("pulse_reflection", 1.6, coll, hexlin(GLOW, 0.0), -0.4)
    animate(refl, N, lambda f: {"location": (rim(ang(f))[0], DY - RY - 0.45, -0.4),
                                "color": hexlin(GLOW, 0.16 * max(0, -math.sin(ang(f))))})

    # pull back to an ultra-wide: the disc becomes small and alone, the cables come in at the left
    camv = cam_anim(cam, N, lambda f: (lerp(DX, -1.4, ease_io(lin01(f, 0, 190))),
                                       lerp(DY + 0.7, 1.6, ease_io(lin01(f, 0, 190))),
                                       lerp(8.0, 27.0, ease_io(lin01(f, 0, 190))), 0.0))
    animate(head, N, lambda f: {"scale": ((camv[f][2] / 8.0) ** 0.7,) * 2 + (1,)})
    screen_text(cam, camv, coll, "line", "Your prompts stay here.", 0.66, 0, 2.55, hexlin(PAPER), 66, None,
                align="CENTER", fade_in=14)


# ------------------------------------------------------------------ S13
def s13():
    """The disc turns face-on, becomes the flat symbol, and the camera pushes into the centre to ink."""
    N = 90
    sc, coll, cam = new_shot("S13", N)
    d = lit_disc(coll)
    turn = lambda f: ease_io(lin01(f, 0, 34))
    dv = disc_anim(d, N, lambda f: dict(x=0.0, y=0.0, size=lerp(4.6, 7.2, turn(f)), rot=18 - 18 * turn(f),
                                        squash=lerp(0.34, 1.0, turn(f)), thick=lerp(0.16, 0.0, turn(f))))
    # flat lighting at the end so it matches the 2D logo: holes and centre go back to ink
    flat = lambda f: smooth(lin01(f, 44, 62))
    for Lo, G in zip(d["lights"], d["glows"]):
        animate(Lo, N, lambda f: {"color": with_alpha(LIT_REST, 0.45 * (1 - flat(f)))})
        animate(G, N, lambda f: {"color": hexlin(GLOW, 0.45 * (1 - flat(f)))})
    animate(d["centre"], N, lambda f: {"color": hexlin(INK, flat(f))})
    animate(d["centre_light"], N, lambda f: {"color": with_alpha(LIT_REST, 1 - flat(f))})
    halo = glow("halo", 12, coll, hexlin(GLOW, 0.3), -0.5)
    animate(halo, N, lambda f: {"color": hexlin(GLOW, 0.3 * (1 - flat(f)))})
    cc = symbol_data()["centre_c"]
    def cam_fn(f):
        cx, cy = disc_world(dv[f], *cc)
        p = ease_in(lin01(f, 30, 86), 2.4)
        return (lerp(0, cx, smooth(lin01(f, 20, 60))), lerp(0, cy, smooth(lin01(f, 20, 60))),
                12.0 * math.exp(math.log(0.03 / 12.0) * p), 0.0)
    cam_anim(cam, N, cam_fn)
    ov = overlay("ink", cam, coll, hexlin(INK, 0))
    animate(ov, N, lambda f: {"color": hexlin(INK, smooth(lin01(f, 82, 88)))})


# ------------------------------------------------------------------ S14
def s14():
    """Flat end card: the real lockup, the tagline, the version line. No glow, no 3D."""
    N = 120
    sc, coll, cam = new_shot("S14", N)
    cam.data.ortho_scale = VIEW_W
    sym = logo_plane(coll, "symbol.svg", 2.5, loc=(0, 1.35), z=0.1)
    wm = wordmark(coll, "wordmark", 5.4, loc=(0, -0.55), z=0.1)
    def fade_in(ob, t0, y, rise=0.12, s0=1.0):
        animate(ob, N, lambda f: {"color": (1, 1, 1, smooth(lin01(f, t0, t0 + 14))),
                                  "location": (0, y - rise * (1 - ease_out(lin01(f, t0, t0 + 18))), 0.1),
                                  "scale": ((lerp(s0, 1.0, ease_out(lin01(f, t0, t0 + 18))),) * 2) + (1,)})
    fade_in(sym, 0, 1.35, 0.0, 0.94)
    fade_in(wm, 8, -0.55)
    camv = [(0.0, 0.0, VIEW_W, 0.0)] * N
    screen_text(cam, camv, coll, "tagline", "Intelligence, on your terms.", 0.44, 0, -1.55, hexlin(PAPER), 20, None,
                align="CENTER", fnt=font_medium(), rise=0.1)
    screen_text(cam, camv, coll, "version", VERSION_LINE, 0.29, 0, -2.3, hexlin(MUTE), 30, None,
                align="CENTER", fnt=font_mono(), rise=0.0)


def build():
    s12()
    s13()
    s14()
