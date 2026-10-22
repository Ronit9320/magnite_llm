"""Pack the restored Blender contours; no rendered PNG frames are needed.

Usage: python app/tools/pack_logo_animation.py path/to/traced-motion.json
"""
import argparse
import json
from pathlib import Path
import struct
import zlib


def pack(source, destination):
    frames = json.loads(source.read_text())
    if len(frames) != 540:
        raise ValueError("Expected the restored 540-frame, 30 fps animation")
    data = bytearray(b"MGL1" + struct.pack(">HH", 30, len(frames)))
    for frame in frames:
        groups = [(0.0, frame["outline"])]
        groups.extend(((1 - dot["color"][0]) * 255 / 223, dot["contours"])
                      for dot in frame["dots"])
        payload = bytearray(struct.pack(">H", len(groups)))
        for strength, contours in groups:
            payload.extend(struct.pack(">fH", strength, len(contours)))
            for contour in contours:
                payload.extend(struct.pack(">H", len(contour)))
                for command, points in contour:
                    if command not in ("M", "L", "C"):
                        raise ValueError(f"Unsupported contour command: {command}")
                    payload.extend(command.encode("ascii"))
                    for x, y in points:
                        payload.extend(struct.pack(">ff", x, 420 - y))
        data.extend(struct.pack(">I", len(payload)))
        data.extend(payload)
    # Qt qCompress format: uncompressed byte count followed by a zlib stream.
    destination.write_bytes(struct.pack(">I", len(data)) + zlib.compress(data, 9))
    print(f"Packed {len(frames)} vector frames: {destination.stat().st_size:,} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1]
                        / "resources/brand/animated-symbol.mgl")
    args = parser.parse_args()
    pack(args.source, args.output)
