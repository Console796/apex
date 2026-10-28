#!/usr/bin/env python3
"""Manual 0.5.1 gate: generate D17's chamber, then run the real Fast compile job.

Requires APEX_P2CE_DIR and linux-asan test binaries. The game install is read-only;
all compiler output lives in /tmp. Exit 0: pass; 1: test failure; 2: could not run.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def main():
    try:
        install = os.environ.get('APEX_P2CE_DIR')
        if not install:
            raise OSError('APEX_P2CE_DIR is unset')
        if Path('/tmp').resolve().is_relative_to(Path(install).resolve()):
            raise OSError('/tmp must be outside APEX_P2CE_DIR')
        env = os.environ.copy()
        env['LSAN_OPTIONS'] = f'suppressions={ROOT / "tests/lsan.supp"}'
        with tempfile.TemporaryDirectory(prefix='apex-chamber-runner-', dir='/tmp') as directory:
            chamber = Path(directory) / 'chamber.vmf'
            env['APEX_CHAMBER_OUT'] = str(chamber)
            subprocess.run([ROOT / 'build/linux-asan/tests/apex_tests',
                            '-tc=*D17 scripted chamber*'], cwd=ROOT, env=env,
                           check=True, timeout=120)
            if not chamber.is_file():
                raise OSError('D17 did not write the chamber VMF')
            env['APEX_CHAMBER_VMF'] = str(chamber)
            for name in ('DISPLAY', 'WAYLAND_DISPLAY'):
                env.pop(name, None)
            env.update(QT_QPA_PLATFORM='offscreen', ASAN_OPTIONS='detect_leaks=0',
                       APEX_SKIP_RECOVERY='1')
            # Probe bus creation first: never retry a failing test as a bus fallback.
            bus = shutil.which('dbus-run-session')
            isolated = bus and subprocess.run([bus, '--', 'true'], env=env,
                                             capture_output=True).returncode == 0
            command = [ROOT / 'build/linux-asan/apex_ui_tests', '-tc=compile_chamber*']
            if isolated:
                command = [bus, '--', *command]
            else:
                env['DBUS_SESSION_BUS_ADDRESS'] = 'disabled:'
            subprocess.run(command, cwd=ROOT, env=env, check=True, timeout=660)
        print('PASS: D17 chamber compiled through QProcessRunner + CompileJob (Fast)')
        return 0
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f'FAIL: chamber runner: {error}')
        return 1
    except OSError as error:
        print(f'Chamber runner could not run: {error}')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
