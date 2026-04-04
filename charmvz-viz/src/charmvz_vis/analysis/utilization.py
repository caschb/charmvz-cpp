"""Per-PE utilization and idle fraction computation.

These are the core derived metrics used by multiple visualizations (Overview,
Time Profile, Extrema Analysis, etc.).
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import polars as pl

from ..derived import bin_spans, compute_entry_spans, compute_idle_spans

if TYPE_CHECKING:
    from ..dataset import TraceDataset


def per_pe_utilization(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.DataFrame:
    """Compute per-PE utilization metrics over a time window.

    Returns
    -------
    DataFrame with columns:
        pe_id, busy_time_us, idle_time_us, overhead_us,
        utilization (0–1), idle_fraction (0–1), window_us
    """
    tr = time_range or ds.time_range_us
    window_us = tr[1] - tr[0]
    if window_us <= 0:
        raise ValueError(f"Invalid time range: {tr}")

    # Busy time: sum of execution durations clipped to window
    spans = compute_entry_spans(ds, pes=pes, time_range=tr)
    busy = (
        spans.with_columns(
            (
                pl.min_horizontal(pl.col("end_time_us").fill_null(tr[1]), pl.lit(tr[1]))
                - pl.max_horizontal(pl.col("start_time_us"), pl.lit(tr[0]))
            )
            .clip(lower_bound=0)
            .alias("clipped_duration"),
        )
        .group_by("pe_id")
        .agg(pl.col("clipped_duration").sum().alias("busy_time_us"))
    )

    # Idle time: sum of idle durations clipped to window
    idles = compute_idle_spans(ds, pes=pes, time_range=tr)
    idle_agg = (
        idles.with_columns(
            (
                pl.min_horizontal(pl.col("end_time_us").fill_null(tr[1]), pl.lit(tr[1]))
                - pl.max_horizontal(pl.col("start_time_us"), pl.lit(tr[0]))
            )
            .clip(lower_bound=0)
            .alias("clipped_duration"),
        )
        .group_by("pe_id")
        .agg(pl.col("clipped_duration").sum().alias("idle_time_us"))
    )

    # All PE IDs
    if pes is not None:
        all_pes = pl.DataFrame({"pe_id": list(pes)}).lazy()
    else:
        all_pes = ds.processing_element.select("pe_id")

    result = (
        all_pes.join(busy, on="pe_id", how="left")
        .join(idle_agg, on="pe_id", how="left")
        .with_columns(
            pl.col("busy_time_us").fill_null(0),
            pl.col("idle_time_us").fill_null(0),
            pl.lit(window_us).alias("window_us"),
        )
        .with_columns(
            (pl.col("window_us") - pl.col("busy_time_us") - pl.col("idle_time_us")).alias(
                "overhead_us"
            ),
            (pl.col("busy_time_us") / pl.col("window_us")).alias("utilization"),
            (pl.col("idle_time_us") / pl.col("window_us")).alias("idle_fraction"),
        )
        .sort("pe_id")
    )

    return result.collect()


def per_interval_utilization(
    ds: TraceDataset,
    bin_width_us: int,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.DataFrame:
    """Compute per-(PE, time-bin) utilization for Overview / Time Profile.

    Returns
    -------
    DataFrame with columns:
        pe_id, bin_start, busy_us, utilization (0–1)
    """
    tr = time_range or ds.time_range_us
    spans = compute_entry_spans(ds, pes=pes, time_range=tr)
    binned = bin_spans(spans, bin_width_us, tr)

    result = (
        binned.group_by("pe_id", "bin_start")
        .agg(pl.col("bin_contribution_us").sum().alias("busy_us"))
        .with_columns(
            (pl.col("busy_us") / pl.lit(bin_width_us)).clip(upper_bound=1.0).alias("utilization"),
        )
        .sort("pe_id", "bin_start")
    )

    return result.collect()
