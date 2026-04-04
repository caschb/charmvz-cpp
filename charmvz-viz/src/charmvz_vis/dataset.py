"""TraceDataset — the central entry point for loading CharmVZ Parquet data.

Usage::

    import charmvz_vis as cv
    ds = cv.TraceDataset("./output/")
    print(ds.num_pes, ds.time_range_us)
"""

from __future__ import annotations

from pathlib import Path
from typing import Sequence

import polars as pl

from . import derived
from .colors import EPColorMap


class TraceDataset:
    """Load and cache a CharmVZ Parquet trace dataset.

    Parameters
    ----------
    trace_dir : str or Path
        Directory containing the 8 Parquet files produced by the ``charmvz``
        C++ pipeline.

    Examples
    --------
    >>> ds = TraceDataset("./output/")
    >>> ds.execution.head().collect()
    """

    # Expected filenames — must match the C++ pipeline output.
    _FILES = {
        "processing_element": "processing_element.parquet",
        "chare_collection": "chare_collection.parquet",
        "entry_method": "entry_method.parquet",
        "chare_instance": "chare_instance.parquet",
        "execution": "execution.parquet",
        "message": "message.parquet",
        "idle_interval": "idle_interval.parquet",
        "migration_episode": "migration_episode.parquet",
    }

    def __init__(self, trace_dir: str | Path) -> None:
        self.trace_dir = Path(trace_dir)
        if not self.trace_dir.is_dir():
            raise FileNotFoundError(f"Trace directory not found: {self.trace_dir}")

        # Validate that required files exist
        missing = []
        for name, filename in self._FILES.items():
            path = self.trace_dir / filename
            if not path.exists():
                missing.append(filename)
        if missing:
            raise FileNotFoundError(
                f"Missing Parquet files in {self.trace_dir}: {', '.join(missing)}"
            )

        # Lazy caches (populated on first access)
        self._tables: dict[str, pl.LazyFrame] = {}
        self._pe_info: dict[str, int | tuple[int, int]] | None = None
        self._ep_color_map: EPColorMap | None = None

    def _scan(self, name: str) -> pl.LazyFrame:
        """Lazy-scan a Parquet file, caching the LazyFrame."""
        if name not in self._tables:
            path = self.trace_dir / self._FILES[name]
            self._tables[name] = pl.scan_parquet(path)
        return self._tables[name]

    # ── Entity table accessors ───────────────────────────────────────────

    @property
    def processing_element(self) -> pl.LazyFrame:
        """ProcessingElement table (one row per PE)."""
        return self._scan("processing_element")

    @property
    def chare_collection(self) -> pl.LazyFrame:
        """ChareCollection table (one row per chare type)."""
        return self._scan("chare_collection")

    @property
    def entry_method(self) -> pl.LazyFrame:
        """EntryMethod table (one row per entry point)."""
        return self._scan("entry_method")

    @property
    def chare_instance(self) -> pl.LazyFrame:
        """ChareInstance table (one row per live chare object)."""
        return self._scan("chare_instance")

    @property
    def execution(self) -> pl.LazyFrame:
        """Execution table (one row per entry method invocation)."""
        return self._scan("execution")

    @property
    def message(self) -> pl.LazyFrame:
        """Message table (one row per CREATION → BEGIN_PROCESSING link)."""
        return self._scan("message")

    @property
    def idle_interval(self) -> pl.LazyFrame:
        """IdleInterval table (one row per idle episode)."""
        return self._scan("idle_interval")

    @property
    def migration_episode(self) -> pl.LazyFrame:
        """MigrationEpisode table (one row per pack/unpack cycle)."""
        return self._scan("migration_episode")

    # ── Convenience metadata ─────────────────────────────────────────────

    def _load_pe_info(self) -> None:
        """Eagerly load small PE metadata."""
        if self._pe_info is not None:
            return
        pe_df = self.processing_element.collect()
        num_pes = pe_df["total_pes"][0]
        global_start = pe_df["global_start_us"][0]
        # Time range: smallest aligned_begin_us to max(end_time_us - global_start_us)
        begin_vals = pe_df["aligned_begin_us"].drop_nulls()
        end_vals = pe_df["end_time_us"].drop_nulls()
        t_start = int(begin_vals.min()) if len(begin_vals) > 0 else 0
        t_end = int(end_vals.max() - global_start) if len(end_vals) > 0 and global_start is not None else 0
        self._pe_info = {
            "num_pes": int(num_pes),
            "global_start_us": int(global_start) if global_start is not None else 0,
            "time_range": (t_start, t_end),
        }

    @property
    def num_pes(self) -> int:
        """Total number of processing elements."""
        self._load_pe_info()
        return self._pe_info["num_pes"]

    @property
    def global_start_us(self) -> int:
        """Global start time in microseconds (from .projrc)."""
        self._load_pe_info()
        return self._pe_info["global_start_us"]

    @property
    def time_range_us(self) -> tuple[int, int]:
        """(start, end) time range of the trace in aligned microseconds."""
        self._load_pe_info()
        return self._pe_info["time_range"]

    # ── Color map ────────────────────────────────────────────────────────

    @property
    def ep_color_map(self) -> EPColorMap:
        """Consistent EP → color mapping for this dataset."""
        if self._ep_color_map is None:
            ep_ids = (
                self.entry_method.select("ep_id").collect()["ep_id"].to_list()
            )
            self._ep_color_map = EPColorMap(ep_ids)
        return self._ep_color_map

    # ── Derived tables ───────────────────────────────────────────────────

    def entry_spans(
        self,
        pes: Sequence[int] | None = None,
        time_range: tuple[int, int] | None = None,
    ) -> pl.LazyFrame:
        """Compute entry_spans derived table (execution + EP name join).

        Parameters
        ----------
        pes : list of PE IDs or None (all PEs)
        time_range : (start_us, end_us) or None (full trace)

        Returns
        -------
        LazyFrame with columns: pe_id, ep_id, ep_name, start_time_us,
        end_time_us, wall_duration_us, src_pe, msg_len, recv_time_us, event,
        instance_id, collection_id, chare_name, and PAPI delta columns.
        """
        return derived.compute_entry_spans(self, pes=pes, time_range=time_range)

    def idle_spans(
        self,
        pes: Sequence[int] | None = None,
        time_range: tuple[int, int] | None = None,
    ) -> pl.LazyFrame:
        """Filtered idle interval spans."""
        return derived.compute_idle_spans(self, pes=pes, time_range=time_range)

    def message_spans(
        self,
        pes: Sequence[int] | None = None,
        time_range: tuple[int, int] | None = None,
    ) -> pl.LazyFrame:
        """Filtered message spans with EP name join."""
        return derived.compute_message_spans(self, pes=pes, time_range=time_range)

    def __repr__(self) -> str:
        self._load_pe_info()
        t0, t1 = self.time_range_us
        duration_s = (t1 - t0) / 1e6
        return (
            f"TraceDataset({self.trace_dir.name!r}, "
            f"{self.num_pes} PEs, {duration_s:.3f}s)"
        )
