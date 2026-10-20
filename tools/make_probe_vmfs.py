#!/usr/bin/env python3
"""Generate the Strata Hammer interop-probe fixtures (plan 0.1.1 P0, D7/D19).

Writes tests/fixtures/probe/*.vmf: a control plus four variants of one tiny map (a 128x128x64 box
and an info_player_start). Each variant changes exactly one thing. Output is CRLF, as Strata writes.
"""
import itertools
import pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "tests/fixtures/probe"
LO, HI = (-64, -64, -64), (64, 64, 0)


def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def fmt(v): return " ".join(f"{x:g}" for x in v)


def faces():
    """Six faces as (outward normal, 4 corners) with Source winding: (p1-p0)x(p2-p0) = -normal."""
    out = []
    for axis in range(3):
        for sign, plane in ((1, HI[axis]), (-1, LO[axis])):
            n = [0, 0, 0]; n[axis] = sign
            a, b = [i for i in range(3) if i != axis]
            corners = []
            for ca, cb in ((LO[a], LO[b]), (HI[a], LO[b]), (HI[a], HI[b]), (LO[a], HI[b])):
                p = [0, 0, 0]; p[axis] = plane; p[a] = ca; p[b] = cb
                corners.append(tuple(p))
            for order in itertools.permutations(range(4)):
                q = [corners[i] for i in order]
                if cross(sub(q[1], q[0]), sub(q[2], q[0])) == tuple(-s * abs(c) for s, c in zip(n, cross(sub(q[1], q[0]), sub(q[2], q[0])))) and any(cross(sub(q[1], q[0]), sub(q[2], q[0]))) and q[0] != q[3] and _convex_loop(q):
                    out.append((tuple(n), q)); break
    return out


def _convex_loop(q):
    # consecutive corners must be edge-adjacent (differ in exactly one in-plane coordinate)
    return all(sum(x != y for x, y in zip(q[i], q[(i + 1) % 4])) == 1 for i in range(4))


def uv(n):
    if n[2]: return "[1 0 0 0] 0.25", "[0 -1 0 0] 0.25"
    if n[0]: return "[0 1 0 0] 0.25", "[0 0 -1 0] 0.25"
    return "[1 0 0 0] 0.25", "[0 0 -1 0] 0.25"


def vmf(marker="strata", extra_versioninfo="", extra_block="", skew_top=False):
    L = ["// Apex-authored probe fixture (MIT). Plan 0.1.1 P0.",
         "versioninfo", "{", f'\t"editorextension" "{marker}"', '\t"editorversion" "400"',
         '\t"editorbuild" "10773"', '\t"mapversion" "1"', '\t"formatversion" "100"', '\t"prefab" "0"']
    if extra_versioninfo: L.append(f"\t{extra_versioninfo}")
    L += ["}", "visgroups", "{", "}", "viewsettings", "{", '\t"bSnapToGrid" "1"', '\t"bShowGrid" "1"',
          '\t"bShowLogicalGrid" "1"', '\t"nGridSpacing" "64"', '\t"bShow3DGrid" "0"',
          '\t"nInstanceVisibility" "2"', "\tviews", "\t{"]
    for i, pos in enumerate(("(0 -300 100)", "(0 0 0)", "(0 0 0)", "(0 0 0)")):
        L += [f"\t\tv{i}", "\t\t{", f'\t\t\t"3d" "{1 if i == 0 else 0}"', f'\t\t\t"position" "{pos}"']
        L += ['\t\t\t"angle" "[15 90 0]"'] if i == 0 else ['\t\t\t"zoom" "0.5"']
        L.append("\t\t}")
    L += ["\t}", "}", "world", "{", '\t"id" "1"', '\t"mapversion" "1"', '\t"classname" "worldspawn"',
          '\t"skyname" "sky_black_nofog"', "\tsolid", "\t{", '\t\t"id" "2"']
    for sid, (n, q) in enumerate(faces(), 1):
        plane = " ".join(f"({fmt(p)})" for p in q[:3])
        pts = [list(p) for p in q]
        if skew_top and n == (0, 0, 1):
            pts[3][2] += 0.5          # 4th point, not part of `plane`: now 0.5 off the plane
        u, v = uv(n)
        L += ["\t\tside", "\t\t{", f'\t\t\t"id" "{sid}"', f'\t\t\t"plane" "{plane}"', "\t\t\tpoint_data",
              "\t\t\t{", '\t\t\t\t"numpts" "4"']
        L += [f'\t\t\t\t"point" "{i} {fmt(p)}"' for i, p in enumerate(pts)]
        L += ["\t\t\t}", '\t\t\t"material" "TOOLS/TOOLSNODRAW"', f'\t\t\t"uaxis" "{u}"', f'\t\t\t"vaxis" "{v}"',
              '\t\t\t"rotation" "0"', '\t\t\t"lightmapscale" "16"', '\t\t\t"smoothing_groups" "0"', "\t\t}"]
    L += ["\t\teditor", "\t\t{", '\t\t\t"color" "0 180 200"', '\t\t\t"visgroupshown" "1"',
          '\t\t\t"visgroupautoshown" "1"', "\t\t}", "\t}", "}", "entity", "{", '\t"id" "3"',
          '\t"classname" "info_player_start"', '\t"angles" "0 90 0"', '\t"origin" "0 0 16"',
          "\teditor", "\t{", '\t\t"color" "0 255 0"', '\t\t"visgroupshown" "1"',
          '\t\t"visgroupautoshown" "1"', '\t\t"logicalpos" "[0 0]"', "\t}", "}"]
    if extra_block: L += extra_block.splitlines()
    L += ["cameras", "{", '\t"activecamera" "-1"', "}", "cordons", "{", '\t"active" "0"', "}"]
    return "\r\n".join(L) + "\r\n"


CASES = {
    "e_control.vmf": {},
    "a_editorext_apex.vmf": {"marker": "apex"},
    "b_unknown_key.vmf": {"extra_versioninfo": '"apexversion" "0.1.3.1"'},
    "c_point_data_mismatch.vmf": {"skew_top": True},
    "d_unknown_block.vmf": {"extra_block": 'apexblock\n{\n\t"foo" "bar"\n}'},
}

if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    for name, kw in CASES.items():
        (OUT / name).write_bytes(vmf(**kw).encode())
        print("wrote", OUT / name)
