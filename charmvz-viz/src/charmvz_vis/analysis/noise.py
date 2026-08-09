"""Noise Miner — detection of recurring computational noise.

Implements analysis 17 of the Projections catalog: find repeated interruptions
to execution caused by OS jitter, system daemons or hardware, and report them
as clusters of similar duration recurring across PEs.

Two signals feed the miner, both named by the catalog:

``execution``
    An entry method invocation whose duration exceeds the median for its own
    entry method. The excess over that median, not the duration itself, is the
    noise estimate — a long entry method is not noise, a normally short one
    that occasionally runs long is.

``idle_gap``
    Wall-clock time on a PE covered by neither an execution nor a recorded idle
    interval. The runtime accounts for both, so a gap between them is time the
    PE spent outside Charm++ entirely.

A cluster is a set of occurrences of similar noise duration. Clusters are built
by histogramming durations at ``bin_width_us``, merging neighbouring bins whose
durations fall within ``merge_threshold`` of the cluster's own lowest duration,
and discarding clusters that appear on fewer than ``min_pes`` PEs or account for
less than ``importance_cutoff`` of total runtime.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import polars as pl

from ..derived import compute_entry_spans, compute_idle_spans

if TYPE_CHECKING:
    from ..dataset import TraceDataset

# Occurrence-level columns, used to give an empty result the same shape as a
# populated one so callers never have to special-case "no noise found".
_OCCURRENCE_SCHEMA: dict[str, pl.DataType] = {
    "cluster_id": pl.Int64,
    "kind": pl.String,
    "pe_id": pl.Int32,
    "time_us": pl.Int64,
    "noise_us": pl.Int64,
}

_SUMMARY_SCHEMA: dict[str, pl.DataType] = {
    "cluster_id": pl.Int64,
    "kind": pl.String,
    "noise_us_median": pl.Float64,
    "noise_us_min": pl.Int64,
    "noise_us_max": pl.Int64,
    "occurrences": pl.UInt32,
    "pe_count": pl.UInt32,
    "total_noise_us": pl.Int64,
    "runtime_fraction": pl.Float64,
    "period_us": pl.Float64,
    "period_cv": pl.Float64,
    "likely_source": pl.String,
}


def _execution_candidates(
    ds: TraceDataset,
    pes: Sequence[int] | None,
    time_range: tuple[int, int],
    bin_width_us: int,
) -> pl.LazyFrame:
    """Executions running longer than the median for their own entry method."""
    spans = compute_entry_spans(ds, pes=pes, time_range=time_range).select(
        "pe_id",
        "ep_id",
        "start_time_us",
        "wall_duration_us",
    )

    # The median is taken per EP and across all PEs: noise is what makes one
    # invocation differ from its own siblings, so the comparison has to be
    # against the same entry method rather than against the trace at large.
    medians = spans.group_by("ep_id").agg(
        pl.col("wall_duration_us").median().alias("_median_us"),
    )

    return (
        spans.join(medians, on="ep_id", how="left")
        .with_columns(
            (pl.col("wall_duration_us") - pl.col("_median_us")).alias("_noise_us"),
        )
        .filter(pl.col("_noise_us") >= bin_width_us)
        .select(
            pl.lit("execution", dtype=pl.String).alias("kind"),
            pl.col("pe_id"),
            pl.col("start_time_us").alias("time_us"),
            pl.col("_noise_us").cast(pl.Int64).alias("noise_us"),
        )
    )


def _idle_gap_candidates(
    ds: TraceDataset,
    pes: Sequence[int] | None,
    time_range: tuple[int, int],
    bin_width_us: int,
) -> pl.LazyFrame:
    """Wall-clock time accounted for by neither an execution nor an idle interval.

    Executions and idle intervals are merged into one set of occupied spans per
    PE; whatever falls between two consecutive merged spans is unaccounted. The
    merge is done with a running maximum of the end time rather than by pairing
    spans, because executions and idle intervals can overlap in a trace and a
    naive gap computation would report the overlap as negative time.
    """
    execs = compute_entry_spans(ds, pes=pes, time_range=time_range).select(
        "pe_id",
        "start_time_us",
        "end_time_us",
    )
    idles = compute_idle_spans(ds, pes=pes, time_range=time_range).select(
        "pe_id",
        "start_time_us",
        "end_time_us",
    )

    occupied = pl.concat([execs, idles], how="vertical").with_columns(
        # An unterminated span covers only its own start, which keeps a null end
        # from swallowing the running maximum.
        pl.col("end_time_us").fill_null(pl.col("start_time_us")),
    )

    return (
        occupied.sort("pe_id", "start_time_us")
        .with_columns(
            pl.col("end_time_us")
            .cum_max()
            .shift(1)
            .over("pe_id")
            .alias("_prev_end_us"),
        )
        .with_columns(
            (pl.col("start_time_us") - pl.col("_prev_end_us")).alias("_noise_us"),
        )
        .filter(pl.col("_noise_us") >= bin_width_us)
        .select(
            pl.lit("idle_gap", dtype=pl.String).alias("kind"),
            pl.col("pe_id"),
            pl.col("_prev_end_us").alias("time_us"),
            pl.col("_noise_us").cast(pl.Int64).alias("noise_us"),
        )
    )


def _assign_clusters(
    histogram: pl.DataFrame,
    bin_width_us: int,
    merge_threshold: float,
) -> pl.DataFrame:
    """Map each populated histogram bin to a cluster id.

    Bins are walked in increasing duration within each kind. A bin joins the
    open cluster when its duration is within ``merge_threshold`` of that
    cluster's *lowest* duration, not of the previous bin's — comparing against
    the previous bin would let a dense histogram chain every bin into a single
    cluster spanning the whole duration range.
    """
    kinds: list[str] = []
    bin_indices: list[int] = []
    cluster_ids: list[int] = []

    next_cluster_id = 0
    # Kinds are walked in a fixed order, not in the order ``group_by`` happened
    # to emit them: cluster ids are part of the public result and have to mean
    # the same thing on two runs over the same trace.
    for kind in sorted(histogram["kind"].unique().to_list()):
        rows = histogram.filter(pl.col("kind") == kind).sort("bin_idx")
        anchor_us: float | None = None
        current_id = -1

        for bin_idx in rows["bin_idx"].to_list():
            # Bin midpoint, the representative duration of everything in it.
            duration_us = (bin_idx + 0.5) * bin_width_us
            if anchor_us is None or duration_us > anchor_us * (1.0 + merge_threshold):
                current_id = next_cluster_id
                next_cluster_id += 1
                anchor_us = duration_us

            kinds.append(kind)
            bin_indices.append(bin_idx)
            cluster_ids.append(current_id)

    return pl.DataFrame(
        {
            "kind": kinds,
            "bin_idx": bin_indices,
            "cluster_id": cluster_ids,
        },
        schema={"kind": pl.String, "bin_idx": pl.Int64, "cluster_id": pl.Int64},
    )


def _classify(
    kind: str,
    median_us: float,
    period_cv: float | None,
    quantum_us: int | None,
    merge_threshold: float,
) -> str:
    """Label a cluster with a likely source.

    This is a heuristic and is advisory only: the trace records that a PE lost
    time, never what took it. A caller chasing a specific source should treat
    the label as a starting point and confirm it against the machine.
    """
    if quantum_us is not None and quantum_us > 0:
        multiple = round(median_us / quantum_us)
        if multiple >= 1 and abs(median_us - multiple * quantum_us) <= (
            merge_threshold * quantum_us
        ):
            return f"scheduler preemption (~{multiple}x quantum)"

    if period_cv is not None and period_cv < 0.25:
        return "periodic daemon"
    if median_us < 1_000:
        return "sub-millisecond jitter"
    if median_us < 100_000:
        return "system interference"
    return (
        "long interruption"
        if kind == "idle_gap"
        else "long interruption during execution"
    )


def noise_occurrences(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    bin_width_us: int = 100,
    merge_threshold: float = 0.1,
    importance_cutoff: float = 0.01,
    min_pes: int = 2,
    quantum_us: int | None = None,
) -> pl.DataFrame:
    """Individual noise events, each tagged with the cluster it belongs to.

    Same parameters as :func:`noise_miner`, which this backs. Useful when the
    temporal distribution of the events matters rather than their summary; the
    Noise Miner plot is built on it.

    Returns
    -------
    DataFrame with columns:
        cluster_id, kind, pe_id, time_us, noise_us
    """
    return _detect(
        ds,
        pes=pes,
        time_range=time_range,
        bin_width_us=bin_width_us,
        merge_threshold=merge_threshold,
        importance_cutoff=importance_cutoff,
        min_pes=min_pes,
        quantum_us=quantum_us,
    )[0]


def noise_miner(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    bin_width_us: int = 100,
    merge_threshold: float = 0.1,
    importance_cutoff: float = 0.01,
    min_pes: int = 2,
    quantum_us: int | None = None,
) -> pl.DataFrame:
    """Detect clusters of recurring computational noise.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE filter
    time_range : time range filter, defaulting to the whole run
    bin_width_us : histogram bin width for grouping durations
    merge_threshold : fractional duration difference within which neighbouring
        bins merge into one cluster
    importance_cutoff : minimum share of total runtime a cluster must account
        for to be reported
    min_pes : minimum number of distinct PEs a cluster must appear on. The
        default of 2 implements the catalog's "peaks that appear repeatedly
        across multiple PEs"; set it to 1 to include PE-local anomalies
    quantum_us : OS scheduling quantum, if known. Clusters at a near-integer
        multiple of it are labelled as scheduler preemption

    Returns
    -------
    DataFrame, one row per cluster, ordered by total noise time descending:
        cluster_id, kind, noise_us_median, noise_us_min, noise_us_max,
        occurrences, pe_count, total_noise_us, runtime_fraction,
        period_us, period_cv, likely_source

    ``period_us`` is the median interval between consecutive occurrences on the
    same PE, and ``period_cv`` the coefficient of variation of those intervals:
    a low value means the interruption recurs on a regular beat. Both are null
    for a cluster whose occurrences never repeat on any one PE.
    """
    return _detect(
        ds,
        pes=pes,
        time_range=time_range,
        bin_width_us=bin_width_us,
        merge_threshold=merge_threshold,
        importance_cutoff=importance_cutoff,
        min_pes=min_pes,
        quantum_us=quantum_us,
    )[1]


def _detect(
    ds: TraceDataset,
    pes: Sequence[int] | None,
    time_range: tuple[int, int] | None,
    bin_width_us: int,
    merge_threshold: float,
    importance_cutoff: float,
    min_pes: int,
    quantum_us: int | None,
) -> tuple[pl.DataFrame, pl.DataFrame]:
    """Run the miner, returning ``(occurrences, summary)``."""
    if bin_width_us <= 0:
        raise ValueError("bin_width_us must be positive")
    if merge_threshold < 0:
        raise ValueError("merge_threshold must be non-negative")
    if not 0.0 <= importance_cutoff <= 1.0:
        raise ValueError("importance_cutoff must be between 0 and 1")
    if min_pes < 1:
        raise ValueError("min_pes must be at least 1")

    tr = time_range or ds.time_range_us
    window_us = tr[1] - tr[0]
    if window_us <= 0:
        raise ValueError(f"Invalid time range: {tr}")

    n_pes = len(pes) if pes is not None else ds.num_pes
    total_runtime_us = window_us * max(1, n_pes)

    candidates = (
        pl.concat(
            [
                _execution_candidates(ds, pes, tr, bin_width_us),
                _idle_gap_candidates(ds, pes, tr, bin_width_us),
            ],
            how="vertical",
        )
        .with_columns((pl.col("noise_us") // bin_width_us).alias("bin_idx"))
        .collect()
    )

    if candidates.is_empty():
        return (
            pl.DataFrame(schema=_OCCURRENCE_SCHEMA),
            pl.DataFrame(schema=_SUMMARY_SCHEMA),
        )

    histogram = candidates.group_by("kind", "bin_idx").agg(pl.len().alias("count"))
    mapping = _assign_clusters(histogram, bin_width_us, merge_threshold)
    assigned = candidates.join(mapping, on=["kind", "bin_idx"], how="left")

    stats = assigned.group_by("cluster_id", "kind").agg(
        pl.col("noise_us").median().alias("noise_us_median"),
        pl.col("noise_us").min().alias("noise_us_min"),
        pl.col("noise_us").max().alias("noise_us_max"),
        pl.len().alias("occurrences"),
        pl.col("pe_id").n_unique().alias("pe_count"),
        pl.col("noise_us").sum().alias("total_noise_us"),
    )

    # Periodicity is measured per PE and then pooled: intervals between
    # occurrences on *different* PEs say nothing about how often the source
    # fires on any one of them.
    intervals = (
        assigned.sort("cluster_id", "pe_id", "time_us")
        .with_columns(
            pl.col("time_us").diff().over("cluster_id", "pe_id").alias("_interval_us"),
        )
        .filter(pl.col("_interval_us").is_not_null())
    )
    periods = intervals.group_by("cluster_id").agg(
        pl.col("_interval_us").median().cast(pl.Float64).alias("period_us"),
        (pl.col("_interval_us").std() / pl.col("_interval_us").mean())
        .cast(pl.Float64)
        .alias("period_cv"),
    )

    summary = (
        stats.join(periods, on="cluster_id", how="left")
        .with_columns(
            (pl.col("total_noise_us") / total_runtime_us).alias("runtime_fraction"),
        )
        .filter(
            (pl.col("pe_count") >= min_pes)
            & (pl.col("runtime_fraction") >= importance_cutoff),
        )
        .sort("total_noise_us", descending=True)
    )

    if summary.is_empty():
        return (
            pl.DataFrame(schema=_OCCURRENCE_SCHEMA),
            pl.DataFrame(schema=_SUMMARY_SCHEMA),
        )

    labels = [
        _classify(
            row["kind"],
            float(row["noise_us_median"]),
            row["period_cv"],
            quantum_us,
            merge_threshold,
        )
        for row in summary.iter_rows(named=True)
    ]
    summary = summary.with_columns(
        pl.Series("likely_source", labels, dtype=pl.String),
    ).select(list(_SUMMARY_SCHEMA))

    survivors = summary["cluster_id"].to_list()
    occurrences = (
        assigned.filter(pl.col("cluster_id").is_in(survivors))
        .select(list(_OCCURRENCE_SCHEMA))
        .sort("cluster_id", "time_us")
    )

    return occurrences, summary
