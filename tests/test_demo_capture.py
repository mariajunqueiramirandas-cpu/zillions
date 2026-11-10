"""Opt-in GPU integration tests; set ZL_GAME_BINARY and run with a display."""
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BINARY = os.environ.get("ZL_GAME_BINARY")


@unittest.skipUnless(BINARY, "set ZL_GAME_BINARY to run the in-engine capture checks")
class DemoCaptureTests(unittest.TestCase):
    def capture(self, directory, *options):
        command = [str(Path(BINARY).resolve()), "--record-demo", str(directory), "--seed", "7", "--size", "256",
                   "--resolution", "1", "--width", "192", "--height", "144", "--zoom", "96",
                   "--demo-seconds", "0.5", "--demo-fps", "12", "--no-vsync", *options]
        return subprocess.run(command, capture_output=True, text=True, timeout=60)

    def test_capture_is_repeatable_and_logs_real_rig_frames(self):
        with tempfile.TemporaryDirectory() as name:
            directories = [Path(name) / "first", Path(name) / "second"]
            for directory in directories:
                result = self.capture(directory)
                self.assertEqual(result.returncode, 0, result.stderr)
            first, second = [json.loads((directory / "frames.json").read_text()) for directory in directories]
            self.assertEqual(first, second)
            self.assertEqual(first["simulation_hz"], 60)
            self.assertEqual(len(first["frames"]), 6)
            self.assertTrue(any(frame["speed"] > 0 for frame in first["frames"]))
            for record in first["frames"]:
                self.assertTrue(0 <= record["phase"] < 1)
                self.assertTrue(0 <= record["body_activity"] <= 1)
                self.assertEqual(len(record["rig"]["left_arm"]), 4)
                for points in record["rig"].values():
                    self.assertTrue(all(math.isfinite(value) for point in points for value in point))
                hashes = [hashlib.sha256((directory / record["file"]).read_bytes()).digest() for directory in directories]
                self.assertEqual(hashes[0], hashes[1])

    def test_fractional_duration_does_not_add_an_extra_frame(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name) / "frames"
            result = self.capture(directory, "--demo-seconds", "0.1", "--demo-fps", "20")
            self.assertEqual(result.returncode, 0, result.stderr)
            metadata = json.loads((directory / "frames.json").read_text())
            self.assertEqual(len(metadata["frames"]), 2)

    def test_existing_recording_directory_is_preserved(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            sentinel = directory / "keep.txt"
            sentinel.write_text("keep this")
            result = self.capture(directory)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(sentinel.read_text(), "keep this")
            self.assertEqual(list(directory.iterdir()), [sentinel])

    def test_invalid_recording_parameters_fail_before_capture(self):
        for options in [("--demo-fps", "0"), ("--demo-seconds", "nan"), ("--no-character",)]:
            with tempfile.TemporaryDirectory() as name:
                directory = Path(name) / "frames"
                result = self.capture(directory, *options)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(directory.exists())


if __name__ == "__main__":
    unittest.main()
