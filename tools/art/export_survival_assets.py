#!/usr/bin/env python3
"""Export image-model masters into runtime canvases; no artwork is synthesized.

This only slices, aligns and downsamples existing alpha artwork. Generated
masters and prompts remain unchanged in assets/source/.
"""
import json
from pathlib import Path
from PIL import Image, PngImagePlugin

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "assets"
SRGB = PngImagePlugin.PngInfo()
SRGB.add(b"sRGB", b"\x00")


def cell(sheet, col, row, cols=4, rows=2):
    w, h = sheet.size
    return (round(col*w/cols), round(row*h/rows), round((col+1)*w/cols), round((row+1)*h/rows))


def save(image, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, pnginfo=SRGB, optimize=True)


def fit(cut, canvas, footprint, baseline):
    bounds = cut.getchannel("A").point(lambda a: 255 if a >= 12 else 0).getbbox()
    assert bounds, "empty generated sprite"
    cut = cut.crop(bounds)
    scale = min(footprint/cut.width, (baseline-2)/cut.height)
    resized = cut.resize((max(1,round(cut.width*scale)), max(1,round(cut.height*scale))), Image.Resampling.LANCZOS)
    output = Image.new("RGBA", canvas)
    output.alpha_composite(resized, ((canvas[0]-resized.width)//2, baseline-resized.height))
    return output, bounds


def export():
    sheet = Image.open(ASSETS / "source/survival-world.png").convert("RGBA")
    manifest = {"model": "image_gen", "date": "2026-11-11", "sprites": [], "icons": [], "tools": []}
    # Common 32px-per-tile canvases; the modest branches are smaller than a bush.
    props = [("branch", 0, 0, 1, (32,20), 25, 15),
             ("branch", 1, 3, 1, (32,20), 25, 15),
             ("stump", 0, 1, 1, (32,28), 22, 23),
             ("rubble", 0, 2, 1, (32,20), 24, 15)]
    # Model columns 0/2/1/3 correspond to front-right/back-right/back-left/front-left
    # after observing their actual long-axis diagonals. No mirroring is used.
    props += [("chest", view, col, 0, (48,40), 34, 31) for view,col in enumerate((0,2,1,3))]
    for name, variant, col, row, size, width, baseline in props:
        box=cell(sheet,col,row)
        image,bounds=fit(sheet.crop(box),size,width,baseline)
        path=f"sprites/{name}_{variant}.png"
        save(image,ASSETS/path)
        # Isometric props anchor at the center of their ground footprint,
        # above the nearest bottom corner. Anchoring to the canvas silhouette's
        # lowest pixel makes wide props appear suspended above their shadows.
        anchor_y={"branch":10,"stump":21,"rubble":12,"chest":27}[name]
        manifest["sprites"].append({"path":path,"size":size,"anchor":[size[0]//2,anchor_y],"source":"source/survival-world.png","source_rect":box,"content_rect":bounds})
    icons=Image.open(ASSETS/"source/survival-icons.png").convert("RGBA")
    for i,name in enumerate(("wood","stone","fiber","rope","axe","pickaxe","chest")):
        box=cell(icons,i%4,i//4)
        image,bounds=fit(icons.crop(box),(32,32),28,30)
        path=f"icons/{name}.png";save(image,ASSETS/path)
        manifest["icons"].append({"path":path,"size":[32,32],"source":"source/survival-icons.png","source_rect":box,"content_rect":bounds})
    tools=Image.open(ASSETS/"source/survival-tools.png").convert("RGBA")
    for col,name in enumerate(("axe","pickaxe")):
        box=cell(tools,col,0,2,1)
        # Keep a common full cell and centered shaft so both hand grips align.
        image=tools.crop(box).resize((128,128),Image.Resampling.LANCZOS)
        path=f"tools/{name}.png";save(image,ASSETS/path)
        manifest["tools"].append({"path":path,"size":[128,128],"source":"source/survival-tools.png","source_rect":box,"shaft_u":0.5})
    (ASSETS/"survival-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    print(f"Exported {len(props)} world sprites, 7 icons and 2 held-tool sprites from image-model masters")


if __name__ == "__main__":
    export()
