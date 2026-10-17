"""Execution policy for every query the package runs.

Every ``collect`` in the package goes through :func:`collect`, so the whole
package runs on the streaming engine regardless of ``POLARS_ENGINE_AFFINITY``
or any ``pl.Config`` the caller has set. The in-memory engine materialises the
full filtered scan before aggregating, which on a multi-gigabyte trace is the
difference between a few hundred MiB and an OOM kill.

The streaming engine's working set is roughly ``threads x prefetched row
groups x projected row-group bytes`` plus the aggregate being built. The
thread pool is fixed when polars is imported, so ``POLARS_MAX_THREADS`` is the
caller's lever; :func:`bounded_scan` is the package's.
"""

from __future__ import annotations

import os
from contextlib import contextmanager
from typing import Iterator

import polars as pl

# Polars re-reads this variable for every query, so it can be set around a
# single collect. Two row groups in flight per scan measured 633 MiB against
# 941 MiB for the default on a 32-thread, 88.5M-row aggregation; one was no
# better than two.
_PREFETCH_VAR = "POLARS_ROW_GROUP_PREFETCH_SIZE"
_PREFETCH_ROW_GROUPS = "2"


@contextmanager
def bounded_scan() -> Iterator[None]:
    """Limit Parquet row-group prefetch for the duration of the block.

    A value the caller already put in the environment wins and is left
    untouched. Otherwise the variable is set and removed again on exit. This
    mutates ``os.environ``, which is process-global: a query started on
    another thread while the block is open sees the same limit.
    """
    if _PREFETCH_VAR in os.environ:
        yield
        return
    os.environ[_PREFETCH_VAR] = _PREFETCH_ROW_GROUPS
    try:
        yield
    finally:
        os.environ.pop(_PREFETCH_VAR, None)


def collect(lf: pl.LazyFrame) -> pl.DataFrame:
    """Collect on the streaming engine with bounded prefetch."""
    with bounded_scan():
        return lf.collect(engine="streaming")
