#!/usr/bin/env python3
"""Compare two tools/dump_tables.py output directories item by item.

  tools/compare_dumps.py A B

Tables are compared after sorting both sides on every column, so a different
row order is not a difference; integer and string columns must be equal,
float columns equal within a relative 1e-9. Figure dumps are compared element
by element with the same float tolerance plus an absolute 1e-12. Floats that
are whole numbers on both sides (below 2**42) must be equal. Items present on
only one side are listed and count as a difference.

Exit status 0 when both sides have the same items and every one matches, 1
otherwise.
"""

from __future__ import annotations

import math
import pathlib
import pickle
import sys

import numpy as np
import polars as pl
from polars.testing import assert_frame_equal

RTOL = 1e-9
# Figure geometry only: matplotlib's hist places a bar at center - width/2,
# which is a few ULPs off a bar placed at its left edge, and relative
# tolerance alone cannot absorb that next to 0.
FIG_ATOL = 1e-12
# Below this a float's ULP is at most 2**-10, so rounding alone cannot turn one
# whole number into another: two different whole numbers here are different
# counts or integer sums (bar heights, image cells), which the relative
# tolerance would otherwise let through at large magnitudes.
EXACT_BELOW = 2.0**42


def whole_number_mismatches(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    with np.errstate(invalid="ignore"):
        whole = (
            (a == np.round(a)) & (b == np.round(b))
            & (np.abs(a) < EXACT_BELOW) & (np.abs(b) < EXACT_BELOW)
        )
    return np.flatnonzero(whole & (a != b))


def compare_tables(a: pl.DataFrame, b: pl.DataFrame) -> str | None:
    if a.schema != b.schema:
        return f"schemas differ: {a.schema} vs {b.schema}"
    if a.height:
        a = a.sort(a.columns, nulls_last=True)
        b = b.sort(b.columns, nulls_last=True)
    try:
        assert_frame_equal(a, b, check_exact=False, rel_tol=RTOL, abs_tol=0)
    except AssertionError as e:
        return str(e).splitlines()[0] + " " + " ".join(str(e).split())[:300]
    for name, dtype in a.schema.items():
        if dtype.is_float():
            bad = whole_number_mismatches(a[name].to_numpy(), b[name].to_numpy())
            if bad.size:
                i = bad[0]
                return (f"{name}: {bad.size} whole numbers differ, "
                        f"first {a[name][i]!r} vs {b[name][i]!r}")
    return None


def compare_values(a, b, path: str) -> str | None:
    if isinstance(a, np.ndarray) or isinstance(b, np.ndarray):
        a, b = np.asarray(a), np.asarray(b)
        if a.shape != b.shape:
            return f"{path}: shape {a.shape} vs {b.shape}"
        if a.dtype.kind in "fc" or b.dtype.kind in "fc":
            if (bad := whole_number_mismatches(a, b)).size:
                i = bad[0]
                return (f"{path}: {bad.size} whole numbers differ, first at flat index {i}: "
                        f"{a.flat[i]!r} vs {b.flat[i]!r}")
            if not np.allclose(a, b, rtol=RTOL, atol=FIG_ATOL, equal_nan=True):
                bad = np.flatnonzero(~np.isclose(a, b, rtol=RTOL, atol=FIG_ATOL, equal_nan=True))
                i = bad[0]
                return (f"{path}: {bad.size} values differ, first at flat index {i}: "
                        f"{a.flat[i]!r} vs {b.flat[i]!r}")
            return None
        return None if np.array_equal(a, b) else f"{path}: arrays differ"
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        if len(a) != len(b):
            return f"{path}: length {len(a)} vs {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            if (err := compare_values(x, y, f"{path}[{i}]")) is not None:
                return err
        return None
    if isinstance(a, dict) and isinstance(b, dict):
        if a.keys() != b.keys():
            return f"{path}: keys {sorted(a)} vs {sorted(b)}"
        for k in a:
            if (err := compare_values(a[k], b[k], f"{path}.{k}")) is not None:
                return err
        return None
    if isinstance(a, float) or isinstance(b, float):
        if a is None or b is None:
            return None if a is b else f"{path}: {a!r} vs {b!r}"
        if whole_number_mismatches(a, b).size:
            return f"{path}: {a!r} vs {b!r}"
        if math.isclose(a, b, rel_tol=RTOL, abs_tol=FIG_ATOL) or (math.isnan(a) and math.isnan(b)):
            return None
        return f"{path}: {a!r} vs {b!r}"
    return None if a == b else f"{path}: {a!r} vs {b!r}"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    a_dir, b_dir = map(pathlib.Path, sys.argv[1:])
    a_items = {p.name: p for p in a_dir.iterdir() if p.suffix in (".parquet", ".pkl")}
    b_items = {p.name: p for p in b_dir.iterdir() if p.suffix in (".parquet", ".pkl")}
    failed = False
    for name in sorted(a_items.keys() | b_items.keys()):
        if name not in a_items or name not in b_items:
            # Usually an item that failed to dump on one side.
            print(f"{name} only in {'A' if name in a_items else 'B'}")
            failed = True
            continue
        if name.endswith(".parquet"):
            err = compare_tables(pl.read_parquet(a_items[name]), pl.read_parquet(b_items[name]))
        else:
            with open(a_items[name], "rb") as fa, open(b_items[name], "rb") as fb:
                err = compare_values(pickle.load(fa), pickle.load(fb), "fig")
        failed |= err is not None
        print(f"{name} {'equal' if err is None else 'DIFFERENT: ' + err}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
