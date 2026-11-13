#!/usr/bin/env python3
"""Regenerate the D10 overlay oracle (plan 0.8.6) from the real VBSP.

    python3 tools/overlay_oracle.py [--reader build/linux-gcc-debug/apex_overlay_reference] [--keep DIR]

Every corpus VMF under $APEX_P2CE_DIR/sdk_content/maps with an info_overlay that lists at least one
face is copied to a temporary directory, compiled with the native P2CE vbsp (-instancepath set to
the corpus, so nested instances resolve) and read back with apex_overlay_reference (bsppp). The
OVERLAYS lump entries and the compiled faces each one lists are written to
tests/fixtures/vmf/overlay/corpus_overlays.vbsp.txt, which tests/document/overlay_oracle_test.cpp
checks against Apex's own parse and clip of the same VMFs.

None of these maps has a displacement, so a leak does not crash vbsp; leaking maps compile without
vis, which does not touch overlays. Requires APEX_P2CE_DIR; never writes under it.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "tests/fixtures/vmf/overlay/corpus_overlays.vbsp.txt"
OVERLAY = re.compile(r'"classname"\s+"info_overlay"', re.I)
SIDES = re.compile(r'"sides"\s+"\s*\d')


def has_listed_overlay(text):
    # An entity block naming info_overlay and a non-empty sides key.
    for block in re.split(r'\n\s*entity\s*\n', text)[1:]:
        if OVERLAY.search(block) and SIDES.search(block):
            return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reader", type=Path, default=ROOT / "build/linux-gcc-debug/apex_overlay_reference")
    parser.add_argument("--keep", type=Path, help="retain the VMFs, BSPs and logs here")
    args = parser.parse_args()
    install = os.environ.get("APEX_P2CE_DIR")
    if not install:
        parser.error("APEX_P2CE_DIR is unset")
    install = Path(install).resolve()
    maps = install / "sdk_content/maps"
    vbsp = install / "bin/linux64/vbsp"
    work = Path(tempfile.mkdtemp(prefix="apex-overlay-oracle-"))
    lines = ["# Apex-authored D10 oracle, written by tools/overlay_oracle.py from the P2CE corpus and the native vbsp.",
             "# map <path under sdk_content/maps>; overlay <id> <texinfo> <unknown0> <unknown1> range <U0 U1 V0 V1>",
             "# uv <4 x (x y)> basisu <xyz> origin <xyz> normal <xyz>; face <bsp index> normal <xyz> points <xyz...>;",
             "# skip <path> <vbsp's last line> for a map that does not compile."]
    compiled = 0
    try:
        for path in sorted(maps.rglob("*.vmf")):
            text = path.read_bytes().decode("latin-1")
            if not has_listed_overlay(text):
                continue
            rel = path.relative_to(maps).as_posix()
            name = f"m{compiled:02d}_{path.stem}"
            vmf = work / f"{name}.vmf"
            shutil.copyfile(path, vmf)
            result = subprocess.run([str(vbsp), "-game", str(install / "p2ce"), "-instancepath", str(maps), str(vmf)],
                                    cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    errors="replace")
            (work / f"{name}.vbsp.log").write_text(result.stdout)
            bsp = vmf.with_suffix(".bsp")
            if result.returncode or not bsp.exists():
                # Corpus maps that reference an instance the corpus lacks do not compile.
                last = [l for l in result.stdout.splitlines() if l.strip()][-1:] or ["no output"]
                lines.append(f"skip {rel} {last[0].strip()}")
                print(f"{rel}: vbsp failed: {last[0].strip()}")
                continue
            dump = subprocess.run([str(args.reader), str(bsp)], check=True, stdout=subprocess.PIPE, text=True).stdout
            lines.append(f"map {rel}")
            lines += [line for line in dump.splitlines() if line.split()[0] in ("overlay", "face")]
            compiled += 1
            print(f"{rel}: {sum(1 for l in dump.splitlines() if l.startswith('overlay'))} overlays")
    finally:
        if args.keep:
            shutil.copytree(work, args.keep, dirs_exist_ok=True)
        shutil.rmtree(work, ignore_errors=True)
    OUTPUT.write_text("\n".join(lines) + "\n")
    print(f"{compiled} maps -> {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
