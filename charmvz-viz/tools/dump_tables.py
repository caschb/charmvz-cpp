#!/usr/bin/env python3
"""Dump the data behind every charmvz_vis table and plot, for comparison.

  tools/dump_tables.py [--paper-oracle] [--skip A,B] TRACE_DIR OUT_DIR [ITEM...]
  tools/dump_tables.py TRACE_DIR list

Writes one file per item into OUT_DIR: ``ITEM.parquet`` for a table,
``ITEM.pkl`` for a figure (every line, patch, collection path, image, text and
legend entry of every axes, which is what a reader of the plot sees). Each item
runs in its own child process, one after another, so the peak is that of the
largest item rather than the sum. Failures are reported and skipped.

Only uses entry points that exist on main as well, so the same script can be
run with an older checkout's interpreter to produce the reference dump. Compare
two dumps with tools/compare_dumps.py.

--paper-oracle computes ``paper_table`` with one streaming aggregation per
time point instead of cumulative_percent_imbalance_by_time: the filter
``start_time_us < point`` per point is the cross join's semantics by
construction, and fits in memory where the old cross join does not.
"""

from __future__ import annotations

import argparse
import pathlib
import pickle
import subprocess
import sys
import time

import matplotlib

matplotlib.use("Agg")

import matplotlib.patches as mpatches  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import polars as pl  # noqa: E402

import charmvz_vis as cv  # noqa: E402
from charmvz_vis import derived  # noqa: E402

BINS = 200


def stream(lf: pl.LazyFrame) -> pl.DataFrame:
    return lf.collect(engine="streaming")


def paper_oracle(ds: cv.TraceDataset, total_time_points: int = 100, nodes: int = 4):
    spans = ds.execution.select("pe_id", "start_time_us", "wall_duration_us")
    bounds = spans.select(
        pl.col("start_time_us").min().alias("lo"), pl.col("start_time_us").max().alias("hi")
    ).pipe(stream)
    lo, hi = bounds.item(0, "lo"), bounds.item(0, "hi")
    step = (hi - lo) / (total_time_points - 1)
    points = [lo + i * step for i in range(1, total_time_points)]
    rows = []
    for point in points:
        loads = (
            spans.filter(pl.col("start_time_us") < point)
            .group_by((pl.col("pe_id") % nodes).alias("node"))
            .agg(pl.col("wall_duration_us").sum().alias("load_us"))
            .pipe(stream)
        )
        if loads.is_empty():
            rows.append((point, 0, 0.0, 0.0))
            continue
        max_load, mean_load = loads["load_us"].max(), loads["load_us"].mean()
        pct = (max_load / mean_load - 1) * 100 if mean_load > 0 else 0.0
        rows.append((point, max_load, mean_load, pct))
    return pl.DataFrame(
        rows,
        schema={
            "time_point_us": pl.Float64,
            "max_load_us": pl.Int64,
            "mean_load_us": pl.Float64,
            "percent_imbalance": pl.Float64,
        },
        orient="row",
    )


def items(ds: cv.TraceDataset, oracle: bool) -> dict:
    t0, t1 = ds.time_range_us
    bin_us = max(1, (t1 - t0) // BINS)
    window = (t0, t0 + (t1 - t0) // 10)
    tables = {
        "per_pe_utilization": lambda: cv.per_pe_utilization(ds),
        "per_interval_utilization": lambda: cv.per_interval_utilization(ds, bin_us),
        "pe_load_by_bin": lambda: derived.pe_load_by_bin(ds, bin_us).pipe(stream),
        "percent_imbalance_by_bin": lambda: stream(derived.percent_imbalance_by_bin(ds, bin_us)),
        "chare_load_by_bin": lambda: derived.chare_load_by_bin(ds, bin_us).pipe(stream),
        "chare_duration_totals": lambda: derived.chare_duration_totals(ds).pipe(stream),
        "chare_frequency_counts": lambda: derived.chare_frequency_counts(ds).pipe(stream),
        "chare_activity_duration": lambda: derived.chare_activity_matrix(ds).pipe(stream),
        "chare_activity_frequency": lambda: derived.chare_activity_matrix(
            ds, metric="frequency"
        ).pipe(stream),
        "noise_miner": lambda: cv.noise_miner(ds),
        "noise_occurrences": lambda: cv.noise_occurrences(ds),
        "paper_table": (
            (lambda: paper_oracle(ds))
            if oracle
            else lambda: stream(derived.cumulative_percent_imbalance_by_time(ds))
        ),
    }
    figures = {
        "fig_time_profile": lambda: cv.time_profile(ds, bin_width_us=bin_us),
        "fig_time_profile_pct": lambda: cv.time_profile(
            ds, bin_width_us=bin_us, as_percentage=True
        ),
        "fig_pe_window": lambda: cv.time_profile(
            ds, bin_width_us=max(1, bin_us // 10), pes=[0], time_range=window
        ),
        "fig_usage_profile": lambda: cv.usage_profile(ds),
        "fig_ep_profile": lambda: cv.ep_profile(ds),
        "fig_exec_hist": lambda: cv.execution_time_histogram(ds),
        "fig_exec_hist_13": lambda: cv.execution_time_histogram(ds, n_bins=13),
        "fig_exec_hist_log": lambda: cv.execution_time_histogram(ds, log_scale=True),
        "fig_exec_hist_acc": lambda: cv.execution_time_histogram(ds, accumulated=True),
        "fig_msg_hist": lambda: cv.message_size_histogram(ds),
        "fig_msg_hist_linear": lambda: cv.message_size_histogram(ds, log_scale=False),
        "fig_noise_timeline": lambda: cv.noise_timeline(ds),
        "fig_timeline_overview": lambda: cv.timeline_overview(ds, bin_width_us=bin_us),
        "fig_percent_imbalance": lambda: cv.percent_imbalance(ds, bin_us),
        "fig_timeline_with_imbalance": lambda: cv.timeline_with_imbalance(
            ds, bin_width_us=bin_us
        ),
        "fig_chare_duration_comparison": lambda: cv.chare_duration_comparison({"a": ds}),
        "fig_chare_frequency_comparison": lambda: cv.chare_frequency_comparison({"a": ds}),
        "fig_chare_heatmap": lambda: cv.chare_activity_heatmap(ds),
        "fig_chare_heatmap_freq": lambda: cv.chare_activity_heatmap(ds, metric="frequency"),
    }
    for attr in ("most_idle_time", "least_idle_time", "overhead",
                 "active_entry_methods", "avg_grain_size"):
        figures[f"fig_extrema_{attr}"] = lambda a=attr: cv.extrema_analysis(ds, attribute=a)
    for metric in ("sent_msgs", "sent_bytes", "recv_msgs", "recv_bytes"):
        figures[f"fig_comm_{metric}"] = lambda m=metric: cv.comm_per_pe(ds, metric=m)
    if ds.has_table("memory_sample"):
        figures["fig_memory_usage"] = lambda: cv.memory_usage(ds)
    if ds.has_table("user_stat"):
        figures["fig_user_stats_over_time"] = lambda: cv.user_stats_over_time(ds)
        figures["fig_user_stats_per_pe"] = lambda: cv.user_stats_per_pe(ds)
    return {**tables, **figures}


def _patch(p: mpatches.Patch) -> tuple:
    if isinstance(p, mpatches.Wedge):
        return ("wedge", p.theta1, p.theta2, p.r, p.width)
    if isinstance(p, mpatches.Rectangle):
        return ("rect", p.get_x(), p.get_y(), p.get_width(), p.get_height())
    return (type(p).__name__,)


def figure_data(fig) -> list[dict]:
    axes = []
    for ax in fig.axes:
        legend = ax.get_legend()
        axes.append(
            {
                "title": ax.get_title(),
                "xlabel": ax.get_xlabel(),
                "ylabel": ax.get_ylabel(),
                "lines": [
                    (line.get_label(), np.asarray(line.get_xdata(), dtype=float),
                     np.asarray(line.get_ydata(), dtype=float))
                    for line in ax.lines
                ],
                "patches": [_patch(p) for p in ax.patches],
                "collections": [
                    [np.asarray(path.vertices, dtype=float) for path in c.get_paths()]
                    for c in ax.collections
                ],
                "images": [np.asarray(im.get_array()) for im in ax.images],
                "texts": [t.get_text() for t in ax.texts],
                "legend": [t.get_text() for t in legend.get_texts()] if legend else [],
                "xticklabels": [t.get_text() for t in ax.get_xticklabels()],
                "yticklabels": [t.get_text() for t in ax.get_yticklabels()],
            }
        )
    return axes


def run_one(trace: str, out: pathlib.Path, name: str, oracle: bool) -> None:
    ds = cv.TraceDataset(trace)
    result = items(ds, oracle)[name]()
    if isinstance(result, pl.DataFrame):
        result.write_parquet(out / f"{name}.parquet")
    else:
        fig = result
        fig.canvas.draw()  # tick labels exist only after a draw
        with open(out / f"{name}.pkl", "wb") as f:
            pickle.dump(figure_data(fig), f)
        plt.close("all")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--paper-oracle", action="store_true")
    parser.add_argument("--skip", default="")
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("trace_dir")
    parser.add_argument("out_dir", nargs="?")
    parser.add_argument("items", nargs="*")
    args = parser.parse_args()

    if args.out_dir in (None, "list"):
        print(" ".join(items(cv.TraceDataset(args.trace_dir), False)))
        return 0
    out = pathlib.Path(args.out_dir)
    if args.child:
        run_one(args.trace_dir, out, args.items[0], args.paper_oracle)
        return 0

    out.mkdir(parents=True, exist_ok=True)
    names = args.items or list(items(cv.TraceDataset(args.trace_dir), args.paper_oracle))
    skip = set(filter(None, args.skip.split(",")))
    failed = 0
    for name in names:
        if name in skip:
            print(f"{name} skipped", flush=True)
            continue
        cmd = [sys.executable, "-I", __file__, "--child", args.trace_dir, str(out), name]
        if args.paper_oracle:
            cmd.insert(3, "--paper-oracle")
        start = time.monotonic()
        rc = subprocess.call(cmd)
        status = "ok" if rc == 0 else f"FAILED rc={rc}"
        failed |= rc != 0
        print(f"{name} {status} wall_s={time.monotonic() - start:.1f}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
