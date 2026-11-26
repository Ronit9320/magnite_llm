"""Run before -a: render the current scene at 3840x2160 into renders_4k/<shot>/."""
import bpy, os
sc = bpy.context.scene
sc.render.resolution_percentage = 200
root = os.path.join(os.path.dirname(bpy.data.filepath), "renders_4k", sc.name, "")
sc.render.filepath = root
try:
    sc.eevee.taa_render_samples = 32
except Exception:
    pass
