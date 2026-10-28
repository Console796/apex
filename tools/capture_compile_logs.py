#!/usr/bin/env python3
"""0.5.2 P0: compile deliberately broken variants of the D17 chamber with the real Linux tools
and store each tool's merged output under tests/compile/logs/ as the fixtures for the diagnostic rules.

Usage: capture_compile_logs.py <chamber.vmf>   (needs APEX_P2CE_DIR; the install is never written to,
all work happens in a /tmp scratch directory.)
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'tests/compile/logs'


def blocks(text, header):
    """Spans (start, end) of top-level `header` blocks nested one level inside `world`, or at top level."""
    spans, depth, i = [], 0, 0
    for m in re.finditer(r'^(\s*)("?\w+"?)\s*$|^\s*([{}])\s*$', text, re.M):
        pass
    return spans


def solid_spans(text):
    """(start, end) character spans of each `solid` block in the world."""
    spans = []
    for m in re.finditer(r'^\t"solid"\n\t\{', text, re.M):
        depth, i = 0, m.end() - 1
        while True:
            if text[i] == '{': depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0: break
            i += 1
        spans.append((m.start(), i + 1))
    return spans


def entity(classname, origin, extra=''):
    return f'entity\n{{\n\t"id" "{9000 + abs(hash(classname + extra)) % 900}"\n\t"classname" "{classname}"\n\t"origin" "{origin}"\n{extra}}}\n'


def variants(base):
    solids = solid_spans(base)
    out = {'baseline': base}
    out['leak'] = base[:solids[0][0]] + base[solids[0][1]:]
    s0, e0 = solids[0]
    degenerate = re.sub(r'"plane" "[^"]*"', '"plane" "(0 0 0) (1 0 0) (2 0 0)"', base[s0:e0], count=1)
    out['invalid_brush'] = base[:s0] + degenerate + base[e0:]
    out['unknown_material'] = base[:s0] + base[s0:e0].replace('tile/white_wall_tile003a', 'apex/not_a_material', 1) + base[e0:]
    out['missing_model'] = base + entity('prop_static', '256 256 64', '\t"model" "models/apex/does_not_exist.mdl"\n\t"skin" "0"\n\t"solid" "6"\n')
    relay = '\t"targetname" "apex_relay"\n\tconnections\n\t{\n\t\t"OnTrigger" "apex_missing_target,Kill,,0,-1"\n\t}\n'
    out['missing_target'] = base + entity('logic_relay', '128 128 64', relay)
    dup = '\t"targetname" "apex_dup"\n'
    out['duplicate_targetname'] = base + entity('info_target', '64 64 64', dup) + entity('info_target', '96 64 64', dup)
    out['missing_instance'] = base + entity('func_instance', '256 256 64', '\t"file" "apex_nowhere.vmf"\n\t"angles" "0 0 0"\n')
    return out


def main():
    install = os.environ.get('APEX_P2CE_DIR')
    if not install or len(sys.argv) != 2:
        print(__doc__); return 2
    install = Path(install)
    game, tools = install / 'p2ce', install / 'bin/linux64'
    base = Path(sys.argv[1]).read_text()
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='apex-logs-', dir='/tmp') as scratch:
        for name, text in variants(base).items():
            work = Path(scratch) / name
            work.mkdir()
            vmf = work / f'{name}.vmf'
            vmf.write_text(text)
            for tool, flags in (('vbsp', []), ('vvis', ['-fast']), ('vrad', ['-fast'])):
                if tool != 'vbsp' and not (work / f'{name}.bsp').exists():
                    break
                target = str(vmf.with_suffix('')) if tool != 'vbsp' else str(vmf)
                run = subprocess.run([str(tools / tool), *flags, '-game', str(game), target],
                                     cwd=work, capture_output=True, text=True, timeout=600,
                                     stdin=subprocess.DEVNULL)
                log = (run.stdout + run.stderr).replace(str(work), '<work>').replace(str(install), '<install>')
                (OUT / f'{name}.{tool}.log').write_text(log + f'\n[exit {run.returncode}]\n')
                if tool == 'vbsp':  # 0.5.3: the files the overlays read (D6), paths made portable
                    for ext in ('lin', 'prt'):
                        produced = work / f'{name}.{ext}'
                        if produced.is_file():
                            (OUT / f'{name}.{ext}').write_bytes(produced.read_bytes())
                print(f'{name}.{tool}: exit {run.returncode}, {len(log.splitlines())} lines')
                if run.returncode != 0:
                    break
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
