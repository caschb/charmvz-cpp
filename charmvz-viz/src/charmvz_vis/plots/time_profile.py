"""Time Profile — stacked area/bar chart of EP time usage over the run.

Shows how the mix of work changes over the course of the run, with each time
bin partitioned by entry method.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..colors import IDLE_COLOR, OTHER_COLOR
from ..derived import bin_spans, compute_entry_spans, compute_idle_spans

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset


def time_profile(
    ds: TraceDataset,
    bin_width_us: int = 1_000_000,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    as_percentage: bool = False,
    top_n: int = 15,
    show_idle: bool = True,
    figsize: tuple[float, float] = (14, 6),
) -> Figure:
    """Create a Time Profile stacked area chart.

    Parameters
    ----------
    ds : TraceDataset
    bin_width_us : time bin width in microseconds (default 1s)
    pes : PE IDs to include, or None for all
    time_range : (start_us, end_us) or None for full trace
    as_percentage : if True, normalize each bin to 100%
    top_n : number of top EPs to show; rest grouped as "Other"
    show_idle : whether to show idle time as a band
    figsize : matplotlib figure size

    Returns
    -------
    matplotlib Figure
    """
    tr = time_range or ds.time_range_us
    n_pes = ds.num_pes if pes is None else len(list(pes))

    # ── Compute per-(bin, ep_id) busy time ───────────────────────────────
    spans = compute_entry_spans(ds, pes=pes, time_range=tr)
    binned = bin_spans(spans, bin_width_us, tr)

    ep_bin_agg = (
        binned.group_by("bin_start", "ep_id", "ep_name")
        .agg(pl.col("bin_contribution_us").sum().alias("total_us"))
        .collect()
    )

    # ── Identify top-N EPs by total time ─────────────────────────────────
    ep_totals = (
        ep_bin_agg.group_by("ep_id", "ep_name")
        .agg(pl.col("total_us").sum().alias("grand_total"))
        .sort("grand_total", descending=True)
    )

    top_eps = ep_totals.head(top_n)
    top_ep_ids = set(top_eps["ep_id"].to_list())
    ep_names = dict(zip(top_eps["ep_id"].to_list(), top_eps["ep_name"].to_list()))

    # ── Build bin × EP matrix ────────────────────────────────────────────
    t_start, t_end = tr
    n_bins = max(1, (t_end - t_start + bin_width_us - 1) // bin_width_us)
    bin_starts = np.arange(t_start, t_start + n_bins * bin_width_us, bin_width_us)

    # Sorted EP list for consistent stacking
    sorted_ep_ids = top_eps["ep_id"].to_list()

    # Initialize matrix: rows = EP, cols = bin
    matrix = np.zeros((len(sorted_ep_ids) + 1, len(bin_starts)))  # +1 for "Other"
    ep_idx_map = {eid: i for i, eid in enumerate(sorted_ep_ids)}
    other_idx = len(sorted_ep_ids)

    for row in ep_bin_agg.iter_rows(named=True):
        bin_idx = (row["bin_start"] - t_start) // bin_width_us
        if 0 <= bin_idx < len(bin_starts):
            if row["ep_id"] in ep_idx_map:
                matrix[ep_idx_map[row["ep_id"]], bin_idx] += row["total_us"]
            else:
                matrix[other_idx, bin_idx] += row["total_us"]

    # ── Idle time per bin ────────────────────────────────────────────────
    idle_row = np.zeros(len(bin_starts))
    if show_idle:
        idles = compute_idle_spans(ds, pes=pes, time_range=tr)
        idle_binned = bin_spans(
            idles, bin_width_us, tr,
            start_col="start_time_us",
            end_col="end_time_us",
            duration_col="duration_us",
        )
        idle_agg = (
            idle_binned.group_by("bin_start")
            .agg(pl.col("bin_contribution_us").sum().alias("idle_us"))
            .collect()
        )
        for row in idle_agg.iter_rows(named=True):
            bin_idx = (row["bin_start"] - t_start) // bin_width_us
            if 0 <= bin_idx < len(bin_starts):
                idle_row[bin_idx] = row["idle_us"]

    # ── Normalize if percentage mode ─────────────────────────────────────
    if as_percentage:
        total_available = n_pes * bin_width_us
        matrix = matrix / total_available * 100
        idle_row = idle_row / total_available * 100
        y_label = "% of available PE-time"
    else:
        # Convert to milliseconds for readability
        matrix = matrix / 1000
        idle_row = idle_row / 1000
        y_label = "Time (ms)"

    # ── Plot ─────────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=figsize)

    x = (bin_starts - t_start) / 1e6  # convert to seconds for x-axis
    colors = ds.ep_color_map.to_list(sorted_ep_ids) + [OTHER_COLOR]
    labels = [ep_names.get(eid, f"EP {eid}") for eid in sorted_ep_ids] + ["Other"]

    # Stack: idle at bottom, then EPs
    if show_idle:
        ax.fill_between(x, 0, idle_row, color=IDLE_COLOR, alpha=0.6, label="Idle", step="mid")
        bottom = idle_row.copy()
    else:
        bottom = np.zeros_like(x)

    for i in range(matrix.shape[0]):
        row_data = matrix[i]
        if row_data.sum() == 0:
            continue
        ax.fill_between(x, bottom, bottom + row_data, color=colors[i],
                        alpha=0.85, label=labels[i], step="mid")
        bottom = bottom + row_data

    ax.set_xlabel("Time (s)")
    ax.set_ylabel(y_label)
    ax.set_title("Time Profile")
    ax.set_xlim(x[0], x[-1])

    # Legend outside plot area
    ax.legend(
        loc="upper left",
        bbox_to_anchor=(1.02, 1),
        fontsize=8,
        frameon=False,
    )

    fig.tight_layout()
    return fig
