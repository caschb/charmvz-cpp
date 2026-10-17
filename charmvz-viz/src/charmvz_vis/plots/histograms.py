"""Histograms — distribution of execution times, message sizes, etc.

Provides four histogram types matching the Projections spec:
- Execution time distribution
- Accumulated execution time
- Message size distribution
- Idle percentage distribution
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import matplotlib.pyplot as plt
import numpy as np

from ..derived import duration_histogram, message_size_histogram_counts

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
    edges, counts = duration_histogram(
        ds, n_bins, log_scale, pes=pes, time_range=time_range, ep_ids=ep_ids
    )

    fig, ax = plt.subplots(figsize=figsize)
    if len(edges) == 0:
        ax.text(0.5, 0.5, "No data", ha="center", va="center", fontsize=14)
        return fig

    if accumulated:
        bin_centers = (edges[:-1] + edges[1:]) / 2
        accumulated_time = counts * bin_centers
        ax.bar(
            bin_centers,
            accumulated_time,
            width=np.diff(edges),
            color="#4e79a7",
            alpha=0.85,
            edgecolor="white",
            linewidth=0.3,
        )
        ax.set_ylabel("Accumulated time (ms)")
        ax.set_title("Accumulated Execution Time Histogram")
    else:
        ax.bar(
            edges[:-1],
            counts,
            width=np.diff(edges),
            align="edge",
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
    edges, counts = message_size_histogram_counts(
        ds, n_bins, log_scale, pes=pes, time_range=time_range
    )

    fig, ax = plt.subplots(figsize=figsize)
    if len(edges) == 0:
        # An empty log histogram may still have messages, all of size 0.
        if log_scale and len(
            message_size_histogram_counts(ds, 1, False, pes=pes, time_range=time_range)[1]
        ):
            ax.text(0.5, 0.5, "All messages have 0 bytes", ha="center", va="center")
        else:
            ax.text(0.5, 0.5, "No data", ha="center", va="center", fontsize=14)
        return fig

    if log_scale:
        ax.set_xscale("log")
    ax.bar(
        edges[:-1],
        counts,
        width=np.diff(edges),
        align="edge",
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
