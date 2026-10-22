# Brand animation

`animated-symbol.mgl` contains the cleaned Bezier contours from the restored
high-resolution logo animation: 540 frames at 30 fps, looping every 18 seconds.
The client paints these paths directly with Qt, preserving transparency and
sharp edges at any display scale. It requires no video codec, GIF, or PNG sequence.
Colors use the existing brand orange and ink tokens, including the source fades.

To repack the asset from the local restoration project:

```bash
python3 app/tools/pack_logo_animation.py design/launch-video/logo-loop-faithful/traced-motion.json
```

The format is a qCompress stream containing the `MGL1` signature, big-endian
16-bit frame rate and frame count, then length-prefixed frame payloads. Each
payload holds groups of closed Bezier contours with a float32 ink blend strength;
coordinates are float32 in a 420-unit, top-down reference canvas.

The generated asset is committed and self-contained. Building and running the
app does not require anything under `design/`. The local rendered sequence in
`design/launch-video/logo-loop-faithful/frames/` (about 9.5 GB) can be deleted
after integration. Keep the MP4 and Blender project for video playback/editing,
and `traced-motion.json` if you want to regenerate the app asset. Rendering the
Blender project can recreate the PNG sequence for video production.
