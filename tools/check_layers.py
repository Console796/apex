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
    "compile": ["core"],
    "lighting": ["geometry", "document", "core"],
    "editor": ["document", "geometry", "core", "compile"],
    "assets": ["core"],
    "render": ["document", "assets", "lighting"],
    "viewport": ["render", "document"],
    "plugin_host": ["editor", "assets", "render"],
    "ui": ["viewport", "editor", "plugin_host"],
    "app": ["ui"],
}
QT_LAYERS = {"ui", "app"}
# Standard header names permitted by the public C++ wrapper.
CXX_HEADERS = set("algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes ciso646 climits clocale cmath codecvt compare complex concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception execution expected filesystem flat_map flat_set format forward_list fstream functional future generator initializer_list iomanip ios iosfwd iostream istream iterator latch limits list locale map mdspan memory memory_resource mutex new numbers numeric optional ostream print queue random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location span spanstream sstream stack stacktrace stdexcept stop_token streambuf string string_view strstream syncstream system_error text_encoding thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector version".split())
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
    # D1: Apex owns model baking; the upstream convenience bake is unsafe.
    for path in sorted(root.rglob("*")):
        if path.is_file():
            for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if "processModelData" in line:
                    errors.append(f"{path.relative_to(root.parent)}:{n}: processModelData is forbidden under src/")
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
                if target in {"apex/apex_plugin.h", "apex/plugin.hpp"} and layer not in {"plugin_host", "ui", "app"}:
                    errors.append(f"{where}: layer '{layer}' must not include the plugin ABI")
                if first in DEPS and first not in allowed:
                    errors.append(f"{where}: layer '{layer}' must not include '{target}'")
                if kind == "<" and re.match(r"Q[A-Z]", first) and layer not in QT_LAYERS:
                    errors.append(f"{where}: Qt header <{target}> outside ui/app (layer '{layer}')")
    public = root.parent / "include" / "apex"
    for path in sorted(public.rglob("*")) if public.is_dir() else []:
        if path.suffix not in {".h", ".hpp"}:
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            kind, target = match.groups()
            if (target.startswith("src/") or target.split("/")[0] in DEPS
                    or (path.name == "apex_plugin.h" and target not in {"stdint.h", "stddef.h"})
                    or (path.name == "plugin.hpp" and not
                        ((kind == '"' and target == "apex_plugin.h") or (kind == "<" and target in CXX_HEADERS)))):
                errors.append(f"{path.relative_to(root.parent)}:{n}: forbidden public include '{target}'")
    for e in errors:
        print(e, file=sys.stderr)
    print(f"check_layers: {'FAIL' if errors else 'ok'} ({len(errors)} violation(s))")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
