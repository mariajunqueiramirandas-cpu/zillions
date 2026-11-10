"""Encoder checks; run with python3 -m unittest discover -s tests -p test_record_demo.py."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image, ImageDraw, ImageSequence

SPEC = importlib.util.spec_from_file_location("record_demo", Path(__file__).resolve().parents[1] / "tools/record_demo.py")
RECORDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RECORDER)


class GifRecorderTests(unittest.TestCase):
    def make_frames(self, directory, count=24):
        records = []
        for i in range(count):
            image = Image.new("RGB", (64, 64), (104, 111, 63))
            ImageDraw.Draw(image).rectangle((i % 48, 20, i % 48 + 8, 40), fill=(160 + i % 60, 40, 35))
            filename = f"frame_{i:05d}.png"
            image.save(directory / filename)
            records.append({"file": filename, "time": i / 24, "stage": "Walk", "speed": 1.5, "phase": i / count})
        metadata = {"fps": 24, "width": 64, "height": 64, "style": "painted", "frames": records}
        (directory / "frames.json").write_text(json.dumps(metadata))

    def test_animation_timing_loop_and_stable_palette(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.make_frames(directory)
            output = directory / "demo.gif"
            metadata = RECORDER.encode_gif(directory, output, captions=False)
            self.assertEqual(metadata["gif_duration_ms"], 1000)
            backgrounds, durations = set(), []
            with Image.open(output) as animation:
                self.assertEqual(animation.info["loop"], 0)
                self.assertEqual(animation.n_frames, 24)
                for frame in ImageSequence.Iterator(animation):
                    self.assertEqual(frame.size, (64, 64))
                    backgrounds.add(frame.convert("RGB").getpixel((63, 63)))
                    durations.append(frame.info["duration"])
            self.assertEqual(sum(durations), 1000)
            self.assertEqual(len(backgrounds), 1)
            self.assertEqual(set(durations), {40, 50})

    def test_captions_extend_the_canvas(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.make_frames(directory, 3)
            output = directory / "demo.gif"
            RECORDER.encode_gif(directory, output)
            with Image.open(output) as animation:
                self.assertEqual(animation.size, (64, 64 + RECORDER.FOOTER))
                self.assertNotEqual(animation.convert("RGB").getpixel((0, 0)), animation.convert("RGB").getpixel((0, 70)))

    def test_failed_encoding_preserves_existing_gif_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.make_frames(directory, 60)
            (directory / "frame_00001.png").unlink()  # Not sampled when building the palette.
            output = directory / "demo.gif"
            output.write_bytes(b"previous recording")
            with self.assertRaises(FileNotFoundError):
                RECORDER.encode_gif(directory, output)
            self.assertEqual(output.read_bytes(), b"previous recording")
            self.assertEqual(list(directory.glob(".demo-*.gif")), [])

    def test_rig_colours_survive_gif_quantization(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.make_frames(directory, 3)
            metadata = json.loads((directory / "frames.json").read_text())
            for frame in metadata["frames"]:
                frame["rig"] = {"left_arm": [[10, 10], [15, 15], [15, 25]], "right_arm": [[45, 10], [40, 15], [40, 25]]}
            (directory / "frames.json").write_text(json.dumps(metadata))
            output = directory / "rig.gif"
            RECORDER.encode_gif(directory, output, captions=False, rig_overlay=True)
            with Image.open(output) as animation:
                image = animation.convert("RGB")
                left, right = image.getpixel((15, 15)), image.getpixel((40, 15))
                self.assertGreater(left[1] - left[0], 70)
                self.assertGreater(left[2] - left[0], 70)
                self.assertGreater(right[0] - right[1], 50)
                self.assertGreater(right[1] - right[2], 50)


if __name__ == "__main__":
    unittest.main()
