"""Tests for the Noise Miner.

Every assertion is against an exact number, because the ``noisy_ds`` fixture
plants noise of a known duration, period and extent. See ``conftest.py`` for
what is planted.
"""

from __future__ import annotations

import matplotlib
import matplotlib.pyplot as plt
import polars as pl
import pytest

from charmvz_vis import TraceDataset
from charmvz_vis.analysis import noise_miner, noise_occurrences
from charmvz_vis.analysis.noise import _assign_clusters

matplotlib.use("Agg")

# Planted signal, restated here so a fixture change that breaks these numbers
# fails loudly instead of silently weakening the tests.
STRETCH_EXCESS_US = 5_000
STRETCH_PERIOD_US = 25_000
STRETCH_PER_PE = 10
STRETCH_PES = 3
GAP_US = 10_000
GAP_PES = 3


def _cluster(summary: pl.DataFrame, kind: str) -> dict:
    """The single cluster of a given kind; fails if there is not exactly one."""
    rows = summary.filter(pl.col("kind") == kind)
    assert len(rows) == 1, f"expected exactly one {kind} cluster, got {len(rows)}"
    return rows.row(0, named=True)


class TestExecutionNoise:
    """Executions running long relative to their own entry method."""

    def test_finds_one_cluster(self, noisy_ds: TraceDataset) -> None:
        cluster = _cluster(noise_miner(noisy_ds), "execution")
        assert cluster["occurrences"] == STRETCH_PER_PE * STRETCH_PES
        assert cluster["pe_count"] == STRETCH_PES

    def test_reports_the_excess_not_the_duration(self, noisy_ds: TraceDataset) -> None:
        # The stretched executions run 6000 µs against a 1000 µs baseline. The
        # noise is the 5000 µs of excess; reporting 6000 would mean the miner is
        # measuring duration rather than anomaly.
        cluster = _cluster(noise_miner(noisy_ds), "execution")
        assert cluster["noise_us_median"] == pytest.approx(STRETCH_EXCESS_US)
        assert cluster["noise_us_min"] == STRETCH_EXCESS_US
        assert cluster["noise_us_max"] == STRETCH_EXCESS_US

    def test_total_and_fraction(self, noisy_ds: TraceDataset) -> None:
        cluster = _cluster(noise_miner(noisy_ds), "execution")
        expected_total = STRETCH_EXCESS_US * STRETCH_PER_PE * STRETCH_PES
        assert cluster["total_noise_us"] == expected_total
        # 4 PEs over a 260 ms window.
        assert cluster["runtime_fraction"] == pytest.approx(
            expected_total / (260_000 * 4)
        )

    def test_periodicity_is_recovered(self, noisy_ds: TraceDataset) -> None:
        cluster = _cluster(noise_miner(noisy_ds), "execution")
        assert cluster["period_us"] == pytest.approx(STRETCH_PERIOD_US)
        assert cluster["period_cv"] == pytest.approx(0.0)

    def test_regular_recurrence_is_labelled(self, noisy_ds: TraceDataset) -> None:
        cluster = _cluster(noise_miner(noisy_ds), "execution")
        assert cluster["likely_source"] == "periodic daemon"


class TestUnaccountedGaps:
    """Time covered by neither an execution nor an idle interval."""

    def test_finds_the_planted_gap(self, noisy_ds: TraceDataset) -> None:
        cluster = _cluster(noise_miner(noisy_ds), "idle_gap")
        assert cluster["occurrences"] == GAP_PES
        assert cluster["pe_count"] == GAP_PES
        assert cluster["noise_us_median"] == pytest.approx(GAP_US)

    def test_single_occurrence_per_pe_has_no_period(
        self, noisy_ds: TraceDataset
    ) -> None:
        # One occurrence per PE means no interval to measure, which must read as
        # "unknown" rather than as a period of zero.
        cluster = _cluster(noise_miner(noisy_ds), "idle_gap")
        assert cluster["period_us"] is None
        assert cluster["period_cv"] is None

    def test_recorded_idle_time_is_not_noise(
        self, idle_covered_ds: TraceDataset
    ) -> None:
        # Same trace, same gap, but the runtime recorded itself as idle across
        # it. Idle time is the runtime waiting for work, so the gap must stop
        # being reported while the execution stretches are untouched.
        summary = noise_miner(idle_covered_ds)
        assert summary.filter(pl.col("kind") == "idle_gap").is_empty()
        assert _cluster(summary, "execution")["occurrences"] == (
            STRETCH_PER_PE * STRETCH_PES
        )

    def test_contiguous_executions_produce_no_gaps(
        self, noisy_ds: TraceDataset
    ) -> None:
        # PE 0 has no planted gap and its executions tile the run, so restricting
        # to it must leave the idle_gap kind empty.
        summary = noise_miner(noisy_ds, pes=[0], min_pes=1)
        assert summary.filter(pl.col("kind") == "idle_gap").is_empty()


class TestFiltering:
    """The cutoffs that keep the report short."""

    def test_importance_cutoff_drops_the_smaller_cluster(
        self, noisy_ds: TraceDataset
    ) -> None:
        # Execution noise is ~14.4% of runtime, the gap ~2.9%.
        summary = noise_miner(noisy_ds, importance_cutoff=0.05)
        assert summary["kind"].to_list() == ["execution"]

    def test_importance_cutoff_can_empty_the_report(
        self, noisy_ds: TraceDataset
    ) -> None:
        summary = noise_miner(noisy_ds, importance_cutoff=0.9)
        assert summary.is_empty()
        # An empty result still carries the full schema.
        assert "likely_source" in summary.columns

    def test_min_pes_requires_cross_pe_recurrence(
        self, noisy_ds: TraceDataset
    ) -> None:
        # Both planted clusters span exactly three PEs.
        assert len(noise_miner(noisy_ds, min_pes=3)) == 2
        assert noise_miner(noisy_ds, min_pes=4).is_empty()

    def test_pe_filter_restricts_the_search(self, noisy_ds: TraceDataset) -> None:
        # PE 3 carries the gap but no execution stretch.
        summary = noise_miner(noisy_ds, pes=[2, 3], min_pes=1)
        kinds = set(summary["kind"].to_list())
        assert kinds == {"execution", "idle_gap"}
        assert _cluster(summary, "execution")["pe_count"] == 1


class TestClusterMerge:
    """The bin-merging rule, exercised directly on a histogram.

    The planted trace puts all of one kind's noise in a single bin, which cannot
    tell the merge rule apart from any other. These go at the function.
    """

    @staticmethod
    def _histogram(bin_indices: list[int]) -> pl.DataFrame:
        return pl.DataFrame(
            {
                "kind": ["execution"] * len(bin_indices),
                "bin_idx": bin_indices,
                "count": [1] * len(bin_indices),
            },
            schema={"kind": pl.String, "bin_idx": pl.Int64, "count": pl.Int64},
        )

    @staticmethod
    def _groups(mapping: pl.DataFrame) -> list[list[int]]:
        return [
            sorted(group["bin_idx"].to_list())
            for _, group in sorted(
                mapping.group_by("cluster_id"),
                key=lambda item: item[0],
            )
        ]

    def test_neighbouring_bins_merge(self) -> None:
        # Bins 10 and 11 are 1050 and 1150 µs; 1150 is inside 1050 x 1.1.
        mapping = _assign_clusters(
            self._histogram([10, 11]), bin_width_us=100, merge_threshold=0.1
        )
        assert self._groups(mapping) == [[10, 11]]

    def test_a_dense_histogram_does_not_chain_into_one_cluster(self) -> None:
        # Every bin here is within 10% of its predecessor, so a rule comparing
        # against the previous bin would swallow the whole range into one
        # cluster. Comparing against the cluster's own lowest duration keeps
        # them apart: 1050/1150, then 1250/1350, then 1450.
        mapping = _assign_clusters(
            self._histogram([10, 11, 12, 13, 14]),
            bin_width_us=100,
            merge_threshold=0.1,
        )
        assert self._groups(mapping) == [[10, 11], [12, 13], [14]]

    def test_distant_bins_stay_separate(self) -> None:
        mapping = _assign_clusters(
            self._histogram([1, 500]), bin_width_us=100, merge_threshold=0.1
        )
        assert self._groups(mapping) == [[1], [500]]

    def test_zero_threshold_gives_one_cluster_per_bin(self) -> None:
        mapping = _assign_clusters(
            self._histogram([10, 11, 12]), bin_width_us=100, merge_threshold=0.0
        )
        assert self._groups(mapping) == [[10], [11], [12]]

    def test_kinds_never_share_a_cluster(self) -> None:
        histogram = pl.DataFrame(
            {
                "kind": ["execution", "idle_gap"],
                "bin_idx": [10, 10],
                "count": [1, 1],
            },
            schema={"kind": pl.String, "bin_idx": pl.Int64, "count": pl.Int64},
        )
        mapping = _assign_clusters(histogram, bin_width_us=100, merge_threshold=0.1)
        assert mapping["cluster_id"].n_unique() == 2


class TestLabelling:
    """The likely-source heuristic."""

    def test_quantum_match_takes_precedence(self, noisy_ds: TraceDataset) -> None:
        # The 5000 µs excess is exactly one quantum, which is a more specific
        # explanation than "it recurs regularly".
        summary = noise_miner(noisy_ds, quantum_us=5_000)
        assert _cluster(summary, "execution")["likely_source"] == (
            "scheduler preemption (~1x quantum)"
        )

    def test_quantum_multiple_is_reported(self, noisy_ds: TraceDataset) -> None:
        summary = noise_miner(noisy_ds, quantum_us=2_500)
        assert _cluster(summary, "execution")["likely_source"] == (
            "scheduler preemption (~2x quantum)"
        )

    def test_unrelated_quantum_falls_through(self, noisy_ds: TraceDataset) -> None:
        summary = noise_miner(noisy_ds, quantum_us=3_000)
        assert _cluster(summary, "execution")["likely_source"] == "periodic daemon"


class TestOccurrences:
    """The per-event view backing the plot."""

    def test_counts_agree_with_the_summary(self, noisy_ds: TraceDataset) -> None:
        summary = noise_miner(noisy_ds)
        occ = noise_occurrences(noisy_ds)
        assert len(occ) == summary["occurrences"].sum()

    def test_only_surviving_clusters_are_returned(
        self, noisy_ds: TraceDataset
    ) -> None:
        summary = noise_miner(noisy_ds, importance_cutoff=0.05)
        occ = noise_occurrences(noisy_ds, importance_cutoff=0.05)
        assert set(occ["cluster_id"].to_list()) == set(summary["cluster_id"].to_list())

    def test_cluster_ids_are_stable_across_calls(
        self, noisy_ds: TraceDataset
    ) -> None:
        # Cluster ids are part of the result and are what joins a summary row to
        # its occurrences, so two runs over one trace must number them alike.
        first = noise_miner(noisy_ds).sort("cluster_id")
        second = noise_miner(noisy_ds).sort("cluster_id")
        assert first["cluster_id"].to_list() == second["cluster_id"].to_list()
        assert first["kind"].to_list() == second["kind"].to_list()

    def test_occurrence_times_fall_inside_the_run(
        self, noisy_ds: TraceDataset
    ) -> None:
        t_start, t_end = noisy_ds.time_range_us
        occ = noise_occurrences(noisy_ds)
        assert occ["time_us"].min() >= t_start
        assert occ["time_us"].max() <= t_end


class TestValidation:
    """Parameter checking."""

    @pytest.mark.parametrize(
        "kwargs",
        [
            {"bin_width_us": 0},
            {"bin_width_us": -100},
            {"merge_threshold": -0.1},
            {"importance_cutoff": 1.5},
            {"importance_cutoff": -0.1},
            {"min_pes": 0},
        ],
    )
    def test_rejects_bad_parameters(
        self, noisy_ds: TraceDataset, kwargs: dict
    ) -> None:
        with pytest.raises(ValueError):
            noise_miner(noisy_ds, **kwargs)

    def test_rejects_inverted_time_range(self, noisy_ds: TraceDataset) -> None:
        with pytest.raises(ValueError):
            noise_miner(noisy_ds, time_range=(1_000, 500))


class TestNoiseTimelinePlot:
    """Smoke tests for the Noise Miner view."""

    def test_produces_figure(self, noisy_ds: TraceDataset) -> None:
        from charmvz_vis.plots.noise import noise_timeline

        fig = noise_timeline(noisy_ds)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_time_weighting(self, noisy_ds: TraceDataset) -> None:
        from charmvz_vis.plots.noise import noise_timeline

        fig = noise_timeline(noisy_ds, weight="time")
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_empty_result_still_plots(self, noisy_ds: TraceDataset) -> None:
        from charmvz_vis.plots.noise import noise_timeline

        fig = noise_timeline(noisy_ds, importance_cutoff=0.9)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_rejects_bad_parameters(self, noisy_ds: TraceDataset) -> None:
        from charmvz_vis.plots.noise import noise_timeline

        with pytest.raises(ValueError):
            noise_timeline(noisy_ds, n_bins=0)
        with pytest.raises(ValueError):
            noise_timeline(noisy_ds, weight="bogus")
