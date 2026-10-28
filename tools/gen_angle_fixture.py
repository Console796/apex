#!/usr/bin/env python3
"""Generate the Source angle oracle with srctools 2.7.0."""

from itertools import chain, product
from pathlib import Path

from srctools import Angle, Matrix


GRID = (-180, -135, -90, -45, -30, 0, 15, 30, 45, 60, 90, 135, 180, 270, 359.5)
OUTPUT = Path(__file__).resolve().parents[1] / "tests/fixtures/entity_angles_srctools.txt"


def main():
    yaw90 = Matrix.from_angle(Angle(0, 90, 0)).forward()
    assert abs(yaw90.x) < 1e-15 and abs(yaw90.y - 1) < 1e-15 and abs(yaw90.z) < 1e-15
    cases = chain(product(GRID, repeat=3), ((450, -405, 720), (-720, 450, -450)))
    with OUTPUT.open("w") as fixture:
        fixture.write("# Apex-authored srctools Matrix.from_angle grid: pitch yaw roll, then forward, left, up.\n")
        for pitch, yaw, roll in cases:
            matrix = Matrix.from_angle(Angle(pitch, yaw, roll))
            values = (pitch, yaw, roll, *matrix.forward(), *matrix.left(), *matrix.up())
            fixture.write(" ".join(format(value, ".17g") for value in values) + "\n")


if __name__ == "__main__":
    main()
