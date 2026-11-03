#!/usr/bin/env python3
"""Compare rendered images using supplied, independently generated validity masks.

Mask images must identify visible, nonemissive, VRAD-lightmapped brush pixels;
nonzero luminance in a VRAD render is NOT a validity mask. Metrics are RGB
component errors; region classification uses the reference's median linear
Rec.709 luminance. Coarse errors use mask-normalized Gaussian blur, evaluated
only at valid pixels. Pillow and NumPy are needed for this analysis tool.
"""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

WEIGHTS = np.array([0.2126, 0.7152, 0.0722])


def image(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255.0


def linear(rgb):
    return np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)


def blur(array, sigma):
    radius = int(np.ceil(3 * sigma))
    grid = np.arange(-radius, radius + 1)
    kernel = np.exp(-0.5 * (grid / sigma) ** 2)
    kernel /= kernel.sum()
    # Pad explicitly: np.convolve(mode="same") otherwise grows small images.
    def convolve(row):
        return np.convolve(np.pad(row, radius, mode="edge"), kernel, mode="valid")
    return np.apply_along_axis(convolve, 0, np.apply_along_axis(convolve, 1, array))


def evaluate(reference, candidate, mask, sigma):
    a, b = image(reference), image(candidate)
    valid = np.asarray(Image.open(mask).convert("L")) > 0
    if a.shape != b.shape or valid.shape != a.shape[:2]:
        raise ValueError("image and mask dimensions differ")
    if not valid.any():
        raise ValueError("mask has no valid pixels")
    x, y = linear(a), linear(b)
    lum = x @ WEIGHTS
    split = float(np.median(lum[valid]))

    def stats(selection, v, w):
        if not selection.any():
            return None
        diff = (w - v)[selection]
        ref_y = float((v @ WEIGHTS)[selection].mean())
        out_y = float((w @ WEIGHTS)[selection].mean())
        return {"pixels": int(selection.sum()), "reference_mean_y": ref_y, "candidate_mean_y": out_y,
                "luminance_ratio": out_y / ref_y if ref_y else None,
                "mae": float(np.abs(diff).mean()), "rmse": float(np.sqrt((diff * diff).mean()))}

    denominator = blur(valid.astype(float), sigma)
    # A normalized convolution avoids treating excluded pixels as black.
    def coarse(v):
        return np.stack([blur(v[..., i] * valid, sigma) / np.maximum(denominator, 1e-12)
                         for i in range(3)], axis=-1)

    return {"shadow_threshold_linear_y": split,
            "linear": stats(valid, x, y),
            "srgb_8bit": stats(valid, a * 255, b * 255),
            "shadow_linear": stats(valid & (lum <= split), x, y),
            "lit_linear": stats(valid & (lum > split), x, y),
            "shadow_srgb_8bit": stats(valid & (lum <= split), a * 255, b * 255),
            "lit_srgb_8bit": stats(valid & (lum > split), a * 255, b * 255),
            "coarse_linear": stats(valid, coarse(x), coarse(y))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pose", action="append", nargs=4, metavar=("NAME", "REFERENCE", "CANDIDATE", "MASK"), required=True)
    parser.add_argument("--blur", type=float, default=8.0, help="Gaussian sigma in pixels (default 8)")
    args = parser.parse_args()
    if args.blur <= 0:
        parser.error("--blur must be positive")
    result = {}
    for name, reference, candidate, mask in args.pose:
        if name in result:
            parser.error(f"duplicate pose {name}")
        result[name] = evaluate(Path(reference), Path(candidate), Path(mask), args.blur)
    aggregate = {}
    for category in ("linear", "srgb_8bit", "shadow_linear", "lit_linear",
                     "shadow_srgb_8bit", "lit_srgb_8bit", "coarse_linear"):
        rows = [pose[category] for pose in result.values() if pose[category] is not None]
        total = sum(row["pixels"] for row in rows)
        if not total:
            aggregate[category] = None
            continue
        mean = lambda key: sum(row[key] * row["pixels"] for row in rows) / total
        ref, out = mean("reference_mean_y"), mean("candidate_mean_y")
        aggregate[category] = {"pixels": total, "reference_mean_y": ref, "candidate_mean_y": out,
                               "luminance_ratio": out / ref if ref else None, "mae": mean("mae"),
                               "rmse": np.sqrt(sum(row["rmse"] ** 2 * row["pixels"] for row in rows) / total)}
    print(json.dumps({"poses": result, "aggregate": aggregate}, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
