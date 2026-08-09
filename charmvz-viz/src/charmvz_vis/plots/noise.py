"""Noise Miner — temporal distribution of detected computational noise.

The companion view to :func:`charmvz_vis.analysis.noise.noise_miner`, which
reports the clusters as a table. This renders where in the run they occurred.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..analysis.noise import noise_occurrences
from ..colors import _BASE_PALETTE

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset


def noise_timeline(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    n_bins: int = 50,
    bin_width_us: int = 100,
    merge_threshold: float = 0.1,
    importance_cutoff: float = 0.01,
    min_pes: int = 2,
    quantum_us: int | None = None,
    weight: str = "count",
    figsize: tuple[float, float] = (12, 5),
) -> Figure:
    """Stacked bar chart of noise events over time, one stack per cluster.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE filter
    time_range : time range filter, defaulting to the whole run
    n_bins : number of time bins along the x axis
    bin_width_us, merge_threshold, importance_cutoff, min_pes, quantum_us :
        passed through to the miner; see
        :func:`charmvz_vis.analysis.noise.noise_miner`
    weight : ``"count"`` to stack occurrence counts, ``"time"`` to stack the
        microseconds lost. Counts show how often a source fires; time shows
        which source actually costs the run
    figsize : figure size

    Returns
    -------
    matplotlib Figure. A run with no detected noise still produces a figure,
    annotated as empty, so a caller sweeping over runs does not have to guard
    every call.
    """
    if n_bins <= 0:
        raise ValueError("n_bins must be positive")
    if weight not in ("count", "time"):
        raise ValueError("weight must be 'count' or 'time'")

    tr = time_range or ds.time_range_us
    occ = noise_occurrences(
        ds,
        pes=pes,
        time_range=tr,
        bin_width_us=bin_width_us,
        merge_threshold=merge_threshold,
        importance_cutoff=importance_cutoff,
        min_pes=min_pes,
        quantum_us=quantum_us,
    )

    fig, ax = plt.subplots(figsize=figsize)

    if occ.is_empty():
        ax.text(
            0.5,
            0.5,
            "No noise clusters above the importance cutoff",
            ha="center",
            va="center",
            transform=ax.transAxes,
            fontsize=11,
            color="#6b6b6b",
        )
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Noise events")
        ax.set_title("Noise Miner")
        fig.tight_layout()
        return fig

    t_start, t_end = tr
    edges = np.linspace(t_start, t_end, n_bins + 1)
    width_us = (t_end - t_start) / n_bins
    centers = (edges[:-1] + edges[1:]) / 2.0

    binned = occ.with_columns(
        ((pl.col("time_us") - t_start) / width_us)
        .cast(pl.Int64)
        .clip(0, n_bins - 1)
        .alias("_time_bin"),
    )

    value = pl.len() if weight == "count" else pl.col("noise_us").sum()
    grouped = binned.group_by("cluster_id", "_time_bin").agg(value.alias("_value"))

    # Order the stack by total contribution so the dominant cluster reads first
    # in the legend and sits at the bottom of every bar.
    order = (
        grouped.group_by("cluster_id")
        .agg(pl.col("_value").sum().alias("_total"))
        .sort("_total", descending=True)["cluster_id"]
        .to_list()
    )

    labels = {
        row["cluster_id"]: f"{row['kind']} ~{row['noise_us_median'] / 1000:.2f} ms"
        for row in occ.group_by("cluster_id")
        .agg(
            pl.col("kind").first(),
            pl.col("noise_us").median().alias("noise_us_median"),
        )
        .iter_rows(named=True)
    }

    bottom = np.zeros(n_bins, dtype=float)
    scale = 1.0 if weight == "count" else 1000.0  # µs -> ms for the time axis

    for i, cluster_id in enumerate(order):
        series = np.zeros(n_bins, dtype=float)
        rows = grouped.filter(pl.col("cluster_id") == cluster_id)
        for bin_idx, val in zip(
            rows["_time_bin"].to_list(), rows["_value"].to_list(), strict=True
        ):
            series[bin_idx] = val / scale

        ax.bar(
            centers / 1e6,
            series,
            width=(width_us / 1e6) * 0.95,
            bottom=bottom,
            color=_BASE_PALETTE[i % len(_BASE_PALETTE)],
            label=labels.get(cluster_id, str(cluster_id)),
            edgecolor="white",
            linewidth=0.2,
        )
        bottom += series

    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Noise events" if weight == "count" else "Time lost (ms)")
    ax.set_title("Noise Miner — temporal distribution of detected noise")
    ax.legend(fontsize=8, ncol=2, framealpha=0.9)
    ax.margins(x=0.01)

    fig.tight_layout()
    return fig
