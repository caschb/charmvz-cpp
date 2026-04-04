"""Entry Method Profile — pie/donut chart of EP time fractions.

Shows which entry methods dominate the runtime globally.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import matplotlib.pyplot as plt
import polars as pl

from ..colors import OTHER_COLOR
from ..derived import compute_entry_spans

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset


def ep_profile(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    top_n: int = 12,
    threshold_pct: float = 1.0,
    donut: bool = True,
    figsize: tuple[float, float] = (8, 8),
) -> Figure:
    """Create an Entry Method Profile pie/donut chart.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE IDs to include, or None for all
    time_range : (start_us, end_us) or None for full trace
    top_n : max number of EPs to show individually
    threshold_pct : EPs below this % are grouped as "Other"
    donut : if True, render as donut chart (hollow center)
    figsize : figure size

    Returns
    -------
    matplotlib Figure
    """
    tr = time_range or ds.time_range_us

    # ── Aggregate total time per EP ──────────────────────────────────────
    spans = compute_entry_spans(ds, pes=pes, time_range=tr)
    ep_agg = (
        spans.group_by("ep_id", "ep_name")
        .agg(pl.col("wall_duration_us").sum().alias("total_us"))
        .sort("total_us", descending=True)
        .collect()
    )

    if len(ep_agg) == 0:
        fig, ax = plt.subplots(figsize=figsize)
        ax.text(0.5, 0.5, "No data", ha="center", va="center", fontsize=14)
        ax.set_axis_off()
        return fig

    grand_total = ep_agg["total_us"].sum()
    threshold = grand_total * threshold_pct / 100

    # Split into top entries and "other"
    ep_agg = ep_agg.with_columns(
        (pl.col("total_us") / grand_total * 100).alias("pct"),
    )

    top = ep_agg.filter(
        (pl.col("total_us") >= threshold) & (pl.int_range(pl.len()) < top_n)
    )
    other_total = grand_total - top["total_us"].sum()

    labels = top["ep_name"].to_list()
    values = top["total_us"].to_list()
    ep_ids = top["ep_id"].to_list()
    colors = ds.ep_color_map.to_list(ep_ids)

    if other_total > 0:
        labels.append("Other")
        values.append(other_total)
        colors.append(OTHER_COLOR)

    # ── Plot ─────────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=figsize)

    wedge_props = {"edgecolor": "white", "linewidth": 1.5}
    if donut:
        wedge_props["width"] = 0.5

    wedges, texts, autotexts = ax.pie(
        values,
        labels=None,
        colors=colors,
        autopct=lambda p: f"{p:.1f}%" if p >= 2 else "",
        startangle=90,
        counterclock=False,
        wedgeprops=wedge_props,
        pctdistance=0.75 if donut else 0.6,
    )

    for t in autotexts:
        t.set_fontsize(8)
        t.set_color("white")
        t.set_fontweight("bold")

    # Legend with EP names
    ax.legend(
        wedges,
        [f"{lbl} ({v / grand_total * 100:.1f}%)" for lbl, v in zip(labels, values)],
        loc="center left",
        bbox_to_anchor=(1.0, 0.5),
        fontsize=8,
        frameon=False,
    )

    if donut:
        total_s = grand_total / 1e6
        ax.text(0, 0, f"{total_s:.2f}s\ntotal", ha="center", va="center",
                fontsize=12, fontweight="bold")

    ax.set_title("Entry Method Profile")
    fig.tight_layout()
    return fig
