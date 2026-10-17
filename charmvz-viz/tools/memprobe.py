#!/usr/bin/env python3
"""Run one charmvz_vis entry point on a trace and report its memory peak.

  tools/memprobe.py TRACE_DIR FUNCTION [--bins N]
  tools/memprobe.py TRACE_DIR list

Prints one line:
  FUNCTION ok wall_s= anon_peak_mib= file_peak_mib= cg_peak_mib= threads=

or ``FUNCTION skipped (...)`` when the trace lacks the optional table the
function reads. Figures are closed, never saved.

The peaks come from the process's own cgroup (memory.stat ``anon`` and
``file``, sampled every 20 ms, and memory.peak), so run it alone in a scope:
tools/constrained_viz.sh does. ``anon`` is the heap; ``ru_maxrss`` is not
used because it over-reports, and the cgroup is what an OOM kill acts on.

--bins N sets the time-bin width of the binned functions to 1/N of the run
(default 200).
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import threading
import time

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402
import polars as pl  # noqa: E402

import charmvz_vis as cv  # noqa: E402

SAMPLE_INTERVAL_S = 0.02
MIB = 1024 * 1024


def cgroup_dir() -> pathlib.Path:
    for line in pathlib.Path("/proc/self/cgroup").read_text().splitlines():
        if line.startswith("0::"):
            return pathlib.Path("/sys/fs/cgroup" + line[3:])
    raise RuntimeError("not on a cgroup v2 hierarchy")


class Sampler(threading.Thread):
    """Track the peak of the cgroup's anon and file pages and memory.current."""

    def __init__(self, cg: pathlib.Path) -> None:
        super().__init__(daemon=True)
        self.cg = cg
        self.anon = self.file = self.current = 0
        self.done = threading.Event()

    def sample(self) -> None:
        for line in (self.cg / "memory.stat").read_text().splitlines():
            key, value = line.split()
            if key == "anon":
                self.anon = max(self.anon, int(value))
            elif key == "file":
                self.file = max(self.file, int(value))
        self.current = max(self.current, int((self.cg / "memory.current").read_text()))

    def run(self) -> None:
        while not self.done.wait(SAMPLE_INTERVAL_S):
            self.sample()

    def stop(self) -> int:
        self.done.set()
        self.join()
        self.sample()
        # memory.peak is exact where it exists; the sampled maximum can miss a
        # spike shorter than the interval.
        peak = self.cg / "memory.peak"
        return int(peak.read_text()) if peak.exists() else self.current


def calls(ds: cv.TraceDataset, bins: int) -> dict:
    t0, t1 = ds.time_range_us
    bin_us = max(1, (t1 - t0) // bins)
    window = (t0, t0 + (t1 - t0) // 10)
    return {
        "per_pe_utilization": lambda: cv.per_pe_utilization(ds),
        "per_interval_utilization": lambda: cv.per_interval_utilization(ds, bin_us),
        "load_imbalance_score": lambda: cv.load_imbalance_score(ds),
        "noise_miner": lambda: cv.noise_miner(ds),
        "noise_occurrences": lambda: cv.noise_occurrences(ds),
        "time_profile": lambda: cv.time_profile(ds, bin_width_us=bin_us),
        "usage_profile": lambda: cv.usage_profile(ds),
        "ep_profile": lambda: cv.ep_profile(ds),
        "execution_time_histogram": lambda: cv.execution_time_histogram(ds),
        "message_size_histogram": lambda: cv.message_size_histogram(ds),
        "comm_per_pe": lambda: cv.comm_per_pe(ds),
        "extrema_analysis": lambda: cv.extrema_analysis(ds),
        "noise_timeline": lambda: cv.noise_timeline(ds),
        "memory_usage": (lambda: cv.memory_usage(ds), "memory_sample"),
        "user_stats_over_time": (lambda: cv.user_stats_over_time(ds), "user_stat"),
        "user_stats_per_pe": (lambda: cv.user_stats_per_pe(ds), "user_stat"),
        "chare_duration_comparison": lambda: cv.chare_duration_comparison({"a": ds}),
        "chare_frequency_comparison": lambda: cv.chare_frequency_comparison({"a": ds}),
        "chare_activity_heatmap": lambda: cv.chare_activity_heatmap(ds),
        "timeline_overview": lambda: cv.timeline_overview(ds, bin_width_us=bin_us),
        "percent_imbalance": lambda: cv.percent_imbalance(ds, bin_us),
        "paper_percent_imbalance": lambda: cv.paper_percent_imbalance(ds),
        "timeline_with_imbalance": lambda: cv.timeline_with_imbalance(ds, bin_width_us=bin_us),
        "timeline_with_paper_imbalance": lambda: cv.timeline_with_paper_imbalance(
            ds, bin_width_us=bin_us
        ),
        "pe_window_1pe_10pct": lambda: cv.time_profile(
            ds, bin_width_us=max(1, bin_us // 10), pes=[0], time_range=window
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("trace_dir")
    parser.add_argument("function", help="entry point to run, or 'list'")
    parser.add_argument("--bins", type=int, default=200)
    args = parser.parse_args()

    ds = cv.TraceDataset(args.trace_dir)
    table = calls(ds, args.bins)
    if args.function == "list":
        print(" ".join(table))
        return 0

    entry = table[args.function]
    fn, required = entry if isinstance(entry, tuple) else (entry, None)
    if required is not None and not ds.has_table(required):
        print(f"{args.function} skipped (no {required} table)")
        return 0

    sampler = Sampler(cgroup_dir())
    sampler.sample()
    sampler.start()
    start = time.monotonic()
    fn()
    wall = time.monotonic() - start
    plt.close("all")
    cg_peak = sampler.stop()

    print(
        f"{args.function} ok wall_s={wall:.1f} anon_peak_mib={sampler.anon / MIB:.0f} "
        f"file_peak_mib={sampler.file / MIB:.0f} cg_peak_mib={cg_peak / MIB:.0f} "
        f"threads={pl.thread_pool_size()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
