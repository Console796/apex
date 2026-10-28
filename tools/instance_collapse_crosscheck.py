#!/usr/bin/env python3
"""Compare Apex collapse with srctools, without copying its implementation.

Usage: .venv-tools/bin/python tools/instance_collapse_crosscheck.py EXECUTABLE [P2CE_DIR]
The optional --report JSON records every failed map and its cause. Nothing is added to ci.py.

Exceptions are reported, never silently accepted:
* missing-template: the SDK references Portal 2 instance files absent from P2CE. Neither
  implementation can collapse those maps; report their names and missing paths.
* srctools-error: reference parser/collapse errors are inconclusive, not passes.

Known semantic differences are deliberately NOT normalized away: srctools does not
substitute materials or unknown keyvalues, substitutes only output targets, and its
incoming-proxy path omits name fixup and uses min(-1, n) for fire counts. Apex resolves
these per D9 (unlimited is not a negative limit). These are not vbsp-verified exceptions.
IDs, editor metadata, ordering and equivalent numeric/angle spelling are excluded.
The count of anonymous-order exceptions can vary with reference set iteration.
Anonymous namespace order is accepted only after proving a global bijection of all
keyvalues and connections; named unknown-key differences are listed explicitly below.
"""
from __future__ import annotations

import argparse
from collections import Counter
import contextlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from itertools import permutations, product

from srctools import Keyvalues
from srctools.fgd import EntityDef, ValueTypes
from srctools.instancing import collapse_all, get_inst_locs
from srctools.math import Angle, Matrix, Vec
from srctools.vmf import VMF

# Explicit differences, checked against the complete result before accepting them.
EXCEPTIONS = {
    'missing-template': 'SDK references files not shipped in this P2CE installation',
    'invalid-instance-file': 'The exact map listed below has an instance without a file key; both implementations reject it',
    'proxy-input-case': 'srctools fails to resolve mixed-case instance inputs; only the exact routes below are accepted (not vbsp-verified)',
    'anonymous-order': 'A globally consistent bijection proves equality; srctools iterates an identity-hashed set of instances',
    'unknown-key-substitution': 'D9 substitutes all keyvalues; srctools skips keys absent from its engine FGD (not vbsp-verified)',
}
# Only these observed keyvalue differences are accepted, and the complete maps must
# compare equal after this exact replacement (and any proved namespace bijection).
INVALID_FILE_EXCEPTIONS = {'instances/test_chambers/sp_a4_laser_cat_box_1_wall.vmf'}
UNKNOWN_KEY_EXCEPTIONS = {
    'instances/p2editor/coop_exit.vmf': ('disable_pti_audio', '$disable_pti_audio', '0'),
    'instances/p2editor/elevator_entrance.vmf': ('disable_pti_audio', '$disable_pti_audio', '0'),
}
# These exact proxy routes are affected by srctools' case-sensitive lookup of an
# input against its lowercased proxy registry. Do not accept other I/O differences.
PROXY_INPUT_EXCEPTIONS = {
    'instances/chamber_hallways/entry_hallway_tall_dirty_nocaps_bustfloor.vmf': [('info_sign', 'info_panel_activate_rl', 'trigger')],
    'instances/coop/coop_endlevel_room.vmf': [(player, local, command) for player in ['blue', 'orange']
        for local, command in [('ballbot_assembler_relay', 'trigger'), ('cam_botview', 'disable'), ('trigger_exit_lift', 'kill')]],
    'instances/coop/coop_team_double_exit_door.vmf': [('exit_door', '@exit_door', 'open')],
    'instances/multiplayer/two_player_airlock.vmf': [
        (player, local, command) for player in ['blue_dropper', 'red_dropper']
        for local, command in [('initial_spawn', 'disable'), ('relay_set_enabled_active', 'trigger')]
    ] + [(door, local, 'trigger') for door in ['door1', 'door2']
         for local in ['airlock_entry_door_close_rl', 'airlock_entry_door_open_rl']],
    'instances/multiplayer/two_player_airlock_tbeam.vmf': [
        (player, local, command) for player in ['blue_dropper', 'red_dropper']
        for local, command in [('initial_spawn', 'disable'), ('relay_set_enabled_active', 'trigger')]
    ] + [(door, local, 'trigger') for door in ['door1', 'door2']
         for local in ['airlock_entry_door_close_rl', 'airlock_entry_door_open_rl']],
    'instances/p2editor/coop_exit.vmf': [('global_ents', 'environment_testchamber', 'trigger')] + [
        (player, local, command) for player in ['blue', 'orange']
        for local, command in [('ballbot_assembler_relay', 'trigger'), ('cam_botview', 'disable'), ('trigger_exit_lift', 'kill')]],
    'instances/p2editor/elevator_entrance.vmf': [('global_ents', 'environment_testchamber', 'trigger')],
    **{f'instances/p2editor/door_entrance_{suffix}.vmf': [('info_sign', 'info_panel_activate_rl', 'trigger')]
       for suffix in ['1', '2', '3', '4', '5', '6', '7', 'coop_1']},
}
KEY_TYPES = {}
AUTO_NAME = re.compile(r'InstanceAuto([0-9]+)(?![0-9])')
NUMBERS = re.compile(r'[\s\d.eE+(),\[\]-]+\Z')
NUMBER = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def normalized(value: str, key: str = '', classname: str = '') -> str:
    if key == 'angles':
        matrix = Matrix.from_angle(Angle.from_str(value))
        return ' '.join(str(round(matrix[row, col], 4) + 0.0) for row in range(3) for col in range(3))
    if KEY_TYPES.get((classname.casefold(), key.casefold())) in {7, 8, 9} and NUMBER.fullmatch(value):
        return f'{round(float(value) % 360, 4) % 360:.10g}'
    if not value or not NUMBERS.fullmatch(value):
        return value
    def number(match: re.Match) -> str:
        value = float(match.group())
        if key == 'angles':
            value %= 360
            if abs(value - 360) < .00005:
                value = 0
        return f'{round(value, 4):.10g}' if abs(value) >= .00005 else '0'
    return ' '.join(NUMBER.sub(number, value).split())


def side_signature(side) -> tuple:
    a, b, c = side.planes
    normal = Vec.cross(b - a, c - a).norm()
    distance = Vec.dot(normal, a)
    # Texture mapping rides along with the plane: a transformed instance has to rotate the UV axes
    # and carry the shifts, scales, rotation, lightmap scale and smoothing groups unchanged.
    # Axis directions are compared loosely (the shift absorbs the transform's translation).
    mapping = tuple((tuple(round(float(v), 3) + 0.0 for v in (axis.x, axis.y, axis.z)),
                     round(float(axis.offset), 2) + 0.0, round(float(axis.scale), 5) + 0.0)
                    for axis in (side.uaxis, side.vaxis))
    display = (side.disp_power, side.disp_flags) if side.is_disp else None
    return (tuple(round(v, 4) + 0.0 for v in normal), round(distance, 3) + 0.0, side.mat, mapping,
            round(float(side.ham_rot), 3) + 0.0, side.lightmap, side.smooth, display)


def solid(solid) -> tuple:
    return tuple(sorted(side_signature(side) for side in solid.sides))


def canonical(vmf: VMF) -> dict:
    entities = Counter()
    brushes = list(vmf.brushes) + [brush for ent in vmf.entities for brush in ent.solids]
    sides = {side.id: side_signature(side) for brush in brushes for side in brush.sides}
    def keyvalue(ent, key, value):
        if KEY_TYPES.get((ent['classname'].casefold(), key.casefold())) == 10:
            # Side-list keyvalues refer to the same faces even when all side IDs change.
            try:
                return json.dumps(sorted(sides[int(word)] for word in value.split()))
            except (KeyError, ValueError):
                pass  # Retain dangling references so a broken link remains a mismatch.
        return normalized(value, key.casefold(), ent['classname'])
    for ent in vmf.entities:
        keys = tuple(sorted((key.casefold(), keyvalue(ent, key, value))
                            for key, value in ent.items() if key.casefold() not in {'id', 'hammerid'}))
        connections = tuple(sorted((out.exp_out().casefold(), out.target, out.exp_in().casefold(),
                                    normalized(out.params), round(out.delay, 4), out.times)
                                   for out in ent.outputs))
        brushes = tuple(sorted(solid(brush) for brush in ent.solids))
        entities[(keys, connections, brushes)] += 1
    world = tuple(sorted((key.casefold(), normalized(value, key.casefold(), 'worldspawn'))
                         for key, value in vmf.spawn.items() if key.casefold() not in {'id', 'hammerid'}))
    return {'entities': entities, 'solids': Counter(solid(brush) for brush in vmf.brushes),
            'world': Counter({world: 1})}


def rewrite_names(item, mapping):
    if isinstance(item, str):
        return AUTO_NAME.sub(lambda match: 'InstanceAuto' + mapping.get(match[1], match[1]), item)
    if isinstance(item, tuple):
        return tuple(rewrite_names(part, mapping) for part in item)
    return item


def renamed_counter(counter, mapping):
    result = Counter()
    for item, count in counter.items():
        result[rewrite_names(item, mapping)] += count
    return result


def namespace_bijection(expected, actual):
    """Accept numbering order only after proving equality of the entire entity graph."""
    def profiles(counter):
        result = {}
        for item, count in counter.items():
            raw = repr(item)
            names = set(AUTO_NAME.findall(raw))
            fingerprint = rewrite_names(item, {name: '#' for name in names})
            for name in names:
                result.setdefault(name, Counter())[fingerprint] += count
        grouped = {}
        for name, profile in result.items():
            grouped.setdefault(tuple(sorted(profile.items())), []).append(name)
        return grouped
    left, right = profiles(expected), profiles(actual)
    if not left or left.keys() != right.keys():
        return None
    candidates = []
    combinations = 1
    for profile, names in left.items():
        other = right[profile]
        if len(names) != len(other):
            return None
        # Ambiguous profiles are rare; refuse an unbounded graph-isomorphism search.
        if len(names) > 6:
            return None
        alternatives = [dict(zip(names, choice)) for choice in permutations(other)]
        combinations *= len(alternatives)
        if combinations > 4096:
            return None
        candidates.append(alternatives)
    for parts in product(*candidates):
        mapping = {key: value for part in parts for key, value in part.items()}
        if renamed_counter(expected, mapping) == actual:
            return mapping
    return None


def justified_difference(name, expected, actual):
    causes = []
    reference = expected['entities']
    if name in UNKNOWN_KEY_EXCEPTIONS:
        key, old, new = UNKNOWN_KEY_EXCEPTIONS[name]
        patched = Counter()
        for (keys, connections, brushes), count in reference.items():
            keys = tuple((k, new if k == key and v == old else v) for k, v in keys)
            patched[keys, connections, brushes] += count
        if patched != reference:
            reference = patched
            causes.append('unknown-key-substitution')
    if name in PROXY_INPUT_EXCEPTIONS:
        routes = {(instance, f'instance:{local};{command}'):
                  (local if local.startswith(('@', '!')) else instance + '-' + local, command)
                  for instance, local, command in PROXY_INPUT_EXCEPTIONS[name]}
        patched = Counter()
        for (keys, connections, brushes), count in reference.items():
            outputs = []
            for output, target, inp, param, delay, times in connections:
                target, inp = routes.get((target, inp), (target, inp))
                outputs.append((output, target, inp, param, delay, times))
            patched[keys, tuple(sorted(outputs)), brushes] += count
        if patched != reference:
            reference = patched
            causes.append('proxy-input-case')
    mapping = None
    if reference != actual['entities']:
        mapping = namespace_bijection(reference, actual['entities'])
        if mapping is None:
            return None
        causes.append('anonymous-order')
    if any(expected[key] != actual[key] for key in expected if key != 'entities') or not causes:
        return None
    return {'causes': causes, 'namespace_bijection': mapping}


def key_types(path: Path) -> None:
    types = {ValueTypes.TARG_DEST_CLASS: 2, ValueTypes.VEC: 3, ValueTypes.VEC_ORIGIN: 3,
             ValueTypes.VEC_LINE: 3, ValueTypes.EXT_VEC_DIRECTION: 4, ValueTypes.ANGLES: 5,
             ValueTypes.VEC_AXIS: 6, ValueTypes.EXT_ANGLE_PITCH: 7,
             ValueTypes.ANGLE_NEG_PITCH: 8, ValueTypes.SIDE_LIST: 10}
    with path.open('w') as file:
        for classname in sorted(EntityDef.engine_classes()):
            definition = EntityDef.engine_def(classname)
            file.write(f'{classname.casefold()}\tclassname\t0\n')
            for key in definition.kv:
                kv = definition.kv[key]
                typ = 1 if kv.type.is_ent_name else types.get(kv.type, 0)
                # Critical keys have compiler-defined types regardless of FGD metadata.
                typ = {'origin': 3, 'angles': 5, 'yaw': 9}.get(key.casefold(), typ)
                KEY_TYPES[classname.casefold(), key.casefold()] = typ
                file.write(f'{classname.casefold()}\t{key.casefold()}\t{typ}\n')


def without_viewsettings(tree: Keyvalues) -> Keyvalues:
    # Strata Hammer saves a 16384 sentinel for 2D view axes; srctools requires
    # 65536. View settings have no compile meaning and are outside comparison.
    if 'viewsettings' in tree:
        del tree['viewsettings']
    return tree


class ReferenceFiles:
    def __init__(self, path: Path):
        self.files = get_inst_locs(path)

    def read_kv1(self, path: str):
        return without_viewsettings(self.files.read_kv1(path))


def parse(text: str) -> VMF:
    return VMF.parse(without_viewsettings(Keyvalues.parse(text)), preserve_ids=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('corpus', nargs='?', type=Path, default=os.environ.get('APEX_P2CE_DIR'))
    parser.add_argument('--report', type=Path)
    parser.add_argument('--map', help='optional relative-path glob for investigating a mismatch')
    args = parser.parse_args()
    if args.corpus is None:
        parser.error('set APEX_P2CE_DIR or supply a corpus directory')
    root = args.corpus / 'sdk_content' / 'maps'
    if not root.is_dir():
        parser.error(f'no SDK maps at {root}')
    report = {'maps_checked': 0, 'maps_with_instances': 0, 'compared': 0, 'matched': 0,
              'causes': Counter(), 'failures': [], 'exceptions': EXCEPTIONS}
    with tempfile.TemporaryDirectory(prefix='apex-instance-oracle-') as temp:
        table = Path(temp) / 'types.tsv'
        key_types(table)
        for path in sorted(root.rglob('*.vmf')):
            if args.map and not path.relative_to(root).match(args.map):
                continue
            report['maps_checked'] += 1
            name = str(path.relative_to(root))
            try:
                source = parse(path.read_text(encoding='utf-8-sig'))
                if not source.by_class['func_instance']:
                    continue
                report['maps_with_instances'] += 1
                # Suppress reference warnings, retaining structured per-map failures.
                with contextlib.redirect_stdout(io.StringIO()):
                    collapse_all(source, ReferenceFiles(path), recur_limit=8)
            except FileNotFoundError as error:
                apex = subprocess.run([str(args.executable.resolve()), str(path), str(table)],
                                      capture_output=True, text=True,
                                      env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
                cause = 'missing-template' if apex.returncode and 'missing template:' in apex.stderr else 'file-lookup-disagreement'
                if name in INVALID_FILE_EXCEPTIONS and str(error) == '' and apex.returncode == 1 and apex.stderr.strip() == 'instance has no file key':
                    cause = 'invalid-instance-file'
                report['causes'][cause] += 1
                report['failures'].append({'map': name, 'cause': cause,
                    'detail': {'srctools': str(error), 'apex_exit': apex.returncode, 'apex': apex.stderr[-4000:]}})
                continue
            except Exception as error:
                report['causes']['srctools-error'] += 1
                report['failures'].append({'map': name, 'cause': 'srctools-error', 'detail': str(error)})
                continue
            apex = subprocess.run([str(args.executable.resolve()), str(path), str(table)],
                                  capture_output=True, text=True,
                                  env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            if apex.returncode:
                report['causes']['apex-error'] += 1
                report['failures'].append({'map': name, 'cause': 'apex-error', 'detail': apex.stderr[-4000:]})
                continue
            try:
                expected, actual = canonical(source), canonical(parse(apex.stdout))
            except Exception as error:
                report['causes']['comparison-error'] += 1
                report['failures'].append({'map': name, 'cause': 'comparison-error', 'detail': str(error)})
                continue
            report['compared'] += 1
            causes = [key for key in expected if expected[key] != actual[key]]
            if not causes:
                report['matched'] += 1
            else:
                justified = justified_difference(name, expected, actual)
                if justified:
                    for cause in justified['causes']:
                        report['causes'][cause] += 1
                    report['failures'].append({'map': name, 'cause': '+'.join(justified['causes']), 'detail': justified})
                    continue
                detail = {}
                for key in causes:
                    report['causes'][key] += 1
                    detail[key] = {'srctools_only': [repr(item) for item in (expected[key] - actual[key]).elements()][:4],
                                   'apex_only': [repr(item) for item in (actual[key] - expected[key]).elements()][:4]}
                report['failures'].append({'map': name, 'cause': '+'.join(causes), 'detail': detail})
            if report['maps_with_instances'] % 20 == 0:
                print(f"checked {report['maps_checked']} maps; {report['maps_with_instances']} with instances", flush=True)
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in {'failures', 'exceptions'}}, indent=2))
    for failure in report['failures']:
        print(f"{failure['map']}: {failure['cause']}")
    return int(any(cause not in EXCEPTIONS for cause in report['causes']))


if __name__ == '__main__':
    raise SystemExit(main())
