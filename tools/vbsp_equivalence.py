#!/usr/bin/env python3
"""Manual D8 gate: compile an original and a re-planed copy of a map in isolation; compare vbspinfo.

Requires APEX_P2CE_DIR and a built apex_replane. Never modifies the game install.
Exit 0 means matching counts, 1 means differences, 2 means the check could not run.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def run(command, cwd, log):
    result = subprocess.run([str(arg) for arg in command], cwd=cwd,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}: exit {result.returncode}\n"
                           f"{result.stdout[-6000:]}")
    return result.stdout


def counts(output):
    result = {}
    for line in output.splitlines():
        match = re.match(r"^(.+?)\s+(\d+|\[variable\])/(\d+)\s+(\d+)/(\d+)\s+\(", line)
        # Variable-size lumps report bytes rather than an object count.
        if not match:
            match = re.match(r"^(.+?)\s+(\[variable\])\s+(\d+)/(\d+)\s+\(", line)
            if match:
                result[match[1].strip()] = (None, int(match[3]))
        else:
            result[match[1].strip()] = (int(match[2]), int(match[4]))
    triangle = re.search(r"Total triangle count:\s*(\d+)", output)
    if triangle:
        result['total triangles'] = (int(triangle[1]), None)
    if 'faces' not in result or len(result) < 20:
        raise RuntimeError('unrecognized or incomplete vbspinfo table')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--replane', type=Path,
                        default=Path(__file__).resolve().parent.parent / 'build/linux-gcc-release/apex_replane')
    parser.add_argument('map', type=Path, help='a complete, sealed VMF to compile (it is copied, never modified)')
    parser.add_argument('--output-dir', type=Path, help='retain VMFs, BSPs and logs here')
    args = parser.parse_args()
    install = os.environ.get('APEX_P2CE_DIR')
    if not install:
        parser.error('APEX_P2CE_DIR is unset')
    root = Path(install).resolve()
    game = next((root / name for name in ('p2ce', 'portal2')
                 if (root / name / 'gameinfo.txt').is_file()), None)
    if game is None:
        parser.error('no p2ce or portal2 gameinfo.txt found')
    temporary = None
    try:
        if args.output_dir:
            work = args.output_dir.resolve()
            work.mkdir(parents=True, exist_ok=False)
        else:
            temporary = tempfile.TemporaryDirectory(prefix='apex-vbsp-')
            work = Path(temporary.name)
        source = args.map.resolve()
        original = work / 'original' / source.name
        replaned = work / 'replaned' / source.name
        original.parent.mkdir()
        replaned.parent.mkdir()
        shutil.copyfile(source, original)
        run([args.replane.resolve(), source, replaned], work, work / 'replane.log')
        plane_value = re.compile(rb'("plane"\s+)"[^"\r\n]*"')
        before, after = original.read_bytes(), replaned.read_bytes()
        if (plane_value.sub(rb'\1"<plane>"', before) !=
                plane_value.sub(rb'\1"<plane>"', after)):
            raise RuntimeError('replane changed content outside plane strings')
        tables = []
        for vmf in (original, replaned):
            run([root / 'bin/linux64/vbsp', '-game', game, '-instancepath', root / 'sdk_content/maps',
                 vmf], vmf.parent, vmf.parent / 'vbsp.log')
            bsp = vmf.with_suffix('.bsp')
            if not bsp.is_file():
                raise RuntimeError(f'vbsp did not produce {bsp}')
            output = run([root / 'bin/linux64/vbspinfo', bsp], vmf.parent,
                         vmf.parent / 'vbspinfo.log')
            tables.append(counts(output))
        print('lump: original objects/bytes -> replaned objects/bytes')
        for key in sorted(tables[0].keys() | tables[1].keys()):
            left, right = (table.get(key) for table in tables)
            print(f'{key}: {left} -> {right}' + (' DIFFERENT' if left != right else ''))
        same = tables[0] == tables[1]
        print('PASS: all vbspinfo counts match' if same else 'FAIL: D8 requires review')
        return 0 if same else 1
    except (OSError, RuntimeError) as error:
        print(f'VBSP equivalence could not complete: {error}')
        return 2
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
