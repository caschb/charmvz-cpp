"""User Stats — application-declared statistics over time and across PEs.

Analyses 14 and 15 of the Projections catalog. Both read the ``user_stat``
table, which is empty unless the traced application called
``traceRegisterUserStat()`` and then ``updateStat()`` / ``updateStatPair()``.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, Sequence

import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..colors import _BASE_PALETTE

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset

Aggregation = Literal["mean", "max", "min"]

_AGGREGATIONS = {
    "mean": pl.col("stat_value").mean(),
    "max": pl.col("stat_value").max(),
    "min": pl.col("stat_value").min(),
}


def _require_user_stat(ds: TraceDataset) -> pl.LazyFrame:
    """The user_stat table, or a clear error naming what is missing.

    An absent table and an empty one mean different things: the first is a
    trace converted before the entity existed, the second an application that
    declares no statistics. Both are reported plainly instead of surfacing as
    an empty plot the caller has to explain.
    """
    lf = ds.user_stat
    if lf is None:
        raise ValueError(
            "This trace has no user_stat.parquet. It was converted by a "
            "pipeline older than the UserStat entity; re-run charmvz over the "
            "original logs."
        )
    return lf


def _stat_label(stat_id: int, name: str | None) -> str:
    return name if name else f"stat {stat_id}"


def _filtered(
    ds: TraceDataset,
    pes: Sequence[int] | None,
    time_range: tuple[int, int] | None,
    stats: Sequence[int] | None,
) -> pl.DataFrame:
    lf = _require_user_stat(ds)
    if pes is not None:
        lf = lf.filter(pl.col("pe_id").is_in(list(pes)))
    if time_range is not None:
        lf = lf.filter(
            (pl.col("time_us") >= time_range[0]) & (pl.col("time_us") <= time_range[1])
        )
    if stats is not None:
        lf = lf.filter(pl.col("stat_id").is_in(list(stats)))
    return lf.collect()


def _empty_figure(figsize: tuple[float, float], title: str, ylabel: str) -> Figure:
    fig, ax = plt.subplots(figsize=figsize)
    ax.text(
        0.5,
        0.5,
        "No user statistics in this trace",
        ha="center",
        va="center",
        transform=ax.transAxes,
        fontsize=11,
        color="#6b6b6b",
    )
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    fig.tight_layout()
    return fig


def user_stats_over_time(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    stats: Sequence[int] | None = None,
    secondary_stats: Sequence[int] | None = None,
    aggregation: Aggregation = "mean",
    use_user_time: bool = False,
    figsize: tuple[float, float] = (12, 5),
) -> Figure:
    """Multi-series line chart of each statistic's value over the run.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE filter
    time_range : filter on ``time_us``
    stats : stat ids to plot; all of them by default
    secondary_stats : stat ids to put on a right-hand axis. Statistics
        registered by one application routinely differ by orders of magnitude,
        and a series plotted against another's scale is a flat line at zero
    aggregation : how to combine PEs reporting at the same instant
    use_user_time : plot against the application's own ``user_time_s`` instead
        of the trace clock. That column is in the application's units, so the
        x axis stops being seconds of run time
    figsize : figure size

    Returns
    -------
    matplotlib Figure.
    """
    if aggregation not in _AGGREGATIONS:
        raise ValueError(f"aggregation must be one of {sorted(_AGGREGATIONS)}")

    secondary = set(secondary_stats or ())
    if stats is not None and not secondary <= set(stats):
        raise ValueError("secondary_stats must be a subset of stats")

    df = _filtered(ds, pes, time_range, stats)
    title = "User Stats Over Time"
    if df.is_empty():
        return _empty_figure(figsize, title, "Value")

    x_col = "user_time_s" if use_user_time else "time_us"
    if use_user_time:
        df = df.filter(pl.col("user_time_s").is_not_null())
        if df.is_empty():
            raise ValueError(
                "use_user_time was requested but no sample carries a "
                "user_time_s: the application called updateStat() rather than "
                "updateStatPair()."
            )

    grouped = (
        df.group_by("stat_id", "name", x_col)
        .agg(_AGGREGATIONS[aggregation].alias("value"))
        .sort("stat_id", x_col)
    )

    fig, ax = plt.subplots(figsize=figsize)
    ax_right = ax.twinx() if secondary else None
    handles = []

    for i, ((stat_id, name), series) in enumerate(
        sorted(grouped.group_by("stat_id", "name"), key=lambda item: item[0][0])
    ):
        x = series[x_col].to_numpy()
        if not use_user_time:
            x = x / 1e6
        on_right = stat_id in secondary
        target = ax_right if on_right else ax
        label = _stat_label(stat_id, name) + (" (right)" if on_right else "")
        (line,) = target.plot(
            x,
            series["value"].to_numpy(),
            label=label,
            color=_BASE_PALETTE[i % len(_BASE_PALETTE)],
            linewidth=1.4,
            linestyle="--" if on_right else "-",
        )
        handles.append(line)

    ax.set_xlabel("Application time" if use_user_time else "Time (s)")
    ax.set_ylabel(f"Value ({aggregation} across PEs)")
    if ax_right is not None:
        ax_right.set_ylabel(f"Value, right axis ({aggregation} across PEs)")
    ax.set_title(title)
    ax.legend(handles=handles, fontsize=8, framealpha=0.9)
    fig.tight_layout()
    return fig


def user_stats_per_pe(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    stats: Sequence[int] | None = None,
    aggregation: Aggregation = "mean",
    figsize: tuple[float, float] = (12, 5),
) -> Figure:
    """Grouped bar chart comparing each statistic across PEs.

    Answers whether an application-level quantity varies between processors,
    which per-PE busy time cannot show: two PEs can be equally busy and be
    working on very different parts of the problem.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE filter
    time_range : filter on ``time_us``
    stats : stat ids to plot; all of them by default
    aggregation : how to reduce each PE's samples over the window
    figsize : figure size

    Returns
    -------
    matplotlib Figure.
    """
    if aggregation not in _AGGREGATIONS:
        raise ValueError(f"aggregation must be one of {sorted(_AGGREGATIONS)}")

    df = _filtered(ds, pes, time_range, stats)
    title = "User Stats Per PE"
    if df.is_empty():
        return _empty_figure(figsize, title, "Value")

    grouped = (
        df.group_by("stat_id", "name", "pe_id")
        .agg(_AGGREGATIONS[aggregation].alias("value"))
        .sort("stat_id", "pe_id")
    )

    pe_ids = sorted(grouped["pe_id"].unique().to_list())
    pe_index = {pe: i for i, pe in enumerate(pe_ids)}
    series = sorted(
        grouped.group_by("stat_id", "name"), key=lambda item: item[0][0]
    )

    fig, ax = plt.subplots(figsize=figsize)
    n_series = len(series)
    width = 0.8 / n_series
    x = np.arange(len(pe_ids))

    for i, ((stat_id, name), rows) in enumerate(series):
        values = np.full(len(pe_ids), np.nan)
        for pe, value in zip(
            rows["pe_id"].to_list(), rows["value"].to_list(), strict=True
        ):
            values[pe_index[pe]] = value
        ax.bar(
            x + (i - (n_series - 1) / 2) * width,
            values,
            width=width,
            label=_stat_label(stat_id, name),
            color=_BASE_PALETTE[i % len(_BASE_PALETTE)],
            edgecolor="white",
            linewidth=0.3,
        )

    ax.set_xlabel("PE")
    ax.set_ylabel(f"Value ({aggregation} over window)")
    ax.set_title(title)
    if len(pe_ids) <= 64:
        ax.set_xticks(x[:: max(1, len(pe_ids) // 16)])
        ax.set_xticklabels(
            [str(pe_ids[i]) for i in range(0, len(pe_ids), max(1, len(pe_ids) // 16))]
        )
    else:
        ax.set_xticks([])
    ax.legend(fontsize=8, framealpha=0.9)
    fig.tight_layout()
    return fig
