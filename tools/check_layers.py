#!/usr/bin/env python3
"""Enforce the layer graph (plan 0.1, section 2 and bite 9) on #includes under src/.

Rules:
  * a layer may include only itself and the layers it (transitively) depends on;
  * Qt headers (<Q...>) are allowed only in ui/ and app/.
"""
import re
import sys
from pathlib import Path

# Direct dependencies, mirroring the target_link_libraries graph in CMakeLists.txt.
DEPS = {
    "core": [],
    "geometry": ["core"],
    "document": ["geometry", "core"],
    "editor": ["document", "geometry", "core"],
    "assets": ["core"],
    "render": ["document", "assets"],
    "viewport": ["render", "document"],
    "ui": ["viewport"],
    "app": ["ui"],
}
QT_LAYERS = {"ui", "app"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


def closure(layer):
    seen, todo = {layer}, [layer]
    while todo:
        for d in DEPS[todo.pop()]:
            if d not in seen:
                seen.add(d)
                todo.append(d)
    return seen


def main():
    root = Path(__file__).resolve().parent.parent / "src"
    errors = []
    for layer in DEPS:
        allowed = closure(layer)
        for path in sorted((root / layer).rglob("*")) if (root / layer).is_dir() else []:
            if path.suffix not in {".hpp", ".cpp", ".h", ".inl", ".mm"}:
                continue
            for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                m = INCLUDE.match(line)
                if not m:
                    continue
                kind, target = m.groups()
                where = f"{path.relative_to(root.parent)}:{n}"
                first = target.split("/")[0]
                if first in DEPS and first not in allowed:
                    errors.append(f"{where}: layer '{layer}' must not include '{target}'")
                if kind == "<" and re.match(r"Q[A-Z]", first) and layer not in QT_LAYERS:
                    errors.append(f"{where}: Qt header <{target}> outside ui/app (layer '{layer}')")
    for e in errors:
        print(e, file=sys.stderr)
    print(f"check_layers: {'FAIL' if errors else 'ok'} ({len(errors)} violation(s))")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
