"""Tests for derived tables and analysis utilities."""

from __future__ import annotations

import pytest

from charmvz_vis import TraceDataset
from charmvz_vis.analysis import load_imbalance_score, per_pe_utilization
from charmvz_vis.derived import (
    chare_duration_totals,
    chare_frequency_counts,
    percent_imbalance_by_bin,
)


class TestEntrySpans:
    """Tests for the entry_spans derived table."""

    def test_has_ep_name(self, ds: TraceDataset) -> None:
        spans = ds.entry_spans().collect()
        assert "ep_name" in spans.columns
        # All rows should have a name
        assert spans["ep_name"].null_count() == 0

    def test_has_chare_name(self, ds: TraceDataset) -> None:
        spans = ds.entry_spans().collect()
        assert "chare_name" in spans.columns

    def test_pe_filter(self, ds: TraceDataset) -> None:
        spans = ds.entry_spans(pes=[0, 1]).collect()
        pe_vals = spans["pe_id"].unique().to_list()
        assert set(pe_vals) <= {0, 1}

    def test_time_filter(self, ds: TraceDataset) -> None:
        spans = ds.entry_spans(time_range=(0, 500_000)).collect()
        # All spans should overlap the specified window
        assert len(spans) > 0
        for row in spans.iter_rows(named=True):
            assert row["start_time_us"] < 500_000

    def test_row_count_matches_execution(self, ds: TraceDataset) -> None:
        spans = ds.entry_spans().collect()
        execs = ds.execution.collect()
        assert len(spans) == len(execs)


class TestIdleSpans:
    """Tests for idle_spans derived table."""

    def test_basic(self, ds: TraceDataset) -> None:
        idles = ds.idle_spans().collect()
        assert len(idles) == 8

    def test_pe_filter(self, ds: TraceDataset) -> None:
        idles = ds.idle_spans(pes=[0]).collect()
        assert all(pe == 0 for pe in idles["pe_id"].to_list())


class TestMessageSpans:
    """Tests for message_spans derived table."""

    def test_has_target_ep_name(self, ds: TraceDataset) -> None:
        msgs = ds.message_spans().collect()
        assert "target_ep_name" in msgs.columns

    def test_row_count(self, ds: TraceDataset) -> None:
        msgs = ds.message_spans().collect()
        assert len(msgs) == 6


class TestPaperDerivedMetrics:
    """Tests for derived metrics used by paper-equivalent plots."""

    def test_chare_duration_totals(self, ds: TraceDataset) -> None:
        totals = chare_duration_totals(ds).collect()
        by_name = dict(zip(totals["chare_name"].to_list(), totals["total_duration_us"].to_list()))
        assert by_name == {
            "Main": 300_000,
            "Worker": 5_650_000,
        }

    def test_chare_frequency_counts(self, ds: TraceDataset) -> None:
        counts = chare_frequency_counts(ds).collect()
        by_name = dict(zip(counts["chare_name"].to_list(), counts["execution_count"].to_list()))
        assert by_name == {
            "Main": 1,
            "Worker": 11,
        }

    def test_percent_imbalance_by_bin(self, ds: TraceDataset) -> None:
        imbalance = percent_imbalance_by_bin(
            ds,
            bin_width_us=1_000_000,
            time_range=(0, 2_000_000),
        ).collect()
        values = imbalance["percent_imbalance"].to_list()
        assert values[0] == pytest.approx(6.6666667)
        assert values[1] == pytest.approx(75.0)


class TestUtilization:
    """Tests for per-PE utilization analysis."""

    def test_returns_all_pes(self, ds: TraceDataset) -> None:
        util = per_pe_utilization(ds)
        assert len(util) == 4

    def test_utilization_bounds(self, ds: TraceDataset) -> None:
        util = per_pe_utilization(ds)
        for u in util["utilization"].to_list():
            assert 0.0 <= u <= 1.0

    def test_components_sum_to_window(self, ds: TraceDataset) -> None:
        util = per_pe_utilization(ds)
        for row in util.iter_rows(named=True):
            total = row["busy_time_us"] + row["idle_time_us"] + row["overhead_us"]
            assert abs(total - row["window_us"]) < 1  # within 1 µs rounding


class TestLoadImbalance:
    """Tests for load imbalance score."""

    def test_returns_float(self, ds: TraceDataset) -> None:
        score = load_imbalance_score(ds)
        assert isinstance(score, float)
        assert score >= 0.0
