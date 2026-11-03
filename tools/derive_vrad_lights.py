#!/usr/bin/env python3
"""Create a VRAD-comparable VMF without changing any authored light keys.

Apex's lightsFromEntity handles each runtime/static pair identically. VRAD does
not bake the runtime classes, so only the classname values are substituted.
Instance VMFs are not rewritten; use this on a map with no runtime lights in
instances, or derive those separately as well.
"""

import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error("source and output must be different files")
    data = args.source.read_bytes()
    pattern = re.compile(rb'(?im)^(\s*"classname"\s+")(light_rt_spot|light_rt)("\s*)$')
    counts = {b"light_rt": 0, b"light_rt_spot": 0}

    def replace(match):
        name = match.group(2).lower()
        counts[name] += 1
        replacement = b"light_spot" if name == b"light_rt_spot" else b"light"
        return match.group(1) + replacement + match.group(3)

    result = pattern.sub(replace, data)
    if not sum(counts.values()):
        parser.error("no runtime light classnames found")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as output:
        output.write(result)
    print(f"wrote {args.output}: {counts[b'light_rt']} point, {counts[b'light_rt_spot']} spot")


if __name__ == "__main__":
    main()
