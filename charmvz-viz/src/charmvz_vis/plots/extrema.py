"""Extrema Analysis — identify and visualize outlier PEs.

Ranks PEs by a chosen metric and highlights the extremes for investigation.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, Sequence

import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..analysis.utilization import per_pe_utilization
from ..derived import compute_entry_spans

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset

Attribute = Literal[
    "most_idle_time",
    "least_idle_time",
    "overhead",
    "active_entry_methods",
    "avg_grain_size",
]

_ATTR_LABELS = {
    "most_idle_time": "Idle Fraction (highest = worst)",
    "least_idle_time": "Utilization (highest = busiest)",
    "overhead": "Overhead (µs)",
    "active_entry_methods": "Distinct Entry Methods",
    "avg_grain_size": "Avg Grain Size (µs)",
}


def extrema_analysis(
    ds: TraceDataset,
    attribute: Attribute = "most_idle_time",
    k: int = 10,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    figsize: tuple[float, float] = (12, 5),
) -> Figure:
    """Identify and visualize the top-k outlier PEs.

    Parameters
    ----------
    ds : TraceDataset
    attribute : metric to rank PEs by
    k : number of outlier PEs to highlight
    pes : PE filter
    time_range : time range filter
    figsize : figure size

    Returns
    -------
    matplotlib Figure with bar chart ranking PEs by the chosen attribute,
    with the top-k outliers highlighted.
    """
    tr = time_range or ds.time_range_us

    if attribute in ("most_idle_time", "least_idle_time", "overhead"):
        util_df = per_pe_utilization(ds, pes=pes, time_range=tr)

        if attribute == "most_idle_time":
            col = "idle_fraction"
            sort_desc = True
        elif attribute == "least_idle_time":
            col = "utilization"
            sort_desc = True
        else:  # overhead
            col = "overhead_us"
            sort_desc = True

        ranked = util_df.sort(col, descending=sort_desc)
        pe_ids = ranked["pe_id"].to_numpy()
        values = ranked[col].to_numpy()

    elif attribute == "active_entry_methods":
        spans = compute_entry_spans(ds, pes=pes, time_range=tr)
        agg = (
            spans.group_by("pe_id")
            .agg(pl.col("ep_id").n_unique().alias("n_eps"))
            .sort("n_eps", descending=True)
            .collect()
        )
        pe_ids = agg["pe_id"].to_numpy()
        values = agg["n_eps"].to_numpy()
        col = "n_eps"

    elif attribute == "avg_grain_size":
        spans = compute_entry_spans(ds, pes=pes, time_range=tr)
        agg = (
            spans.group_by("pe_id")
            .agg(pl.col("wall_duration_us").mean().alias("avg_dur"))
            .sort("avg_dur", descending=True)
            .collect()
        )
        pe_ids = agg["pe_id"].to_numpy()
        values = agg["avg_dur"].to_numpy()
        col = "avg_dur"

    else:
        raise ValueError(f"Unknown attribute: {attribute}")

    # ── Plot ─────────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=figsize)

    n = len(pe_ids)
    x = np.arange(n)
    colors = np.full(n, "#4e79a7", dtype=object)
    # Highlight top-k
    for i in range(min(k, n)):
        colors[i] = "#e15759"

    ax.bar(x, values, color=colors, alpha=0.85, edgecolor="white", linewidth=0.3)

    # Label top-k bars with PE ID
    for i in range(min(k, n)):
        ax.text(
            i, values[i], f"PE {pe_ids[i]}",
            ha="center", va="bottom", fontsize=7, fontweight="bold",
            color="#e15759",
        )

    ax.set_xlabel("PE rank")
    ax.set_ylabel(_ATTR_LABELS.get(attribute, attribute))
    ax.set_title(f"Extrema Analysis — {_ATTR_LABELS.get(attribute, attribute)}")

    # No individual x-tick labels (too many); rank is the x-axis
    if n <= 64:
        ax.set_xticks(x[::max(1, n // 16)])
    else:
        ax.set_xticks([])

    fig.tight_layout()
    return fig
