"""Paper-equivalent CharmVZ visualizations.

These plots recreate the semantics of the Python paper figures from the
normalized Parquet output produced by the C++ converter.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, Literal

import matplotlib.colors as mcolors
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..colors import IDLE_COLOR
from ..derived import (
    chare_activity_matrix,
    chare_duration_totals,
    chare_frequency_counts,
    chare_load_by_bin,
    cumulative_percent_imbalance_by_time,
    percent_imbalance_by_bin,
)

if TYPE_CHECKING:
    from matplotlib.axes import Axes
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset


Metric = Literal["duration", "frequency"]


def _chare_order_from_runs(
    runs: Mapping[str, TraceDataset],
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> list[str]:
    totals: dict[str, int] = {}
    for ds in runs.values():
        df = chare_duration_totals(ds, pes=pes, time_range=time_range).collect()
        for row in df.iter_rows(named=True):
            totals[row["chare_name"]] = totals.get(row["chare_name"], 0) + row["total_duration_us"]
    return [name for name, _ in sorted(totals.items(), key=lambda item: (-item[1], item[0]))]


def _chare_colors(chare_names: Sequence[str]) -> dict[str, str]:
    palette = list(plt.get_cmap("tab20").colors) + list(plt.get_cmap("tab20b").colors)
    return {
        name: mcolors.to_hex(palette[i % len(palette)])
        for i, name in enumerate(chare_names)
    }


def _bin_starts(time_range: tuple[int, int], bin_width_us: int) -> np.ndarray:
    t_start, t_end = time_range
    n_bins = max(1, (t_end - t_start + bin_width_us - 1) // bin_width_us)
    return np.arange(t_start, t_start + n_bins * bin_width_us, bin_width_us)


def _resolve_timeline_chares(
    df: pl.DataFrame,
    chare_order: Sequence[str] | None,
    chares: Sequence[str] | None,
    top_n_chares: int | None,
) -> list[str]:
    if top_n_chares is not None and top_n_chares <= 0:
        raise ValueError("top_n_chares must be positive when provided")

    if chare_order is not None:
        resolved = list(chare_order)
    elif chares is not None:
        resolved = list(chares)
    else:
        totals = (
            df.group_by("chare_name")
            .agg(pl.col("load_us").sum().alias("total"))
            .sort(["total", "chare_name"], descending=[True, False])
        )
        resolved = totals["chare_name"].to_list()

    if top_n_chares is not None:
        resolved = resolved[:top_n_chares]
    return resolved


def _add_timeline_legend(
    ax: Axes,
    colors: Mapping[str, str],
    *,
    include_idle: bool = True,
) -> None:
    handles = []
    if include_idle:
        handles.append(mpatches.Patch(facecolor=IDLE_COLOR, edgecolor="none", label="Idle"))
    handles.extend(
        mpatches.Patch(facecolor=color, edgecolor="none", label=chare_name)
        for chare_name, color in colors.items()
    )
    if handles:
        ax.legend(
            handles=handles,
            title="Chare",
            loc="upper center",
            bbox_to_anchor=(0.5, 1.15),
            ncol=min(4, len(handles)),
            frameon=False,
            fontsize=8,
            title_fontsize=9,
        )


def chare_duration_comparison(
    runs: Mapping[str, TraceDataset],
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    top_n: int | None = 12,
    figsize: tuple[float, float] = (12, 6),
) -> Figure:
    """Compare total clipped chare execution duration across runs."""
    chare_order = _chare_order_from_runs(runs, pes=pes, time_range=time_range)
    if top_n is not None:
        chare_order = chare_order[:top_n]

    run_names = list(runs.keys())
    values = np.zeros((len(chare_order), len(run_names)))
    for run_idx, ds in enumerate(runs.values()):
        df = chare_duration_totals(ds, pes=pes, time_range=time_range).collect()
        totals = dict(zip(df["chare_name"].to_list(), df["total_duration_us"].to_list()))
        for chare_idx, chare_name in enumerate(chare_order):
            values[chare_idx, run_idx] = totals.get(chare_name, 0) / 1e6

    fig, ax = plt.subplots(figsize=figsize)
    x = np.arange(len(run_names))
    width = 0.8 / max(1, len(chare_order))
    colors = _chare_colors(chare_order)
    for i, chare_name in enumerate(chare_order):
        offset = (i - (len(chare_order) - 1) / 2) * width
        ax.bar(x + offset, values[i], width, label=chare_name, color=colors[chare_name], alpha=0.9)

    ax.set_xticks(x)
    ax.set_xticklabels(run_names, rotation=20, ha="right")
    ax.set_ylabel("Total duration (s)")
    ax.set_title("Chare Duration Comparison")
    ax.legend(loc="upper left", bbox_to_anchor=(1.02, 1), fontsize=8, frameon=False)
    fig.tight_layout()
    return fig


def chare_frequency_comparison(
    runs: Mapping[str, TraceDataset],
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    top_n: int | None = 12,
    figsize: tuple[float, float] = (12, 6),
) -> Figure:
    """Compare chare execution frequencies across runs."""
    chare_order = _chare_order_from_runs(runs, pes=pes, time_range=time_range)
    if top_n is not None:
        chare_order = chare_order[:top_n]

    run_names = list(runs.keys())
    values = np.zeros((len(chare_order), len(run_names)))
    for run_idx, ds in enumerate(runs.values()):
        df = chare_frequency_counts(ds, pes=pes, time_range=time_range).collect()
        counts = dict(zip(df["chare_name"].to_list(), df["execution_count"].to_list()))
        for chare_idx, chare_name in enumerate(chare_order):
            values[chare_idx, run_idx] = counts.get(chare_name, 0)

    fig, ax = plt.subplots(figsize=figsize)
    x = np.arange(len(run_names))
    width = 0.8 / max(1, len(chare_order))
    colors = _chare_colors(chare_order)
    for i, chare_name in enumerate(chare_order):
        offset = (i - (len(chare_order) - 1) / 2) * width
        ax.bar(x + offset, values[i], width, label=chare_name, color=colors[chare_name], alpha=0.9)

    ax.set_xticks(x)
    ax.set_xticklabels(run_names, rotation=20, ha="right")
    ax.set_ylabel("Executions")
    ax.set_title("Chare Frequency Comparison")
    ax.legend(loc="upper left", bbox_to_anchor=(1.02, 1), fontsize=8, frameon=False)
    fig.tight_layout()
    return fig


def chare_activity_heatmap(
    ds: TraceDataset,
    metric: Metric = "duration",
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    figsize: tuple[float, float] | None = None,
) -> Figure:
    """Show per-PE chare activity as a heatmap."""
    df = chare_activity_matrix(ds, pes=pes, time_range=time_range, metric=metric).collect()
    if len(df) == 0:
        fig, ax = plt.subplots(figsize=figsize or (8, 5))
        ax.text(0.5, 0.5, "No data", ha="center", va="center")
        ax.set_axis_off()
        return fig

    pe_ids = sorted(df["pe_id"].unique().to_list())
    chare_totals = (
        df.group_by("chare_name")
        .agg(pl.col("activity").sum().alias("total"))
        .sort(["total", "chare_name"], descending=[True, False])
    )
    chare_names = chare_totals["chare_name"].to_list()
    pe_pos = {pe: i for i, pe in enumerate(pe_ids)}
    chare_pos = {name: i for i, name in enumerate(chare_names)}
    matrix = np.zeros((len(pe_ids), len(chare_names)))
    for row in df.iter_rows(named=True):
        matrix[pe_pos[row["pe_id"]], chare_pos[row["chare_name"]]] = row["activity"]

    if metric == "duration":
        matrix = matrix / 1000.0
        cbar_label = "Duration (ms)"
    else:
        cbar_label = "Executions"

    if figsize is None:
        figsize = (max(7, len(chare_names) * 0.8), max(4, len(pe_ids) * 0.25))
    fig, ax = plt.subplots(figsize=figsize)
    image = ax.imshow(matrix, aspect="auto", interpolation="nearest", cmap="viridis")
    ax.set_xlabel("Chare")
    ax.set_ylabel("PE")
    ax.set_title("Chare Activity")
    ax.set_xticks(np.arange(len(chare_names)))
    ax.set_xticklabels(chare_names, rotation=35, ha="right")
    ax.set_yticks(np.arange(len(pe_ids)))
    ax.set_yticklabels(pe_ids)
    fig.colorbar(image, ax=ax, label=cbar_label)
    fig.tight_layout()
    return fig


def _draw_timeline_overview(
    ax: Axes,
    ds: TraceDataset,
    bin_width_us: int,
    pes: Sequence[int] | None,
    time_range: tuple[int, int],
    chare_order: Sequence[str] | None,
    chares: Sequence[str] | None = None,
    top_n_chares: int | None = None,
    show_legend: bool = True,
) -> None:
    df = chare_load_by_bin(ds, bin_width_us, pes=pes, time_range=time_range).collect()
    pe_ids = (
        sorted(ds.processing_element.select("pe_id").collect()["pe_id"].to_list())
        if pes is None
        else sorted(pes)
    )
    bin_starts = _bin_starts(time_range, bin_width_us)
    pe_pos = {pe: i for i, pe in enumerate(pe_ids)}
    bin_pos = {int(bin_start): i for i, bin_start in enumerate(bin_starts)}

    chare_order = _resolve_timeline_chares(df, chare_order, chares, top_n_chares)
    chare_pos = {name: i + 1 for i, name in enumerate(chare_order)}
    visible_chares = set(chare_order)
    chare_colors = _chare_colors(chare_order)

    matrix = np.zeros((len(pe_ids), len(bin_starts)), dtype=int)
    best_loads: dict[tuple[int, int], int] = {}
    for row in df.iter_rows(named=True):
        if row["chare_name"] not in visible_chares:
            continue
        key = (row["pe_id"], row["bin_start"])
        if key not in best_loads or row["load_us"] > best_loads[key]:
            best_loads[key] = row["load_us"]
            if row["pe_id"] in pe_pos and row["bin_start"] in bin_pos:
                matrix[pe_pos[row["pe_id"]], bin_pos[row["bin_start"]]] = chare_pos.get(
                    row["chare_name"], 0
                )

    colors = [IDLE_COLOR] + [chare_colors[name] for name in chare_order]
    cmap = mcolors.ListedColormap(colors)
    norm = mcolors.BoundaryNorm(np.arange(len(colors) + 1) - 0.5, len(colors))
    extent = [
        (bin_starts[0] - time_range[0]) / 1e6,
        (bin_starts[-1] + bin_width_us - time_range[0]) / 1e6,
        len(pe_ids) - 0.5,
        -0.5,
    ]
    ax.imshow(matrix, aspect="auto", interpolation="nearest", cmap=cmap, norm=norm, extent=extent)
    ax.set_ylabel("PE")
    ax.set_title("Timeline Overview")
    if len(pe_ids) <= 32:
        ax.set_yticks(np.arange(len(pe_ids)))
        ax.set_yticklabels(pe_ids)
    if show_legend:
        _add_timeline_legend(ax, chare_colors)


def timeline_overview(
    ds: TraceDataset,
    color_by: Literal["chare"] = "chare",
    bin_width_us: int = 100_000,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    chares: Sequence[str] | None = None,
    top_n_chares: int | None = None,
    show_legend: bool = True,
    figsize: tuple[float, float] = (14, 6),
) -> Figure:
    """Render a PE-by-time image colored by dominant chare per bin."""
    if color_by != "chare":
        raise ValueError("timeline_overview currently supports color_by='chare'")
    tr = time_range or ds.time_range_us
    fig, ax = plt.subplots(figsize=figsize)
    _draw_timeline_overview(
        ax,
        ds,
        bin_width_us,
        pes,
        tr,
        chare_order=None,
        chares=chares,
        top_n_chares=top_n_chares,
        show_legend=show_legend,
    )
    ax.set_xlabel("Time (s)")
    fig.tight_layout()
    return fig


def percent_imbalance(
    ds: TraceDataset,
    bin_width_us: int,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    figsize: tuple[float, float] = (12, 4),
) -> Figure:
    """Plot percent load imbalance over time."""
    tr = time_range or ds.time_range_us
    df = percent_imbalance_by_bin(ds, bin_width_us, pes=pes, time_range=tr).collect()
    fig, ax = plt.subplots(figsize=figsize)
    x = (df["bin_start"].to_numpy() - tr[0]) / 1e6
    ax.plot(x, df["percent_imbalance"].to_numpy(), color="#4e79a7", linewidth=1.8)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Imbalance (%)")
    ax.set_title("Percent Imbalance")
    ax.set_ylim(bottom=0)
    fig.tight_layout()
    return fig


def paper_percent_imbalance(
    ds: TraceDataset,
    *,
    total_time_points: int = 100,
    time_points_us: Sequence[int | float] | None = None,
    total_nodes: int = 4,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    max_imbalance: float | None = None,
    figsize: tuple[float, float] = (12, 4),
) -> Figure:
    """Plot cumulative percent imbalance using the original paper semantics."""
    tr = time_range or ds.time_range_us
    df = cumulative_percent_imbalance_by_time(
        ds,
        total_time_points=total_time_points,
        time_points_us=time_points_us,
        total_nodes=total_nodes,
        pes=pes,
        time_range=time_range,
    ).collect()

    fig, ax = plt.subplots(figsize=figsize)
    x = (df["time_point_us"].to_numpy() - tr[0]) / 1e6
    ax.plot(x, df["percent_imbalance"].to_numpy(), color="purple", linewidth=1.8)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Percent Imbalance")
    ax.set_title("Paper Percent Imbalance")
    ax.set_ylim(0, max_imbalance if max_imbalance is not None else None)
    fig.tight_layout()
    return fig


def timeline_with_imbalance(
    ds: TraceDataset,
    bin_width_us: int = 100_000,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    chares: Sequence[str] | None = None,
    top_n_chares: int | None = None,
    show_legend: bool = True,
    figsize: tuple[float, float] = (14, 8),
) -> Figure:
    """Create a timeline overview with percent imbalance below it."""
    tr = time_range or ds.time_range_us
    fig, (timeline_ax, imbalance_ax) = plt.subplots(
        2,
        1,
        figsize=figsize,
        sharex=True,
        gridspec_kw={"height_ratios": [3, 1]},
    )
    _draw_timeline_overview(
        timeline_ax,
        ds,
        bin_width_us,
        pes,
        tr,
        chare_order=None,
        chares=chares,
        top_n_chares=top_n_chares,
        show_legend=show_legend,
    )

    df = percent_imbalance_by_bin(ds, bin_width_us, pes=pes, time_range=tr).collect()
    x = (df["bin_start"].to_numpy() - tr[0]) / 1e6
    imbalance_ax.plot(x, df["percent_imbalance"].to_numpy(), color="#4e79a7", linewidth=1.8)
    imbalance_ax.set_xlabel("Time (s)")
    imbalance_ax.set_ylabel("Imbalance (%)")
    imbalance_ax.set_ylim(bottom=0)
    fig.tight_layout()
    return fig


def timeline_with_paper_imbalance(
    ds: TraceDataset,
    bin_width_us: int = 100_000,
    *,
    total_time_points: int = 100,
    time_points_us: Sequence[int | float] | None = None,
    total_nodes: int = 4,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    chares: Sequence[str] | None = None,
    top_n_chares: int | None = None,
    show_legend: bool = True,
    max_imbalance: float | None = None,
    figsize: tuple[float, float] = (14, 6),
) -> Figure:
    """Create a timeline overview overlaid with paper-compatible imbalance."""
    tr = time_range or ds.time_range_us
    fig, timeline_ax = plt.subplots(figsize=figsize)
    _draw_timeline_overview(
        timeline_ax,
        ds,
        bin_width_us,
        pes,
        tr,
        chare_order=None,
        chares=chares,
        top_n_chares=top_n_chares,
        show_legend=show_legend,
    )

    df = cumulative_percent_imbalance_by_time(
        ds,
        total_time_points=total_time_points,
        time_points_us=time_points_us,
        total_nodes=total_nodes,
        pes=pes,
        time_range=time_range,
    ).collect()
    x = (df["time_point_us"].to_numpy() - tr[0]) / 1e6
    imbalance_ax = timeline_ax.twinx()
    imbalance_ax.plot(x, df["percent_imbalance"].to_numpy(), color="purple", linewidth=1.8)
    imbalance_ax.set_ylabel("Percent Imbalance")
    imbalance_ax.set_ylim(0, max_imbalance if max_imbalance is not None else None)
    timeline_ax.set_xlabel("Time (s)")
    fig.tight_layout()
    return fig
