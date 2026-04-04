"""Histograms — distribution of execution times, message sizes, etc.

Provides four histogram types matching the Projections spec:
- Execution time distribution
- Accumulated execution time
- Message size distribution
- Idle percentage distribution
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, Sequence

import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..colors import IDLE_COLOR
from ..derived import compute_entry_spans, compute_idle_spans

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset


def execution_time_histogram(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    ep_ids: Sequence[int] | None = None,
    n_bins: int = 50,
    log_scale: bool = False,
    accumulated: bool = False,
    figsize: tuple[float, float] = (12, 5),
) -> Figure:
    """Histogram of entry method execution durations.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE filter
    time_range : time range filter
    ep_ids : specific EP IDs to include (None = all)
    n_bins : number of histogram bins
    log_scale : if True, use logarithmic x-axis
    accumulated : if True, y-axis = total time in bin (count × bin center)
    figsize : figure size

    Returns
    -------
    matplotlib Figure
    """
    tr = time_range or ds.time_range_us
    spans = compute_entry_spans(ds, pes=pes, time_range=tr)

    if ep_ids is not None:
        spans = spans.filter(pl.col("ep_id").is_in(list(ep_ids)))

    durations = spans.select("wall_duration_us").drop_nulls().collect()["wall_duration_us"]
    durations_ms = durations.to_numpy() / 1000.0  # convert to ms

    if len(durations_ms) == 0:
        fig, ax = plt.subplots(figsize=figsize)
        ax.text(0.5, 0.5, "No data", ha="center", va="center", fontsize=14)
        return fig

    fig, ax = plt.subplots(figsize=figsize)

    if log_scale:
        durations_ms = durations_ms[durations_ms > 0]
        bins = np.logspace(
            np.log10(durations_ms.min()),
            np.log10(durations_ms.max()),
            n_bins,
        )
    else:
        bins = n_bins

    if accumulated:
        counts, bin_edges = np.histogram(durations_ms, bins=bins)
        bin_centers = (bin_edges[:-1] + bin_edges[1:]) / 2
        accumulated_time = counts * bin_centers
        ax.bar(
            bin_centers,
            accumulated_time,
            width=np.diff(bin_edges),
            color="#4e79a7",
            alpha=0.85,
            edgecolor="white",
            linewidth=0.3,
        )
        ax.set_ylabel("Accumulated time (ms)")
        ax.set_title("Accumulated Execution Time Histogram")
    else:
        ax.hist(
            durations_ms,
            bins=bins,
            color="#4e79a7",
            alpha=0.85,
            edgecolor="white",
            linewidth=0.3,
        )
        ax.set_ylabel("Count")
        ax.set_title("Execution Time Histogram")

    if log_scale:
        ax.set_xscale("log")

    ax.set_xlabel("Duration (ms)")
    fig.tight_layout()
    return fig


def message_size_histogram(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    n_bins: int = 50,
    log_scale: bool = True,
    figsize: tuple[float, float] = (12, 5),
) -> Figure:
    """Histogram of message sizes.

    Parameters
    ----------
    ds : TraceDataset
    pes : filter on source PE
    time_range : filter on send time
    n_bins : number of histogram bins
    log_scale : if True, use log x-axis (recommended for message sizes)
    figsize : figure size

    Returns
    -------
    matplotlib Figure
    """
    tr = time_range or ds.time_range_us

    msgs = ds.message_spans(pes=pes, time_range=tr)
    sizes = msgs.select("msg_len").drop_nulls().collect()["msg_len"]
    sizes_bytes = sizes.to_numpy().astype(float)

    if len(sizes_bytes) == 0:
        fig, ax = plt.subplots(figsize=figsize)
        ax.text(0.5, 0.5, "No data", ha="center", va="center", fontsize=14)
        return fig

    fig, ax = plt.subplots(figsize=figsize)

    if log_scale:
        sizes_bytes = sizes_bytes[sizes_bytes > 0]
        if len(sizes_bytes) == 0:
            ax.text(0.5, 0.5, "All messages have 0 bytes", ha="center", va="center")
            return fig
        bins = np.logspace(
            np.log10(sizes_bytes.min()),
            np.log10(sizes_bytes.max()),
            n_bins,
        )
        ax.set_xscale("log")
    else:
        bins = n_bins

    ax.hist(
        sizes_bytes,
        bins=bins,
        color="#f28e2b",
        alpha=0.85,
        edgecolor="white",
        linewidth=0.3,
    )

    ax.set_xlabel("Message size (bytes)")
    ax.set_ylabel("Count")
    ax.set_title("Message Size Histogram")
    fig.tight_layout()
    return fig
