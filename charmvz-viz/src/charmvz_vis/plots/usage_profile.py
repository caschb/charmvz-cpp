"""Usage Profile — stacked bar chart of EP time per PE.

Shows how work is distributed across processors, with each bar divided by
entry method.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..colors import IDLE_COLOR, OTHER_COLOR
from ..derived import compute_entry_spans, compute_idle_spans

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset


def usage_profile(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    top_n: int = 15,
    show_idle: bool = True,
    as_percentage: bool = False,
    figsize: tuple[float, float] | None = None,
) -> Figure:
    """Create a Usage Profile stacked bar chart.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE IDs to include, or None for all
    time_range : (start_us, end_us) or None for full trace
    top_n : number of top EPs to show; rest grouped as "Other"
    show_idle : whether to show idle time
    as_percentage : if True, normalize each PE bar to 100%
    figsize : figure size; auto-scaled if None

    Returns
    -------
    matplotlib Figure
    """
    tr = time_range or ds.time_range_us
    window_us = tr[1] - tr[0]

    # ── Per-(PE, EP) total duration ──────────────────────────────────────
    spans = compute_entry_spans(ds, pes=pes, time_range=tr)
    pe_ep_agg = (
        spans.with_columns(
            (
                pl.min_horizontal(pl.col("end_time_us").fill_null(tr[1]), pl.lit(tr[1]))
                - pl.max_horizontal(pl.col("start_time_us"), pl.lit(tr[0]))
            )
            .clip(lower_bound=0)
            .alias("clipped_us")
        )
        .group_by("pe_id", "ep_id", "ep_name")
        .agg(pl.col("clipped_us").sum().alias("total_us"))
        .collect()
    )

    # ── Identify top-N EPs globally ──────────────────────────────────────
    ep_totals = (
        pe_ep_agg.group_by("ep_id", "ep_name")
        .agg(pl.col("total_us").sum().alias("grand_total"))
        .sort("grand_total", descending=True)
    )
    top_eps = ep_totals.head(top_n)
    top_ep_ids = top_eps["ep_id"].to_list()
    ep_names = dict(zip(top_eps["ep_id"].to_list(), top_eps["ep_name"].to_list()))

    # ── All PE IDs ───────────────────────────────────────────────────────
    all_pe_ids = sorted(pe_ep_agg["pe_id"].unique().to_list())
    pe_positions = {pe: i for i, pe in enumerate(all_pe_ids)}
    n_pes = len(all_pe_ids)

    # ── Build PE × EP matrix ─────────────────────────────────────────────
    n_eps = len(top_ep_ids) + 1  # +1 for Other
    matrix = np.zeros((n_eps, n_pes))
    ep_idx_map = {eid: i for i, eid in enumerate(top_ep_ids)}
    other_idx = len(top_ep_ids)

    for row in pe_ep_agg.iter_rows(named=True):
        pe_idx = pe_positions[row["pe_id"]]
        if row["ep_id"] in ep_idx_map:
            matrix[ep_idx_map[row["ep_id"]], pe_idx] += row["total_us"]
        else:
            matrix[other_idx, pe_idx] += row["total_us"]

    # ── Idle time per PE ─────────────────────────────────────────────────
    idle_row = np.zeros(n_pes)
    if show_idle:
        idles = compute_idle_spans(ds, pes=pes, time_range=tr)
        idle_agg = (
            idles.with_columns(
                (
                    pl.min_horizontal(pl.col("end_time_us").fill_null(tr[1]), pl.lit(tr[1]))
                    - pl.max_horizontal(pl.col("start_time_us"), pl.lit(tr[0]))
                )
                .clip(lower_bound=0)
                .alias("clipped_us")
            )
            .group_by("pe_id")
            .agg(pl.col("clipped_us").sum().alias("idle_us"))
            .collect()
        )
        for row in idle_agg.iter_rows(named=True):
            if row["pe_id"] in pe_positions:
                idle_row[pe_positions[row["pe_id"]]] = row["idle_us"]

    # ── Normalize ────────────────────────────────────────────────────────
    if as_percentage:
        totals = matrix.sum(axis=0) + idle_row
        totals = np.where(totals == 0, 1, totals)
        matrix = matrix / totals * 100
        idle_row = idle_row / totals * 100
        y_label = "% of time"
    else:
        matrix = matrix / 1000  # ms
        idle_row = idle_row / 1000
        y_label = "Time (ms)"

    # ── Plot ─────────────────────────────────────────────────────────────
    if figsize is None:
        figsize = (max(8, n_pes * 0.25), 6)
    fig, ax = plt.subplots(figsize=figsize)

    x = np.arange(n_pes)
    bar_width = 0.8
    colors = ds.ep_color_map.to_list(top_ep_ids) + [OTHER_COLOR]
    labels = [ep_names.get(eid, f"EP {eid}") for eid in top_ep_ids] + ["Other"]

    # Stack idle at bottom
    if show_idle:
        ax.bar(x, idle_row, bar_width, color=IDLE_COLOR, alpha=0.6, label="Idle")
        bottom = idle_row.copy()
    else:
        bottom = np.zeros(n_pes)

    for i in range(matrix.shape[0]):
        if matrix[i].sum() == 0:
            continue
        ax.bar(x, matrix[i], bar_width, bottom=bottom, color=colors[i],
               alpha=0.85, label=labels[i])
        bottom += matrix[i]

    # X-axis labels — show PE IDs
    if n_pes <= 64:
        ax.set_xticks(x)
        ax.set_xticklabels(all_pe_ids, fontsize=max(4, 8 - n_pes // 20), rotation=90)
    else:
        # For large PE counts, show every Nth label
        step = max(1, n_pes // 32)
        ax.set_xticks(x[::step])
        ax.set_xticklabels([all_pe_ids[i] for i in range(0, n_pes, step)], fontsize=6)

    ax.set_xlabel("PE")
    ax.set_ylabel(y_label)
    ax.set_title("Usage Profile")

    ax.legend(
        loc="upper left",
        bbox_to_anchor=(1.02, 1),
        fontsize=8,
        frameon=False,
    )

    fig.tight_layout()
    return fig
