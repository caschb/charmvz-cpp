"""Tests for derived tables and analysis utilities."""

from __future__ import annotations

import numpy as np
import polars as pl
import pytest

from charmvz_vis import TraceDataset
from charmvz_vis.analysis import load_imbalance_score, per_pe_utilization
from charmvz_vis.derived import (
    _histogram,
    bin_loads,
    chare_duration_totals,
    chare_frequency_counts,
    cumulative_percent_imbalance_by_time,
    duration_histogram,
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


class TestBinLoads:
    def test_splits_spans_across_bins(self) -> None:
        spans = pl.LazyFrame(
            {
                "pe_id": [0, 0, 1, 1, 1],
                "start_time_us": [50, 120, 300, 450, -30],
                "end_time_us": [320, 200, 300, None, 40],
            },
            schema={"pe_id": pl.Int32, "start_time_us": pl.Int64, "end_time_us": pl.Int64},
        )
        loads = bin_loads(spans, 100, (0, 500), ["pe_id"]).sort("pe_id", "bin_start")
        # 50..320 crosses three boundaries; 120..200 ends on one and stays in
        # bin 1; the zero-length span at 300 falls in no bin; the open span
        # runs to the window end; -30..40 is clipped at the window start.
        assert loads.rows() == [
            (0, 0, 50),
            (0, 100, 180),
            (0, 200, 100),
            (0, 300, 20),
            (1, 0, 40),
            (1, 400, 50),
        ]


class TestDurationHistogram:
    @pytest.mark.parametrize("log_scale", [False, True])
    @pytest.mark.parametrize("n_bins", [5, 13, 50])
    def test_matches_numpy(self, ds: TraceDataset, n_bins: int, log_scale: bool) -> None:
        durations_ms = ds.execution.collect()["wall_duration_us"].to_numpy() / 1000.0
        if log_scale:
            # logspace rounds the first edge above the 300 ms minimum, so numpy
            # leaves both 300 ms executions out.
            bins = np.logspace(np.log10(durations_ms.min()), np.log10(durations_ms.max()), n_bins)
        else:
            bins = n_bins
        expected_counts, expected_edges = np.histogram(durations_ms, bins=bins)

        edges, counts = duration_histogram(ds, n_bins, log_scale)
        np.testing.assert_array_equal(edges, expected_edges)
        np.testing.assert_array_equal(counts, expected_counts)

    @pytest.mark.parametrize(("log_scale", "n_bins"), [(False, 5), (True, 13)])
    def test_values_on_and_just_below_edges(self, log_scale: bool, n_bins: int) -> None:
        # Over [0.3, 0.8] these edges and their predecessors include values
        # whose arithmetic (or log-space) bin index is one off from numpy's in
        # each direction, so only the correction step gets them right.
        if log_scale:
            edges = np.logspace(np.log10(0.3), np.log10(0.8), n_bins)
        else:
            edges = np.linspace(0.3, 0.8, n_bins + 1)
        inner = edges[1:-1]
        values = np.concatenate([[0.3, 0.8], inner, np.nextafter(inner, -np.inf)])
        expected_counts, expected_edges = np.histogram(
            values, bins=edges if log_scale else n_bins
        )

        edges, counts = _histogram(pl.LazyFrame({"x": values}), n_bins, log_scale)
        np.testing.assert_array_equal(edges, expected_edges)
        np.testing.assert_array_equal(counts, expected_counts)

    @pytest.mark.parametrize(
        ("values", "n_bins"),
        [
            # logspace repeats edges over a range this narrow, which the
            # log-space estimate cannot place.
            ([1e11, 1e11 + 1e-3], 50),
            # One edge, so no bins at all.
            ([1.0, 5.0, 9.0], 1),
        ],
    )
    def test_degenerate_log_edges_match_numpy(self, values: list[float], n_bins: int) -> None:
        bins = np.logspace(np.log10(min(values)), np.log10(max(values)), n_bins)
        expected_counts, expected_edges = np.histogram(values, bins=bins)

        edges, counts = _histogram(pl.LazyFrame({"x": values}), n_bins, log_scale=True)
        np.testing.assert_array_equal(edges, expected_edges)
        np.testing.assert_array_equal(counts, expected_counts)


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

    def test_cumulative_percent_imbalance_by_time_uses_paper_semantics(
        self,
        ds: TraceDataset,
    ) -> None:
        imbalance = cumulative_percent_imbalance_by_time(
            ds,
            time_points_us=[250_000, 1_000_000, 2_000_000],
            total_nodes=4,
        ).collect()
        values = imbalance["percent_imbalance"].to_list()
        # PE 3 has not started by 250 ms: the mean is over the three nodes
        # that have, (300 + 500 + 450) / 3 ms, not over all four.
        assert values[0] == pytest.approx(20.0)
        assert values[1] == pytest.approx(10.0)
        assert values[2] == pytest.approx(21.2121212)

    def test_equally_spaced_points_match_explicit_points(self, ds: TraceDataset) -> None:
        # The default points are bucketed arithmetically, explicit ones by
        # search. The last default point equals the latest start, which must
        # not count toward it.
        default = cumulative_percent_imbalance_by_time(ds, total_time_points=5).collect()
        explicit = cumulative_percent_imbalance_by_time(
            ds, time_points_us=default["time_point_us"].to_list()
        ).collect()
        assert default.height == 4
        assert default.equals(explicit)

    def test_explicit_points_count_pes_missing_from_metadata(self, tiny_trace) -> None:
        # Explicit points are bucketed one PE at a time; PE 3 has to be found
        # from its executions to give the same values as the test above.
        pe_path = tiny_trace / "processing_element.parquet"
        pl.read_parquet(pe_path).filter(pl.col("pe_id") != 3).write_parquet(pe_path)
        imbalance = cumulative_percent_imbalance_by_time(
            TraceDataset(tiny_trace),
            time_points_us=[250_000, 1_000_000, 2_000_000],
        ).collect()
        assert imbalance["percent_imbalance"].to_list() == pytest.approx(
            [20.0, 10.0, 21.2121212]
        )


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
