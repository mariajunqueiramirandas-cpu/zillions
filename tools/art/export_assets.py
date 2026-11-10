#!/usr/bin/env python3
"""Slice image-model source sheets into the canvases in docs/art-direction.md.

Requires Pillow. Run from any directory; outputs are relative to the repository.
This is export/packing only: all painted content comes from assets/source/.
"""
import json
from pathlib import Path

from PIL import Image, ImageDraw, PngImagePlugin

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "assets"
PREVIEWS = ROOT / "docs/art"
SRGB = PngImagePlugin.PngInfo()
SRGB.add(b"sRGB", b"\x00")


def save(image, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, pnginfo=SRGB, optimize=True)


def cell(sheet, col, row, cols, rows):
    w, h = sheet.size
    return (round(col * w / cols), round(row * h / rows),
            round((col + 1) * w / cols), round((row + 1) * h / rows))


def export():
    manifest = {"authoring_pixels_per_tile": 32,
                "sprite_alpha": "straight; premultiply once when uploading to the renderer",
                "ground": [], "sprites": []}
    sheet = Image.open(ASSETS / "source/ground.png").convert("RGB")
    names = ["grass", "forest", "dirt", "rocks", "sand", "mountain", "shallows"]
    for i, name in enumerate(names):
        box = cell(sheet, i % 4, i // 4, 4, 2)
        # Avoid sampling the neighbouring material at fractional sheet divisions.
        box = (box[0] + 3, box[1] + 3, box[2] - 3, box[3] - 3)
        image = sheet.crop(box).resize((256, 256), Image.Resampling.LANCZOS)
        path = f"ground/{name}.png"
        save(image, ASSETS / path)
        manifest["ground"].append({"path": path, "source": "source/ground.png",
                                   "source_rect": box, "size": [256, 256]})

    # name, count, canvas, anchor, source sheet, row, grid, source footprint
    families = [
        ("broadleaf", 6, (64, 76), (32, 66), "trees", 0, (6, 2), 0.98),
        ("conifer", 4, (44, 80), (22, 71), "trees", 1, (6, 2), 0.98),
        ("bush", 3, (34, 26), (17, 20), "plants", 0, (4, 5), 0.92),
        ("stone", 3, (46, 40), (23, 27), "rocks", 0, (4, 4), 0.83),
        ("iron", 3, (46, 40), (23, 27), "rocks", 1, (4, 4), 0.83),
        ("gold", 3, (46, 40), (23, 27), "rocks", 2, (4, 4), 0.83),
        ("boulder", 4, (46, 44), (23, 31), "rocks", 3, (4, 4), 0.83),
        ("tuft", 4, (20, 16), (10, 14), "plants", 1, (4, 5), 0.98),
        ("flower", 4, (20, 16), (10, 14), "plants", 2, (4, 5), 0.98),
        ("reed", 3, (20, 30), (10, 28), "plants", 3, (4, 5), 0.98),
        ("pebble", 3, (22, 14), (11, 9), "plants", 4, (4, 5), 0.83),
    ]
    for name, count, size, anchor, source, row, grid, footprint in families:
        sheet = Image.open(ASSETS / f"source/{source}.png").convert("RGBA")
        for col in range(count):
            box = cell(sheet, col, row, *grid)
            if source == "plants":
                # The model's plant rows are not evenly spaced. These measured
                # boundaries keep tall reeds out of the flower row's crops.
                bands = ((0, 300), (300, 550), (550, 780), (780, 1180), (1180, 1402))
                box = (box[0], bands[row][0], box[2], bands[row][1])
            cut = sheet.crop(box)
            # Ignore near-invisible source matte when locating the painted object.
            bounds = cut.getchannel("A").point(lambda a: 255 if a >= 24 else 0).getbbox()
            assert bounds, (name, col)
            x0, y0, x1, y1 = bounds
            bounds = (max(0, x0 - 2), max(0, y0 - 2),
                      min(cut.width, x1 + 2), min(cut.height, y1 + 2))
            cut = cut.crop(bounds)
            sx, sy = cut.width / 2, cut.height * footprint
            # Trees have visible trunk bases, measured on the source sheet.
            if source == "trees":
                sx = sheet.width / 12 - bounds[0]
                sy = (414 if row == 0 else 942 - box[1]) - bounds[1]
            ax, ay = anchor
            w, h = size
            scale = min((ax - 1) / sx, (w - ax - 1) / (cut.width - sx),
                        (ay - 1) / sy, (h - ay - 1) / (cut.height - sy))
            resized = cut.resize((max(1, round(cut.width * scale)),
                                  max(1, round(cut.height * scale))), Image.Resampling.LANCZOS)
            image = Image.new("RGBA", size)
            image.alpha_composite(resized, (round(ax - sx * scale), round(ay - sy * scale)))
            # PNG stores straight alpha; transparent texels carry no hidden colour.
            pixels = image.load()
            for y in range(h):
                for x in range(w):
                    if pixels[x, y][3] == 0:
                        pixels[x, y] = (0, 0, 0, 0)
            path = f"sprites/{name}_{col}.png"
            save(image, ASSETS / path)
            manifest["sprites"].append({"path": path, "family": name,
                                         "variant": col, "size": size, "anchor": anchor,
                                         "source": f"source/{source}.png",
                                         "source_rect": box, "content_rect": bounds,
                                         "source_anchor": [sx, sy]})
    (ASSETS / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    previews(manifest)


def previews(manifest):
    grass = Image.open(ASSETS / "ground/grass.png").convert("RGBA")
    # Identical composition at 8, 16, 32 and 64 screen pixels per tile.
    for ppt in (8, 16, 32, 64):
        scale = ppt / 32
        cw, ch = max(88, round(150 * scale)), max(70, round(120 * scale))
        board = Image.new("RGBA", (cw * 6, ch * 7 + 40), "#1A1610")
        d = ImageDraw.Draw(board)
        d.text((12, 12), f"ZILLIONS / {ppt} pixels per tile / painted scenery", fill="#D8CFB4")
        tile_size = max(1, round(256 * scale))
        ground = grass.resize((tile_size, tile_size), Image.Resampling.LANCZOS)
        for i, item in enumerate(manifest["sprites"]):
            x, y = i % 6 * cw, i // 6 * ch + 40
            patch = Image.new("RGBA", (cw - 2, ch - 2))
            for ty in range(0, ch, tile_size):
                for tx in range(0, cw, tile_size):
                    patch.paste(ground, (tx, ty))
            board.paste(patch, (x, y))
            sprite = Image.open(ASSETS / item["path"])
            sprite = sprite.resize((max(1, round(sprite.width * scale)),
                                    max(1, round(sprite.height * scale))), Image.Resampling.LANCZOS)
            ax, ay = item["anchor"]
            base_y = y + ch - 23
            board.alpha_composite(sprite, (round(x + cw / 2 - ax * scale),
                                            round(base_y - ay * scale)))
            d.text((x + 5, y + ch - 17), Path(item["path"]).stem, fill="#D8CFB4",
                   stroke_width=1, stroke_fill="#1A1610")
        save(board.convert("RGB"), PREVIEWS / f"sprites-{ppt}px.png")

    for repeats, zoom, suffix in ((1, 1, "swatches"), (3, 1, "tile-3x3"), (6, 0.25, "tile-6x6")):
        side = round(256 * repeats * zoom)
        board = Image.new("RGB", (side * 4, (side + 28) * 2), "#1A1610")
        d = ImageDraw.Draw(board)
        for i, item in enumerate(manifest["ground"]):
            image = Image.open(ASSETS / item["path"])
            tiled = Image.new("RGB", (256 * repeats, 256 * repeats))
            for y in range(repeats):
                for x in range(repeats):
                    tiled.paste(image, (x * 256, y * 256))
            tiled = tiled.resize((side, side), Image.Resampling.LANCZOS)
            x, y = i % 4 * side, i // 4 * (side + 28)
            board.paste(tiled, (x, y + 28))
            d.text((x + 8, y + 8), Path(item["path"]).stem, fill="#D8CFB4")
        save(board, PREVIEWS / f"ground-{suffix}.png")


if __name__ == "__main__":
    export()
