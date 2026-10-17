"""Derived table computations that bridge entity tables to visualization needs.

The C++ pipeline writes normalized entity tables (Execution, Message, etc.).
Many visualizations expect a denormalized ``entry_spans`` table with EP names
joined in, durations pre-computed, etc.  This module produces those derived
tables as Polars LazyFrames so that predicate pushdown still applies.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, Sequence, TypeVar

import numpy as np
import polars as pl

from .engine import collect
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


def execution_rows(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    columns: Sequence[str] | None = None,
) -> pl.LazyFrame:
    """The filtered ``execution`` scan, without the name joins of entry_spans.

    Aggregate these rows on integer keys and call :func:`attach_names` on the
    result. Joined onto every execution, the name strings are carried through
    the whole scan and are what pushes a streaming aggregation over a large
    trace past a few GiB; joined onto the aggregate they cost nothing.
    ``ep_id`` is the entry_method key, so leaving the join out keeps the same
    rows.
    """
    lf = apply_filters(ds.execution, pes=pes, time_range=time_range)
    return lf if columns is None else lf.select(columns)


def distinct_pes(*frames: pl.LazyFrame) -> list[int]:
    """Sorted ``pe_id`` values occurring in any of ``frames``.

    Loops that hold one PE's rows at a time take their PEs from the rows
    themselves: a PE missing from ``processing_element`` still has its rows
    counted, as it would by a single query over the whole table.
    """
    found: set[int] = set()
    for frame in frames:
        found.update(collect(frame.select("pe_id").unique())["pe_id"].to_list())
    return sorted(found)


FrameT = TypeVar("FrameT", pl.DataFrame, pl.LazyFrame)


def attach_names(
    frame: FrameT,
    ds: TraceDataset,
    *,
    ep: bool = False,
    chare: bool = False,
) -> FrameT:
    """Join ``ep_name`` and/or ``chare_name`` onto a frame keyed by ``ep_id``.

    Row order is not preserved; sort afterwards where it matters.
    """
    em_cols = ["ep_id"]
    if ep:
        em_cols.append("ep_name")
    if chare:
        em_cols.append("collection_id")
    em_lf = ds.entry_method.select(
        "ep_id", pl.col("name").alias("ep_name"), "collection_id"
    ).select(em_cols)
    lf = frame.lazy().join(em_lf, on="ep_id", how="left")
    if chare:
        cc_lf = ds.chare_collection.select(
            "collection_id", pl.col("name").alias("chare_name")
        )
        lf = lf.join(cc_lf, on="collection_id", how="left").drop("collection_id")
    return collect(lf) if isinstance(frame, pl.DataFrame) else lf


def clipped_duration(time_range: tuple[int, int]) -> pl.Expr:
    """Span time inside ``time_range``; an open span runs to its end."""
    return (
        pl.min_horizontal(pl.col("end_time_us").fill_null(time_range[1]), pl.lit(time_range[1]))
        - pl.max_horizontal(pl.col("start_time_us"), pl.lit(time_range[0]))
    ).clip(lower_bound=0)


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


def _bin_bounds(
    spans: pl.LazyFrame,
    bin_width_us: int,
    time_range: tuple[int, int],
    start_col: str,
    end_col: str,
) -> pl.LazyFrame:
    """Add each span's window-clipped bounds and its first and last bin index.

    A span with ``_first_bin_idx > _last_bin_idx`` (zero length, on a bin
    boundary) falls in no bin.
    """
    t_start, t_end = time_range
    return spans.with_columns(
        pl.max_horizontal(pl.col(start_col), pl.lit(t_start)).alias("_clipped_start"),
        pl.min_horizontal(
            pl.col(end_col).fill_null(pl.lit(t_end)),
            pl.lit(t_end),
        ).alias("_clipped_end"),
    ).with_columns(
        (
            (pl.col("_clipped_start") - t_start) // bin_width_us
        ).alias("_first_bin_idx"),
        (
            (pl.col("_clipped_end") - t_start - 1).clip(lower_bound=0) // bin_width_us
        ).alias("_last_bin_idx"),
    )


def _bin_overlap(bin_width_us: int, t_start: int) -> pl.Expr:
    """Part of a multi-bin span inside the bin ``_bin_idx``."""
    bin_start = pl.col("_bin_idx") * bin_width_us + t_start
    return (
        pl.min_horizontal(pl.col("_clipped_end"), bin_start + bin_width_us)
        - pl.max_horizontal(pl.col("_clipped_start"), bin_start)
    ).clip(lower_bound=0)


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
    Use :func:`bin_loads` for sums per bin: this union scans the input twice
    at once, which a large trace cannot afford.

    Returns
    -------
    LazyFrame with all original columns plus:
        ``bin_start`` (int64): start of the bin
        ``bin_contribution_us`` (int64): µs of the span falling in this bin
    """
    t_start = time_range[0]
    result = _bin_bounds(spans, bin_width_us, time_range, start_col, end_col)

    single_bin = result.filter(
        pl.col("_first_bin_idx") == pl.col("_last_bin_idx")
    ).with_columns(
        (pl.col("_first_bin_idx") * bin_width_us + t_start).alias("bin_start"),
        (pl.col("_clipped_end") - pl.col("_clipped_start")).alias("bin_contribution_us"),
    )

    multi_bin = (
        result.filter(pl.col("_first_bin_idx") < pl.col("_last_bin_idx"))
        .with_columns(
            pl.int_ranges(pl.col("_first_bin_idx"), pl.col("_last_bin_idx") + 1).alias(
                "_bin_idx"
            ),
        )
        .explode("_bin_idx")
        .with_columns(
            (pl.col("_bin_idx") * bin_width_us + t_start).alias("bin_start"),
            _bin_overlap(bin_width_us, t_start).alias("bin_contribution_us"),
        )
    )

    combined = pl.concat([single_bin, multi_bin], how="diagonal_relaxed")
    drop_cols = [c for c in combined.collect_schema().names() if c.startswith("_")]
    return combined.drop(drop_cols)


def bin_loads(
    spans: pl.LazyFrame,
    bin_width_us: int,
    time_range: tuple[int, int],
    keys: Sequence[str],
    start_col: str = "start_time_us",
    end_col: str = "end_time_us",
) -> pl.DataFrame:
    """Busy time per ``keys`` and time bin, with :func:`bin_spans`' clipping.

    Equal to grouping ``bin_spans`` on ``keys`` and ``bin_start`` and summing
    ``bin_contribution_us``, but in two sequential streaming passes rather
    than one union of two concurrent scans. The first sums the spans that lie
    in a single bin; the second splits the spans that cross a bin boundary,
    of which there are at most ``total busy time / bin_width_us`` (2,941 of
    88.5M in a 200-bin ChaNGa run). Very small bin widths make the second
    pass large.

    Returns
    -------
    DataFrame with columns ``*keys``, ``bin_start``, ``load_us``.
    """
    t_start = time_range[0]
    bounded = _bin_bounds(
        spans.select(*keys, start_col, end_col), bin_width_us, time_range, start_col, end_col
    )
    first, last = pl.col("_first_bin_idx"), pl.col("_last_bin_idx")

    single = collect(
        bounded.filter(first == last)
        .group_by(*keys, first.alias("_bin_idx"))
        .agg((pl.col("_clipped_end") - pl.col("_clipped_start")).sum().alias("load_us"))
    )
    multi = collect(
        bounded.filter(first < last)
        .with_columns(pl.int_ranges(first, last + 1).alias("_bin_idx"))
        .explode("_bin_idx")
        .group_by(*keys, "_bin_idx")
        .agg(_bin_overlap(bin_width_us, t_start).sum().alias("load_us"))
    )

    return (
        pl.concat([single, multi], how="vertical_relaxed")
        .group_by(*keys, "_bin_idx")
        .agg(pl.col("load_us").sum())
        .select(
            *keys,
            (pl.col("_bin_idx") * bin_width_us + t_start).alias("bin_start"),
            "load_us",
        )
    )


def _by_chare(ep_totals: pl.LazyFrame, ds: TraceDataset, keys: Sequence[str]) -> pl.LazyFrame:
    """Re-aggregate per-EP sums of every non-key column by chare name.

    Grouping on the name rather than ``collection_id`` keeps two collections
    that share a name merged, as they always were.
    """
    values = [c for c in ep_totals.collect_schema().names() if c not in (*keys, "ep_id")]
    return (
        attach_names(ep_totals, ds, chare=True)
        .group_by(*keys, "chare_name")
        .agg(pl.col(values).sum())
    )


def chare_duration_totals(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Total clipped execution time grouped by chare collection name."""
    tr = time_range or ds.time_range_us
    per_ep = (
        execution_rows(ds, pes=pes, time_range=tr)
        .group_by("ep_id")
        .agg(clipped_duration(tr).sum().alias("total_duration_us"))
    )
    return _by_chare(per_ep, ds, []).sort("total_duration_us", descending=True)


def chare_frequency_counts(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> pl.LazyFrame:
    """Execution count grouped by chare collection name."""
    per_ep = (
        execution_rows(ds, pes=pes, time_range=time_range)
        .group_by("ep_id")
        .agg(pl.len().alias("execution_count"))
    )
    return _by_chare(per_ep, ds, []).sort("execution_count", descending=True)


def chare_activity_matrix(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    metric: Literal["duration", "frequency"] = "duration",
) -> pl.LazyFrame:
    """Per-(PE, chare) activity using total duration or execution count."""
    tr = time_range or ds.time_range_us
    if metric == "duration":
        value_expr = clipped_duration(tr).sum().alias("activity")
    elif metric == "frequency":
        value_expr = pl.len().alias("activity")
    else:
        raise ValueError("metric must be 'duration' or 'frequency'")

    per_ep = (
        execution_rows(ds, pes=pes, time_range=tr)
        .group_by("pe_id", "ep_id")
        .agg(value_expr)
    )
    return _by_chare(per_ep, ds, ["pe_id"]).sort("pe_id", "chare_name")


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

    loads = bin_loads(
        execution_rows(ds, pes=pes, time_range=tr), bin_width_us, tr, ["pe_id"]
    ).lazy()

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


def _buckets_equally_spaced(
    spans: pl.LazyFrame, points: list[float], first: float, step: float, total_nodes: int
) -> pl.DataFrame | None:
    """Load and row count per ``(node, k)``, ``k`` = number of points <= start.

    ``points`` are ``first + i * step`` for ``i = 1..len(points)``, so ``k`` is
    estimated arithmetically and corrected by one step against the points
    themselves. A join against a table of ``len(points) + 1`` rows is all the
    streaming engine has to hold.

    Returns ``None`` when some start is still outside its bucket after the
    correction. That happens when rounding makes sampled points coincide (start
    times so large that the step is a few ULPs), which shifts the indices of
    the deduplicated points away from the arithmetic estimate.
    """
    n_points = len(points)
    # Row k holds points k - 1 .. k + 2 (1-based, infinite past either end):
    # the bounds of bucket k, and those of its neighbours to check a
    # one-step correction without joining again.
    inf = float("inf")
    padded = [-inf, -inf, *points, inf, inf]
    lookup = pl.LazyFrame(
        {
            "_k": list(range(n_points + 1)),
            "_below2": padded[0 : n_points + 1],
            "_below": padded[1 : n_points + 2],
            "_at": padded[2 : n_points + 3],
            "_at2": padded[3 : n_points + 4],
        },
        schema={
            "_k": pl.Int64,
            "_below2": pl.Float64,
            "_below": pl.Float64,
            "_at": pl.Float64,
            "_at2": pl.Float64,
        },
    )
    s = pl.col("start_time_us").cast(pl.Float64)
    if step > 0:
        estimate = ((s - first) / step).floor().cast(pl.Int64).clip(0, n_points)
    else:
        estimate = pl.lit(0, dtype=pl.Int64)
    early, late = s < pl.col("_below"), s >= pl.col("_at")
    outside = (early & (s < pl.col("_below2"))) | (late & (s >= pl.col("_at2")))
    buckets = collect(
        spans.select("pe_id", "start_time_us", "wall_duration_us", estimate.alias("_k"))
        .join(lookup, on="_k", how="left")
        .group_by(
            (pl.col("pe_id") % total_nodes).alias("node"),
            pl.col("_k") - early.cast(pl.Int64) + late.cast(pl.Int64),
        )
        .agg(
            pl.col("wall_duration_us").sum().alias("load_us"),
            pl.len().alias("rows"),
            outside.sum().alias("_outside"),
        )
    )
    if buckets["_outside"].sum() > 0:
        return None
    return buckets.drop("_outside")


def _buckets_per_pe(
    spans: pl.LazyFrame, points: list[float], total_nodes: int
) -> pl.DataFrame:
    """:func:`_buckets_equally_spaced` for arbitrary sorted ``points``.

    Bucketing by search is not streaming-safe, so the starts are searched one
    PE at a time; the peak is the busiest PE's executions.
    """
    sorted_points = np.asarray(points, dtype=np.float64)
    parts = []
    for pe in distinct_pes(spans):
        rows = collect(spans.filter(pl.col("pe_id") == pe))
        k = np.searchsorted(
            sorted_points, rows["start_time_us"].to_numpy().astype(np.float64), side="right"
        )
        parts.append(
            rows.with_columns(pl.Series("_k", k, dtype=pl.Int64))
            .group_by((pl.col("pe_id") % total_nodes).alias("node"), "_k")
            .agg(pl.col("wall_duration_us").sum().alias("load_us"), pl.len().alias("rows"))
        )
    if not parts:
        return pl.DataFrame(
            schema={"node": pl.Int32, "_k": pl.Int64, "load_us": pl.Int64, "rows": pl.UInt32}
        )
    return (
        pl.concat(parts)
        .group_by("node", "_k")
        .agg(pl.col("load_us").sum(), pl.col("rows").sum())
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
    sampled time point. A node with no execution before a point is left out of
    that point's max and mean rather than counted as zero load.

    Each execution is counted once, in the bucket between the two points that
    enclose its start, and the per-point loads are cumulative sums over the
    buckets. For the default equally spaced points the buckets are computed in
    one streaming pass; explicit ``time_points_us`` (and default points that
    rounding made coincide) are bucketed one PE at a time, which holds the
    busiest PE's executions in memory.
    """
    if total_nodes <= 0:
        raise ValueError("total_nodes must be positive")
    if total_time_points < 2 and time_points_us is None:
        raise ValueError("total_time_points must be at least 2")

    spans = execution_rows(
        ds, pes=pes, time_range=time_range,
        columns=["pe_id", "start_time_us", "wall_duration_us"],
    ).filter(pl.col("start_time_us").is_not_null())

    if time_points_us is None:
        bounds = collect(
            spans.select(
                pl.col("start_time_us").min().alias("min_start_us"),
                pl.col("start_time_us").max().alias("max_start_us"),
            )
        )
        min_start = bounds.item(0, "min_start_us")
        max_start = bounds.item(0, "max_start_us")
        if min_start is None or max_start is None:
            points: list[float] = []
        elif min_start == max_start:
            points = [float(max_start)]
            step = 0.0
        else:
            step = (max_start - min_start) / (total_time_points - 1)
            points = [min_start + i * step for i in range(1, total_time_points)]
    else:
        points = [float(point) for point in time_points_us]

    schema = {
        "time_point_us": pl.Float64,
        "max_load_us": pl.Int64,
        "mean_load_us": pl.Float64,
        "percent_imbalance": pl.Float64,
    }
    if not points:
        return pl.LazyFrame(schema=schema)

    unique_points = sorted(set(points))
    n_points = len(unique_points)
    buckets = None
    if time_points_us is None:
        buckets = _buckets_equally_spaced(
            spans, unique_points, float(min_start), step, total_nodes
        )
    if buckets is None:
        buckets = _buckets_per_pe(spans, unique_points, total_nodes)

    # Point j (0-based) covers the starts in buckets 0..j; bucket n_points
    # holds starts at or after the last point and never counts.
    grid = buckets.select("node").unique().join(
        pl.DataFrame({"_k": list(range(n_points))}, schema={"_k": pl.Int64}), how="cross"
    )
    cumulative = (
        grid.join(buckets, on=["node", "_k"], how="left", nulls_equal=True)
        .with_columns(pl.col("load_us").fill_null(0), pl.col("rows").fill_null(0))
        .sort("node", "_k", nulls_last=True)
        .with_columns(
            pl.col("load_us").cum_sum().over("node").alias("load_us"),
            pl.col("rows").cum_sum().over("node").alias("rows"),
        )
    )
    per_point = (
        cumulative.filter(pl.col("rows") > 0)
        .group_by("_k")
        .agg(
            pl.col("load_us").max().alias("max_load_us"),
            pl.col("load_us").mean().alias("mean_load_us"),
        )
    )
    table = (
        pl.DataFrame({"time_point_us": unique_points, "_k": list(range(n_points))},
                     schema={"time_point_us": pl.Float64, "_k": pl.Int64})
        .join(per_point, on="_k", how="left")
        .drop("_k")
    )

    return (
        pl.LazyFrame({"time_point_us": points}, schema={"time_point_us": pl.Float64})
        .join(table.lazy(), on="time_point_us", how="left")
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
        .select(list(schema))
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
    per_ep = bin_loads(
        execution_rows(ds, pes=pes, time_range=tr), bin_width_us, tr, ["pe_id", "ep_id"]
    ).lazy()
    return _by_chare(per_ep, ds, ["pe_id", "bin_start"]).sort(
        "pe_id", "bin_start", "load_us", descending=[False, False, True]
    )


def _histogram(
    values: pl.LazyFrame, n_bins: int, log_scale: bool
) -> tuple[np.ndarray, np.ndarray]:
    """``np.histogram`` of the column ``x`` without materialising the column.

    Uniform bins follow numpy's own algorithm step for step: edges from
    ``np.linspace`` over ``[min, max]``, an arithmetic bin index, and one
    correction against the edges for rounding within an ULP of an edge, so the
    counts are numpy's exactly. Log bins take their edges from ``np.logspace``
    as the plots always have; numpy then bins by search, which is reproduced
    by estimating the index in log space, correcting it by one step, and
    checking every value landed inside its bin, else counting by search.

    Returns ``(edges, counts)``, both empty when no value qualifies. A log
    histogram with ``n_bins`` below 2 has no bins.
    """
    x = pl.col("x")
    if log_scale:
        values = values.filter(x > 0)
    lo, hi = collect(values.select(x.min(), x.max().alias("hi"))).row(0)
    if lo is None:
        return np.empty(0), np.empty(0, dtype=np.int64)

    if log_scale:
        edges = np.logspace(np.log10(lo), np.log10(hi), n_bins)
        n = len(edges) - 1
        if n < 1:
            return edges, np.empty(0, dtype=np.int64)
        # Rounding in logspace can put the outer edges inside [min, max], and
        # numpy drops values outside the edges.
        values = values.filter((x >= edges[0]) & (x <= edges[-1]))
        log_lo, log_hi = np.log10(edges[0]), np.log10(edges[-1])
        if log_hi > log_lo:
            estimate = ((x.log10() - log_lo) / (log_hi - log_lo) * n).floor()
        else:
            estimate = pl.lit(n - 1)
        estimate = estimate.cast(pl.Int64).clip(0, n - 1)
    else:
        if lo == hi:
            lo, hi = lo - 0.5, hi + 0.5
        edges = np.linspace(lo, hi, n_bins + 1)
        n = n_bins
        estimate = ((x - lo) / (hi - lo) * n).cast(pl.Int64)
        estimate = pl.when(estimate == n).then(n - 1).otherwise(estimate)

    bounds = pl.LazyFrame(
        {"_k": np.arange(n, dtype=np.int64), "_lo": edges[:-1], "_hi": edges[1:]}
    )
    k = pl.col("_k")
    below = x < pl.col("_lo")
    above = ~below & (x >= pl.col("_hi")) & (k != n - 1)
    binned = (
        values.select("x", estimate.alias("_k"))
        .join(bounds, on="_k", how="left")
        .select("x", (k - below.cast(pl.Int64) + above.cast(pl.Int64)).alias("_k"))
    )
    if log_scale:
        last_closed = (k == n - 1) & (x <= pl.col("_hi"))
        inside = (x >= pl.col("_lo")) & ((x < pl.col("_hi")) | last_closed)
        binned = binned.join(bounds, on="_k", how="left").with_columns(
            (~inside).alias("_outside")
        )
        agg = [pl.len().alias("count"), pl.col("_outside").sum()]
    else:
        agg = [pl.len().alias("count")]

    per_bin = collect(binned.group_by("_k").agg(agg))
    if log_scale and per_bin["_outside"].sum() > 0:
        return edges, _counts_by_search(values, edges)
    counts = np.zeros(n, dtype=np.int64)
    counts[per_bin["_k"].to_numpy()] = per_bin["count"].to_numpy()
    return edges, counts


def _counts_by_search(values: pl.LazyFrame, edges: np.ndarray) -> np.ndarray:
    """np.histogram's counts for explicit ``edges``, by its own definition.

    numpy counts the values below each edge (at or below the last one) and
    takes differences. Here that is one streaming pass with a sum per edge,
    exact for any edges but ``len(edges)`` comparisons per value, so it is
    only the fallback for edges the log-space estimate cannot resolve: a range
    narrow enough that ``logspace`` repeats edges.
    """
    x = pl.col("x")
    below = [(x < edge).sum().alias(f"_{i}") for i, edge in enumerate(edges[:-1])]
    below.append((x <= edges[-1]).sum().alias(f"_{len(edges) - 1}"))
    return np.diff(np.asarray(collect(values.select(below)).row(0), dtype=np.int64))


def duration_histogram(
    ds: TraceDataset,
    n_bins: int = 50,
    log_scale: bool = False,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    ep_ids: Sequence[int] | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Histogram ``(edges, counts)`` of execution wall durations in ms.

    ``n_bins`` is the number of bins, or with ``log_scale`` the number of
    log-spaced edges (one bin fewer). With ``log_scale`` only positive
    durations count.
    """
    lf = execution_rows(ds, pes=pes, time_range=time_range or ds.time_range_us)
    if ep_ids is not None:
        lf = lf.filter(pl.col("ep_id").is_in(list(ep_ids)))
    values = lf.select((pl.col("wall_duration_us") / 1000.0).alias("x")).drop_nulls()
    return _histogram(values, n_bins, log_scale)


def message_size_histogram_counts(
    ds: TraceDataset,
    n_bins: int = 50,
    log_scale: bool = True,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Histogram ``(edges, counts)`` of message sizes in bytes.

    Filters on the source PE and the send time, and bins like
    :func:`duration_histogram`.
    """
    lf = apply_filters(
        ds.message,
        pes=pes,
        time_range=time_range or ds.time_range_us,
        pe_col="src_pe",
        start_col="send_time_us",
        end_col=None,
    )
    values = lf.select(pl.col("msg_len").cast(pl.Float64).alias("x")).drop_nulls()
    return _histogram(values, n_bins, log_scale)
