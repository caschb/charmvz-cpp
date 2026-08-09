"""Tests for the User Stats and Memory Usage views.

Both read tables that exist only when the traced application instruments
itself, so the ``instrumented_ds`` fixture is the only data available. The
``ds`` fixture deliberately lacks both tables and is used to check the
absent-table paths.
"""

from __future__ import annotations

import matplotlib
import matplotlib.pyplot as plt
import pytest

from charmvz_vis import TraceDataset
from charmvz_vis.plots.memory import memory_usage
from charmvz_vis.plots.user_stats import user_stats_over_time, user_stats_per_pe

matplotlib.use("Agg")


class TestDatasetAccessors:
    """The two optional tables."""

    def test_absent_when_uninstrumented(self, ds: TraceDataset) -> None:
        assert ds.user_stat is None
        assert ds.memory_sample is None

    def test_present_when_instrumented(self, instrumented_ds: TraceDataset) -> None:
        assert instrumented_ds.user_stat is not None
        assert instrumented_ds.memory_sample is not None

    def test_a_missing_table_does_not_block_loading(self, ds: TraceDataset) -> None:
        # An uninstrumented trace is the common case and must stay usable.
        assert ds.execution.collect().height > 0

    def test_user_time_is_null_for_update_stat(
        self, instrumented_ds: TraceDataset
    ) -> None:
        # Stat 1 stands for a call to updateStat(), which supplies no time.
        import polars as pl

        rows = (
            instrumented_ds.user_stat.filter(pl.col("stat_id") == 1)
            .collect()
        )
        assert rows.height > 0
        assert rows["user_time_s"].null_count() == rows.height


class TestUserStatsOverTime:
    """Catalog analysis 14."""

    def test_produces_figure(self, instrumented_ds: TraceDataset) -> None:
        fig = user_stats_over_time(instrumented_ds)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_one_line_per_stat(self, instrumented_ds: TraceDataset) -> None:
        fig = user_stats_over_time(instrumented_ds)
        assert len(fig.axes[0].get_lines()) == 2
        plt.close(fig)

    def test_stat_filter(self, instrumented_ds: TraceDataset) -> None:
        fig = user_stats_over_time(instrumented_ds, stats=[0])
        assert len(fig.axes[0].get_lines()) == 1
        plt.close(fig)

    def test_registered_names_are_used_as_labels(
        self, instrumented_ds: TraceDataset
    ) -> None:
        fig = user_stats_over_time(instrumented_ds)
        labels = {line.get_label() for line in fig.axes[0].get_lines()}
        assert labels == {"residual", "particle count"}
        plt.close(fig)

    @pytest.mark.parametrize("aggregation", ["mean", "max", "min"])
    def test_aggregations(
        self, instrumented_ds: TraceDataset, aggregation: str
    ) -> None:
        fig = user_stats_over_time(instrumented_ds, aggregation=aggregation)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_aggregation_across_pes_differs(
        self, instrumented_ds: TraceDataset
    ) -> None:
        # residual is 10/(i+1) + pe, so PE 0 and PE 1 differ by exactly 1 and
        # max must sit above min at every point.
        lo = user_stats_over_time(instrumented_ds, stats=[0], aggregation="min")
        hi = user_stats_over_time(instrumented_ds, stats=[0], aggregation="max")
        lo_y = lo.axes[0].get_lines()[0].get_ydata()
        hi_y = hi.axes[0].get_lines()[0].get_ydata()
        assert all(
            high - low == pytest.approx(1.0)
            for low, high in zip(lo_y, hi_y, strict=True)
        )
        plt.close(lo)
        plt.close(hi)

    def test_user_time_axis(self, instrumented_ds: TraceDataset) -> None:
        # Stat 0 carries a user time; the x values must come from that column
        # (0.5, 1.0, 1.5) and not from the trace clock in seconds (1, 2, 3).
        fig = user_stats_over_time(instrumented_ds, stats=[0], use_user_time=True)
        x = fig.axes[0].get_lines()[0].get_xdata()
        assert list(x) == pytest.approx([0.5, 1.0, 1.5])
        plt.close(fig)

    def test_user_time_axis_rejects_stats_without_one(
        self, instrumented_ds: TraceDataset
    ) -> None:
        with pytest.raises(ValueError, match="updateStat"):
            user_stats_over_time(instrumented_ds, stats=[1], use_user_time=True)

    def test_rejects_unknown_aggregation(
        self, instrumented_ds: TraceDataset
    ) -> None:
        with pytest.raises(ValueError):
            user_stats_over_time(instrumented_ds, aggregation="median")

    def test_absent_table_names_the_remedy(self, ds: TraceDataset) -> None:
        with pytest.raises(ValueError, match="re-run charmvz"):
            user_stats_over_time(ds)


class TestUserStatsPerPe:
    """Catalog analysis 15."""

    def test_produces_figure(self, instrumented_ds: TraceDataset) -> None:
        fig = user_stats_per_pe(instrumented_ds)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_one_bar_group_per_stat(self, instrumented_ds: TraceDataset) -> None:
        fig = user_stats_per_pe(instrumented_ds)
        assert len(fig.axes[0].containers) == 2
        plt.close(fig)

    def test_pe_filter(self, instrumented_ds: TraceDataset) -> None:
        fig = user_stats_per_pe(instrumented_ds, pes=[0])
        for container in fig.axes[0].containers:
            assert len(container) == 1
        plt.close(fig)

    def test_time_range_changes_the_aggregate(
        self, instrumented_ds: TraceDataset
    ) -> None:
        # residual on PE 0 is 10, 5, 3.33 at the three instants. Restricting to
        # the first alone must move the mean.
        full = user_stats_per_pe(instrumented_ds, stats=[0])
        first = user_stats_per_pe(
            instrumented_ds, stats=[0], time_range=(0, 1_500_000)
        )
        full_v = full.axes[0].containers[0][0].get_height()
        first_v = first.axes[0].containers[0][0].get_height()
        assert first_v == pytest.approx(10.0)
        assert full_v != pytest.approx(first_v)
        plt.close(full)
        plt.close(first)

    def test_absent_table_names_the_remedy(self, ds: TraceDataset) -> None:
        with pytest.raises(ValueError, match="re-run charmvz"):
            user_stats_per_pe(ds)


class TestMemoryUsage:
    """Catalog analysis 10."""

    def test_produces_figure(self, instrumented_ds: TraceDataset) -> None:
        fig = memory_usage(instrumented_ds)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_one_line_per_pe(self, instrumented_ds: TraceDataset) -> None:
        fig = memory_usage(instrumented_ds)
        assert len(fig.axes[0].get_lines()) == 2
        plt.close(fig)

    def test_unit_conversion(self, instrumented_ds: TraceDataset) -> None:
        # PE 0 starts at 64 MiB.
        mib = memory_usage(instrumented_ds, pes=[0], unit="mib")
        gib = memory_usage(instrumented_ds, pes=[0], unit="gib")
        assert mib.axes[0].get_lines()[0].get_ydata()[0] == pytest.approx(64.0)
        assert gib.axes[0].get_lines()[0].get_ydata()[0] == pytest.approx(0.0625)
        plt.close(mib)
        plt.close(gib)

    def test_aggregate_mode_draws_an_envelope(
        self, instrumented_ds: TraceDataset
    ) -> None:
        fig = memory_usage(instrumented_ds, aggregate=True)
        ax = fig.axes[0]
        assert len(ax.get_lines()) == 1
        assert len(ax.collections) == 1  # the fill_between band
        plt.close(fig)

    def test_rejects_unknown_unit(self, instrumented_ds: TraceDataset) -> None:
        with pytest.raises(ValueError):
            memory_usage(instrumented_ds, unit="furlongs")

    def test_absent_table_names_the_remedy(self, ds: TraceDataset) -> None:
        with pytest.raises(ValueError, match="re-run charmvz"):
            memory_usage(ds)
