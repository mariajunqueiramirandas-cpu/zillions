#!/usr/bin/env python3
"""Record an actual in-engine player demo and encode a looping, captioned GIF.

Requires Pillow (as do the artwork tools). PNG capture uses the game's normal
renderer and 60 Hz player controller; encoding uses one palette for the clip.
"""
import argparse
from contextlib import nullcontext
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FOOTER = 48


def frame_image(directory, record, metadata, captions, rig_overlay=False):
    with Image.open(directory / record["file"]) as source:
        scene = source.convert("RGB")
    if scene.size != (metadata["width"], metadata["height"]):
        raise ValueError(f"Unexpected dimensions in {record['file']}")
    if rig_overlay:
        if "rig" not in record:
            raise ValueError("Rig overlay requires a capture from the current game build")
        draw = ImageDraw.Draw(scene, "RGBA")
        for name, points in record["rig"].items():
            color = (80, 220, 235, 220) if name.startswith("left") else (250, 163, 70, 220)
            if name == "spine":
                color = (235, 227, 202, 220)
            points = [tuple(point) for point in points]
            draw.line(points, fill=color, width=2)
            for x, y in points:
                draw.ellipse((x - 2, y - 2, x + 2, y + 2), fill=color)
    if not captions:
        return scene
    result = Image.new("RGB", (scene.width, scene.height + FOOTER), (35, 35, 29))
    result.paste(scene, (0, 0))
    draw = ImageDraw.Draw(result)
    draw.line((0, scene.height, scene.width, scene.height), fill=(87, 83, 67))
    title = record["stage"] + (" | rig overlay" if rig_overlay else "")
    draw.text((12, scene.height + 5), title, font=ImageFont.load_default(size=15), fill=(235, 227, 202))
    detail = f"{metadata['style']} | {record['time']:.2f}s | gait {record['speed']:.2f} m/s | phase {record['phase']:.2f}"
    draw.text((12, scene.height + 26), detail, font=ImageFont.load_default(size=12), fill=(184, 181, 163))
    return result


def encode_gif(directory, output, captions=True, colors=128, rig_overlay=False):
    metadata = json.loads((directory / "frames.json").read_text())
    records = metadata["frames"]
    fps = metadata["fps"]
    if not records or not 1 <= fps <= 50:
        raise ValueError("GIF input needs frames at 1..50 fps")
    for record in records:
        if Path(record["file"]).name != record["file"]:
            raise ValueError("Frame filenames must stay in the recording directory")

    # A shared palette prevents the scenery/character colours changing between
    # frames. Include samples throughout every gait, not just the first pose.
    count = min(24, len(records))
    sheet = Image.new("RGB", (160 * count, 132), (35, 35, 29))
    for i in range(count):
        index = round(i * (len(records) - 1) / max(count - 1, 1))
        sample = frame_image(directory, records[index], metadata, captions, rig_overlay)
        sample.thumbnail((160, 132), Image.Resampling.LANCZOS)
        sheet.paste(sample, (160 * i, 0))
    palette = sheet.quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    if rig_overlay:
        # Thin, saturated debug lines disappear when palette training sees
        # only downsampled scenery. Reserve their colours explicitly.
        entries = palette.getpalette()
        for i, color in enumerate(((80, 220, 235), (250, 163, 70), (235, 227, 202))):
            offset = (colors - 3 + i) * 3
            entries[offset:offset + 3] = color
        palette.putpalette(entries)

    def images():
        for record in records:
            frame = frame_image(directory, record, metadata, captions, rig_overlay)
            yield frame.quantize(palette=palette, dither=Image.Dither.NONE)

    # GIF delays are centiseconds. Distribute rounding across frames so rates
    # such as 24 fps retain the requested total duration rather than drifting.
    delays = [(round((i + 1) * 100 / fps) - round(i * 100 / fps)) * 10 for i in range(len(records))]
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".demo-", suffix=".gif", dir=output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        sequence = images()
        first = next(sequence)
        first.save(temporary, format="GIF", save_all=True, append_images=sequence,
                   duration=delays, loop=0, disposal=1, optimize=False)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    metadata["gif"] = output.name
    metadata["gif_duration_ms"] = sum(delays)
    metadata["captions"] = captions
    metadata["gif_palette_colors"] = colors
    metadata["rig_overlay"] = rig_overlay
    return metadata


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="output .gif (a JSON state log is saved beside it)")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/game" / ("zillions.exe" if os.name == "nt" else "zillions"))
    parser.add_argument("--seconds", type=float, default=8.0, help="clip duration, 0.1..30 (default 8)")
    parser.add_argument("--fps", type=int, default=20, help="GIF frame rate, 1..50 (default 20)")
    parser.add_argument("--width", type=int, default=512, help="scene width (default 512)")
    parser.add_argument("--height", type=int, default=384, help="scene height, excluding captions (default 384)")
    parser.add_argument("--zoom", type=float, default=128, help="pixels per tile (default 128)")
    parser.add_argument("--colors", type=int, choices=(64, 128, 256), default=128, help="shared GIF palette size (default 128)")
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--size", type=int, default=256)
    parser.add_argument("--resolution", type=int, choices=(1, 2, 4), default=1)
    parser.add_argument("--style", choices=("painted", "smooth"), default="painted")
    parser.add_argument("--look", type=int, default=0, help="seeded look, 0 for the hero")
    parser.add_argument("--hood", action="store_true")
    parser.add_argument("--no-captions", action="store_true")
    parser.add_argument("--rig-overlay", action="store_true", help="overlay the actual joints: cyan left, orange right")
    parser.add_argument("--keep-frames", type=Path, help="retain PNGs in this new or empty directory")
    args = parser.parse_args()
    if args.output.suffix.lower() != ".gif":
        parser.error("output must have a .gif extension")
    if not math.isfinite(args.seconds) or not 0.1 <= args.seconds <= 30 or not 1 <= args.fps <= 50:
        parser.error("use 0.1..30 seconds and 1..50 fps")
    if not 64 <= args.width <= 2048 or not 64 <= args.height <= 2048 or not math.isfinite(args.zoom) or not 1 <= args.zoom <= 256:
        parser.error("use dimensions 64..2048 and zoom 1..256")
    if args.size < 64 * args.resolution or args.size > 4096 or not 0 <= args.seed < 2**64 or not 0 <= args.look < 2**64:
        parser.error("invalid map size or seed")
    pixels = args.width * (args.height + (0 if args.no_captions else FOOTER)) * math.ceil(args.seconds * args.fps - 1e-9)
    if pixels > 256_000_000:
        parser.error("clip is too large; reduce dimensions, frame rate or duration")
    if not args.binary.is_file():
        parser.error(f"game binary not found: {args.binary}; build the game first or pass --binary")
    return args


def write_metadata(path, metadata):
    """Keep a capture's state log readable as one line per rendered frame."""
    with path.open("w", encoding="utf-8") as stream:
        stream.write("{\n")
        for key, value in metadata.items():
            if key != "frames":
                stream.write(f"  {json.dumps(key)}: {json.dumps(value)},\n")
        stream.write('  "frames": [\n')
        for i, frame in enumerate(metadata["frames"]):
            stream.write((",\n" if i else "") + "    " + json.dumps(frame, separators=(",", ":")))
        stream.write("\n  ]\n}\n")


def main():
    args = arguments()
    output = args.output.resolve()
    command = [str(args.binary.resolve()), "--record-demo", "DIRECTORY", "--demo-seconds", str(args.seconds),
               "--demo-fps", str(args.fps), "--width", str(args.width), "--height", str(args.height),
               "--zoom", str(args.zoom), "--seed", str(args.seed), "--size", str(args.size),
               "--resolution", str(args.resolution), "--character-style", args.style, "--look", str(args.look), "--no-vsync"]
    if args.hood:
        command.append("--hood")
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY"):
        xvfb = shutil.which("xvfb-run")
        if not xvfb:
            raise RuntimeError("No display: install Xvfb or run this tool with DISPLAY set")
        prefix = [xvfb, "-a", "-s", f"-screen 0 {max(800, args.width)}x{max(600, args.height)}x24 -nolisten tcp"]
    else:
        prefix = []
    context = nullcontext(str(args.keep_frames.resolve())) if args.keep_frames else tempfile.TemporaryDirectory(prefix="zillions-demo-")
    with context as temporary:
        directory = Path(temporary) / "frames" if not args.keep_frames else Path(temporary)
        command[2] = str(directory)
        subprocess.run(prefix + command, cwd=ROOT, check=True)
        metadata = encode_gif(directory, output, captions=not args.no_captions, colors=args.colors, rig_overlay=args.rig_overlay)
        metadata["frames_directory"] = str(directory) if args.keep_frames else None
        write_metadata(output.with_suffix(".json"), metadata)
    print(f"Wrote {output}: {len(metadata['frames'])} rendered frames, {metadata['gif_duration_ms'] / 1000:.2f}s, looping")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"record_demo: {error}", file=sys.stderr)
        sys.exit(1)
