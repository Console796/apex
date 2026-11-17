#!/usr/bin/env python3
"""Measure how much a structural brush fragments vis (plan 0.8.9 P0, D16).

    python3 tools/structural_probe.py [--keep DIR]

The P2CE corpus is all instances, none of which seals, so vbsp writes no .prt for any of it. This probe
builds its own maps instead: a sealed 1024x1024x256 room with one extra brush, compiled once with the brush
structural and once as func_detail, with the native P2CE vbsp. The .prt header gives the cluster and portal
counts; the difference between the two compiles is what that one brush costs vis. Prints a table that
docs/plans/probe-results/structural_0_8_9_p0.md records. Requires APEX_P2CE_DIR; never writes under it.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

MATERIAL = "TILE/WHITE_WALL_TILE003A"
ROOM = ((-512, -512, 0), (512, 512, 256))
WALL = 16


def side(sid, points):
    plane = " ".join(f"({p[0]:g} {p[1]:g} {p[2]:g})" for p in points)
    return (f'\t\tside\n\t\t{{\n\t\t\t"id" "{sid}"\n\t\t\t"plane" "{plane}"\n\t\t\t"material" "{MATERIAL}"\n'
            f'\t\t\t"uaxis" "[1 0 0 0] 0.25"\n\t\t\t"vaxis" "[0 -1 0 0] 0.25"\n\t\t\t"rotation" "0"\n'
            f'\t\t\t"lightmapscale" "16"\n\t\t\t"smoothing_groups" "0"\n\t\t}}\n')


def box(ids, lo, hi):
    (x1, y1, z1), (x2, y2, z2) = lo, hi
    planes = [((x1, y2, z2), (x2, y2, z2), (x2, y1, z2)), ((x1, y1, z1), (x2, y1, z1), (x2, y2, z1)),
              ((x1, y2, z2), (x1, y1, z2), (x1, y1, z1)), ((x2, y2, z1), (x2, y1, z1), (x2, y1, z2)),
              ((x2, y2, z2), (x1, y2, z2), (x1, y2, z1)), ((x2, y1, z1), (x1, y1, z1), (x1, y1, z2))]
    text = f'\tsolid\n\t{{\n\t\t"id" "{next(ids)}"\n'
    for p in planes:
        text += side(next(ids), p)
    return text + "\t}\n"


def room(ids):
    (x1, y1, z1), (x2, y2, z2) = ROOM
    w = WALL
    return "".join(box(ids, lo, hi) for lo, hi in [
        ((x1 - w, y1 - w, z1 - w), (x2 + w, y2 + w, z1)), ((x1 - w, y1 - w, z2), (x2 + w, y2 + w, z2 + w)),
        ((x1 - w, y1, z1), (x1, y2, z2)), ((x2, y1, z1), (x2 + w, y2, z2)),
        ((x1, y1 - w, z1), (x2, y1, z2)), ((x1, y2, z1), (x2, y2 + w, z2))])


def vmf(extra, detail):
    ids = iter(range(1, 100000))
    world = room(ids) + ("" if detail or not extra else box(ids, *extra))
    entity = ""
    if extra and detail:
        entity = (f'entity\n{{\n\t"id" "{next(ids)}"\n\t"classname" "func_detail"\n' + box(ids, *extra) + "}\n")
    start = (f'entity\n{{\n\t"id" "{next(ids)}"\n\t"classname" "info_player_start"\n'
             f'\t"origin" "-448 -448 8"\n\t"angles" "0 0 0"\n}}\n')
    return (f'versioninfo\n{{\n\t"editorversion" "400"\n\t"mapversion" "1"\n\t"formatversion" "100"\n\t"prefab" "0"\n}}\n'
            f'world\n{{\n\t"id" "0"\n\t"mapversion" "1"\n\t"classname" "worldspawn"\n\t"skyname" "sky_black_nofog"\n'
            f"{world}}}\n{entity}{start}")


def variants():
    yield "baseline", None
    for s in (4, 8, 16, 32, 64, 128, 256):
        yield f"floor cube {s}", ((-s / 2, -s / 2, 0), (s / 2, s / 2, s))
    for s in (8, 16, 32, 64):
        yield f"floating cube {s}", ((-s / 2, -s / 2, 128 - s / 2), (s / 2, s / 2, 128 + s / 2))
    for w in (4, 8, 16, 32, 64, 128):
        yield f"pillar {w}", ((-w / 2, -w / 2, 0), (w / 2, w / 2, 256))
    for t in (1, 2, 4, 8, 16, 32):
        yield f"half wall {t}", ((-t / 2, -512, 0), (t / 2, 0, 256))
    for t in (1, 2, 4, 8, 16):
        yield f"floor slab {t}", ((-256, -256, 0), (256, 256, t))
    for h in (8, 16, 32, 64):
        yield f"trim 512x16x{h}", ((-256, -512, 0), (256, -496, h))


def compile_counts(vbsp, game, work, name, text):
    path = work / f"{name}.vmf"
    path.write_text(text)
    result = subprocess.run([str(vbsp), "-game", str(game), str(path)], cwd=work, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace")
    prt = path.with_suffix(".prt")
    if result.returncode or not prt.exists() or "LEAKED" in result.stdout:
        raise SystemExit(f"{name}: vbsp failed or leaked; see {work}")
    lines = prt.read_text().split("\n")
    return int(lines[1]), int(lines[2])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", type=Path, help="retain the VMFs, BSPs, PRTs and logs here")
    args = parser.parse_args()
    install = os.environ.get("APEX_P2CE_DIR")
    if not install:
        parser.error("APEX_P2CE_DIR is unset")
    install = Path(install).resolve()
    vbsp, game = install / "bin/linux64/vbsp", install / "p2ce"
    work = Path(tempfile.mkdtemp(prefix="apex-structural-probe-"))
    try:
        print("| Brush | Structural clusters / portals | As func_detail | Portals added |")
        print("| --- | --- | --- | --- |")
        for i, (label, extra) in enumerate(variants()):
            structural = compile_counts(vbsp, game, work, f"s{i:02d}", vmf(extra, False))
            detail = compile_counts(vbsp, game, work, f"d{i:02d}", vmf(extra, True))
            print(f"| {label} | {structural[0]} / {structural[1]} | {detail[0]} / {detail[1]} | "
                  f"{structural[1] - detail[1]} |", flush=True)
    finally:
        if args.keep:
            shutil.copytree(work, args.keep, dirs_exist_ok=True)
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
