#!/usr/bin/env python3
"""Regenerate the 0.8.5 displacement lightmap oracle from the real VBSP and VRAD.

    python3 tools/disp_lightmap_oracle.py [--keep DIR]

Each variant of tests/fixtures/vmf/disp/test1_disp.vmf (lightmap axes and scale, start corner, a
narrowed or trapezoid base) is sealed in tools/disp_oracle.py's hull and compiled with the native
P2CE vbsp. A lit variant also gets one point light and `vrad -bounce 0 -noextra`. The displaced
face's stored lightmap mins and extents, plus the style-0 HDR luxels of lit variants, are written to
tests/fixtures/vmf/disp/test1_disp.lightmap.txt, which tests/lighting/charting_test.cpp and
tests/lighting/baker_test.cpp check. Findings: docs/plans/probe-results/displacement_lighting_0_8_5_p0.md.

VBSP takes a displaced side's base face from `point_data`, not from the planes, so a variant that
moves a plane moves the matching points too ("point" edits; Apex charts from the planes and skips
them). Requires APEX_P2CE_DIR; never writes under it.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import disp_oracle  # noqa: E402

ROOT = disp_oracle.ROOT
OUTPUT = ROOT / "tests/fixtures/vmf/disp/test1_disp.lightmap.txt"
SIDE = 355
LIGHT = ((-70, -1500, 115), "255 255 255 300")

NARROW = [("side", 359, "plane", "(96 -1472 64) (-96 -1472 64) (-96 -1472 0)"),
          ("point", SIDE, "0", "-96 -1472 64"), ("point", SIDE, "1", "96 -1472 64")]
TRAPEZOID = [("side", 357, "plane", "(-96 -1440 64) (-64 -1632 64) (-64 -1632 0)"),
             ("point", SIDE, "3", "-64 -1632 64")]
ROTATED = [("side", SIDE, "uaxis", "[0.8 0.6 0 0] 0.25"), ("side", SIDE, "vaxis", "[0.6 -0.8 0 0] 0.25")]


def scale(value):
    return [("side", SIDE, "lightmapscale", str(value))]


def start(position):
    return [("disp", None, "startposition", position)]


# name -> (edits, lit). Edits: ("disp", None, key, value) a dispinfo scalar; ("side", id, key, value)
# a side key; ("point", id, n, "x y z") a point_data point (VBSP's base face only).
VARIANTS = {
    "base": ([], True),
    "start_1": (start("[96 -1440 64]"), True),
    "start_2": (start("[96 -1632 64]"), False),
    "shift": ([("side", SIDE, "uaxis", "[1 0 0 32] 0.25"), ("side", SIDE, "vaxis", "[0 -1 0 16] 0.25")], False),
    "rotated": (ROTATED, True),
    "rotated_scale10": (ROTATED + scale(10), False),
    "scale1": (scale(1), False),
    "scale4": (scale(4), False),
    "scale7": (scale(7), False),
    "scale13": (scale(13), False),
    "scale15": (scale(15), False),
    "unnormalised_u": ([("side", SIDE, "uaxis", "[1 0 0.5 0] 0.25"),
                        ("side", SIDE, "vaxis", "[0.6 -0.8 0 0] 0.25")], False),
    "narrow": (NARROW, True),
    "narrow_start_2": (NARROW + start("[96 -1632 64]"), True),
    "trapezoid_scale15": (TRAPEZOID + scale(15), False),
    "trapezoid_scale15_start_1": (TRAPEZOID + scale(15) + start("[96 -1440 64]"), True),
}


def side_block(text, side_id):
    begin = text.index('\t\t\t"id" "%d"\r\n' % side_id)
    return begin, text.index("\r\n\t\t}\r\n", begin)


def apply(text, edits):
    for kind, side, key, value in edits:
        if kind == "disp":
            text = disp_oracle.apply(text, [(None, key, value)])
            continue
        begin, end = side_block(text, side)
        needle = '"point" "%s ' % key if kind == "point" else '"%s" "' % key
        at = text.index(needle, begin, end)
        close = text.index('"\r\n', at + len(needle))
        text = text[:at] + needle + value + text[close:]
    return text


def light_entity():
    origin, colour = LIGHT
    return ('entity\r\n{\r\n\t"id" "9200"\r\n\t"classname" "light"\r\n\t"_light" "%s"\r\n'
            '\t"origin" "%s"\r\n}\r\n' % (colour, " ".join(map(str, origin))))


def lump(data, index):
    offset, length, version, fourcc = struct.unpack_from("<IIII", data, 8 + 16 * index)
    if fourcc:
        raise SystemExit("compressed lump %d" % index)
    return data[offset:offset + length], version


def displaced_face(data, hdr):
    raw, version = lump(data, 58 if hdr else 7)
    if version < 2:
        raise SystemExit("FACES version %d; expected the 72-byte v2 face" % version)
    faces = [struct.unpack_from("<IBBxxiiiii4Bif2i2iiIII", raw, k * 72) for k in range(len(raw) // 72)]
    found = [f for f in faces if f[6] >= 0]
    if len(found) != 1:
        raise SystemExit("expected one displaced face, found %d" % len(found))
    f = found[0]
    return {"lightofs": f[12], "mins": f[14:16], "extents": f[16:18], "styles": f[8:12]}


def luxels(data, face):
    raw, _ = lump(data, 53)
    count = (face["extents"][0] + 1) * (face["extents"][1] + 1)
    out = []
    for k in range(count):
        r, g, b, e = struct.unpack_from("<BBBb", raw, face["lightofs"] + 4 * k)
        scale_ = 2.0 ** e / 255.0
        out.append(0.0 if r == g == b == 0 else ((r + .5) + (g + .5) + (b + .5)) * scale_ / 3)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", type=Path, help="retain the VMFs, BSPs and logs here")
    args = parser.parse_args()
    install = os.environ.get("APEX_P2CE_DIR")
    if not install:
        parser.error("APEX_P2CE_DIR is unset")
    install = Path(install).resolve()
    tools = install / "bin/linux64"
    source = disp_oracle.FIXTURE.read_bytes().decode("ascii")
    work = Path(tempfile.mkdtemp(prefix="apex-disp-lightmap-oracle-"))
    origin, colour = LIGHT
    lines = ["# Apex-authored 0.8.5 oracle, written by tools/disp_lightmap_oracle.py from test1_disp.vmf and the",
             "# native P2CE vbsp/vrad. variant <name>; edit - <dispinfo key> <value>; side <id> <key> <value>;",
             "# point <side> <n> <x y z> (point_data: VBSP's base face only); chart <mins s t> <extents s t>",
             "# (FACES_HDR of the displaced face); lit variants: light <x y z> <_light> and luxels <values>",
             "# (style 0, mean of RGB, (m + 0.5) 2^e / 255, s fastest) from vrad -bounce 0 -noextra."]
    try:
        for name, (edits, lit) in VARIANTS.items():
            vmf = work / f"{name}.vmf"
            text = disp_oracle.sealed(apply(source, edits))
            if lit:
                text += light_entity()
            vmf.write_bytes(text.encode("ascii"))
            commands = [[str(tools / "vbsp"), "-game", str(install / "p2ce"), str(vmf)]]
            if lit:
                commands.append([str(tools / "vrad"), "-bounce", "0", "-noextra", "-game", str(install / "p2ce"),
                                 str(vmf.with_suffix(".bsp"))])
            for command in commands:
                result = subprocess.run(command, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                (work / f"{name}.{Path(command[0]).name}.log").write_text(result.stdout)
                if result.returncode:
                    raise SystemExit(f"{Path(command[0]).name} failed on {name} (exit {result.returncode}); log in {work}")
            data = vmf.with_suffix(".bsp").read_bytes()
            face = displaced_face(data, lit)
            lines.append(f"variant {name}")
            for kind, side, key, value in edits:
                lines.append(f"edit - {key} {value}" if kind == "disp" else f"{kind} {side} {key} {value}")
            lines.append("chart %d %d %d %d" % (*face["mins"], *face["extents"]))
            if lit:
                lines.append("light %s %s" % (" ".join(map(str, origin)), colour))
                lines.append("luxels " + " ".join(format(v, ".6g") for v in luxels(data, face)))
        OUTPUT.write_text("\n".join(lines) + "\n")
        print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(VARIANTS)} variants)")
    finally:
        if args.keep:
            shutil.copytree(work, args.keep, dirs_exist_ok=True)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
