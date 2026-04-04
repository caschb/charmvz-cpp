"""Standardized PE and time-range filtering for LazyFrames.

These helpers ensure every visualization applies filters in the same way,
using Polars predicate pushdown so that only the needed row groups are read
from Parquet.
"""

from __future__ import annotations

from typing import Sequence

import polars as pl


def apply_pe_filter(
    lf: pl.LazyFrame,
    pes: Sequence[int] | None,
    pe_col: str = "pe_id",
) -> pl.LazyFrame:
    """Filter a LazyFrame to the given PE set.

    Parameters
    ----------
    lf : LazyFrame
        The input frame.
    pes : sequence of ints or None
        PE IDs to include.  ``None`` means all PEs (no filter).
    pe_col : str
        Name of the PE column.

    Returns
    -------
    LazyFrame with PE filter applied (or unchanged if *pes* is None).
    """
    if pes is None:
        return lf
    return lf.filter(pl.col(pe_col).is_in(list(pes)))


def apply_time_filter(
    lf: pl.LazyFrame,
    time_range: tuple[int, int] | None,
    start_col: str = "start_time_us",
    end_col: str | None = "end_time_us",
) -> pl.LazyFrame:
    """Filter a LazyFrame to spans overlapping a time window.

    A span overlaps ``[t_start, t_end]`` when ``span.start < t_end`` AND
    ``span.end > t_start`` (or ``span.end`` is null — treat as open-ended).

    Parameters
    ----------
    lf : LazyFrame
        The input frame.
    time_range : (t_start, t_end) in µs, or None for full range.
    start_col : str
        Column with the span start timestamp.
    end_col : str or None
        Column with the span end timestamp.  If None, only ``start_col``
        is compared against the range (point events).

    Returns
    -------
    LazyFrame with time filter applied (or unchanged if *time_range* is None).
    """
    if time_range is None:
        return lf
    t_start, t_end = time_range
    cond = pl.col(start_col) < t_end
    if end_col is not None:
        cond = cond & (pl.col(end_col).is_null() | (pl.col(end_col) > t_start))
    else:
        cond = cond & (pl.col(start_col) >= t_start)
    return lf.filter(cond)


def apply_filters(
    lf: pl.LazyFrame,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    pe_col: str = "pe_id",
    start_col: str = "start_time_us",
    end_col: str | None = "end_time_us",
) -> pl.LazyFrame:
    """Apply both PE and time-range filters in one call."""
    lf = apply_pe_filter(lf, pes, pe_col=pe_col)
    lf = apply_time_filter(lf, time_range, start_col=start_col, end_col=end_col)
    return lf
