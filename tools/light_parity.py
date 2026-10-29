#!/usr/bin/env python3
"""Lighting 0.6.2: irradiance parity with native VRAD (plan 0.6, D3 test 3).

Generates small sealed reference scenes, compiles each with native P2CE vbsp, vvis -fast and
vrad -bounce 0 -noextra -aoscale 0 (HDR, the only lightmap P2CE writes) in a fresh /tmp directory,
reconstructs every face fragment's luxels from the BSP, and compares them with the light model
evaluated from the worldlight lump. Nothing is written inside APEX_P2CE_DIR, and every rule used
here was measured (docs/plans/probe-results/lighting_0_6_2_reference.md); no Valve SDK code.

Reference decoding (measured): luxel = ColorRGBExp32 m·2^e/255 per channel, stored by truncation,
so a nonzero channel is decoded at the bucket middle (m + 0.5)·2^e/255.
Sampling (measured, -noextra): a luxel is the mean of four samples at (s ± ½, t ± ½) in lightmap
space, each lifted 1 world unit along the face normal. `--sampling centre` evaluates at the luxel
centre instead, to show what the sampling operator is worth.

Mask: unbumped, lit, non-displacement faces only; a luxel is dropped if its ±1-luxel square is not
inside its face fragment, if any of its samples cannot see a point or spot light (or the sky for the
sun), or if it is within one luxel of such a luxel. Statistics are on Rec. 709 luminance with
error = |model − ref| / max(ref, floor), floor = 1% of the scene's 99th-percentile reference
luminance; luxels where both values are below the floor are counted as dark and left out.

Usage: tools/light_parity.py [--scene NAME ...] [--report-only] [--model measured|0.6.1]
                             [--sampling vrad|centre] [--keep] [--fixture PATH]
Exit status 1 when a scene exceeds the provisional targets (mean 2%, p99 6%) unless --report-only.
"""
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from light_probe import parse_worldlights, read_lumps  # noqa: E402

LUMP_PLANES, LUMP_VERTEXES, LUMP_TEXINFO, LUMP_EDGES, LUMP_SURFEDGES = 1, 3, 6, 12, 13
LUMP_LIGHTING_HDR, LUMP_WORLDLIGHTS_HDR, LUMP_FACES_HDR = 53, 54, 58
SURF_SKY, SURF_NOLIGHT, SURF_BUMPLIGHT = 0x4, 0x400, 0x800
FACE = struct.Struct('<IBBHiiiii4BifiiiiiIII')  # BSP v25 dface_t, 72 bytes (measured)
WALL = 'dev/dev_measuregeneric01b'
SKY = 'tools/toolsskybox'
TARGET_MEAN, TARGET_P99 = 0.02, 0.06
REC709 = (0.2126, 0.7152, 0.0722)


# ---------------------------------------------------------------- vector helpers
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def length(a): return math.sqrt(dot(a, a))
def unit(a): return mul(a, 1.0 / length(a))


# ---------------------------------------------------------------- scene geometry
class Brush:
    """Convex brush from (point, outward normal, material) planes with integer components."""

    def __init__(self, planes):
        self.sides = planes
        self.planes = [(unit(n), dot(unit(n), p), m) for p, n, m in planes]

    def first_hit(self, origin, direction, tmax):
        """Entry distance and material along the ray, or None. A start inside counts as a hit at 0."""
        tmin, entry = 0.0, None
        for n, d, mat in self.planes:
            denom = dot(n, direction)
            dist = d - dot(n, origin)
            if abs(denom) < 1e-12:
                if dist < 0:
                    return None
                continue
            t = dist / denom
            if denom < 0:
                if t > tmin:
                    tmin, entry = t, mat
            elif t < tmax:
                tmax = t
            if tmin > tmax:
                return None
        return tmin, entry


def box(lo, hi, material=WALL, faces=None):
    """Axis-aligned brush; faces maps '+x', '-z', ... to a material override."""
    (x1, y1, z1), (x2, y2, z2) = lo, hi
    faces = faces or {}
    planes = [((x2, 0, 0), (1, 0, 0), '+x'), ((x1, 0, 0), (-1, 0, 0), '-x'),
              ((0, y2, 0), (0, 1, 0), '+y'), ((0, y1, 0), (0, -1, 0), '-y'),
              ((0, 0, z2), (0, 0, 1), '+z'), ((0, 0, z1), (0, 0, -1), '-z')]
    return Brush([(p, n, faces.get(k, material)) for p, n, k in planes])


def room(half, zlo, zhi, open_top=False, t=16):
    """Sealed box room; with open_top the ceiling's underside is sky (a courtyard)."""
    hx, hy = half
    top = {'-z': SKY} if open_top else None
    return [box((-hx - t, -hy - t, zlo - t), (hx + t, hy + t, zlo)),
            box((-hx - t, -hy - t, zhi), (hx + t, hy + t, zhi + t), faces=top),
            box((-hx - t, -hy - t, zlo), (-hx, hy + t, zhi)),
            box((hx, -hy - t, zlo), (hx + t, hy + t, zhi)),
            box((-hx, -hy - t, zlo), (hx, -hy, zhi)),
            box((-hx, hy, zlo), (hx, hy + t, zhi))]


def _axes(n):
    ax = max(range(3), key=lambda i: abs(n[i]))
    if ax == 2:
        return '[1 0 0 0] 0.25', '[0 -1 0 0] 0.25'
    if ax == 0:
        return '[0 1 0 0] 0.25', '[0 0 -1 0] 0.25'
    return '[1 0 0 0] 0.25', '[0 0 -1 0] 0.25'


def make_vmf(brushes, entities, lms):
    ids = iter(range(1, 1000000))
    out = ['versioninfo\n{\n\t"editorversion" "400"\n\t"editorbuild" "10773"\n\t"mapversion" "1"\n'
           '\t"formatversion" "100"\n\t"prefab" "0"\n}\n'
           'world\n{\n\t"id" "1"\n\t"mapversion" "1"\n\t"classname" "worldspawn"\n\t"skyname" "sky_black_nofog"\n']
    for b in brushes:
        out.append(f'\tsolid\n\t{{\n\t\t"id" "{next(ids)}"\n')
        for p, n, mat in b.sides:
            # Three points whose (p2 - p0) x (p1 - p0) is the outward normal (Hammer's winding).
            a = (64, 0, 0) if abs(n[0]) < max(abs(n[1]), abs(n[2])) else (0, 64, 0)
            nn, k = dot(n, n), dot(a, n)
            a = tuple(a[i] * nn - k * n[i] for i in range(3))
            pts = (p, add(p, a), add(p, cross(a, n)))
            u, v = _axes(n)
            plane = ' '.join(f'({x:g} {y:g} {z:g})' for x, y, z in pts)
            out.append(f'\t\tside\n\t\t{{\n\t\t\t"id" "{next(ids)}"\n\t\t\t"plane" "{plane}"\n'
                       f'\t\t\t"material" "{mat}"\n\t\t\t"uaxis" "{u}"\n\t\t\t"vaxis" "{v}"\n'
                       f'\t\t\t"rotation" "0"\n\t\t\t"lightmapscale" "{lms}"\n\t\t\t"smoothing_groups" "0"\n\t\t}}\n')
        out.append('\t}\n')
    out.append('}\n')
    for cls, origin, keys in entities:
        out.append(f'entity\n{{\n\t"id" "{next(ids)}"\n\t"classname" "{cls}"\n'
                   f'\t"origin" "{origin[0]:g} {origin[1]:g} {origin[2]:g}"\n')
        out.extend(f'\t"{k}" "{v}"\n' for k, v in keys.items())
        out.append('}\n')
    return ''.join(out)


# ---------------------------------------------------------------- reference scenes
def _point(**k):
    base = {'_light': '255 255 255 200', '_lightHDR': '-1 -1 -1 1', '_lightscaleHDR': '1',
            '_constant_attn': '0', '_linear_attn': '0', '_quadratic_attn': '1'}
    base.update(k)
    return base


def _spot(**k):
    return _point(**{'_inner_cone': '25', '_cone': '40', '_exponent': '1', 'pitch': '-70', 'angles': '0 30 0', **k})


def _env(**k):
    base = {'_light': '255 240 220 300', '_lightHDR': '-1 -1 -1 1', '_lightscaleHDR': '1',
            '_ambient': '0 0 0 0', '_ambientHDR': '-1 -1 -1 1', '_AmbientScaleHDR': '1',
            'pitch': '-55', 'angles': '0 30 0'}
    base.update(k)
    return base


def scenes():
    big = room((512, 512), 0, 512)
    courtyard = room((384, 384), 0, 512, open_top=True)
    return {
        'point': dict(desc='one coloured point light (q=1) in a 1024x1024x512 room',
                      brushes=big, lms=16,
                      ents=[('light', (96, -64, 256), _point(_light='255 200 150 200'))]),
        'spot': dict(desc='spot 25/40 exponent 1, pitch -70 yaw 30',
                     brushes=big, lms=16, ents=[('light_spot', (-128, -64, 448), _spot())]),
        'spot_exp': dict(desc='spot 20/45 exponent 3, aimed at a wall (pitch -20)',
                         brushes=big, lms=16,
                         ents=[('light_spot', (0, 0, 256), _spot(_inner_cone='20', _cone='45', _exponent='3',
                                                                 pitch='-20', angles='0 0 0'))]),
        'fifty': dict(desc='point light with _fifty_percent_distance 200, _zero_percent_distance 600',
                      brushes=big, lms=16,
                      ents=[('light', (0, 128, 160), _point(_fifty_percent_distance='200',
                                                            _zero_percent_distance='600'))]),
        'attn': dict(desc='two point lights, c/l/q 1/0.02/0.0005 and linear only',
                     brushes=big, lms=16,
                     ents=[('light', (-256, 0, 192), _point(_constant_attn='1', _linear_attn='0.02',
                                                            _quadratic_attn='0.0005')),
                           ('light', (256, 128, 320), _point(_light='128 192 255 200', _linear_attn='1',
                                                             _quadratic_attn='0'))]),
        'occluded': dict(desc='point light beside a pillar (exercises the occlusion mask)',
                         brushes=big + [box((-64, -64, 0), (64, 64, 384))], lms=16,
                         ents=[('light', (-256, 0, 128), _point())]),
        'sun': dict(desc='open-top courtyard, light_environment sun only (pitch -55 yaw 30)',
                    brushes=courtyard, lms=16, ents=[('light_environment', (0, 0, 256), _env())]),
        'sky': dict(desc='open-top courtyard, sky ambient only (255 255 255 200, no sun)',
                    brushes=courtyard, lms=32, vrad=['-extrasky', '64'],
                    ents=[('light_environment', (0, 0, 256), _env(_light='0 0 0 0', _ambient='255 255 255 200'))]),
    }


# ---------------------------------------------------------------- compile
def compile_scene(root, work, name, text, vrad_args):
    game = root / 'p2ce'
    bindir = root / 'bin/linux64'
    vmf = work / f'{name}.vmf'
    vmf.write_text(text)
    env = dict(os.environ, LD_LIBRARY_PATH=str(bindir))
    for tool, args in (('vbsp', ['-game', game, vmf]), ('vvis', ['-fast', '-game', game, vmf.with_suffix('.bsp')]),
                       ('vrad', [*vrad_args, '-game', game, vmf.with_suffix('.bsp')])):
        r = subprocess.run([str(bindir / tool), *map(str, args)], cwd=work, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        (work / f'{name}.{tool}.log').write_text(r.stdout)
        if r.returncode:
            raise RuntimeError(f'{tool} failed on {name} ({r.returncode}):\n{r.stdout[-3000:]}')
        if tool == 'vbsp' and 'LEAKED' in r.stdout:
            raise RuntimeError(f'{name}: map leaked')
    return vmf.with_suffix('.bsp')


# ---------------------------------------------------------------- BSP reconstruction
def _solve3(rows, rhs):
    """Solve a 3x3 system by Cramer's rule."""
    def det(m):
        return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
    d = det(rows)
    out = []
    for c in range(3):
        m = [list(r) for r in rows]
        for r in range(3):
            m[r][c] = rhs[r]
        out.append(det(m) / d)
    return tuple(out)


class Fragment:
    """One compiled face: plane, lightmap frame, polygon and decoded style-0 luxels."""

    def __init__(self, index, rec, planes, texinfo, verts, edges, surfedges, lighting):
        (planenum, _side, _onnode, _pad, firstedge, numedges, ti, disp, _fog, s0, s1, s2, s3, lightofs,
         _area, mins0, mins1, size0, size1, _orig, _nprims, _fprim, _smooth) = rec
        self.index = index
        nx, ny, nz, d = struct.unpack_from('<4f', planes, 20 * planenum)
        self.normal, self.dist = (nx, ny, nz), d  # the referenced plane faces the lit side (measured)
        vecs = struct.unpack_from('<16fi', texinfo, 72 * ti)
        self.lm_s, self.lm_t, self.flags = vecs[8:12], vecs[12:16], vecs[16]
        self.styles = (s0, s1, s2, s3)
        self.disp = disp
        self.mins, self.w, self.h = (mins0, mins1), size0 + 1, size1 + 1
        poly = []
        for k in range(numedges):
            e = struct.unpack_from('<i', surfedges, 4 * (firstedge + k))[0]
            v0, v1 = struct.unpack_from('<2I', edges, 8 * abs(e))
            v = v0 if e >= 0 else v1
            poly.append(struct.unpack_from('<3f', verts, 12 * v))
        self.poly_st = [self.st(p) for p in poly]
        self.luxels = None
        if lightofs >= 0 and s0 == 0:
            self.luxels = []
            for i in range(self.w * self.h):
                r, g, b, e = struct.unpack_from('<BBBb', lighting, lightofs + 4 * i)
                scale = 2.0 ** e / 255
                self.luxels.append((0.0, 0.0, 0.0) if r == g == b == 0 else
                                   ((r + 0.5) * scale, (g + 0.5) * scale, (b + 0.5) * scale))

    def st(self, p):
        return (dot(self.lm_s[:3], p) + self.lm_s[3] - self.mins[0], dot(self.lm_t[:3], p) + self.lm_t[3] - self.mins[1])

    def world(self, s, t):
        """Lightmap coordinates (relative to mins) to the world point on the face plane."""
        return _solve3([self.lm_s[:3], self.lm_t[:3], self.normal],
                       [s + self.mins[0] - self.lm_s[3], t + self.mins[1] - self.lm_t[3], self.dist])

    def inside(self, s, t):
        """Point inside the (convex) face polygon in lightmap space, either winding."""
        sign = 0
        n = len(self.poly_st)
        for i in range(n):
            (s0, t0), (s1, t1) = self.poly_st[i], self.poly_st[(i + 1) % n]
            c = (s1 - s0) * (t - t0) - (t1 - t0) * (s - s0)
            if abs(c) < 1e-6:
                continue
            if sign == 0:
                sign = 1 if c > 0 else -1
            elif (c > 0) != (sign > 0):
                return False
        return True


def read_bsp(path):
    version, lumps = read_lumps(path)
    if version != 25:
        raise RuntimeError(f'{path}: BSP v{version}, the layout here is measured for v25')
    ver, faces, _ = lumps[LUMP_FACES_HDR]
    if not faces or len(faces) % FACE.size:
        raise RuntimeError(f'{path}: HDR face lump missing or not {FACE.size}-byte records (v{ver}); LDR is not used')
    lighting = lumps[LUMP_LIGHTING_HDR][1]
    if not lighting:
        raise RuntimeError(f'{path}: no HDR lighting lump; LDR fallback is rejected')
    args = [lumps[i][1] for i in (LUMP_PLANES, LUMP_TEXINFO, LUMP_VERTEXES, LUMP_EDGES, LUMP_SURFEDGES)]
    frags = [Fragment(i, FACE.unpack_from(faces, i * FACE.size), *args, lighting) for i in range(len(faces) // FACE.size)]
    _, lights = parse_worldlights(lumps[LUMP_WORLDLIGHTS_HDR][1])
    return frags, lights


# ---------------------------------------------------------------- light model (mirrors lighting::irradiance)
def angular(c, model):
    """Surface response to light arriving at cosine c. Measured: (c + c²)/2; 0.6.1 assumed Lambert."""
    if c <= 0:
        return 0.0
    return c if model == '0.6.1' else 0.5 * c * (1 + c)


def direct(light, p, n, model):
    """Irradiance scale (multiplies the intensity) of a point, spot or sun record, before visibility."""
    kind = light['type']
    if kind == 3:  # sun: the record's normal is the direction the light travels
        return angular(-dot(n, light['normal']), model)
    if kind not in (1, 2):
        return 0.0
    delta = sub(light['origin'], p)
    d = length(delta)
    if d <= 0 or (light['radius'] > 0 and d > light['radius']):
        return 0.0
    to_light = mul(delta, 1 / d)
    atten = light['constant'] + light['linear'] * d + light['quadratic'] * d * d
    scale = angular(dot(n, to_light), model) / max(atten, 1e-12)
    if kind == 2:
        along = -dot(to_light, light['normal'])
        if along <= light['stopdot2']:
            return 0.0
        if model != '0.6.1':
            scale *= along  # measured: the spot is a cosine emitter inside its cone
        if along < light['stopdot']:
            edge = (along - light['stopdot2']) / (light['stopdot'] - light['stopdot2'])
            if light['exponent'] not in (0, 1):
                edge = edge ** light['exponent']
            scale *= edge
    return scale


def hemisphere(model, count=2048):
    """Equal-weight directions (cos θ, sin θ cos φ, sin θ sin φ) distributed by the angular response:
    a Fibonacci spiral whose cos θ follows the response's CDF, so the sky fraction is a plain count."""
    golden = (math.sqrt(5) - 1) / 2
    out = []
    for i in range(count):
        xi = (i + 0.5) / count
        if model == '0.6.1':
            c = math.sqrt(xi)  # pdf ∝ c on dω = dc dφ
        else:
            lo, hi = 0.0, 1.0  # pdf ∝ (c + c²)/2, CDF = (3c² + 2c³)/5
            for _ in range(40):
                mid = (lo + hi) / 2
                lo, hi = (mid, hi) if (3 * mid * mid + 2 * mid ** 3) / 5 < xi else (lo, mid)
            c = (lo + hi) / 2
        sn = math.sqrt(max(0.0, 1 - c * c))
        ph = 2 * math.pi * ((i * golden) % 1.0)
        out.append((c, sn * math.cos(ph), sn * math.sin(ph)))
    return out


class Scene:
    def __init__(self, brushes):
        self.brushes = brushes

    def first_hit(self, origin, direction, tmax=1e9):
        best = None
        for b in self.brushes:
            h = b.first_hit(origin, direction, tmax)
            if h is not None and (best is None or h[0] < best[0]):
                best = h
                tmax = h[0]
        return best

    def sees_point(self, p, q):
        return self.first_hit(p, sub(q, p), 1.0) is None

    def sees_sky(self, p, direction):
        h = self.first_hit(p, direction)
        return h is None or h[1] == SKY

    def sky_fraction(self, p, n, quad):
        """Ambient: the response-weighted fraction of the hemisphere whose rays reach a sky face (measured)."""
        a = (1, 0, 0) if abs(n[0]) < 0.9 else (0, 1, 0)
        t1 = unit(cross(n, a))
        t2 = cross(n, t1)
        hits = 0
        for c, x, y in quad:
            d = (c * n[0] + x * t1[0] + y * t2[0], c * n[1] + x * t1[1] + y * t2[1], c * n[2] + x * t1[2] + y * t2[2])
            hits += self.sees_sky(p, d)
        return hits / len(quad)


# ---------------------------------------------------------------- comparison
LIFT = 1.0  # measured: samples sit one world unit off the face


def sample_points(frag, u, t, sampling):
    offsets = ((0.0, 0.0),) if sampling == 'centre' else ((-0.5, -0.5), (0.5, -0.5), (-0.5, 0.5), (0.5, 0.5))
    lift = 0.0 if sampling == 'centre' else LIFT
    return [add(frag.world(u + a, t + b), mul(frag.normal, lift)) for a, b in offsets]


def luminance(c):
    return dot(REC709, c)


def evaluate(scene_def, frags, lights, model, sampling, quad):
    geo = Scene(scene_def['brushes'])
    rows, total, faces_used = [], 0, 0
    for f in frags:
        if f.luxels is None or f.disp != -1 or f.flags & (SURF_SKY | SURF_NOLIGHT):
            continue
        if f.flags & SURF_BUMPLIGHT:
            total += f.w * f.h
            continue
        faces_used += 1
        keep, shadow = {}, set()
        for t in range(f.h):
            for u in range(f.w):
                total += 1
                if not all(f.inside(u + a, t + b) for a in (-1, 1) for b in (-1, 1)):
                    continue
                pts = sample_points(f, u, t, sampling)
                vis_pts = sample_points(f, u, t, 'vrad')
                rgb = [0.0, 0.0, 0.0]
                blocked = False
                for light in lights:
                    kind = light['type']
                    if not any(light['intensity']):
                        continue
                    if kind == 5:
                        centre = add(f.world(u, t), mul(f.normal, LIFT))
                        k = geo.sky_fraction(centre, f.normal, quad)
                    else:
                        for q in vis_pts:
                            if kind == 3:
                                if direct(light, q, f.normal, model) > 0 and not geo.sees_sky(q, mul(light['normal'], -1)):
                                    blocked = True
                            elif kind in (1, 2) and direct(light, q, f.normal, model) > 0 and \
                                    not geo.sees_point(q, light['origin']):
                                blocked = True
                        k = sum(direct(light, q, f.normal, model) for q in pts) / len(pts)
                    for i in range(3):
                        rgb[i] += light['intensity'][i] * k
                if blocked:
                    shadow.add((u, t))
                keep[(u, t)] = (tuple(rgb), f.luxels[t * f.w + u], f.world(u, t), vis_pts)
        for (u, t), (model_rgb, ref, p, vis) in keep.items():
            if any((u + a, t + b) in shadow for a in (-1, 0, 1) for b in (-1, 0, 1)):
                continue
            rows.append(dict(face=f.index, u=u, t=t, p=p, pts=vis, n=f.normal, model=model_rgb, ref=ref))
    return rows, total, faces_used


def percentile(values, q):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]


def stats(rows, pick):
    ref = [pick(r['ref']) for r in rows]
    mod = [pick(r['model']) for r in rows]
    floor = 0.01 * percentile(ref, 0.99)
    errs, dark = [], 0
    for a, b in zip(mod, ref):
        if max(a, b) < floor:
            dark += 1
            continue
        errs.append(abs(a - b) / max(b, floor))
    if not errs:
        return dict(n=0, dark=dark, mean=0.0, p99=0.0, max=0.0, bias=0.0, floor=floor)
    lit = [(a, b) for a, b in zip(mod, ref) if max(a, b) >= floor]
    bias = sum((a - b) / max(b, floor) for a, b in lit) / len(lit)
    return dict(n=len(errs), dark=dark, mean=sum(errs) / len(errs), p99=percentile(errs, 0.99), max=max(errs),
                bias=bias, floor=floor)


def fmt(s):
    return (f"mean {100 * s['mean']:6.3f}%  p99 {100 * s['p99']:6.3f}%  max {100 * s['max']:7.3f}%  "
            f"bias {100 * s['bias']:+6.3f}%  n {s['n']:5d}  dark {s['dark']:5d}")


def fixture_rows(rows, lights, scene_def, limit=32):
    """A deterministic subsample for the C++ parity check: lights, sample points, normal, reference."""
    lights = [l for l in lights if any(l['intensity'])]
    if any(l['type'] == 5 for l in lights):
        return None  # ambient needs the scene's sky visibility, which the fixture does not carry
    lit = [r for r in rows if luminance(r['ref']) > 0]
    step = max(1, len(lit) // limit)
    keys = ('type', 'origin', 'intensity', 'normal', 'stopdot', 'stopdot2', 'exponent', 'radius', 'constant',
            'linear', 'quadratic')
    return dict(description=scene_def['desc'],
                lights=[{k: l[k] for k in keys} for l in lights],
                samples=[dict(points=[list(q) for q in r['pts']], normal=list(r['n']), reference=list(r['ref']),
                              face=r['face'], luxel=[r['u'], r['t']]) for r in lit[::step][:limit]])


def main():
    all_scenes = scenes()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--scene', action='append', choices=list(all_scenes))
    ap.add_argument('--report-only', action='store_true', help='always exit 0')
    ap.add_argument('--model', choices=['measured', '0.6.1'], default='measured',
                    help='light model: the measured rules (default) or the 0.6.1 provisional shapes')
    ap.add_argument('--sampling', choices=['vrad', 'centre'], default='vrad')
    ap.add_argument('--supersample', action='store_true', help='let VRAD supersample (drop -noextra)')
    ap.add_argument('--ao', action='store_true', help='keep VRAD baked AO (drop -aoscale 0)')
    ap.add_argument('--keep', action='store_true', help='keep the /tmp work directory')
    ap.add_argument('--fixture', type=Path, help='write a subsample of reference luxels as JSON')
    args = ap.parse_args()
    install = os.environ.get('APEX_P2CE_DIR')
    if not install:
        sys.exit('APEX_P2CE_DIR is unset')
    root = Path(install).resolve()
    work = Path(tempfile.mkdtemp(prefix='apex-lightparity-', dir='/tmp')).resolve()
    if work.is_relative_to(root):
        sys.exit('work directory must be outside APEX_P2CE_DIR')
    vrad = ['-bounce', '0'] + ([] if args.supersample else ['-noextra']) + ([] if args.ao else ['-aoscale', '0'])
    print(f'vrad {" ".join(vrad)}; model {args.model}; sampling {args.sampling}')
    quad = hemisphere(args.model)
    failed, fixture = [], {}
    try:
        for name in args.scene or list(all_scenes):
            sd = all_scenes[name]
            bsp = compile_scene(root, work, name, make_vmf(sd['brushes'], sd['ents'], sd['lms']), vrad + sd.get('vrad', []))
            frags, lights = read_bsp(bsp)
            rows, total, faces = evaluate(sd, frags, lights, args.model, args.sampling, quad)
            lum = stats(rows, luminance)
            chans = [stats(rows, lambda c, i=i: c[i]) for i in range(3)]
            excl = 1 - len(rows) / total if total else 0
            print(f'\n{name}: {sd["desc"]}')
            print(f'  {len(lights)} worldlights, {faces} faces, {total} luxels, {len(rows)} compared, '
                  f'excluded {100 * excl:.1f}%')
            print(f'  Y  {fmt(lum)}')
            for label, s in zip('RGB', chans):
                print(f'  {label}  {fmt(s)}')
            ok = lum['mean'] <= TARGET_MEAN and lum['p99'] <= TARGET_P99
            print(f'  {"pass" if ok else "FAIL"} (targets mean {100 * TARGET_MEAN:g}%, p99 {100 * TARGET_P99:g}%)')
            if not ok:
                failed.append(name)
            if args.fixture:
                fx = fixture_rows(rows, lights, sd)
                if fx:
                    fixture[name] = fx
        if args.fixture:
            args.fixture.write_text(json.dumps({
                'source': 'tools/light_parity.py ' + ' '.join(sys.argv[1:]),
                'vrad': vrad,
                'decode': 'channel = (m + 0.5) * 2^e / 255 for a nonzero luxel (truncated ColorRGBExp32)',
                'sampling': 'reference = mean of the light model at the four points (lightmap s +- 1/2, t +- 1/2, '
                            'lifted 1 unit along the normal)',
                'scenes': fixture}, indent=1) + '\n')
    finally:
        if args.keep:
            print(f'\nwork directory kept: {work}')
        else:
            shutil.rmtree(work, ignore_errors=True)
    if failed:
        print(f'\nabove target: {", ".join(failed)}')
        if not args.report_only:
            sys.exit(1)


if __name__ == '__main__':
    main()
