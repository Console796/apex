#!/usr/bin/env python3
"""D17 gate 2: compile a copied chamber VMF with native vbsp and vvis -fast.

APEX_P2CE_DIR supplies the read-only game install. All output goes into a fresh
system temporary directory. Exit 0: pass; 1: compile/leak failure; 2: could not run.
"""
import argparse
import os
from pathlib import Path
import shutil
import tempfile

from vbsp_equivalence import compile_vbsp, game_directory, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('map', type=Path)
    args = parser.parse_args()
    try:
        install = os.environ.get('APEX_P2CE_DIR')
        if not install:
            raise OSError('APEX_P2CE_DIR is unset')
        root = Path(install).resolve()
        game = game_directory(root)
        for tool in ('vbsp', 'vvis'):
            executable = root / 'bin/linux64' / tool
            if not executable.is_file() or not os.access(executable, os.X_OK):
                raise OSError(f'cannot execute {executable}')
        source = args.map.resolve(strict=True)
        # Do not let TMPDIR redirect compiler writes into the game installation.
        if Path('/tmp').resolve().is_relative_to(root):
            raise OSError('/tmp must be outside APEX_P2CE_DIR')
        with tempfile.TemporaryDirectory(prefix='apex-chamber-', dir='/tmp') as directory:
            work = Path(directory).resolve()
            if work.is_relative_to(root):
                raise OSError('temporary directory must be outside APEX_P2CE_DIR')
            vmf = work / source.name
            shutil.copyfile(source, vmf)
            try:
                output = compile_vbsp(root, game, vmf)
                if 'LEAKED' in output.upper() or any(work.glob('*.lin')):
                    print('FAIL: vbsp reports a leak')
                    return 1
                bsp = vmf.with_suffix('.bsp')
                if not bsp.is_file():
                    print('FAIL: vbsp did not produce a BSP')
                    return 1
                run([root / 'bin/linux64/vvis', '-fast', '-game', game, bsp],
                    work, work / 'vvis.log')
            except RuntimeError as error:
                print(f'FAIL: chamber compile: {error}')
                return 1
        print('PASS: vbsp completed without leaks; vvis -fast completed')
        return 0
    except OSError as error:
        print(f'Chamber compile could not run: {error}')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
