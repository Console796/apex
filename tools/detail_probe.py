#!/usr/bin/env python3
"""Compile a synthetic detail-prop probe with P2CE's Windows vbsp under Wine (plan 0.8.9 P0, D16).

    python3 tools/detail_probe.py [--keep DIR] [--runs N] [--dump] [--fixture]

Nothing in P2CE ships a detail.vbsp, and no corpus map has a displacement, so the probe brings its own:
a scratch game directory whose gameinfo.txt searches itself first and then the real install (by absolute
path), holding materials/apexprobe/*.vmt with "%detailtype" and a detail.vbsp naming those types. The map is
a sealed room with a displaced floor and a flat block, both using the probe materials. It is compiled N times
and the dprp game lump of each compile is compared byte for byte; --dump prints the decoded lump of the first.
--fixture compiles the map without the flat block and writes the VMF, detail.vbsp and decoded lump to
tests/fixtures/detail/, which tests/document/detail_props_oracle_test.cpp reads: one displacement face is the case
where every field, scale included, is reproducible (the Gaussian scale stream keeps a cached value across faces).
The Windows vbsp.exe runs under Wine because the native Linux vbsp never places a detail prop: it draws with
glibc rand() but divides by 32767 (MSVC's RAND_MAX), so every draw is far above 1 and no model is ever selected
(docs/plans/probe-results/detail_props_0_8_9_p0.md). Requires APEX_P2CE_DIR and wine; never writes under it.
"""
import argparse
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/fixtures/detail"

DETAIL_VBSP = """detail.vbsp
{
	apexprobe_grass
	{
		"density" "4000.0"
		"Group1"
		{
			"alpha" "0"
			"tall"
			{
				"amount" "0.6"
				"sprite" "0 0 128 128 512"
				"spritesize" "0.5 0 24 32"
				"spriterandomscale" "0.25"
				"detailOrientation" "2"
				"sway" "1"
				"minAngle" "20"
				"maxAngle" "50"
			}
			"short"
			{
				"amount" "0.3"
				"sprite" "128 0 128 128 512"
				"spritesize" "0.5 0 16 16"
				"spriterandomscale" "0"
				"detailOrientation" "1"
				"upright" "1"
			}
		}
		"Group2"
		{
			"alpha" "1"
			"cross"
			{
				"amount" "1"
				"sprite_shape" "cross"
				"sprite" "256 0 128 128 512"
				"spritesize" "0.5 0 32 32"
				"spriterandomscale" "0.1"
				"shape_angle" "30"
				"shape_size" "0.5"
				"detailOrientation" "2"
				"minAngle" "20"
				"maxAngle" "50"
			}
		}
	}
	apexprobe_flat
	{
		"density" "1500.0"
		"Group1"
		{
			"alpha" "1"
			"flat"
			{
				"amount" "1"
				"sprite" "0 128 128 128 512"
				"spritesize" "0.5 0 12 12"
				"detailOrientation" "0"
				"minAngle" "20"
				"maxAngle" "50"
			}
		}
	}
}
"""

GRASS_VMT = 'LightmappedGeneric\n{\n\t"$basetexture" "tile/white_floor_tile002a"\n\t"%detailtype" "apexprobe_grass"\n}\n'
FLAT_VMT = 'LightmappedGeneric\n{\n\t"$basetexture" "tile/white_floor_tile002a"\n\t"%detailtype" "apexprobe_flat"\n}\n'
WALL = "TILE/WHITE_WALL_TILE003A"


def wine_path(path):
    return f"Z:{path}"


def gameinfo(install):
    paths = [install / "p2ce", install / "hammer", install / "portal2/portal2/portal2.vpk", install / "portal2/portal2"]
    lines = "\n".join(f'\t\t\tGame\t"{wine_path(p)}"' for p in paths)
    return (f'"GameInfo"\n{{\n\tgame "apexprobe"\n\tFileSystem\n\t{{\n\t\tSteamAppId 440000\n\t\tSearchPaths\n\t\t{{\n'
            f'\t\t\tgame+mod+default_write_path |gameinfo_path|.\n{lines}\n\t\t}}\n\t}}\n}}\n')


class Ids:
    def __init__(self): self.n = 0
    def __call__(self): self.n += 1; return self.n


def side(ids, points, material, disp=""):
    plane = " ".join(f"({p[0]:g} {p[1]:g} {p[2]:g})" for p in points)
    return (f'\t\tside\n\t\t{{\n\t\t\t"id" "{ids()}"\n\t\t\t"plane" "{plane}"\n\t\t\t"material" "{material}"\n'
            f'\t\t\t"uaxis" "[1 0 0 0] 0.25"\n\t\t\t"vaxis" "[0 -1 0 0] 0.25"\n\t\t\t"rotation" "0"\n'
            f'\t\t\t"lightmapscale" "16"\n\t\t\t"smoothing_groups" "0"\n{disp}\t\t}}\n')


def box(ids, lo, hi, top=WALL, disp=""):
    (x1, y1, z1), (x2, y2, z2) = lo, hi
    planes = [((x1, y2, z2), (x2, y2, z2), (x2, y1, z2)), ((x1, y1, z1), (x2, y1, z1), (x2, y2, z1)),
              ((x1, y2, z2), (x1, y1, z2), (x1, y1, z1)), ((x2, y2, z1), (x2, y1, z1), (x2, y1, z2)),
              ((x2, y2, z2), (x1, y2, z2), (x1, y2, z1)), ((x2, y1, z1), (x1, y1, z1), (x1, y1, z2))]
    text = f'\tsolid\n\t{{\n\t\t"id" "{ids()}"\n'
    for i, p in enumerate(planes):
        text += side(ids, p, top if i == 0 else WALL, disp if i == 0 else "")
    return text + "\t}\n"


def dispinfo(power, start):
    n = (1 << power) + 1
    rows = lambda f: "".join(f'\t\t\t\t\t"row{r}" "{" ".join(f(r, c) for c in range(n))}"\n' for r in range(n))
    block = lambda name, f: f"\t\t\t\t{name}\n\t\t\t\t{{\n{rows(f)}\t\t\t\t}}\n"
    height = lambda r, c: 80 * math.sin(r / (n - 1) * math.pi) * math.cos(c / (n - 1) * math.pi * 1.5) + 24
    alpha = lambda r, c: 255.0 * c / (n - 1)
    tags = "".join(f'\t\t\t\t\t"row{r}" "{" ".join(["9"] * (2 * (n - 1)))}"\n' for r in range(n - 1))
    verts = " ".join(["-1"] * 10)
    return (f'\t\t\tdispinfo\n\t\t\t{{\n\t\t\t\t"power" "{power}"\n\t\t\t\t"startposition" "[{start[0]:g} {start[1]:g} {start[2]:g}]"\n'
            f'\t\t\t\t"flags" "0"\n\t\t\t\t"elevation" "0"\n\t\t\t\t"subdiv" "0"\n'
            + block("normals", lambda r, c: "0 0 1")
            + block("distances", lambda r, c: f"{height(r, c):.6g}")
            + block("offsets", lambda r, c: "0 0 0")
            + block("offset_normals", lambda r, c: "0 0 1")
            + block("alphas", lambda r, c: f"{alpha(r, c):.6g}")
            + f"\t\t\t\ttriangle_tags\n\t\t\t\t{{\n{tags}\t\t\t\t}}\n"
            + f'\t\t\t\tallowed_verts\n\t\t\t\t{{\n\t\t\t\t\t"10" "{verts}"\n\t\t\t\t}}\n'
            + "\t\t\t}\n")


def probe_vmf(flat=True):
    ids = Ids()
    lo, hi, w = (-512, -512, -64), (512, 512, 256), 16
    (x1, y1, z1), (x2, y2, z2) = lo, hi
    world = "".join(box(ids, a, b) for a, b in [
        ((x1 - w, y1 - w, z1 - w), (x2 + w, y2 + w, z1)), ((x1 - w, y1 - w, z2), (x2 + w, y2 + w, z2 + w)),
        ((x1 - w, y1, z1), (x1, y2, z2)), ((x2, y1, z1), (x2 + w, y2, z2)),
        ((x1, y1 - w, z1), (x2, y1, z2)), ((x1, y2, z1), (x2, y2 + w, z2))])
    world += box(ids, (-384, -384, -32), (128, 128, 0), "APEXPROBE/GRASS", dispinfo(3, (-384, -384, 0)))
    if flat:
        world += box(ids, (192, 192, -64), (448, 320, 16), "APEXPROBE/FLAT")
    start = f'entity\n{{\n\t"id" "{ids()}"\n\t"classname" "info_player_start"\n\t"origin" "-448 448 -56"\n\t"angles" "0 0 0"\n}}\n'
    return (f'versioninfo\n{{\n\t"editorversion" "400"\n\t"mapversion" "1"\n\t"formatversion" "100"\n\t"prefab" "0"\n}}\n'
            f'world\n{{\n\t"id" "{ids()}"\n\t"mapversion" "1"\n\t"classname" "worldspawn"\n\t"skyname" "sky_black_nofog"\n'
            f'\t"detailvbsp" "detail.vbsp"\n\t"detailmaterial" "detail/detailsprites"\n{world}}}\n{start}')


def game_lump(bsp, fourcc):
    data = bsp.read_bytes()
    assert data[:4] == b"VBSP", "not a VBSP file"
    offset, length = struct.unpack_from("<II", data, 8 + 35 * 16)
    count, = struct.unpack_from("<i", data, offset)
    for i in range(count):
        ident, flags, version, at, size = struct.unpack_from("<4sHHii", data, offset + 4 + i * 16)
        if ident[::-1] == fourcc.encode():
            assert not flags & 1, "compressed game lump"
            return version, data[at:at + size]
    return None, b""


def decode_dprp(version, lump):
    at = 0
    def take(fmt):
        nonlocal at
        values = struct.unpack_from("<" + fmt, lump, at); at += struct.calcsize("<" + fmt); return values
    models = [take("128s")[0].split(b"\0")[0].decode() for _ in range(take("i")[0])]
    sprites = [take("8f") for _ in range(take("i")[0])]
    stride = 52 if version >= 4 else 48
    objects = []
    for _ in range(take("i")[0]):
        start = at
        origin, angles = take("3f"), take("3f")
        model, leaf = take("HH")
        _lighting, _styles = take("I"), take("I")
        count, sway, shape_angle, shape_size, orientation = take("5B")
        take("3x"); kind = take("B")[0]; take("3x")
        scale = take("f")[0] if version >= 4 else 1.0
        assert at - start == stride
        objects.append((origin, angles, model, leaf, sway, shape_angle, shape_size, orientation, kind, scale))
    return models, sprites, objects


def dump_lines(sprites, objects):
    lines = ["sprite " + " ".join(f"{v:.9g}" for v in s) for s in sprites]
    for o in objects:
        origin, angles, model, leaf, sway, sa, ss, orient, kind, scale = o
        lines.append("object " + " ".join(f"{v:.9g}" for v in origin) + " angles " + " ".join(f"{v:.9g}" for v in angles)
                     + f" model {model} leaf {leaf} sway {sway} shape {sa} {ss} orient {orient} type {kind} scale {scale:.9g}")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", type=Path, help="retain the scratch game, VMF, BSPs and logs here")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--dump", action="store_true")
    parser.add_argument("--fixture", action="store_true", help=f"write the single-displacement oracle to {FIXTURE}")
    args = parser.parse_args()
    install = os.environ.get("APEX_P2CE_DIR")
    if not install:
        parser.error("APEX_P2CE_DIR is unset")
    install = Path(install).resolve()
    vbsp = install / "bin/win64/vbsp.exe"
    work = Path(tempfile.mkdtemp(prefix="apex-detail-probe-"))
    game = work / "apexprobe"
    try:
        (game / "materials/apexprobe").mkdir(parents=True)
        (game / "gameinfo.txt").write_text(gameinfo(install))
        (game / "detail.vbsp").write_text(DETAIL_VBSP)
        (game / "materials/apexprobe/grass.vmt").write_text(GRASS_VMT)
        (game / "materials/apexprobe/flat.vmt").write_text(FLAT_VMT)
        lumps = []
        for run in range(args.runs):
            vmf = work / f"detail_probe_{run}.vmf"
            vmf.write_text(probe_vmf(flat=not args.fixture))
            result = subprocess.run(["wine", str(vbsp), "-game", wine_path(game), wine_path(vmf)], cwd=work,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                                    env=dict(os.environ, WINEDEBUG="-all"))
            vmf.with_suffix(".vbsp.log").write_text(result.stdout)
            bsp = vmf.with_suffix(".bsp")
            if result.returncode or not bsp.exists() or "LEAKED" in result.stdout:
                raise SystemExit(f"run {run}: vbsp failed or leaked; see {work}")
            lumps.append(game_lump(bsp, "dprp"))
        version, first = lumps[0]
        models, sprites, objects = decode_dprp(version, first)
        print(f"dprp version {version}, {len(first)} bytes: {len(models)} models, {len(sprites)} sprites, "
              f"{len(objects)} objects")
        print("identical across runs" if all(l == lumps[0] for l in lumps) else "DIFFERS between runs")
        if args.dump:
            print("\n".join(dump_lines(sprites, objects)))
        if args.fixture:
            FIXTURE.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(work / "detail_probe_0.vmf", FIXTURE / "detail_probe.vmf")
            (FIXTURE / "detail.vbsp").write_text(DETAIL_VBSP)
            (FIXTURE / "dprp.txt").write_text("\n".join(dump_lines(sprites, objects)) + "\n")
            print(f"wrote {FIXTURE}")
    finally:
        if args.keep:
            shutil.copytree(work, args.keep, dirs_exist_ok=True)
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
