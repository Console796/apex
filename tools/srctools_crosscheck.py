#!/usr/bin/env python3
"""Cross-check Apex's VMF parse against srctools (plan 0.1.4, corpus case 3).

    .venv-tools/bin/python tools/srctools_crosscheck.py <apex_vmf_dump> <dir-or-vmf>...

For every VMF, compares entity, solid and side counts, the sorted object and side ids, and the
sorted plane strings between `apex_vmf_dump` and srctools. Exits non-zero on any difference.
Set up: python3 -m venv .venv-tools && .venv-tools/bin/pip install srctools
"""
import subprocess
import sys
from pathlib import Path

from srctools import Keyvalues
from srctools.vmf import VMF


def normalize_plane(text):
    nums = [float(t) for t in text.replace("(", " ").replace(")", " ").split()]
    return tuple(round(n, 4) for n in nums)


def apex_dump(tool, path):
    out = subprocess.run([tool, str(path)], capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(out.stderr.strip())
    ents, solids, sides = [], [], []
    for line in out.stdout.splitlines():
        kind, *rest = line.split("\t")
        if kind == "entity":
            ents.append(int(rest[0]))
        elif kind == "solid":
            solids.append(int(rest[0]))
        else:
            sides.append((int(rest[0]), normalize_plane(rest[1])))
    return sorted(ents), sorted(solids), sorted(sides)


def srctools_dump(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        kv = Keyvalues.parse(f, filename=str(path))
    vmf = VMF.parse(kv, preserve_ids=True)
    ents = [e.id for e in vmf.entities]
    solids = [s.id for s in vmf.iter_wbrushes(world=True, detail=True)]
    solids += [b.id for e in vmf.entities for b in e.solids]
    sides = []
    for brush in list(vmf.brushes) + [b for e in vmf.entities for b in e.solids]:
        for side in brush.sides:
            p = [side.planes[i] for i in range(3)]
            sides.append((side.id, tuple(round(c, 4) for v in p for c in (v.x, v.y, v.z))))
    return sorted(ents), sorted(set(solids)), sorted(sides)


def main():
    tool = sys.argv[1]
    files = []
    for arg in sys.argv[2:]:
        p = Path(arg)
        files += sorted(p.rglob("*.vmf")) if p.is_dir() else [p]
    bad = unchecked = 0
    for path in files:
        try:
            a = apex_dump(tool, path)
        except Exception as e:  # noqa: BLE001
            print(f"ERROR {path}: apex: {e}")
            bad += 1
            continue
        try:
            b = srctools_dump(path)
        except Exception as e:  # noqa: BLE001  srctools rejects some editor view data
            print(f"UNCHECKED {path}: srctools: {e}")
            unchecked += 1
            continue
        if any(i == 0 for i, _ in a[2]):
            # srctools renumbers a side whose id is 0; Apex keeps the file's value (D7).
            a = (a[0], a[1], sorted((0, p) for _, p in a[2]))
            b = (b[0], b[1], sorted((0, p) for _, p in b[2]))
        names = ("entities", "solids", "sides")
        for name, x, y in zip(names, a, b):
            if x != y:
                print(f"DIFF {path} {name}: apex={len(x)} srctools={len(y)}")
                bad += 1
                break
    print(f"checked {len(files) - unchecked} files, {unchecked} unchecked (srctools could not parse), {bad} differ")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
