#!/usr/bin/env python3
"""Check THIRD_PARTY.md against the build's dependencies (plan 0.1, D4).

Fails if a CPMAddPackage NAME or find_package() name in the CMake files is missing from the table,
or if a listed licence is not allowed. Also checks that text fixtures carry the Apex-authored marker
(bite 10).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALLOWED = {"MIT", "BSD-2-Clause", "BSD-3-Clause", "Zlib", "BSL-1.0", "Apache-2.0", "MPL-2.0",
           "LGPL-2.1", "LGPL-3.0", "LGPL-2.1-or-later", "LGPL-3.0-or-later", "Unlicense"}
# find_package names that are not third-party code we ship or link (build tooling).
IGNORED_FIND = {"Git", "Python3", "Threads"}
FIXTURE_EXT = {".vmf", ".vmt", ".kv", ".txt"}


def listed():
    rows = {}
    for line in (ROOT / "THIRD_PARTY.md").read_text(encoding="utf-8").splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if line.startswith("|") and len(cells) >= 2 and cells[0] not in {"Name", ""} \
                and not set(cells[0]) <= {"-"}:
            rows[cells[0]] = cells[1]
    return rows


def required():
    names = set()
    files = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake")),
             *sorted((ROOT / "tests").rglob("CMakeLists.txt"))]
    for f in files:
        if f.name == "CPM.cmake":
            continue
        text = f.read_text(encoding="utf-8")
        names |= set(re.findall(r"CPMAddPackage\s*\(\s*NAME\s+(\S+)", text))
        for m in re.findall(r"find_package\s*\(\s*(\w+)", text):
            if m not in IGNORED_FIND:
                names.add(m)
    return names


def main():
    errors = []
    rows = listed()
    for name in sorted(required()):
        # Qt6 is listed once for the whole family.
        if name not in rows and not (name.startswith("Qt") and "Qt6" in rows):
            errors.append(f"dependency '{name}' is missing from THIRD_PARTY.md")
    for name, lic in rows.items():
        parts = {p.strip() for p in re.split(r"\s+(?:OR|AND)\s+|/", lic)}
        if not parts <= ALLOWED:
            errors.append(f"'{name}': licence '{lic}' is not on the allowed list")
    fx = ROOT / "tests" / "fixtures"
    for p in sorted(fx.rglob("*")) if fx.is_dir() else []:
        if p.is_file() and p.suffix in FIXTURE_EXT:
            head = "\n".join(p.read_text(encoding="utf-8", errors="replace").splitlines()[:5])
            if "Apex-authored" not in head:
                errors.append(f"{p.relative_to(ROOT)}: missing 'Apex-authored' header")
    for e in errors:
        print(e, file=sys.stderr)
    print(f"check_licenses: {'FAIL' if errors else 'ok'} ({len(errors)} problem(s))")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
