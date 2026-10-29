#!/usr/bin/env python3
"""Lighting 0.6 P0 probe: measure how native P2CE VRAD turns light entity keys into worldlights.

Generates sealed box-room VMFs (many lights per map, spaced far apart and matched back by origin),
compiles each with native vbsp, vvis -fast and vrad -fast in a fresh /tmp directory, then parses
LUMP_WORLDLIGHTS (15) and LUMP_WORLDLIGHTS_HDR (54) directly with struct and prints one row per
input light. Nothing is written inside APEX_P2CE_DIR. Authority is measurement only.

Usage: tools/light_probe.py [--group NAME ...] [--keep] [--raw] [--vrad-arg=-final ...]
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

LUMP_WORLDLIGHTS, LUMP_WORLDLIGHTS_HDR = 15, 54
EMIT = {0: 'surface', 1: 'point', 2: 'spot', 3: 'skylight', 4: 'quakelight', 5: 'skyambient'}
HALF = 2560          # room interior half-extent
SPACING = 640        # distance between probe lights on the grid


# ---------------------------------------------------------------- VMF generation
def _side(sid, pts, material):
    plane = ' '.join(f'({x:g} {y:g} {z:g})' for x, y, z in pts)
    return (f'\t\tside\n\t\t{{\n\t\t\t"id" "{sid}"\n\t\t\t"plane" "{plane}"\n'
            f'\t\t\t"material" "{material}"\n\t\t\t"uaxis" "[1 0 0 0] 0.25"\n'
            f'\t\t\t"vaxis" "[0 -1 0 0] 0.25"\n\t\t\t"rotation" "0"\n'
            f'\t\t\t"lightmapscale" "64"\n\t\t\t"smoothing_groups" "0"\n\t\t}}\n')


def _block(ids, lo, hi, material, inner_material=None):
    """Axis-aligned brush; inner_material (if given) goes on the bottom face (the ceiling slab)."""
    (x1, y1, z1), (x2, y2, z2) = lo, hi
    faces = [
        ((x1, y2, z2), (x2, y2, z2), (x2, y1, z2)),   # top
        ((x1, y1, z1), (x2, y1, z1), (x2, y2, z1)),   # bottom
        ((x1, y2, z2), (x1, y1, z2), (x1, y1, z1)),   # -x
        ((x2, y2, z1), (x2, y1, z1), (x2, y1, z2)),   # +x
        ((x2, y2, z2), (x1, y2, z2), (x1, y2, z1)),   # +y
        ((x2, y1, z1), (x1, y1, z1), (x1, y1, z2)),   # -y
    ]
    out = f'\tsolid\n\t{{\n\t\t"id" "{next(ids)}"\n'
    for i, f in enumerate(faces):
        out += _side(next(ids), f, inner_material if (i == 1 and inner_material) else material)
    return out + '\t}\n'


def make_vmf(lights, sky=False):
    """lights: list of (classname, origin, keys). A sky map gets a toolsskybox ceiling."""
    ids = iter(range(1, 100000))
    h, t, zlo, zhi = HALF, 64, -256, 1024
    wall = 'dev/dev_measuregeneric01b'
    ceiling = 'tools/toolsskybox' if sky else wall
    solids = [
        _block(ids, (-h - t, -h - t, zlo - t), (h + t, h + t, zlo), wall),
        _block(ids, (-h - t, -h - t, zhi), (h + t, h + t, zhi + t), wall, ceiling),
        _block(ids, (-h - t, -h - t, zlo), (-h, h + t, zhi), wall),
        _block(ids, (h, -h - t, zlo), (h + t, h + t, zhi), wall),
        _block(ids, (-h, -h - t, zlo), (h, -h, zhi), wall),
        _block(ids, (-h, h, zlo), (h, h + t, zhi), wall),
    ]
    text = ('versioninfo\n{\n\t"editorversion" "400"\n\t"editorbuild" "10773"\n\t"mapversion" "1"\n'
            '\t"formatversion" "100"\n\t"prefab" "0"\n}\n'
            'world\n{\n\t"id" "1"\n\t"mapversion" "1"\n\t"classname" "worldspawn"\n'
            '\t"skyname" "sky_black_nofog"\n' + ''.join(solids) + '}\n')
    for cls, origin, keys in lights + [('info_player_start', (0, 0, zlo + 16), {})]:
        text += f'entity\n{{\n\t"id" "{next(ids)}"\n\t"classname" "{cls}"\n'
        text += f'\t"origin" "{origin[0]:g} {origin[1]:g} {origin[2]:g}"\n'
        for k, v in keys.items():
            text += f'\t"{k}" "{v}"\n'
        text += '}\n'
    return text


# ---------------------------------------------------------------- BSP parsing
def read_lumps(path):
    data = Path(path).read_bytes()
    ident, version = struct.unpack_from('<4si', data, 0)
    if ident != b'VBSP':
        raise RuntimeError(f'{path}: not a VBSP file')
    lumps = {}
    for i in range(64):
        ofs, ln, ver, fourcc = struct.unpack_from('<iii4s', data, 8 + 16 * i)
        lumps[i] = (ver, data[ofs:ofs + ln], fourcc)
    return version, lumps


# Inferred record layout (see docs/plans/probe-results/lighting_0_6_p0.md); trailing words that are
# not identified are kept raw so a size change is visible rather than silently misparsed.
FIELDS = [('origin', '3f'), ('intensity', '3f'), ('normal', '3f'), ('shadow_cast_offset', '3f'),
          ('cluster', 'i'), ('type', 'B'), ('mode_b1', 'B'), ('mode_b2', 'B'), ('mode_b3', 'B'),
          ('style', 'i'), ('stopdot', 'f'), ('stopdot2', 'f'),
          ('exponent', 'f'), ('radius', 'f'), ('constant', 'f'), ('linear', 'f'),
          ('quadratic', 'f'), ('flags', 'i'), ('texinfo', 'i'), ('owner', 'i')]
KNOWN = struct.calcsize('<' + ''.join(f for _, f in FIELDS))


def parse_worldlights(blob, count_hint=None):
    """Return (record_size, [dict]). Record size defaults to the 100-byte layout when it divides."""
    if not blob:
        return 0, []
    size = None
    for candidate in (KNOWN, 88, 104, 108, 112, 116, 120, 124, 128):
        if len(blob) % candidate == 0 and (count_hint is None or len(blob) // candidate == count_hint):
            size = candidate
            break
    if size is None:
        raise RuntimeError(f'cannot infer worldlight size from {len(blob)} bytes')
    out = []
    for base in range(0, len(blob), size):
        rec, pos = {}, base
        for name, f in FIELDS:
            vals = struct.unpack_from('<' + f, blob, pos)
            pos += struct.calcsize('<' + f)
            rec[name] = vals if len(vals) > 1 else vals[0]
        extra = blob[base + KNOWN: base + size]
        rec['extra_i'] = struct.unpack(f'<{len(extra)//4}i', extra) if extra else ()
        rec['raw'] = blob[base:base + size]
        out.append(rec)
    return size, out


# ---------------------------------------------------------------- compile
def compile_map(root, game, work, name, text, vrad_args):
    vmf = work / f'{name}.vmf'
    vmf.write_text(text)
    bindir = root / 'bin/linux64'
    env = dict(os.environ, LD_LIBRARY_PATH=str(bindir))
    logs = []
    for tool, args in (('vbsp', ['-game', game, vmf]), ('vvis', ['-fast', '-game', game, vmf.with_suffix('.bsp')]),
                       ('vrad', ['-fast', *vrad_args, '-game', game, vmf.with_suffix('.bsp')])):
        r = subprocess.run([str(bindir / tool), *map(str, args)], cwd=work, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        (work / f'{name}.{tool}.log').write_text(r.stdout)
        logs.append(r.stdout)
        if r.returncode:
            raise RuntimeError(f'{tool} failed on {name} ({r.returncode}):\n{r.stdout[-3000:]}')
        if tool == 'vbsp' and 'LEAKED' in r.stdout:
            raise RuntimeError(f'{name}: map leaked')
    return vmf.with_suffix('.bsp'), logs


# ---------------------------------------------------------------- probe cases
def L(cls, **keys):
    return (cls, {k.replace('__', ''): v for k, v in keys.items()})


def point(**k):
    base = {'_light': '255 255 255 200', '_lightHDR': '-1 -1 -1 1', '_lightscaleHDR': '1',
            '_constant_attn': '0', '_linear_attn': '0', '_quadratic_attn': '1'}
    base.update(k)
    return base


def spot(**k):
    base = point(_inner_cone='30', _cone='45', _exponent='1', pitch='-90', angles='0 0 0')
    base.update(k)
    return base


GROUPS = {
    'color': [
        *[('light', f'R={r} A={a}', point(_light=f'{r} 255 64 {a}')) for r in (255, 128, 64, 0) for a in (100, 200, 400)],
        ('light', 'HDR explicit 255 0 0 50', point(_light='255 255 255 200', _lightHDR='255 0 0 50')),
        ('light', 'HDR explicit 128 128 128 400', point(_light='255 255 255 200', _lightHDR='128 128 128 400')),
        ('light', 'scaleHDR 2', point(_lightscaleHDR='2')),
        ('light', 'scaleHDR 0.5 + HDR expl', point(_lightHDR='255 255 255 100', _lightscaleHDR='0.5')),
        ('light', 'three-value _light', point(_light='255 128 64')),
        ('light', 'one-value _light 128', point(_light='128')),
        ('light', '_light missing', {k: v for k, v in point().items() if k != '_light'}),
    ],
    'attn': [
        ('light', 'c0 l0 q1', point()),
        ('light', 'c1 l0 q0', point(_constant_attn='1', _quadratic_attn='0')),
        ('light', 'c0 l1 q0', point(_linear_attn='1', _quadratic_attn='0')),
        ('light', 'c0 l0 q0 (all zero)', point(_quadratic_attn='0')),
        ('light', 'c1 l1 q1', point(_constant_attn='1', _linear_attn='1', _quadratic_attn='1')),
        ('light', 'c2 l0 q0', point(_constant_attn='2', _quadratic_attn='0')),
        ('light', 'c0 l0 q2', point(_quadratic_attn='2')),
        ('light', 'c0 l0.5 q0.25', point(_linear_attn='0.5', _quadratic_attn='0.25')),
        ('light', '50%=100 0%=0', point(_fifty_percent_distance='100')),
        ('light', '50%=100 0%=400', point(_fifty_percent_distance='100', _zero_percent_distance='400')),
        ('light', '50%=200 0%=400', point(_fifty_percent_distance='200', _zero_percent_distance='400')),
        ('light', '50%=100 0%=400 hard', point(_fifty_percent_distance='100', _zero_percent_distance='400', _hardfalloff='1')),
        ('light', '50%=200 0%=1000', point(_fifty_percent_distance='200', _zero_percent_distance='1000')),
        ('light', '0%=400 only', point(_zero_percent_distance='400')),
        ('light', '_distance 300', point(_distance='300')),
        ('light', '_distance 300 c1 q0', point(_distance='300', _constant_attn='1', _quadratic_attn='0')),
        ('light', 'hard_radius_override 500', point(_hard_radius_override='500')),
        ('light', 'c0 l0 q1 A=400', point(_light='255 255 255 400')),
    ],
    'spot': [
        ('light_spot', 'default 30/45 e1 p-90', spot()),
        ('light_spot', 'inner 10 cone 20', spot(_inner_cone='10', _cone='20')),
        ('light_spot', 'inner 60 cone 80', spot(_inner_cone='60', _cone='80')),
        ('light_spot', 'inner 45 cone 30 (inv)', spot(_inner_cone='45', _cone='30')),
        ('light_spot', 'inner 0 cone 0', spot(_inner_cone='0', _cone='0')),
        ('light_spot', 'cone 90', spot(_inner_cone='89', _cone='90')),
        ('light_spot', 'exponent 0', spot(_exponent='0')),
        ('light_spot', 'exponent 5', spot(_exponent='5')),
        ('light_spot', 'pitch -30 yaw 0', spot(pitch='-30')),
        ('light_spot', 'pitch -30 yaw 90', spot(pitch='-30', angles='0 90 0')),
        ('light_spot', 'pitch 0 angles 30 45 0', spot(pitch='0', angles='30 45 0')),
        ('light_spot', 'pitch 0 angles 0 0 0', spot(pitch='0')),
        ('light_spot', 'no pitch, angles -45 0 0', {k: v for k, v in spot(angles='-45 0 0').items() if k != 'pitch'}),
        ('light_spot', 'spot c1 q0 50%=100', spot(_fifty_percent_distance='100')),
        ('light_rt_spot', 'rt_spot defaults', spot()),
    ],
    'misc': [
        ('light_spot', 'inner 0 cone 45', spot(_inner_cone='0', _cone='45')),
        ('light_spot', 'inner 20 cone 0', spot(_inner_cone='20', _cone='0')),
        ('light_spot', 'cone missing', {k: v for k, v in spot().items() if k != '_cone'}),
        ('light_spot', 'inner missing', {k: v for k, v in spot().items() if k != '_inner_cone'}),
        ('light_spot', 'exponent missing', {k: v for k, v in spot().items() if k != '_exponent'}),
        ('light_spot', 'pitch -30 angles 60 0 0', spot(pitch='-30', angles='60 0 0')),
        ('light_spot', 'pitch 20 angles 0 0 0', spot(pitch='20')),
        ('light_spot', 'pitch missing angles 0 0 30', {k: v for k, v in spot(angles='-30 0 45').items() if k != 'pitch'}),
        ('light', 'castentityshadow 0', point(_castentityshadow='0')),
        ('light', 'shadoworiginoffset 1 2 3', point(_shadoworiginoffset='1 2 3')),
        ('light', 'initially dark sf1', point(spawnflags='1')),
        ('light', 'hard_radius_threshold 8', point(_hard_radius_threshold='8')),
        ('light', '50%=100 0%=400 hard (cmp)', point(_fifty_percent_distance='100', _zero_percent_distance='400', _hardfalloff='1')),
        ('light', '50%=100 0%=400 (cmp)', point(_fifty_percent_distance='100', _zero_percent_distance='400')),
        ('light', '50%=100 0%=50 (zero<fifty)', point(_fifty_percent_distance='100', _zero_percent_distance='50')),
        ('light', '50%=100 0%=300', point(_fifty_percent_distance='100', _zero_percent_distance='300')),
        ('light', 'c0 l0 q1 + _distance 0', point(_distance='0')),
        ('light', 'c-1 l0 q0 (negative)', point(_constant_attn='-1', _quadratic_attn='0')),
        ('light', 'targetname style 0 #2', point(targetname='other')),
        ('light', 'targetname shared', point(targetname='switchable')),
        ('light', 'targetname shared #2', point(targetname='switchable')),
        ('light_rt', 'rt removeaftercompile 0', point(_removeaftercompile='0')),
        ('light', 'light removeaftercompile 1', point(_removeaftercompile='1')),
    ],
    'falloff': [
        ('light', f'50%={f} 0%={z}', point(_fifty_percent_distance=str(f), _zero_percent_distance=str(z)))
        for f, z in ((100, 150), (100, 200), (100, 250), (100, 300), (100, 350), (100, 400), (100, 500),
                     (100, 800), (100, 1600), (200, 600), (50, 150), (200, 400), (200, 800), (300, 900),
                     (64, 256), (128, 512), (256, 1024), (100, 101), (100, 100))
    ],
    'falloff_sweep': [
        ('light', f'50%=100 0%={z}', point(_fifty_percent_distance='100', _zero_percent_distance=str(z)))
        for z in (110, 125, 140, 160, 175, 190, 210, 225, 240, 260, 275, 290, 310, 325, 340, 360, 375,
                  390, 425, 450, 475, 550, 600, 650, 700, 750, 900, 1000, 1100, 1200, 1300, 1400, 1500,
                  1550, 1580, 1590, 1610, 1700, 2000, 2500, 3000, 5000, 10000)
    ],
    'rt': [
        ('light', 'light (no modes)', point()),
        ('light', 'light _directmode 2', point(_directmode='2')),
        ('light', 'light _indirectmode 0', point(_indirectmode='0')),
        ('light_rt', 'rt FGD defaults (d2 i1 s2)', point(_directmode='2', _indirectmode='1', _specularmode='2')),
        ('light_rt', 'rt no mode keys', point()),
        ('light_rt', 'rt d1 i1 s0', point(_directmode='1', _indirectmode='1', _specularmode='0')),
        ('light_rt', 'rt d0 i1', point(_directmode='0', _indirectmode='1')),
        ('light_rt', 'rt d2 i0', point(_directmode='2', _indirectmode='0')),
        ('light_rt', 'rt d0 i0', point(_directmode='0', _indirectmode='0')),
        ('light_rt', 'rt d1 i0', point(_directmode='1', _indirectmode='0')),
        ('light_rt', 'rt s2 d1 i1', point(_directmode='1', _indirectmode='1', _specularmode='2')),
        ('light_rt', 'rt shadowed sf2', point(spawnflags='2')),
        ('light_rt', 'rt removeaftercompile', point(_removeaftercompile='1')),
        ('light_rt_spot', 'rt_spot d1 i1', spot(_directmode='1', _indirectmode='1')),
        ('light_rt_spot', 'rt_spot d2 i0', spot(_directmode='2', _indirectmode='0')),
        ('light', 'light style 10', point(style='10')),
        ('light', 'light targetname', point(targetname='switchable')),
    ],
}
# Dense 50%/0% ratio sweep (f=1000, r = z/f in 0.05 steps from 1.05 to 16), split to fit the 64-light grid.
_FINE = list(range(1050, 16050, 50))
for _i in range(0, len(_FINE), 60):
    GROUPS[f'falloff_fine{_i // 60}'] = [
        ('light', f'50%=1000 0%={z}', point(_fifty_percent_distance='1000', _zero_percent_distance=str(z)))
        for z in _FINE[_i:_i + 60]]

ENV_CASES = [
    ('env pitch -45 yaw 30', {'_light': '255 255 255 200', '_ambient': '255 255 255 20', 'pitch': '-45', 'angles': '0 30 0'}),
    ('env pitch -60 ambient 128/64/0 50', {'_light': '255 128 64 300', '_ambient': '128 64 0 50', 'pitch': '-60', 'angles': '0 0 0', 'SunSpreadAngle': '0'}),
    ('env angles -30 90 0 no pitch', {'_light': '255 255 255 100', '_ambient': '255 255 255 20', 'angles': '-30 90 0'}),
    ('env HDR explicit', {'_light': '255 255 255 200', '_lightHDR': '255 0 0 100', '_lightscaleHDR': '2',
                          '_ambient': '255 255 255 20', '_ambientHDR': '0 0 255 40', '_AmbientScaleHDR': '3', 'pitch': '-45', 'angles': '0 0 0'}),
    ('env spread 20', {'_light': '255 255 255 200', '_ambient': '255 255 255 20', 'pitch': '-45', 'angles': '0 0 0', 'SunSpreadAngle': '20'}),
    ('env no sky faces', {'_light': '255 255 255 200', '_ambient': '255 255 255 20', 'pitch': '-45', 'angles': '0 0 0', '__nosky': '1'}),
]


def grid():
    n = (2 * HALF) // SPACING
    for iy in range(n):
        for ix in range(n):
            yield (-HALF + SPACING // 2 + ix * SPACING, -HALF + SPACING // 2 + iy * SPACING, 256)


def fmtv(v, p=4):
    return '(' + ' '.join(f'{x:.{p}g}' for x in v) + ')'


def row(rec):
    return (f"{EMIT.get(rec['type'], rec['type']):9} I={fmtv(rec['intensity'], 6)} n={fmtv(rec['normal'])} "
            f"c/l/q={rec['constant']:.6g}/{rec['linear']:.6g}/{rec['quadratic']:.6g} r={rec['radius']:.6g} "
            f"sd={rec['stopdot']:.6g} sd2={rec['stopdot2']:.6g} e={rec['exponent']:.6g} st={rec['style']} "
            f"fl={rec['flags']:#x} modes={rec['mode_b1']}/{rec['mode_b2']}/{rec['mode_b3']} "
            f"so={fmtv(rec['shadow_cast_offset'])}" + (f" x={rec['extra_i']}" if rec['extra_i'] else ''))


def report(name, bsp, inputs, raw, fixture=None):
    version, lumps = read_lumps(bsp)
    print(f'\n=== {name}: {bsp.name} BSP v{version}')
    for lump in (LUMP_WORLDLIGHTS, LUMP_WORLDLIGHTS_HDR):
        ver, blob, _ = lumps[lump]
        size, recs = parse_worldlights(blob, None)
        label = 'LDR' if lump == LUMP_WORLDLIGHTS else 'HDR'
        print(f'-- {label} lump {lump}: v{ver} {len(blob)} bytes, record {size}, {len(recs)} lights')
        if not recs:
            continue
        used = set()
        for label_in, origin, cls, keys in inputs:
            hit = [i for i, r in enumerate(recs) if origin is not None and i not in used and
                   all(abs(a - b) < 0.5 for a, b in zip(r['origin'], origin))]
            if hit:
                used.add(hit[0])
                print(f'  {cls:13} {label_in:30} {row(recs[hit[0]])}')
                if fixture is not None and lump == LUMP_WORLDLIGHTS_HDR:
                    fixture.append(fixture_case(cls, label_in, origin, keys, [recs[hit[0]]]))
                if raw:
                    print('    raw', recs[hit[0]]['raw'].hex())
            elif origin is not None:
                print(f'  {cls:13} {label_in:30} (no worldlight)')
                if fixture is not None and lump == LUMP_WORLDLIGHTS_HDR:
                    fixture.append(fixture_case(cls, label_in, origin, keys, []))
            else:  # light_environment: report every record it produced
                if fixture is not None and lump == LUMP_WORLDLIGHTS_HDR:
                    fixture.append(fixture_case(cls, label_in, (0, 0, 512), keys, recs))
                for i, r in enumerate(recs):
                    used.add(i)
                    print(f'  {cls:13} {label_in:30} {row(r)}')
        for i, r in enumerate(recs):
            if i not in used:
                print(f'  {"(unmatched)":14} {"o=" + fmtv(r["origin"]):34} {row(r)}')
                if raw:
                    print('    raw', r['raw'].hex())


FIXTURE_FIELDS = ('type', 'intensity', 'normal', 'shadow_cast_offset', 'style', 'stopdot', 'stopdot2',
                  'exponent', 'radius', 'constant', 'linear', 'quadratic', 'flags', 'mode_b1', 'mode_b2', 'mode_b3')


def fixture_case(cls, label, origin, keys, recs):
    # Point-light normals are leftover bytes (non-deterministic), so they are not recorded.
    out = []
    for r in recs:
        rec = {k: r[k] for k in FIXTURE_FIELDS}
        rec['type'] = EMIT.get(r['type'], r['type'])
        if rec['type'] == 'point':
            del rec['normal']
        for old, new in (('mode_b1', 'specular_mode'), ('mode_b2', 'direct_mode'), ('mode_b3', 'indirect_mode')):
            rec[new] = rec.pop(old)
        out.append(rec)
    return {'label': label, 'classname': cls, 'origin': list(origin),
            'keys': {k: v for k, v in keys.items() if not k.startswith('__')}, 'worldlights': out}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--group', action='append', choices=[*GROUPS, 'env'])
    ap.add_argument('--keep', action='store_true', help='keep the /tmp work directory')
    ap.add_argument('--raw', action='store_true', help='print raw record hex')
    ap.add_argument('--vrad-arg', action='append', default=[], help='extra vrad argument (e.g. -final)')
    ap.add_argument('--fixture', type=Path, help='write every case and its HDR worldlights as JSON (test fixture)')
    args = ap.parse_args()
    install = os.environ.get('APEX_P2CE_DIR')
    if not install:
        sys.exit('APEX_P2CE_DIR is unset')
    root = Path(install).resolve()
    game = root / 'p2ce'
    work = Path(tempfile.mkdtemp(prefix='apex-lightprobe-', dir='/tmp')).resolve()
    if work.is_relative_to(root):
        sys.exit('work directory must be outside APEX_P2CE_DIR')
    fixture = [] if args.fixture else None
    try:
        for group in args.group or [*GROUPS, 'env']:
            if group == 'env':
                for i, (label, keys) in enumerate(ENV_CASES):
                    keys = dict(keys)
                    sky = not keys.pop('__nosky', None)
                    origin = (0, 0, 512)
                    bsp, _ = compile_map(root, game, work, f'env{i}', make_vmf([('light_environment', origin, keys)], sky=sky),
                                         args.vrad_arg)
                    report(f'env{i} {label}', bsp, [(label, None, 'light_environment', keys)], args.raw, fixture)
                continue
            cases = GROUPS[group]
            pts = list(grid())
            if len(cases) > len(pts):
                sys.exit(f'group {group} has too many cases')
            lights, inputs = [], []
            for (cls, label, keys), o in zip(cases, pts):
                lights.append((cls, o, keys))
                inputs.append((label, o, cls, keys))
            bsp, _ = compile_map(root, game, work, group, make_vmf(lights), args.vrad_arg)
            report(group, bsp, inputs, args.raw, fixture)
        if fixture is not None:
            args.fixture.write_text(json.dumps({'source': 'tools/light_probe.py ' + ' '.join(sys.argv[1:]),
                                                'cases': fixture}, indent=1) + '\n')
    finally:
        if args.keep:
            print(f'\nwork directory kept: {work}')
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
