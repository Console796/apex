#!/usr/bin/env python3
"""Focused tests for the offline VRAD analysis helpers (NumPy and Pillow)."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("metrics", ROOT / "tools/vrad_image_metrics.py")
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)


class VradToolsTest(unittest.TestCase):
    def test_derived_lights_only_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / "source.vmf", Path(tmp) / "derived.vmf"
            original = b'"classname" "light_rt"\r\n"_light" "1 2 3 4"\r\n"classname" "light_rt_spot"\r\n'
            src.write_bytes(original)
            cmd = [sys.executable, str(ROOT / "tools/derive_vrad_lights.py"), str(src), str(dst)]
            subprocess.run(cmd, check=True, capture_output=True, timeout=5)
            self.assertEqual(dst.read_bytes(), original.replace(b'"light_rt"', b'"light"').replace(
                b'"light_rt_spot"', b'"light_spot"'))
            self.assertNotEqual(subprocess.run(cmd, capture_output=True, timeout=5).returncode, 0)
            self.assertEqual(src.read_bytes(), original)

    def test_masked_convolution_and_empty_mask(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ref = np.full((8, 8, 3), 100, dtype=np.uint8)
            candidate = ref.copy()
            candidate[0, 0] = 255
            mask = np.full((8, 8), 255, dtype=np.uint8)
            mask[0, 0] = 0
            for name, data in (("ref", ref), ("candidate", candidate), ("mask", mask)):
                Image.fromarray(data).save(root / f"{name}.png")
            paths = [root / f"{name}.png" for name in ("ref", "candidate", "mask")]
            result = metrics.evaluate(*paths, 1)
            self.assertEqual(result["linear"]["pixels"], 63)
            self.assertEqual(result["linear"]["rmse"], 0)
            self.assertEqual(result["coarse_linear"]["rmse"], 0)
            Image.fromarray(np.zeros((8, 8), dtype=np.uint8)).save(paths[2])
            with self.assertRaises(ValueError):
                metrics.evaluate(*paths, 1)


if __name__ == "__main__":
    unittest.main()
