#!/usr/bin/env python3
"""Statistics over a luxels-A-B.csv written by apex_render_pose --diff.

Prints area-weighted quantiles of A/B (luminance) overall, per hybrid class, and binned by distance to
the nearest facing surface (corner), to the face's own edges, and to the nearest rebaked face; writes
histogram and binned-ratio plots next to the CSV (or into --out).

  tools/lightmap_diff_report.py OUT/luxels-hybrid-vrad.csv [--out DIR] [--label NAME]
"""
import argparse
import pathlib

import numpy as np

CLASSES = {2: "rebaked", 1: "bounce ring", 0: "frozen", -1: "all"}
BINS = [0, 4, 8, 16, 32, 64, 128, 256.001]


def luminance(r, g, b):
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def weighted_quantiles(values, weights, qs):
    if len(values) == 0:
        return [float("nan")] * len(qs)
    order = np.argsort(values)
    v, w = values[order], weights[order]
    cumulative = np.cumsum(w) / w.sum()
    return [float(v[min(np.searchsorted(cumulative, q), len(v) - 1)]) for q in qs]


def summary(log2, area):
    """Ratio quantiles plus the area share within 10% and 25% and the energy-weighted mean ratio."""
    p5, p25, p50, p75, p95 = (2 ** q for q in weighted_quantiles(log2, area, [0.05, 0.25, 0.5, 0.75, 0.95]))
    total = area.sum()
    within = lambda limit: float(area[np.abs(log2) <= np.log2(limit)].sum() / total) if total else float("nan")
    return dict(luxels=len(log2), area=float(total), p5=p5, p25=p25, median=p50, p75=p75, p95=p95,
                within10=within(1.1), within25=within(1.25))


def row(name, s):
    return (f"| {name} | {s['luxels']} | {s['p5']:.3f} | {s['p25']:.3f} | {s['median']:.3f} | {s['p75']:.3f} "
            f"| {s['p95']:.3f} | {100 * s['within10']:.1f}% | {100 * s['within25']:.1f}% |")


HEADER = ("| subset | luxels | p5 | p25 | median | p75 | p95 | within 10% | within 25% |\n"
          "|---|---:|---:|---:|---:|---:|---:|---:|---:|")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--label", default=None, help="A/B label for titles (default from the file name)")
    args = parser.parse_args()
    out = args.out or args.csv.parent
    out.mkdir(parents=True, exist_ok=True)
    label = args.label or args.csv.stem.removeprefix("luxels-").replace("-", "/")
    stem = args.csv.stem

    data = np.genfromtxt(args.csv, delimiter=",", names=True)
    valid = data["valid"] > 0
    d = data[valid]
    a = luminance(d["a_r"], d["a_g"], d["a_b"])
    b = luminance(d["b_r"], d["b_g"], d["b_b"])
    log2 = np.log2(a / b)
    area = d["area"]
    cls = d["class"].astype(int)
    hybrid = (cls >= 0).any()

    lines = [f"## {label}: {valid.sum()} valid of {len(data)} luxels", "",
             f"Energy ratio (sum of A*area / sum of B*area): {np.sum(a * area) / np.sum(b * area):.4f}", "", HEADER,
             row("all", summary(log2, area))]
    if hybrid:
        for c in (2, 1, 0):
            mask = cls == c
            if mask.any():
                lines.append(row(CLASSES[c], summary(log2[mask], area[mask])))

    def binned(column, title):
        lines.extend(["", f"By {title} (units):", "", HEADER])
        result = []
        for lo, hi in zip(BINS[:-1], BINS[1:]):
            mask = (d[column] >= lo) & (d[column] < hi)
            if mask.sum() < 20:
                continue
            s = summary(log2[mask], area[mask])
            result.append((lo, hi, s))
            lines.append(row(f"{lo:g}-{hi:g}" if hi < 256 else f"{lo:g}+", s))
        return result

    corner = binned("corner", "distance to the nearest facing surface")
    edge = binned("edge", "distance to the face's own edge")
    gap = binned("dirty_gap", "distance to the nearest rebaked face") if hybrid else []

    report = "\n".join(lines) + "\n"
    print(report)
    (out / f"{stem}-report.md").write_text(report)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, 3 if hybrid else 2, figsize=(15 if hybrid else 10, 4), constrained_layout=True)
    edges = np.linspace(-1.5, 1.5, 61)
    ax = axes[0]
    ax.hist(np.clip(log2, -1.5, 1.5), bins=edges, weights=area / area.sum(), color="#555", label="all")
    if hybrid:
        for c, colour in ((2, "#e8811a"), (1, "#d4b72c")):
            mask = cls == c
            if mask.any():
                ax.hist(np.clip(log2[mask], -1.5, 1.5), bins=edges, weights=area[mask] / area.sum(),
                        histtype="step", lw=1.5, color=colour, label=CLASSES[c])
    ax.axvline(0, color="k", lw=0.8)
    ax.set_xlabel(f"log2({label.replace('/', ' / ')}) luminance")
    ax.set_ylabel("share of lit area")
    ax.legend()

    def plot_binned(ax, result, xlabel):
        if not result:
            ax.set_visible(False)
            return
        x = np.arange(len(result))
        median = [s["median"] for *_, s in result]
        ax.fill_between(x, [s["p25"] for *_, s in result], [s["p75"] for *_, s in result], alpha=0.25, color="#2a6ebb",
                        label="p25-p75")
        ax.plot(x, [s["p5"] for *_, s in result], ":", color="#2a6ebb", label="p5 / p95")
        ax.plot(x, [s["p95"] for *_, s in result], ":", color="#2a6ebb")
        ax.plot(x, median, "o-", color="#2a6ebb", label="median")
        ax.axhline(1, color="k", lw=0.8)
        ax.set_xticks(x, [f"{lo:g}-{hi:g}" if hi < 256 else f"{lo:g}+" for lo, hi, _ in result])
        ax.set_xlabel(xlabel)
        ax.set_ylabel("A / B")
        ax.legend(fontsize=8)

    plot_binned(axes[1], corner, "distance to nearest facing surface")
    if hybrid:
        plot_binned(axes[2], gap, "distance to nearest rebaked face")
    fig.suptitle(label)
    fig.savefig(out / f"{stem}-report.png", dpi=110)
    print(out / f"{stem}-report.png")


if __name__ == "__main__":
    main()
