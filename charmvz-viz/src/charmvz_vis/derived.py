"""Derived table computations that bridge entity tables to visualization needs.

The C++ pipeline writes normalized entity tables (Execution, Message, etc.).
Many visualizations expect a denormalized ``entry_spans`` table with EP names
joined in, durations pre-computed, etc.  This module produces those derived
tables as Polars LazyFrames so that predicate pushdown still applies.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, Sequence

import polars as pl

from .filters import apply_filters

if TYPE_CHECKING:
    from .dataset import TraceDataset


def compute_entry_spans(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Build the ``entry_spans`` derived table.

    This is the most-used derived table.  It joins Execution with EntryMethod
    (for EP and chare names) and applies PE / time-range filters.

    Returns
    -------
    LazyFrame with columns:
        pe_id, event, ep_id, ep_name, collection_id, chare_name,
        start_time_us, end_time_us, wall_duration_us,
        cpu_duration_us, queue_wait_us,
        src_pe, msg_len, recv_time_us, instance_id,
        papi_delta_0..5
    """
    exec_lf = apply_filters(
        ds.execution,
        pes=pes,
        time_range=time_range,
        pe_col="pe_id",
        start_col="start_time_us",
        end_col="end_time_us",
    )

    # Join with entry_method for EP name and chare info
    em_lf = ds.entry_method.select(
        "ep_id",
        pl.col("name").alias("ep_name"),
        "collection_id",
    )

    # Join with chare_collection for chare name
    cc_lf = ds.chare_collection.select(
        "collection_id",
        pl.col("name").alias("chare_name"),
    )

    spans = (
        exec_lf
        .join(em_lf, on="ep_id", how="left")
        .join(cc_lf, on="collection_id", how="left")
    )

    return spans


def compute_idle_spans(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Filtered idle interval spans."""
    return apply_filters(
        ds.idle_interval,
        pes=pes,
        time_range=time_range,
        pe_col="pe_id",
        start_col="start_time_us",
        end_col="end_time_us",
    )


def compute_message_spans(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Filtered message spans with target EP name joined.

    Parameters
    ----------
    pes : filter on **source** PE (``src_pe`` column).
    time_range : filter on ``send_time_us``.
    """
    msg_lf = apply_filters(
        ds.message,
        pes=pes,
        time_range=time_range,
        pe_col="src_pe",
        start_col="send_time_us",
        end_col=None,  # point event — no end column
    )

    em_lf = ds.entry_method.select(
        "ep_id",
        pl.col("name").alias("target_ep_name"),
    )

    return msg_lf.join(em_lf, on="ep_id", how="left")


def bin_spans(
    spans: pl.LazyFrame,
    bin_width_us: int,
    time_range: tuple[int, int],
    start_col: str = "start_time_us",
    end_col: str = "end_time_us",
    duration_col: str = "wall_duration_us",
) -> pl.LazyFrame:
    """Assign spans to time bins, clipping at bin boundaries.

    For each span, computes the contribution (in µs) to every bin it overlaps.
    This is the core helper for Time Profile, Overview, and similar binned
    visualizations.

    Returns
    -------
    LazyFrame with all original columns plus:
        ``bin_start`` (int64): start of the bin
        ``bin_contribution_us`` (int64): µs of the span falling in this bin
    """
    t_start, t_end = time_range

    # Clip span boundaries to the global window
    result = spans.with_columns(
        pl.max_horizontal(pl.col(start_col), pl.lit(t_start)).alias("_clipped_start"),
        pl.min_horizontal(
            pl.col(end_col).fill_null(pl.lit(t_end)),
            pl.lit(t_end),
        ).alias("_clipped_end"),
    )

    # Compute which bins the span touches
    result = result.with_columns(
        (
            (pl.col("_clipped_start") - t_start) // bin_width_us
        ).alias("_first_bin_idx"),
        (
            (pl.col("_clipped_end") - t_start - 1).clip(lower_bound=0) // bin_width_us
        ).alias("_last_bin_idx"),
    )

    # For spans that fit within a single bin, emit directly
    single_bin = result.filter(pl.col("_first_bin_idx") == pl.col("_last_bin_idx"))
    single_bin = single_bin.with_columns(
        (pl.col("_first_bin_idx") * bin_width_us + t_start).alias("bin_start"),
        (pl.col("_clipped_end") - pl.col("_clipped_start")).alias("bin_contribution_us"),
    )

    # For multi-bin spans, explode into per-bin rows
    multi_bin = result.filter(pl.col("_first_bin_idx") < pl.col("_last_bin_idx"))

    # Generate bin indices via int_ranges
    multi_bin = multi_bin.with_columns(
        pl.int_ranges(pl.col("_first_bin_idx"), pl.col("_last_bin_idx") + 1).alias(
            "_bin_indices"
        ),
    ).explode("_bin_indices")

    multi_bin = multi_bin.with_columns(
        (pl.col("_bin_indices") * bin_width_us + t_start).alias("bin_start"),
    )

    # Compute per-bin contribution: clip span to bin boundaries
    multi_bin = multi_bin.with_columns(
        (
            pl.min_horizontal(
                pl.col("_clipped_end"),
                pl.col("bin_start") + bin_width_us,
            )
            - pl.max_horizontal(
                pl.col("_clipped_start"),
                pl.col("bin_start"),
            )
        )
        .clip(lower_bound=0)
        .alias("bin_contribution_us"),
    )

    # Union and drop temporaries
    combined = pl.concat([single_bin, multi_bin], how="diagonal_relaxed")
    drop_cols = [c for c in combined.collect_schema().names() if c.startswith("_")]
    return combined.drop(drop_cols)


def _clipped_entry_spans(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Entry spans with a ``clipped_wall_duration_us`` column."""
    tr = time_range or ds.time_range_us
    return compute_entry_spans(ds, pes=pes, time_range=tr).with_columns(
        (
            pl.min_horizontal(pl.col("end_time_us").fill_null(tr[1]), pl.lit(tr[1]))
            - pl.max_horizontal(pl.col("start_time_us"), pl.lit(tr[0]))
        )
        .clip(lower_bound=0)
        .alias("clipped_wall_duration_us")
    )


def chare_duration_totals(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Total clipped execution time grouped by chare collection name."""
    return (
        _clipped_entry_spans(ds, pes=pes, time_range=time_range)
        .group_by("chare_name")
        .agg(pl.col("clipped_wall_duration_us").sum().alias("total_duration_us"))
        .sort("total_duration_us", descending=True)
    )


def chare_frequency_counts(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Execution count grouped by chare collection name."""
    return (
        compute_entry_spans(ds, pes=pes, time_range=time_range)
        .group_by("chare_name")
        .agg(pl.len().alias("execution_count"))
        .sort("execution_count", descending=True)
    )


def chare_activity_matrix(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    metric: Literal["duration", "frequency"] = "duration",
) -> pl.LazyFrame:
    """Per-(PE, chare) activity using total duration or execution count."""
    spans = _clipped_entry_spans(ds, pes=pes, time_range=time_range)
    if metric == "duration":
        value_expr = pl.col("clipped_wall_duration_us").sum().alias("activity")
    elif metric == "frequency":
        value_expr = pl.len().alias("activity")
    else:
        raise ValueError("metric must be 'duration' or 'frequency'")

    return (
        spans.group_by("pe_id", "chare_name")
        .agg(value_expr)
        .sort("pe_id", "chare_name")
    )


def pe_load_by_bin(
    ds: TraceDataset,
    bin_width_us: int,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Busy time per selected PE and time bin, including zero-load rows."""
    if bin_width_us <= 0:
        raise ValueError("bin_width_us must be positive")

    tr = time_range or ds.time_range_us
    t_start, t_end = tr
    pe_values = (
        ds.processing_element.select("pe_id")
        if pes is None
        else pl.LazyFrame({"pe_id": list(pes)})
    )
    n_bins = max(1, (t_end - t_start + bin_width_us - 1) // bin_width_us)
    bins = pl.LazyFrame(
        {"bin_start": [t_start + i * bin_width_us for i in range(n_bins)]},
    )

    spans = compute_entry_spans(ds, pes=pes, time_range=tr)
    loads = (
        bin_spans(spans, bin_width_us, tr)
        .group_by("pe_id", "bin_start")
        .agg(pl.col("bin_contribution_us").sum().alias("load_us"))
    )

    return (
        pe_values.join(bins, how="cross")
        .join(loads, on=["pe_id", "bin_start"], how="left")
        .with_columns(pl.col("load_us").fill_null(0))
        .sort("pe_id", "bin_start")
    )


def percent_imbalance_by_bin(
    ds: TraceDataset,
    bin_width_us: int,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Percent load imbalance per time bin.

    Imbalance is ``((max(load) / mean(load)) - 1) * 100``.  Bins where mean
    load is zero report 0% imbalance.
    """
    return (
        pe_load_by_bin(ds, bin_width_us, pes=pes, time_range=time_range)
        .group_by("bin_start")
        .agg(
            pl.col("load_us").max().alias("max_load_us"),
            pl.col("load_us").mean().alias("mean_load_us"),
        )
        .with_columns(
            pl.when(pl.col("mean_load_us") > 0)
            .then(((pl.col("max_load_us") / pl.col("mean_load_us")) - 1) * 100)
            .otherwise(0.0)
            .alias("percent_imbalance")
        )
        .sort("bin_start")
    )


def cumulative_percent_imbalance_by_time(
    ds: TraceDataset,
    *,
    total_time_points: int = 100,
    time_points_us: Sequence[int | float] | None = None,
    total_nodes: int = 4,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Paper-compatible cumulative percent imbalance over sampled time points.

    This mirrors the original notebook calculation used for the paper plots:
    for each sampled time point, include every execution whose start time is
    earlier than that point, group load by ``pe_id % total_nodes``, and compute
    ``((max(load) / mean(load)) - 1) * 100``.  Durations are not clipped at the
    sampled time point.
    """
    if total_nodes <= 0:
        raise ValueError("total_nodes must be positive")
    if total_time_points < 2 and time_points_us is None:
        raise ValueError("total_time_points must be at least 2")

    spans = compute_entry_spans(ds, pes=pes, time_range=time_range).select(
        "pe_id",
        "start_time_us",
        "wall_duration_us",
    )

    if time_points_us is None:
        bounds = spans.select(
            pl.col("start_time_us").min().alias("min_start_us"),
            pl.col("start_time_us").max().alias("max_start_us"),
        ).collect()
        min_start = bounds.item(0, "min_start_us")
        max_start = bounds.item(0, "max_start_us")
        if min_start is None or max_start is None:
            points: list[float] = []
        elif min_start == max_start:
            points = [float(max_start)]
        else:
            step = (max_start - min_start) / (total_time_points - 1)
            points = [min_start + i * step for i in range(1, total_time_points)]
    else:
        points = [float(point) for point in time_points_us]

    if not points:
        return pl.LazyFrame(
            schema={
                "time_point_us": pl.Float64,
                "max_load_us": pl.Int64,
                "mean_load_us": pl.Float64,
                "percent_imbalance": pl.Float64,
            }
        )

    time_points = pl.LazyFrame({"time_point_us": points})
    node_loads = (
        spans.with_columns((pl.col("pe_id") % total_nodes).alias("node"))
        .join(time_points, how="cross")
        .filter(pl.col("start_time_us") < pl.col("time_point_us"))
        .group_by("time_point_us", "node")
        .agg(pl.col("wall_duration_us").sum().alias("load_us"))
    )

    return (
        time_points.join(
            node_loads.group_by("time_point_us").agg(
                pl.col("load_us").max().alias("max_load_us"),
                pl.col("load_us").mean().alias("mean_load_us"),
            ),
            on="time_point_us",
            how="left",
        )
        .with_columns(
            pl.when(pl.col("mean_load_us") > 0)
            .then(((pl.col("max_load_us") / pl.col("mean_load_us")) - 1) * 100)
            .otherwise(0.0)
            .alias("percent_imbalance")
        )
        .with_columns(
            pl.col("max_load_us").fill_null(0),
            pl.col("mean_load_us").fill_null(0.0),
        )
        .sort("time_point_us")
    )


def chare_load_by_bin(
    ds: TraceDataset,
    bin_width_us: int,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Busy time per ``(PE, bin, chare)`` for timeline overview plots."""
    if bin_width_us <= 0:
        raise ValueError("bin_width_us must be positive")
    tr = time_range or ds.time_range_us
    spans = compute_entry_spans(ds, pes=pes, time_range=tr)
    return (
        bin_spans(spans, bin_width_us, tr)
        .group_by("pe_id", "bin_start", "chare_name")
        .agg(pl.col("bin_contribution_us").sum().alias("load_us"))
        .sort("pe_id", "bin_start", "load_us", descending=[False, False, True])
    )
