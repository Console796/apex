#!/usr/bin/env python3
"""Enforce the layer graph (plan 0.1, section 2 and bite 9) on #includes under src/.

Rules:
  * a layer may include only itself and the layers it (transitively) depends on;
  * Qt headers (<Q...>) are allowed only in ui/ and app/;
  * the public plugin headers under include/apex/ include nothing from src/ (0.9.9, plugins.md D12): apex_plugin.h
    only <stdint.h> and <stddef.h>, plugin.hpp only "apex_plugin.h" and C++ standard headers, and no header there
    names a src/ layer or reaches into src/ with a path.

    tools/check_layers.py [ROOT]     check ROOT (default: this checkout)
    tools/check_layers.py --self-test  plant violations in a temporary tree and require them to be reported
"""
import re
import sys
import tempfile
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


def check(repo):
    """Returns the list of violations under repo/src and repo/include/apex."""
    root = repo / "src"
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
    public = repo / "include" / "apex"
    for path in sorted(public.rglob("*")) if public.is_dir() else []:
        if path.suffix not in {".h", ".hpp"}:
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = INCLUDE.match(line)
            if not match:
                continue
            kind, target = match.groups()
            parts = target.replace("\\", "/").split("/")
            if (target.startswith("src/") or "src" in parts or ".." in parts or parts[0] in DEPS
                    or (path.name == "apex_plugin.h" and target not in {"stdint.h", "stddef.h"})
                    or (path.name == "plugin.hpp" and not
                        ((kind == '"' and target == "apex_plugin.h") or (kind == "<" and target in CXX_HEADERS)))):
                errors.append(f"{path.relative_to(root.parent)}:{n}: forbidden public include '{target}'")
    return errors


def self_test():
    """The include/apex/ rule must flag each planted violation and pass a clean tree."""
    clean = {
        "apex_plugin.h": '#include <stdint.h>\n#include <stddef.h>\n',
        "plugin.hpp": '#include "apex_plugin.h"\n#include <string>\n#include <vector>\n',
    }
    planted = [
        ("apex_plugin.h", '#include "core/math.hpp"\n', "a src/ layer header in the C header"),
        ("apex_plugin.h", '#include <string.h>\n', "a C header beyond stdint.h and stddef.h"),
        ("apex_plugin.h", '#include "src/core/math.hpp"\n', "a src/ path"),
        ("apex_plugin.h", '#include "../../src/core/math.hpp"\n', "a relative path into src/"),
        ("plugin.hpp", '#include "document/document.hpp"\n', "a src/ layer header in the wrapper"),
        ("plugin.hpp", '#include <QString>\n', "a Qt header in the wrapper"),
        ("plugin.hpp", '#include <apex/other.h>\n', "an angle-bracket Apex header in the wrapper"),
        ("plugin.hpp", '#include "../../src/editor/tool.hpp"\n', "a relative path into src/"),
        ("extra.hpp", '#include "plugin_host/plugin_host.hpp"\n', "a layer header in any other public header"),
        ("extra.h", '#include "src/ui/theme.hpp"\n', "a src/ path in any other public header"),
        ("extra.hpp", '#include "../../src/core/math.hpp"\n', "a relative path into src/ in any other public header"),
    ]
    failures = []
    with tempfile.TemporaryDirectory(prefix="apex-layers-") as tmp:
        repo = Path(tmp)
        (repo / "src").mkdir()
        public = repo / "include" / "apex"
        public.mkdir(parents=True)

        def write(files):
            for old in public.iterdir():
                old.unlink()
            for name, text in files.items():
                (public / name).write_text(text, encoding="utf-8")
        write(clean)
        found = check(repo)
        if found:
            failures.append(f"a clean tree was reported: {found}")
        for name, text, what in planted:
            write({**clean, name: text if name in ("extra.h", "extra.hpp") else clean[name] + text})
            found = check(repo)
            if not any(name in e and "forbidden public include" in e for e in found):
                failures.append(f"not flagged: {what} ({name}: {text.strip()})")
        # A src/ file that includes a layer it does not depend on, and a Qt header below ui/.
        write(clean)
        (repo / "src" / "core").mkdir()
        (repo / "src" / "core" / "bad.cpp").write_text('#include "editor/tool.hpp"\n#include <QString>\n', encoding="utf-8")
        found = check(repo)
        if not any("must not include 'editor/tool.hpp'" in e for e in found):
            failures.append("a layer violation under src/ was not flagged")
        if not any("Qt header" in e for e in found):
            failures.append("a Qt header below ui/ was not flagged")
    for f in failures:
        print(f"check_layers self-test: {f}", file=sys.stderr)
    print(f"check_layers self-test: {'FAIL' if failures else 'ok'} ({len(planted) + 3} cases)")
    return 1 if failures else 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    repo = Path(argv[1]).resolve() if len(argv) > 1 else Path(__file__).resolve().parent.parent
    errors = check(repo)
    for e in errors:
        print(e, file=sys.stderr)
    print(f"check_layers: {'FAIL' if errors else 'ok'} ({len(errors)} violation(s))")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
