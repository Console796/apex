#!/usr/bin/env python3
"""Image test for apex_render_pose --compare and --density (0.7.5 D7).

Renders one pose of a small lit room, then checks that the compare and density images differ from the plain
ones and that each is reproducible byte for byte. Needs a GPU and APEX_P2CE_DIR (the game's materials);
exits 77 (ctest SKIP) without them. Usage: render_pose_compare.py RENDER_POSE_BINARY ROOM_VMF
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77


def render(binary, vmf, game, out, *extra):
    pose = ["--pose", "p", "-120", "-100", "100", "20", "40", "0"]
    command = [binary, "--vmf", vmf, "--game", game, "--width", "320", "--height", "180", *pose,
               "--bake-passes", "2", "--bake-bounces", "0", "--out", str(out), *extra]
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"render_pose failed ({result.returncode}): {result.stderr[-2000:]}")


def main():
    binary, vmf = sys.argv[1:3]
    game = os.environ.get("APEX_P2CE_DIR")
    if not game or not Path(game).is_dir():
        print("APEX_P2CE_DIR not set; skipping")
        return SKIP
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        runs = {
            "plain": ("p-baked.png", "--mode", "baked"),
            "density": ("p-baked-density.png", "--mode", "baked", "--density"),
            "compare": ("p-compare.png", "--compare", "lit,baked"),
            "wide": ("p-compare.png", "--compare", "lit,baked,0.25"),
            "lit": ("p.png",),
        }
        images = {}
        for name, (file, *args) in runs.items():
            for attempt in (1, 2):
                out = root / f"{name}{attempt}"
                render(binary, vmf, game, out, *args)
                images[(name, attempt)] = (out / file).read_bytes()
            if images[(name, 1)] != images[(name, 2)]:
                sys.exit(f"{name} image is not deterministic")
        for name in ("density", "compare", "wide"):
            if images[(name, 1)] == images[("plain", 1)]:
                sys.exit(f"{name} image equals the plain baked image")
        if images[("compare", 1)] == images[("lit", 1)]:
            sys.exit("compare image equals the lit image")
        if images[("compare", 1)] == images[("wide", 1)]:
            sys.exit("the split fraction did not change the compare image")
    print("compare and density images differ from plain and are deterministic")
    return 0


sys.exit(main())
