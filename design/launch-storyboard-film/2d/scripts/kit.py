"""Shared toolkit for the 2D Magnite launch film.

Everything is flat: an orthographic camera looks straight down -Z, every
object is an unlit emission surface whose colour and alpha come from the
object's own Color property (so they can be animated per object), and depth
order is set by Z. Animation is computed in Python per frame and baked to
linear F-curves, so each shot is a deterministic function of the frame.
"""
import bpy, math, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # .../launch-storyboard-film/2d
FONTS = os.path.join(ROOT, "fonts")
RENDERS = os.path.join(ROOT, "renders")
# Source material (logo SVGs, app screenshots, loss.csv) lives in the main checkout's
# gitignored film folder. Override with MAGNITE_FILM_SRC.
FILM_SRC = os.environ.get("MAGNITE_FILM_SRC",
                          "/home/guardian/Projects/model_training/magnite_llm/design/launch-storyboard-film")
ASSETS = os.path.join(FILM_SRC, "assets")
FOOTAGE = os.path.join(FILM_SRC, "footage")
DATA = os.path.join(FILM_SRC, "data")

FPS = 30
RES = (1920, 1080)
VIEW_W = 16.0                                      # ortho_scale at "normal" framing
CAM_Z = 100.0
VERSION_LINE = "v0.2 preview  ·  macOS  ·  Windows  ·  Linux"

# ---------------------------------------------------------------- colour
def hexlin(h, a=1.0):
    h = h.lstrip("#")
    c = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]
    return (lin[0], lin[1], lin[2], a)

ORANGE   = "#FF4A0E"
GLOW     = "#FF5C28"
INK      = "#201411"
GRAPHITE = "#09090B"
SURFACE  = "#111113"
PAPER    = "#F1EFE7"
COLD     = "#85B9F4"
GREY     = "#63636B"
LINE     = "#36363C"
CARD     = "#15181D"
MUTE     = "#888F9A"
COLDTXT  = "#C3D2E4"

def mix(c1, c2, t):
    return tuple(a + (b - a) * t for a, b in zip(c1, c2))

def with_alpha(c, a):
    return (c[0], c[1], c[2], a)

# ---------------------------------------------------------------- easing
def clamp(x, a=0.0, b=1.0):
    return a if x < a else b if x > b else x

def lerp(a, b, t):
    return a + (b - a) * t

def lin01(f, f0, f1):
    return clamp((f - f0) / float(f1 - f0)) if f1 != f0 else (1.0 if f >= f1 else 0.0)

def smooth(t):
    t = clamp(t); return t * t * (3 - 2 * t)

def ease_in(t, p=2):
    return clamp(t) ** p

def ease_out(t, p=3):
    return 1 - (1 - clamp(t)) ** p

def ease_io(t):
    t = clamp(t)
    return 4 * t ** 3 if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2

def ease_out_back(t, s=1.7):
    t = clamp(t) - 1
    return 1 + (s + 1) * t ** 3 + s * t ** 2

def wobble(t, seed=0.0):
    """Deterministic smooth noise in roughly [-1, 1]."""
    return (math.sin(t * 1.7 + seed) * 0.5 + math.sin(t * 3.1 + seed * 2.3) * 0.3
            + math.sin(t * 5.3 + seed * 0.7) * 0.2)

# ---------------------------------------------------------------- materials
def _flat_material(name, kind):
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    try:
        m.surface_render_method = "BLENDED"
    except Exception:
        pass
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    info = nt.nodes.new("ShaderNodeObjectInfo")
    emi = nt.nodes.new("ShaderNodeEmission")
    tr = nt.nodes.new("ShaderNodeBsdfTransparent")
    mx = nt.nodes.new("ShaderNodeMixShader")
    nt.links.new(info.outputs["Color"], emi.inputs["Color"])
    emi.inputs["Strength"].default_value = 1.0
    alpha = info.outputs["Alpha"]
    if kind in ("glow", "fadeup", "fadedown"):
        tc = nt.nodes.new("ShaderNodeTexCoord")
        sep = nt.nodes.new("ShaderNodeSeparateXYZ")
        nt.links.new(tc.outputs["UV"], sep.inputs[0])
        if kind == "glow":
            sub = nt.nodes.new("ShaderNodeVectorMath"); sub.operation = "SUBTRACT"
            sub.inputs[1].default_value = (0.5, 0.5, 0.0)
            nt.links.new(tc.outputs["UV"], sub.inputs[0])
            ln = nt.nodes.new("ShaderNodeVectorMath"); ln.operation = "LENGTH"
            nt.links.new(sub.outputs[0], ln.inputs[0])
            mr = nt.nodes.new("ShaderNodeMapRange")
            mr.inputs["From Min"].default_value = 0.5
            mr.inputs["From Max"].default_value = 0.0
            mr.clamp = True
            nt.links.new(ln.outputs["Value"], mr.inputs["Value"])
            pw = nt.nodes.new("ShaderNodeMath"); pw.operation = "POWER"
            pw.inputs[1].default_value = 2.2
            nt.links.new(mr.outputs[0], pw.inputs[0])
            fall = pw.outputs[0]
        else:
            mr = nt.nodes.new("ShaderNodeMapRange")
            mr.clamp = True
            nt.links.new(sep.outputs["Y"], mr.inputs["Value"])
            if kind == "fadedown":
                mr.inputs["From Min"].default_value = 1.0
                mr.inputs["From Max"].default_value = 0.0
            fs = nt.nodes.new("ShaderNodeMath"); fs.operation = "POWER"
            fs.inputs[1].default_value = 1.6
            nt.links.new(mr.outputs[0], fs.inputs[0])
            fall = fs.outputs[0]
        mul = nt.nodes.new("ShaderNodeMath"); mul.operation = "MULTIPLY"
        nt.links.new(info.outputs["Alpha"], mul.inputs[0])
        nt.links.new(fall, mul.inputs[1])
        alpha = mul.outputs[0]
    nt.links.new(alpha, mx.inputs[0])
    nt.links.new(tr.outputs[0], mx.inputs[1])
    nt.links.new(emi.outputs[0], mx.inputs[2])
    nt.links.new(mx.outputs[0], out.inputs["Surface"])
    return m

def mat_flat():     return _flat_material("M_Flat", "flat")
def mat_glow():     return _flat_material("M_Glow", "glow")
def mat_fadeup():   return _flat_material("M_FadeUp", "fadeup")      # transparent bottom -> opaque top
def mat_fadedown(): return _flat_material("M_FadeDown", "fadedown")  # opaque bottom -> transparent top

def mat_image(name, path, use_alpha=False):
    """Emission image plane; alpha from Object Color, UV transform via a Mapping node named 'Map'.
    Point the Image Texture at a movie file later to swap in real footage."""
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    try:
        m.surface_render_method = "BLENDED"
    except Exception:
        pass
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping"); mp.name = "Map"
    tex = nt.nodes.new("ShaderNodeTexImage"); tex.name = "Footage"
    tex.image = bpy.data.images.load(path, check_existing=True)
    tex.extension = "CLIP"
    info = nt.nodes.new("ShaderNodeObjectInfo")
    emi = nt.nodes.new("ShaderNodeEmission")
    tr = nt.nodes.new("ShaderNodeBsdfTransparent")
    mx = nt.nodes.new("ShaderNodeMixShader")
    nt.links.new(tc.outputs["UV"], mp.inputs["Vector"])
    nt.links.new(mp.outputs[0], tex.inputs["Vector"])
    nt.links.new(tex.outputs["Color"], emi.inputs["Color"])
    if use_alpha:      # artwork with transparency (logo rasters): object alpha x image alpha
        mul = nt.nodes.new("ShaderNodeMath"); mul.operation = "MULTIPLY"
        nt.links.new(info.outputs["Alpha"], mul.inputs[0])
        nt.links.new(tex.outputs["Alpha"], mul.inputs[1])
        nt.links.new(mul.outputs[0], mx.inputs[0])
    else:
        nt.links.new(info.outputs["Alpha"], mx.inputs[0])
    nt.links.new(tr.outputs[0], mx.inputs[1])
    nt.links.new(emi.outputs[0], mx.inputs[2])
    nt.links.new(mx.outputs[0], out.inputs["Surface"])
    return m

# ---------------------------------------------------------------- fonts
def font(name):
    path = os.path.join(FONTS, name)
    for f in bpy.data.fonts:
        if f.filepath and os.path.abspath(bpy.path.abspath(f.filepath)) == path:
            return f
    return bpy.data.fonts.load(path)

def font_semibold(): return font("SpaceGrotesk-SemiBold.ttf")
def font_medium():   return font("SpaceGrotesk-Medium.ttf")
def font_mono():     return font("JetBrainsMono-Medium.ttf")

# ---------------------------------------------------------------- geometry
def _link(ob, coll, parent=None):
    coll.objects.link(ob)
    if parent is not None:
        ob.parent = parent
    return ob

def poly(name, pts, coll, color, z=0.0, mat=None, parent=None, loc=(0, 0)):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    me = bpy.data.meshes.new(name)
    me.from_pydata([(x, y, 0.0) for x, y in pts], [], [list(range(len(pts)))])
    uv = me.uv_layers.new(name="UV")
    for loop in me.loops:
        x, y = pts[loop.vertex_index]
        uv.data[loop.index].uv = ((x - minx) / max(maxx - minx, 1e-9),
                                  (y - miny) / max(maxy - miny, 1e-9))
    me.materials.append(mat or mat_flat())
    ob = bpy.data.objects.new(name, me)
    ob.location = (loc[0], loc[1], z)
    ob.color = color if len(color) == 4 else (*color, 1.0)
    return _link(ob, coll, parent)

def rect(name, w, h, coll, color, z=0.0, mat=None, parent=None, loc=(0, 0)):
    pts = [(-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2)]
    return poly(name, pts, coll, color, z, mat, parent, loc)

def rrect_pts(w, h, r, seg=6):
    r = max(1e-4, min(r, w / 2 - 1e-4, h / 2 - 1e-4))
    pts = []
    for cx, cy, a0 in ((w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90),
                       (-w / 2 + r, -h / 2 + r, 180), (w / 2 - r, -h / 2 + r, 270)):
        for i in range(seg + 1):
            a = math.radians(a0 + 90 * i / seg)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

def rrect(name, w, h, r, coll, color, z=0.0, mat=None, parent=None, loc=(0, 0)):
    return poly(name, rrect_pts(w, h, r), coll, color, z, mat, parent, loc)

def rrect_outline(name, w, h, r, t, coll, color, z=0.0, parent=None, loc=(0, 0)):
    """Rounded-rect stroke of thickness t, centred on the w x h edge."""
    outer = rrect_pts(w + t, h + t, r + t / 2)
    inner = rrect_pts(w - t, h - t, max(r - t / 2, 1e-3))
    n = len(outer)
    me = bpy.data.meshes.new(name)
    me.from_pydata([(x, y, 0) for x, y in outer + inner], [],
                   [[i, (i + 1) % n, n + (i + 1) % n, n + i] for i in range(n)])
    me.uv_layers.new(name="UV")
    me.materials.append(mat_flat())
    ob = bpy.data.objects.new(name, me)
    ob.location = (loc[0], loc[1], z)
    ob.color = color
    return _link(ob, coll, parent)

def circle_pts(r, seg=48, sx=1.0, sy=1.0):
    return [(r * sx * math.cos(2 * math.pi * i / seg), r * sy * math.sin(2 * math.pi * i / seg))
            for i in range(seg)]

def circle(name, r, coll, color, z=0.0, mat=None, parent=None, loc=(0, 0), seg=48):
    return poly(name, circle_pts(r, seg), coll, color, z, mat, parent, loc)

def glow(name, size, coll, color, z=0.0, parent=None, loc=(0, 0), sy=1.0):
    """Soft radial sprite; size is the full diameter."""
    return rect(name, size, size * sy, coll, color, z, mat_glow(), parent, loc)

def ring(name, r_in, r_out, coll, color, z=0.0, parent=None, loc=(0, 0), seg=64):
    me = bpy.data.meshes.new(name)
    verts, faces = [], []
    for i in range(seg):
        a = 2 * math.pi * i / seg
        verts += [(r_in * math.cos(a), r_in * math.sin(a), 0), (r_out * math.cos(a), r_out * math.sin(a), 0)]
    for i in range(seg):
        j = (i + 1) % seg
        faces.append([2 * i, 2 * i + 1, 2 * j + 1, 2 * j])
    me.from_pydata(verts, [], faces)
    me.uv_layers.new(name="UV")
    me.materials.append(mat_flat())
    ob = bpy.data.objects.new(name, me)
    ob.location = (loc[0], loc[1], z)
    ob.color = color
    return _link(ob, coll, parent)

def empty(name, coll, loc=(0, 0, 0), parent=None):
    ob = bpy.data.objects.new(name, None)
    ob.location = loc
    return _link(ob, coll, parent)

def text(name, body, size, coll, color, z=0.0, fnt=None, align="CENTER", valign="CENTER",
         parent=None, loc=(0, 0), spacing=1.0):
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = body
    cu.font = fnt or font_semibold()
    cu.size = size
    cu.align_x = align
    cu.align_y = valign
    cu.space_character = spacing
    cu.materials.append(mat_flat())
    ob = bpy.data.objects.new(name, cu)
    ob.location = (loc[0], loc[1], z)
    ob.color = color
    return _link(ob, coll, parent)

def nurbs(name, pts, coll, color, width, z=0.0, parent=None, order=4):
    """Smooth open curve on the control points; width is the full stroke width."""
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "3D"
    cu.bevel_depth = width / 2
    cu.bevel_resolution = 2
    cu.resolution_u = 20
    cu.use_fill_caps = True
    sp = cu.splines.new("NURBS")
    sp.points.add(len(pts) - 1)
    for p, (x, y) in zip(sp.points, pts):
        p.co = (x, y, 0.0, 1.0)
    sp.use_endpoint_u = True
    sp.order_u = min(order, len(pts))
    cu.materials.append(mat_flat())
    ob = bpy.data.objects.new(name, cu)
    ob.location = (0, 0, z)
    ob.color = color
    return _link(ob, coll, parent)

def animate_points(curve_ob, n, fn):
    """fn(f) -> list of (x, y) for every control point of the first spline."""
    cu = curve_ob.data
    np_ = len(cu.splines[0].points)
    vals = [fn(f) for f in range(n)]
    animate(cu, n, lambda f: {f"splines[0].points[{k}].co": (vals[f][k][0], vals[f][k][1], 0.0, 1.0)
                              for k in range(np_)})

# ---------------------------------------------------------------- baking
def _fcurve(idb, path, index):
    ad = idb.animation_data or idb.animation_data_create()
    if ad.action is None:
        ad.action = bpy.data.actions.new(idb.name + "_act")
    act = ad.action
    if hasattr(act, "fcurve_ensure_for_datablock"):
        return act.fcurve_ensure_for_datablock(idb, path, index=index)
    return act.fcurves.find(path, index=index) or act.fcurves.new(path, index=index)

def bake(idb, path, index, vals, start=1):
    fc = _fcurve(idb, path, index)
    kp = fc.keyframe_points
    n = len(vals)
    kp.add(n)
    co = [0.0] * (2 * n)
    co[0::2] = [float(start + k) for k in range(n)]
    co[1::2] = [float(v) for v in vals]
    kp.foreach_set("co", co)
    try:
        kp.foreach_set("interpolation", [1] * n)          # LINEAR
    except Exception:
        for p in kp:
            p.interpolation = "LINEAR"
    fc.update()

def animate(idb, n, fn, start=1):
    """fn(f) for f in 0..n-1 returns {data_path: value or tuple}. Varying channels are
    baked one key per frame; constant ones are just set."""
    samples = {}
    for f in range(n):
        for path, val in fn(f).items():
            if isinstance(val, (tuple, list)):
                for i, v in enumerate(val):
                    samples.setdefault((path, i), []).append(v)
            else:
                samples.setdefault((path, -1), []).append(val)
    for (path, i), vals in samples.items():
        if max(vals) - min(vals) < 1e-7:
            if i < 0:
                owner, _, attr = path.rpartition(".")
                setattr(idb.path_resolve(owner) if owner else idb, attr, vals[0])
            else:
                idb.path_resolve(path)[i] = vals[0]
        else:
            bake(idb, path, max(i, 0), vals, start)

# ---------------------------------------------------------------- scenes
def world():
    w = bpy.data.worlds.get("W_Graphite")
    if w:
        return w
    w = bpy.data.worlds.new("W_Graphite")
    w.use_nodes = True
    bg = next(n for n in w.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = hexlin(GRAPHITE)
    bg.inputs["Strength"].default_value = 1.0
    return w

def remove_scene(name):
    sc = bpy.data.scenes.get(name)
    if not sc:
        return
    for coll in list(sc.collection.children_recursive):
        for ob in list(coll.objects):
            bpy.data.objects.remove(ob, do_unlink=True)
        bpy.data.collections.remove(coll)
    for ob in list(sc.collection.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    bpy.data.scenes.remove(sc)

def new_shot(name, nframes, samples=24):
    remove_scene(name)
    sc = bpy.data.scenes.new(name)
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x, sc.render.resolution_y = RES
    sc.render.resolution_percentage = 100
    sc.render.fps = FPS
    sc.frame_start, sc.frame_end = 1, nframes
    sc.world = world()
    sc.render.film_transparent = False
    try:
        sc.view_settings.view_transform = "Standard"
        sc.view_settings.look = "None"
    except TypeError:
        pass
    try:
        sc.eevee.taa_render_samples = samples
    except Exception:
        pass
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGB"
    sc.render.filepath = os.path.join(RENDERS, name, "")
    coll = bpy.data.collections.new(name + "_coll")
    sc.collection.children.link(coll)
    cd = bpy.data.cameras.new(name + "_cam")
    cd.type = "ORTHO"
    cd.ortho_scale = VIEW_W
    cd.clip_start, cd.clip_end = 0.05, 500
    cam = bpy.data.objects.new(name + "_cam", cd)
    cam.location = (0, 0, CAM_Z)
    coll.objects.link(cam)
    sc.camera = cam
    return sc, coll, cam

def overlay(name, cam, coll, color, z_off=1.0):
    """Full-frame plane glued in front of the camera (fades, flicker, black)."""
    ob = rect(name, 400, 400, coll, color, 0, None, cam)
    ob.location = (0, 0, -z_off)
    return ob

def cam_anim(cam, n, fn):
    """fn(f) -> (x, y, ortho_scale, rot_deg). Returns the sampled values."""
    vals = [fn(f) for f in range(n)]
    animate(cam, n, lambda f: {"location": (vals[f][0], vals[f][1], CAM_Z),
                               "rotation_euler": (0.0, 0.0, math.radians(vals[f][3]))})
    animate(cam.data, n, lambda f: {"ortho_scale": vals[f][2]})
    return vals

# ---------------------------------------------------------------- the disc (logo symbol)
import re as _re

def _bez_path(d, steps=3):
    nums = [float(x) for x in _re.findall(r"-?\d+(?:\.\d+)?", d)]
    assert d.strip().startswith("M") and set(_re.findall(r"[A-Za-z]", d)) <= {"M", "C", "Z"}
    pts = [(nums[0], nums[1])]
    cur = pts[0]
    for i in range(2, len(nums) - 5, 6):
        p1, p2, p3 = (nums[i], nums[i + 1]), (nums[i + 2], nums[i + 3]), (nums[i + 4], nums[i + 5])
        for s in range(1, steps + 1):
            t = s / steps; u = 1 - t
            pts.append((u ** 3 * cur[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                        u ** 3 * cur[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
        cur = p3
    if math.dist(pts[0], pts[-1]) < 1e-3:
        pts.pop()
    return pts

_SYMBOL = None
def symbol_data():
    """Logo symbol normalised to diameter 1 around the outline centre, y up.
    Returns dict(outline=[pts], centre=[pts], centre_c=(x,y), holes=[(x, y, r)])."""
    global _SYMBOL
    if _SYMBOL:
        return _SYMBOL
    svg = open(os.path.join(ASSETS, "symbol.svg")).read()
    paths = _re.findall(r'<path[^>]*\sd="([^"]+)"', svg)
    circles = [tuple(float(v) for v in m) for m in
               _re.findall(r'<circle[^>]*cx="([\d.]+)"[^>]*cy="([\d.]+)"[^>]*r="([\d.]+)"', svg)]
    outline, centre = _bez_path(paths[0]), _bez_path(paths[1])
    xs = [p[0] for p in outline]; ys = [p[1] for p in outline]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    k = 1.0 / max(max(xs) - min(xs), max(ys) - min(ys))
    tr = lambda p: ((p[0] - cx) * k, -(p[1] - cy) * k)
    cen = [tr(p) for p in centre]
    _SYMBOL = dict(outline=[tr(p) for p in outline], centre=cen,
                   centre_c=(sum(p[0] for p in cen) / len(cen), sum(p[1] for p in cen) / len(cen)),
                   holes=[(*tr((x, y)), r * k) for x, y, r in circles])
    return _SYMBOL

DISC_SIDE = "#A9310A"     # the disc's darker rim, seen when it lies on the floor

def build_disc(coll, name, z=0.0, lights=False):
    """Root (position + squash) > spin (rotation + size) > body, ink holes, centre shape.
    A matching 'side' copy under the body gives thickness when squashed.
    With lights=True every hole also gets a light overlay and a glow sprite (off by default)."""
    sd = symbol_data()
    root = empty(name, coll, (0, 0, z))
    spin = empty(name + "_spin", coll, (0, 0, 0), root)
    side = empty(name + "_side", coll, (0, 0, -0.02), root)
    d = dict(root=root, spin=spin, side_e=side, holes=[], lights=[], glows=[])
    d["side"] = poly(name + "_sidebody", sd["outline"], coll, hexlin(DISC_SIDE), 0, parent=side)
    d["body"] = poly(name + "_body", sd["outline"], coll, hexlin(ORANGE), 0, parent=spin)
    cx, cy = sd["centre_c"]
    d["centre"] = poly(name + "_centre", [(x - cx, y - cy) for x, y in sd["centre"]], coll,
                       hexlin(INK), 0.002, parent=spin, loc=(cx, cy))
    for i, (x, y, r) in enumerate(sd["holes"]):
        d["holes"].append(circle(f"{name}_h{i:02d}", r, coll, hexlin(INK), 0.002, parent=spin, loc=(x, y), seg=20))
    if lights:
        d["centre_light"] = poly(name + "_centreL", [(x - cx, y - cy) for x, y in sd["centre"]], coll,
                                 hexlin(PAPER, 0), 0.004, parent=spin, loc=(cx, cy))
        d["centre_glow"] = glow(name + "_centreG", 0.22, coll, hexlin(GLOW, 0), 0.006, spin, (cx, cy))
        for i, (x, y, r) in enumerate(sd["holes"]):
            d["lights"].append(circle(f"{name}_L{i:02d}", r, coll, hexlin(PAPER, 0), 0.004, parent=spin, loc=(x, y), seg=20))
            d["glows"].append(glow(f"{name}_G{i:02d}", r * 5, coll, hexlin(GLOW, 0), 0.006, spin, (x, y)))
    return d

def disc_anim(d, n, fn):
    """fn(f) -> dict(x, y, size, squash, rot[deg], thick[world units, screen-down offset of the rim])."""
    vals = [fn(f) for f in range(n)]
    animate(d["root"], n, lambda f: {"location": (vals[f]["x"], vals[f]["y"], d["root"].location.z),
                                     "scale": (1.0, vals[f].get("squash", 1.0), 1.0)})
    for key, extra in (("spin", None), ("side_e", True)):
        ob = d[key]
        def g(f, ob=ob, extra=extra):
            v = vals[f]; s = v["size"]
            out = {"rotation_euler": (0, 0, math.radians(v.get("rot", 0.0))), "scale": (s, s, 1)}
            if extra:
                sq = max(v.get("squash", 1.0), 1e-3)
                out["location"] = (0, -v.get("thick", 0.0) / sq, -0.02)
            return out
        animate(ob, n, g)
    return vals

def hole_order(centre_first=True):
    """Indices of holes sorted from the centre hole outward."""
    sd = symbol_data(); cx, cy = sd["centre_c"]
    return sorted(range(len(sd["holes"])), key=lambda i: math.dist(sd["holes"][i][:2], (cx, cy)))

def disc_world(dv, lx, ly):
    """World position of a disc-local point for one frame of disc_anim values."""
    a = math.radians(dv.get("rot", 0.0)); s = dv["size"]
    x = (lx * math.cos(a) - ly * math.sin(a)) * s
    y = (lx * math.sin(a) + ly * math.cos(a)) * s
    return (dv["x"] + x, dv["y"] + y * dv.get("squash", 1.0))


# ---------------------------------------------------------------- on-screen type (screen-locked titles)
# Screen units: the frame is 16 wide and 9 tall, centred on 0, whatever the camera's ortho scale.
SCREEN_H = 9.0

def screen_text(cam, camv, coll, name, body, size, x, y, color, t_in, t_out=None, align="LEFT",
                fnt=None, rise=0.14, pop=0.0, depth=0.5, fade_in=8, fade_out=6, backing=0.0, spacing=1.0, color_fn=None):
    """Text glued to the camera so it reads as a title, not an object in the scene.
    color is a linear RGB(A) tuple; camv are cam_anim's sampled values. backing>0 adds a soft dark pad."""
    n = len(camv)
    ob = text(name, body, size, coll, with_alpha(color, 0), 0, fnt, align, "CENTER", cam, (0, 0), spacing)
    pad = None
    if backing:
        w = 0.55 * size * len(body)
        cx = {"LEFT": x + w / 2, "RIGHT": x - w / 2}.get(align, x)
        pad = rrect(name + "_pad", w * 1.12 + size, size * 1.9, size * 0.35, coll, hexlin(GRAPHITE, 0), 0, None, cam)
    def alpha(f):
        a = smooth(lin01(f, t_in, t_in + fade_in))
        if t_out is not None:
            a *= 1 - smooth(lin01(f, t_out, t_out + fade_out))
        return a
    def fn(f):
        k = camv[f][2] / VIEW_W
        dy = -rise * (1 - ease_out(lin01(f, t_in, t_in + 14)))
        s = k * (1 + pop * (1 - ease_out(lin01(f, t_in, t_in + 7))))
        return {"location": (x * k, (y + dy) * k, -depth), "scale": (s, s, 1),
                "color": with_alpha(color_fn(f) if color_fn else color, alpha(f))}
    animate(ob, n, fn)
    if pad:
        animate(pad, n, lambda f: {"location": (cx * camv[f][2] / VIEW_W, y * camv[f][2] / VIEW_W, -depth - 0.01),
                                   "scale": (camv[f][2] / VIEW_W,) * 2 + (1,),
                                   "color": hexlin(GRAPHITE, backing * alpha(f))})
    return ob

def chapter(cam, camv, coll, num, title, t_in, t_done, t_out=None, small=False):
    """Numbered chapter label, the same in every chapter: '01' over 'Choose a model'.
    The number is grey until the step is done, then turns orange (orange = done)."""
    if small:      # tag in the corner of the footage shots
        x, y, ns, ts = -7.3, 4.05, 0.26, 0.26
        numob = screen_text(cam, camv, coll, f"ch{num}_n", f"{num:02d}", ns, x, y, hexlin(ORANGE), t_in, t_out,
                            fnt=font_mono(), rise=0.0)
        screen_text(cam, camv, coll, f"ch{num}_t", title.upper(), ts, x + 0.55, y, hexlin(MUTE), t_in, t_out,
                    fnt=font_mono(), rise=0.0, spacing=1.1)
        return numob
    x, y = -7.2, -3.75
    numob = screen_text(cam, camv, coll, f"ch{num}_n", f"{num:02d}", 0.34, x + 0.03, y + 0.78, hexlin(MUTE), t_in, t_out,
                        fnt=font_mono(), color_fn=lambda f: mix(hexlin(MUTE), hexlin(ORANGE), smooth(lin01(f, t_done, t_done + 6))))
    screen_text(cam, camv, coll, f"ch{num}_t", title, 0.66, x, y, hexlin(PAPER), t_in + 3, t_out)
    return numob


# ---------------------------------------------------------------- logo art (rasterised from the supplied SVGs)
# The wordmark's glyphs overlap and rely on nonzero fill, which Blender curves can't do, so the
# SVG itself is rendered to a large PNG with rsvg-convert. The name is never retyped.
GEN = os.path.join(ROOT, "build")

def svg_png(svg_name, width_px=4096):
    os.makedirs(GEN, exist_ok=True)
    out = os.path.join(GEN, os.path.splitext(svg_name)[0] + f"_{width_px}.png")
    if not os.path.exists(out):
        import subprocess, tempfile
        src = open(os.path.join(ASSETS, svg_name)).read()
        src = src.replace("@brandText", PAPER).replace("@brandOrange", ORANGE).replace("@brandInk", INK)
        with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as t:
            t.write(src)
        subprocess.run(["rsvg-convert", "-w", str(width_px), "-o", out, t.name], check=True)
        os.unlink(t.name)
    return out

def logo_plane(coll, svg_name, width, parent=None, loc=(0, 0), z=0.0):
    """A plane carrying the SVG art at its true aspect; fade it with the object's colour alpha."""
    svg = open(os.path.join(ASSETS, svg_name)).read()
    vb = [float(v) for v in _re.search(r'viewBox="([^"]+)"', svg).group(1).split()]
    stem = os.path.splitext(svg_name)[0]
    m = mat_image("M_logo_" + stem, svg_png(svg_name), use_alpha=True)
    return rect(stem, width, width * vb[3] / vb[2], coll, (1, 1, 1, 0), z, m, parent, loc)

def wordmark(coll, name, width, parent=None, loc=(0, 0), z=0.0):
    return logo_plane(coll, "wordmark.svg", width, parent, loc, z)
