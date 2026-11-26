"""Rebuild one batch of shots into film2d.blend.
Usage: blender -b [film2d.blend | --factory-startup] -P scripts/build.py -- batch1
"""
import bpy, sys, os, importlib, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit

batch = sys.argv[sys.argv.index("--") + 1]
# drop the factory cube/light/camera if this is a fresh file
for name in ("Cube", "Light", "Camera"):
    ob = bpy.data.objects.get(name)
    if ob and not bpy.data.filepath:
        bpy.data.objects.remove(ob, do_unlink=True)
t = time.time()
importlib.import_module(batch).build()
bpy.data.orphans_purge(do_recursive=True)
out = os.path.join(kit.ROOT, "film2d.blend")
bpy.ops.wm.save_as_mainfile(filepath=out, compress=True)
print(f"[build] {batch} done in {time.time() - t:.1f}s -> {out}")
