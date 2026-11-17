#!/usr/bin/env python3
"""Check docs/plugins against the sources it quotes (0.9.9).

  * Every fenced block preceded by `<!-- snippet: <repo path> -->` must appear verbatim (line for line,
    ignoring indentation and trailing blanks) in that file. `...` or `// ...` alone on a line separates
    parts that may be apart in the file.
  * Relative markdown links between the pages must resolve and must stay inside docs/plugins (the pages
    ship alone in the SDK archive).
  * Every page listed in README.md exists, and every page is listed.
  * docs/plugin-sdk.md stays a short pointer, so the content is not duplicated.
  * Nothing in the tree refers to a heading of the old docs/plugin-sdk.md.

Usage: check_docs.py [ROOT]    Exit 0 when clean, 1 with a message per problem.
       check_docs.py --self-test  Plant each kind of problem in a temporary tree and require it to be reported.
"""
import pathlib
import re
import sys

MARK = re.compile(r"<!-- snippet: (\S+) -->\n```[a-z]*\n(.*?)\n```", re.S)
FENCE = re.compile(r"^```(c|cpp|c\+\+|cmake)\n", re.M)
LINK = re.compile(r"\]\(([^)#\s]+)(#[^)]*)?\)")
SPLIT = {"...", "// ...", "/* ... */"}


def norm(lines):
    return [l.strip() for l in lines]


def contains(hay, needle):
    n = len(needle)
    return any(hay[i:i + n] == needle for i in range(len(hay) - n + 1))


def check(root):
    errors = []
    docs = root / "docs" / "plugins"
    pages = sorted(docs.glob("*.md"))
    if not pages:
        return [f"{docs}: no pages"]
    for page in pages:
        text = page.read_text(encoding="utf-8")
        rel = page.relative_to(root)
        for m in MARK.finditer(text):
            src = root / m.group(1)
            if not src.is_file():
                errors.append(f"{rel}: snippet source {m.group(1)} does not exist")
                continue
            hay = norm(src.read_text(encoding="utf-8").split("\n"))
            parts, cur = [], []
            for line in m.group(2).split("\n"):
                if line.strip() in SPLIT:
                    parts.append(cur); cur = []
                else:
                    cur.append(line)
            parts.append(cur)
            for part in parts:
                part = norm(part)
                while part and not part[-1]: part.pop()
                while part and not part[0]: part.pop(0)
                if part and not contains(hay, part):
                    errors.append(f"{rel}: snippet from {m.group(1)} does not match the file near: {part[0][:70]!r}")
        if len(re.findall(r"<!-- snippet:", text)) != len(MARK.findall(text)):
            errors.append(f"{rel}: a snippet marker is not followed by a fenced block")
        for m in FENCE.finditer(text):
            before = text[:m.start()].rstrip("\n").split("\n")[-1]
            if not before.startswith("<!-- snippet:"):
                errors.append(f"{rel}: a {m.group(1)} block has no snippet marker (quote the header or an example)")
        for m in LINK.finditer(text):
            target = m.group(1)
            if re.match(r"[a-z]+:", target):
                continue
            resolved = (page.parent / target).resolve()
            if docs.resolve() not in resolved.parents and resolved != docs.resolve():
                errors.append(f"{rel}: link {target} leaves docs/plugins")
            elif not resolved.exists():
                errors.append(f"{rel}: link {target} does not resolve")
    index = (docs / "README.md").read_text(encoding="utf-8") if (docs / "README.md").exists() else ""
    listed = set(re.findall(r"\]\(([a-z-]+\.md)\)", index))
    for page in pages:
        if page.name != "README.md" and page.name not in listed:
            errors.append(f"docs/plugins/{page.name}: not listed in README.md")
    pointer = root / "docs" / "plugin-sdk.md"
    if pointer.exists() and len(pointer.read_text(encoding="utf-8").split("\n")) > 20:
        errors.append("docs/plugin-sdk.md: must stay a short pointer to docs/plugins/")
    for path in root.rglob("*"):
        if path.suffix in {".h", ".hpp", ".cpp", ".c", ".as", ".py", ".md", ".txt", ".cmake"} and path.is_file():
            parts = path.relative_to(root).parts
            if parts[0] in {".git", "build", "third_party", "external"} or "build" in parts[0]:
                continue
            if path.name == "check_docs.py" or path == pointer or parts[:2] == ("docs", "plans"):
                continue
            try:
                body = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            if re.search(r"plugin-sdk\.md\S*\s*(?:\"|,)?\s*[\w\"]*\s*Versioning|plugin-sdk\.md 4\.|docs/plugin-sdk\.md[\"' ]*, *\"", body):
                errors.append(f"{path.relative_to(root)}: refers to a section of the old docs/plugin-sdk.md")
    return errors


def self_test():
    import tempfile
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        (root / "docs" / "plugins").mkdir(parents=True)
        (root / "ex.c").write_text("int main(void) {\n    return 0;\n}\n")
        (root / "docs" / "plugin-sdk.md").write_text("pointer\n")
        page = root / "docs" / "plugins" / "a.md"
        index = root / "docs" / "plugins" / "README.md"
        index.write_text("[a](a.md)\n")
        def run(body, expect):
            page.write_text(body)
            got = check(root)
            ok = (not got) if expect is None else any(expect in e for e in got)
            if not ok:
                failures.append(f"expected {expect!r}, got {got}")
        run("<!-- snippet: ex.c -->\n```c\nint main(void) {\n  return 0;\n}\n```\n", None)
        run("<!-- snippet: ex.c -->\n```c\nint main(void) {\n    return 1;\n}\n```\n", "does not match")
        run("<!-- snippet: ex.c -->\n```c\nint main(void) {\n...\n}\n```\n", None)
        run("<!-- snippet: nope.c -->\n```c\nx\n```\n", "does not exist")
        run("```c\nint x;\n```\n", "no snippet marker")
        run("[x](missing.md)\n", "does not resolve")
        run("[x](../plugin-sdk.md)\n", "leaves docs/plugins")
        run("<!-- snippet: ex.c -->\nno fence\n", "not followed")
        (root / "docs" / "plugins" / "b.md").write_text("b\n")
        run("ok\n", "not listed")
        (root / "docs" / "plugins" / "b.md").unlink()
        (root / "docs" / "plugin-sdk.md").write_text("long\n" * 30)
        run("ok\n", "short pointer")
    for f in failures:
        print("self-test FAILED:", f)
    print(f"check_docs self-test: {'FAILED' if failures else 'ok'}")
    return 1 if failures else 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--self-test":
        return self_test()
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path(__file__).resolve().parent.parent
    errors = check(root)
    for e in errors:
        print(e)
    print(f"check_docs: {len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
