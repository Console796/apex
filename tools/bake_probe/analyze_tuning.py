#!/usr/bin/env python3
"""Summarize --diagnostics CSVs using the run's one fitted scale (stdlib only)."""
import argparse
import csv
import math
import re
import statistics
from pathlib import Path


def luminance(row, prefix):
    return sum(w * row[prefix + c] for w, c in zip((.2126, .7152, .0722), 'rgb'))


def quantile(values, q):
    values = sorted(values)
    x = q * (len(values) - 1)
    lo, hi = math.floor(x), math.ceil(x)
    return values[lo] * (hi - x) + values[hi] * (x - lo) if lo != hi else values[lo]


def summarize(path):
    scale = float(re.search(r'global-scale=(\S+)', (path / 'summary.txt').read_text())[1])
    rows = []
    for pose in range(1, 5):
        with (path / f'pose{pose}-samples.csv').open() as stream:
            for row in csv.DictReader(stream):
                row = {k: float(v) for k, v in row.items()}
                if not row['contaminated'] and luminance(row, 'ratio_') > 1e-8:
                    row['pose'] = pose
                    rows.append(row)
    groups = {
        'all': lambda r: True,
        'direct-lit': lambda r: not r['shadow'],
        'shadow': lambda r: r['shadow'],
        'floor': lambda r: r['normal_z'] > .9,
        'ceiling': lambda r: r['normal_z'] < -.9,
        'wall': lambda r: abs(r['normal_z']) < .1,
        'nearest <64': lambda r: r['light_distance'] < 64,
        'nearest 64..128': lambda r: 64 <= r['light_distance'] < 128,
        'nearest >=128': lambda r: r['light_distance'] >= 128,
        'spot outside': lambda r: r['spot_ramp'] == 0,
        'spot ramp': lambda r: 0 < r['spot_ramp'] < 1,
        'spot inner': lambda r: r['spot_ramp'] == 1,
        'edge <8': lambda r: r['edge_distance'] < 8,
        'edge >=8': lambda r: r['edge_distance'] >= 8,
    }
    groups.update({f'pose{p}': lambda r, p=p: r['pose'] == p for p in range(1, 5)})
    print(f'\n{path}: scale={scale:g}')
    print('group | n | p10/median/p90 | median bounce share | miss/sky/unmapped/backface/mapped %')
    for name, select in groups.items():
        selected = [r for r in rows if select(r)]
        if not selected:
            continue
        ratios = [scale * luminance(r, 'baked_') / luminance(r, 'ratio_') for r in selected]
        census = [100 * sum(r['ray_' + k] for r in selected) / (128 * len(selected))
                  for k in ('miss', 'sky', 'unmapped', 'backface', 'mapped')]
        print(f'{name} | {len(selected)} | ' + '/'.join(f'{quantile(ratios, q):.4f}' for q in (.1, .5, .9))
              + f' | {statistics.median(r["bounce_share"] for r in selected):.4f} | '
              + '/'.join(f'{v:.3f}' for v in census))
    shadow = [r for r in rows if r['shadow']]
    print('Shadow samples with positive (>1e-6) point-hit direct under each counterfactual:')
    for key in ('direct_lift_01', 'direct_lift_1', 'direct_lift_4', 'spots_as_points', 'unoccluded_direct'):
        print(f'  {key}: {sum(r[key] > 1e-6 for r in shadow)}/{len(shadow)}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', type=Path, nargs='+')
    for run in parser.parse_args().runs:
        summarize(run)
