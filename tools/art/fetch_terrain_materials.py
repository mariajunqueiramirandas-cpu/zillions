#!/usr/bin/env python3
"""Import unmodified 1K CC0 Poly Haven textures with verified provider hashes.

No credentials or paid API. Sprite authoring and source sheets are independent.
"""
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets/ground"
MATERIALS = {
    "grass": ("grass_ground", "Diffuse"), "forest": ("forest_ground_04", "Diffuse"),
    "dirt": ("brown_mud_dry", "Diffuse"), "rocks": ("rocks_ground_02", "Diffuse"),
    "sand": ("coast_sand_04", "Diffuse"), "shallows": ("coast_sand_01", "Diffuse"),
    "mountain": ("rock_face_03", "Diffuse"), "mountain_arm": ("rock_face_03", "arm"),
    "mountain_height": ("rock_face_03", "Displacement"),
}


def get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Zillions CC0 terrain material importer"})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    catalogue = json.loads(get("https://api.polyhaven.com/assets?t=textures"))
    metadata = {}
    manifest = {"provider": "Poly Haven", "license": "CC0-1.0",
                "license_url": "https://polyhaven.com/license", "resolution": 1024,
                "processing": "Unmodified PNGs; grading and projection happen in the renderer.", "materials": []}
    for name, (asset, component) in MATERIALS.items():
        if asset not in metadata:
            metadata[asset] = json.loads(get("https://api.polyhaven.com/files/" + asset))
        info = metadata[asset][component]["1k"]["png"]
        path = OUT / (name + ".png")
        contents = path.read_bytes() if path.exists() else b""
        if hashlib.md5(contents).hexdigest() != info["md5"]:
            contents = get(info["url"])
            if hashlib.md5(contents).hexdigest() != info["md5"]:
                raise RuntimeError("Provider checksum mismatch: " + asset + "/" + component)
            temporary = path.with_suffix(".tmp")
            temporary.write_bytes(contents)
            temporary.replace(path)
        manifest["materials"].append({"path": "ground/" + path.name, "asset": asset, "component": component,
            "source": "https://polyhaven.com/a/" + asset, "download": info["url"],
            "authors": catalogue[asset]["authors"], "license": "CC0-1.0", "provider_md5": info["md5"],
            "sha256": hashlib.sha256(contents).hexdigest(),
            "size": [1024, 1024], "bytes": len(contents)})
        print(name, component, len(contents), "bytes", flush=True)
    (OUT / "materials.json").write_text(json.dumps(manifest, indent=2) + "\n")
    path = ROOT / "assets/manifest.json"
    inventory = json.loads(path.read_text())
    inventory["ground"] = [entry for entry in manifest["materials"] if entry["component"] == "Diffuse"]
    path.write_text(json.dumps(inventory, indent=2) + "\n")


if __name__ == "__main__":
    main()
