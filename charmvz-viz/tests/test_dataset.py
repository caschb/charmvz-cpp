"""Tests for TraceDataset loading and metadata."""

from __future__ import annotations

import pytest

from charmvz_vis import TraceDataset


class TestTraceDataset:
    """Test suite for TraceDataset."""

    def test_loads_successfully(self, ds: TraceDataset) -> None:
        assert ds.num_pes == 4

    def test_time_range(self, ds: TraceDataset) -> None:
        t0, t1 = ds.time_range_us
        assert t0 >= 0
        assert t1 > t0

    def test_repr(self, ds: TraceDataset) -> None:
        r = repr(ds)
        assert "4 PEs" in r

    def test_entity_tables_are_lazy(self, ds: TraceDataset) -> None:
        import polars as pl

        assert isinstance(ds.execution, pl.LazyFrame)
        assert isinstance(ds.message, pl.LazyFrame)
        assert isinstance(ds.idle_interval, pl.LazyFrame)

    @pytest.mark.parametrize(
        ("table", "expected_rows"),
        [
            ("execution", 12),
            ("message", 6),
            ("idle_interval", 8),
            ("entry_method", 3),
        ],
    )
    def test_table_row_counts(
        self, ds: TraceDataset, table: str, expected_rows: int
    ) -> None:
        assert getattr(ds, table).collect().height == expected_rows

    def test_missing_dir_raises(self, tmp_path) -> None:
        with pytest.raises(FileNotFoundError):
            TraceDataset(tmp_path / "nonexistent")

    def test_missing_files_raises(self, tmp_path) -> None:
        d = tmp_path / "empty_trace"
        d.mkdir()
        with pytest.raises(FileNotFoundError, match="Missing Parquet"):
            TraceDataset(d)

    def test_ep_color_map(self, ds: TraceDataset) -> None:
        cm = ds.ep_color_map
        # Should return a color for each EP
        assert cm[0].startswith("#")
        assert cm[1].startswith("#")
        assert cm[2].startswith("#")
