#!/usr/bin/env python3
"""Check retained VRAD BSP replay reproduces the original pose render."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

binary, vmf = sys.argv[1:3]
game = os.environ.get("APEX_P2CE_DIR")
if not game or not Path(game).is_dir():
    print("APEX_P2CE_DIR not set; skipping")
    sys.exit(77)
with tempfile.TemporaryDirectory(dir="/tmp/opencode") as temp:
    root = Path(temp)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", TMPDIR=temp)
    base = [binary, "--vmf", vmf, "--game", game, "--width", "320", "--height", "180",
            "--mode", "vrad", "--vrad-scale", "1", "--vrad-mask", "--no-specular", "--pose", "p",
            "64", "64", "64", "0", "45", "0"]
    fresh = subprocess.run(base + ["--vrad-flags", "-final", "--keep-vrad", "--out", str(root / "fresh")],
                           env=env, capture_output=True, text=True, check=True, timeout=120)
    match = re.search(r"^vrad directory=(.*)$", fresh.stdout, re.MULTILINE)
    if not match:
        raise RuntimeError("VRAD did not report its retained directory")
    bsps = list(Path(match.group(1)).rglob("*.bsp"))
    if len(bsps) != 1:
        raise RuntimeError(f"expected one BSP, got {bsps}")
    cached = subprocess.run(base + ["--vrad-bsp", str(bsps[0]), "--out", str(root / "cached")],
                            env=env, capture_output=True, text=True, check=True, timeout=120)
    assert "vrad cached-bsp=" in cached.stdout
    assert (root / "fresh/p-vrad.png").read_bytes() == (root / "cached/p-vrad.png").read_bytes()
    assert (root / "fresh/p-vrad-mask.png").read_bytes() == (root / "cached/p-vrad-mask.png").read_bytes()
    try:
        from PIL import Image
    except ImportError:
        print("Pillow unavailable; skipping pixel coverage assertion")
    else:
        values = set(Image.open(root / "fresh/p-vrad-mask.png").convert("L").getdata())
        assert 255 in values and values <= {0, 255}, f"expected valid binary mask pixels, got {values}"
print("retained VRAD replay is byte-identical")
