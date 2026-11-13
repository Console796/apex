#!/usr/bin/env python3
"""Regenerate the D2 displacement oracle (plan 0.8 D2) from the real VBSP.

    python3 tools/disp_oracle.py [--reader build/linux-gcc-debug/apex_disp_reference] [--keep DIR]

For each variant of tests/fixtures/vmf/disp/test1_disp.vmf (the fixture as-is, then edits to its
dispinfo text: each base corner as the start, elevation, offsets, tilted offset normals), a copy is
sealed inside a hull room, compiled with the native P2CE vbsp in a temporary directory and read
back with apex_disp_reference (bsppp). The variant's edits and the compiled vertices are written to
tests/fixtures/vmf/disp/test1_disp.vbsp.txt, which tests/geometry/displacement_test.cpp checks.

The hull is needed: this vbsp (Apr 23 2026) crashes in "Finding displacement neighbors" on an
unsealed map with a displacement. Requires APEX_P2CE_DIR; never writes under it.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/fixtures/vmf/disp/test1_disp.vmf"
OUTPUT = ROOT / "tests/fixtures/vmf/disp/test1_disp.vbsp.txt"
SIZE = 9  # power 3


def rows(fn):
    return {f"row{r}": " ".join(fn(r, c) for c in range(SIZE)) for r in range(SIZE)}


def num(value):
    return format(value, "g")


# name -> list of (block or None, key, value). Corners of the displaced face, from its point_data.
VARIANTS = {
    "base": [],
    "start_0": [(None, "startposition", "[-96 -1440 64]")],
    "start_1": [(None, "startposition", "[96 -1440 64]")],
    "start_2": [(None, "startposition", "[96 -1632 64]")],
    "elevation": [(None, "elevation", "8")],
    "offsets": [("offsets", k, v) for k, v in rows(
        lambda r, c: f"{num(c * 0.5 - 2)} {num(r * 0.25)} {num((r + c) % 3 - 1)}").items()],
    "elevation_tilted": [(None, "elevation", "8"), (None, "startposition", "[96 -1440 64]")] +
        [("offset_normals", k, v) for k, v in rows(lambda r, c: "0.6 0 0.8" if c % 2 else "0 -0.6 0.8").items()],
}


def apply(text, edits):
    """Replace one dispinfo scalar or one row value, keeping the CRLF text otherwise intact."""
    for block, key, value in edits:
        if block is None:
            pattern = re.compile(r'(\t\t\t\t"%s" ")[^"]*(")' % re.escape(key))
        else:
            start = text.index("\t\t\t\t" + block + "\r\n")
            end = text.index("}", start)
            pattern = re.compile(r'(\t\t\t\t\t"%s" ")[^"]*(")' % re.escape(key))
            match = pattern.search(text, start, end)
            text = text[:match.start()] + match.group(1) + value + match.group(2) + text[match.end():]
            continue
        text, n = pattern.subn(lambda m: m.group(1) + value + m.group(2), text, count=1)
        assert n == 1, key
    return text


def box(ident, lo, hi):
    p = lambda x, y, z: f"({x:g} {y:g} {z:g})"
    (lx, ly, lz), (hx, hy, hz) = lo, hi
    sides = [(p(lx, hy, hz), p(hx, hy, hz), p(hx, ly, hz), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"),
             (p(lx, ly, lz), p(hx, ly, lz), p(hx, hy, lz), "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"),
             (p(lx, hy, hz), p(lx, ly, hz), p(lx, ly, lz), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),
             (p(hx, hy, lz), p(hx, ly, lz), p(hx, ly, hz), "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"),
             (p(hx, ly, lz), p(lx, ly, lz), p(lx, ly, hz), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"),
             (p(hx, hy, hz), p(lx, hy, hz), p(lx, hy, lz), "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25")]
    out = f'\tsolid\r\n\t{{\r\n\t\t"id" "{ident}"\r\n'
    for n, (a, b, c, u, v) in enumerate(sides):
        out += (f'\t\tside\r\n\t\t{{\r\n\t\t\t"id" "{ident * 10 + n}"\r\n\t\t\t"plane" "{a} {b} {c}"\r\n'
                f'\t\t\t"material" "TOOLS/TOOLSNODRAW"\r\n\t\t\t"uaxis" "{u}"\r\n\t\t\t"vaxis" "{v}"\r\n'
                f'\t\t\t"rotation" "0"\r\n\t\t\t"lightmapscale" "16"\r\n\t\t\t"smoothing_groups" "0"\r\n\t\t}}\r\n')
    return out + "\t}\r\n"


def sealed(text):
    """Enclose the world in a 16-unit hull 64 units outside the fixture's points; add a player start."""
    points = [tuple(map(float, m.groups())) for m in
              re.finditer(r'"point" "\d+ (\S+) (\S+) (\S+)"', text)]
    lo = [min(p[i] for p in points) - 64 for i in range(3)]
    hi = [max(p[i] for p in points) + 64 for i in range(3)]
    t = 16
    hull = ""
    for n, (a, b) in enumerate([
            ((lo[0] - t, lo[1] - t, lo[2] - t), (hi[0] + t, hi[1] + t, lo[2])),
            ((lo[0] - t, lo[1] - t, hi[2]), (hi[0] + t, hi[1] + t, hi[2] + t)),
            ((lo[0] - t, lo[1] - t, lo[2]), (lo[0], hi[1] + t, hi[2])),
            ((hi[0], lo[1] - t, lo[2]), (hi[0] + t, hi[1] + t, hi[2])),
            ((lo[0], lo[1] - t, lo[2]), (hi[0], lo[1], hi[2])),
            ((lo[0], hi[1], lo[2]), (hi[0], hi[1] + t, hi[2]))]):
        hull += box(9000 + n, a, b)
    world_end = text.index("}\r\ncameras")
    centre = " ".join(format((lo[i] + hi[i]) / 2, "g") for i in range(2)) + " " + format(hi[2] - 32, "g")
    player = (f'entity\r\n{{\r\n\t"id" "9100"\r\n\t"classname" "info_player_start"\r\n'
              f'\t"origin" "{centre}"\r\n\t"angles" "0 0 0"\r\n}}\r\n')
    return text[:world_end] + hull + "}\r\n" + player + text[world_end + 3:]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reader", type=Path, default=ROOT / "build/linux-gcc-debug/apex_disp_reference")
    parser.add_argument("--keep", type=Path, help="retain the VMFs, BSPs and logs here")
    args = parser.parse_args()
    install = os.environ.get("APEX_P2CE_DIR")
    if not install:
        parser.error("APEX_P2CE_DIR is unset")
    install = Path(install).resolve()
    vbsp = install / "bin/linux64/vbsp"
    source = FIXTURE.read_bytes().decode("ascii")
    work = Path(tempfile.mkdtemp(prefix="apex-disp-oracle-"))
    lines = ["# Apex-authored D2 oracle, written by tools/disp_oracle.py from test1_disp.vmf and the native P2CE vbsp.",
             "# variant <name>; edit <block|-> <key> <value>; vert <index> <x> <y> <z> <alpha>; tris <DISP_TRIS tags>.",
             "# Positions are the engine rebuild in tools/disp_reference: base corners from the nearest-start rotation,",
             "# rows along corner0->corner1, columns along corner0->corner3, plus DISP_VERTS vector * distance."]
    try:
        for name, edits in VARIANTS.items():
            vmf = work / f"{name}.vmf"
            vmf.write_bytes(sealed(apply(source, edits)).encode("ascii"))
            result = subprocess.run([str(vbsp), "-game", str(install / "p2ce"), str(vmf)], cwd=work,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            (work / f"{name}.vbsp.log").write_text(result.stdout)
            if result.returncode:
                raise SystemExit(f"vbsp failed on {name} (exit {result.returncode}); log in {work}")
            dump = subprocess.run([str(args.reader), str(vmf.with_suffix(".bsp"))], check=True,
                                  stdout=subprocess.PIPE, text=True).stdout
            (work / f"{name}.dump.txt").write_text(dump)
            lines.append(f"variant {name}")
            lines += [f"edit {block or '-'} {key} {value}" for block, key, value in edits]
            for line in dump.splitlines():
                words = line.split()
                if words[0] == "vert":
                    lines.append(" ".join(["vert", words[1], *words[7:10], words[6]]))
                elif words[0] == "tris":
                    lines.append(line)
        OUTPUT.write_text("\n".join(lines) + "\n")
        print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(VARIANTS)} variants)")
    finally:
        if args.keep:
            shutil.copytree(work, args.keep, dirs_exist_ok=True)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
