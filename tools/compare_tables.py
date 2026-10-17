#!/usr/bin/env python3
"""Compare two charmvz output directories table by table.

--exact      every table must match row for row, ids and row order included.
             Use for two runs of the same build (e.g. budgeted vs unlimited).
--canonical  surrogate ids are replaced by what they stand for and row order
             is ignored. Use across builds whose id assignment or emission
             order legitimately differs: instance_id becomes the chare's
             (collection_id, index_0..5) key, message_id and migration_id are
             dropped, and both sides are sorted on every column.

Exit status 0 when every table matches, 1 otherwise.

Run with an isolated interpreter, e.g.
  uv run --with pyarrow python -I tools/compare_tables.py --exact A B
"""

import argparse
import pathlib
import sys

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

DROPPED_IDS = {"message": ["message_id"], "migration_episode": ["migration_id"]}
INDEX_COLUMNS = ["collection_id"] + [f"index_{i}" for i in range(6)]


def instance_keys(directory: pathlib.Path) -> tuple[pa.Array, pa.Array]:
    t = pq.read_table(directory / "chare_instance.parquet")
    parts = [pc.cast(t[c], pa.string()) for c in INDEX_COLUMNS]
    key = pc.binary_join_element_wise(*parts, ":")
    return t["instance_id"].combine_chunks(), key.combine_chunks()


def canonical(directory: pathlib.Path, name: str, t: pa.Table) -> pa.Table:
    for column in DROPPED_IDS.get(name, []):
        t = t.drop_columns([column])
    if "instance_id" in t.column_names:
        if name == "chare_instance":
            t = t.drop_columns(["instance_id"])
        else:
            ids, keys = instance_keys(directory)
            pos = pc.index_in(t["instance_id"], value_set=ids)
            t = t.set_column(
                t.column_names.index("instance_id"),
                "instance_id",
                pc.take(keys, pos),
            )
    if t.num_rows == 0:
        return t
    return t.sort_by([(c, "ascending") for c in t.column_names])


def first_difference(a: pa.Table, b: pa.Table) -> str:
    if a.schema != b.schema:
        return f"schemas differ:\n{a.schema}\n---\n{b.schema}"
    if a.num_rows != b.num_rows:
        return f"row counts differ: {a.num_rows} vs {b.num_rows}"
    for column in a.column_names:
        x = a[column].combine_chunks()
        y = b[column].combine_chunks()
        if x.equals(y):
            continue
        for i in range(len(x)):
            if x[i] != y[i]:
                return f"column {column} row {i}: {x[i]} vs {y[i]}"
    return "tables differ"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--exact", action="store_true")
    mode.add_argument("--canonical", action="store_true")
    parser.add_argument("left", type=pathlib.Path)
    parser.add_argument("right", type=pathlib.Path)
    args = parser.parse_args()

    left = {p.stem for p in args.left.glob("*.parquet")}
    right = {p.stem for p in args.right.glob("*.parquet")}
    ok = True
    if left != right:
        print(f"table sets differ: only left {sorted(left - right)}, "
              f"only right {sorted(right - left)}")
        ok = False

    for name in sorted(left & right):
        a = pq.read_table(args.left / f"{name}.parquet")
        b = pq.read_table(args.right / f"{name}.parquet")
        if args.canonical:
            a = canonical(args.left, name, a)
            b = canonical(args.right, name, b)
        if a.equals(b):
            print(f"identical  {name} ({a.num_rows} rows)")
        else:
            print(f"DIFFERENT  {name}: {first_difference(a, b)}")
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
