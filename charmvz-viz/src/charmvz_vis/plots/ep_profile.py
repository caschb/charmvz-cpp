"""Entry Method Profile — ranked horizontal bars of recorded execution shares.

Shares use all selected stored wall durations, including methods grouped as Other.
"""

from __future__ import annotations

import textwrap
from typing import TYPE_CHECKING, Sequence

import matplotlib.pyplot as plt
import polars as pl

from ..colors import OTHER_COLOR
from ..derived import attach_names, execution_rows
from ..engine import collect

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
    figsize: tuple[float, float] = (12, 7),
) -> Figure:
    """Create a ranked Entry Method Profile bar chart.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE IDs to include, or None for all
    time_range : overlapping (start_us, end_us) window, or None for full trace
        Selected rows contribute their full stored wall_duration_us; idle is excluded.
    top_n : max number of EPs to show individually; ties use ascending ep_id
    threshold_pct : EPs below this % are grouped as "Other"
    figsize : figure size

    Returns
    -------
    matplotlib Figure
    """
    tr = time_range or ds.time_range_us

    # ── Aggregate total time per EP ──────────────────────────────────────
    ep_agg = collect(
        attach_names(
            execution_rows(ds, pes=pes, time_range=tr)
            .group_by("ep_id")
            .agg(pl.col("wall_duration_us").sum().alias("total_us")),
            ds,
            ep=True,
        ).sort(["total_us", "ep_id"], descending=[True, False])
    )

    grand_total = ep_agg["total_us"].sum() or 0
    fig, ax = plt.subplots(figsize=figsize)
    fig.text(0.5, 0.02,
             f"Total recorded execution duration: {grand_total / 1e6:,.2f} s",
             ha="center", fontsize=10)
    ax.set_title("Entry Method Profile")
    if len(ep_agg) == 0 or grand_total == 0:
        message = ("No selected executions" if len(ep_agg) == 0
                   else "Zero total recorded execution duration")
        ax.text(0.5, 0.5, message, transform=ax.transAxes,
                ha="center", va="center", fontsize=12)
        ax.set_axis_off()
        return fig

    threshold = grand_total * threshold_pct / 100

    # Split into top entries and "other"
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

    # Allocate row space for wrapped C++ signatures, including unbroken names.
    labels = [textwrap.fill(label, 38) for label in labels]
    heights = [max(1, label.count("\n") + 1) for label in labels]
    positions = []
    cursor = 0
    for height in heights:
        positions.append(cursor + height / 2)
        cursor += height + 0.7
    percentages = [value / grand_total * 100 for value in values]
    bars = ax.barh(positions, percentages, height=0.65, color=colors)
    ax.set_yticks(positions, labels, fontsize=9)
    ax.set_ylim(cursor, -0.4)
    ax.set_xlim(0, 100)
    ax.set_xlabel("Share of selected recorded execution duration (%)")
    ax.set_axisbelow(True)
    ax.grid(axis="x", alpha=0.25)
    ax.tick_params(axis="y", length=0)
    for spine in ("top", "right", "left"):
        ax.spines[spine].set_visible(False)
    for bar, pct in zip(bars, percentages):
        label = "<0.1%" if 0 < pct < 0.1 else f"{pct:.1f}%"
        ax.annotate(label, (pct, bar.get_y() + bar.get_height() / 2),
                    xytext=(5, 0), textcoords="offset points", va="center",
                    fontsize=9, annotation_clip=False)
    fig.tight_layout(rect=(0, 0.07, 0.95, 1))
    return fig
