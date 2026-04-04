"""Smoke tests for plot functions — verify they produce valid figures."""

from __future__ import annotations

import matplotlib
import matplotlib.pyplot as plt
import pytest

from charmvz_vis import TraceDataset

# Use non-interactive backend for tests
matplotlib.use("Agg")


class TestTimePlotSmoke:
    """Smoke tests for time_profile."""

    def test_produces_figure(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.time_profile import time_profile

        fig = time_profile(ds, bin_width_us=500_000)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_percentage_mode(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.time_profile import time_profile

        fig = time_profile(ds, bin_width_us=500_000, as_percentage=True)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)


class TestUsageProfileSmoke:
    """Smoke tests for usage_profile."""

    def test_produces_figure(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.usage_profile import usage_profile

        fig = usage_profile(ds)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)


class TestEPProfileSmoke:
    """Smoke tests for ep_profile."""

    def test_produces_figure(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.ep_profile import ep_profile

        fig = ep_profile(ds)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)


class TestHistogramSmoke:
    """Smoke tests for histogram functions."""

    def test_execution_time(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.histograms import execution_time_histogram

        fig = execution_time_histogram(ds, n_bins=10)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_message_size(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.histograms import message_size_histogram

        fig = message_size_histogram(ds, n_bins=10)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)


class TestCommPerPESmoke:
    """Smoke tests for comm_per_pe."""

    def test_sent_msgs(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.comm_per_pe import comm_per_pe

        fig = comm_per_pe(ds, metric="sent_msgs")
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_recv_bytes(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.comm_per_pe import comm_per_pe

        fig = comm_per_pe(ds, metric="recv_bytes")
        assert isinstance(fig, plt.Figure)
        plt.close(fig)


class TestExtremaSmoke:
    """Smoke tests for extrema_analysis."""

    def test_most_idle(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.extrema import extrema_analysis

        fig = extrema_analysis(ds, attribute="most_idle_time", k=2)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)

    def test_avg_grain_size(self, ds: TraceDataset) -> None:
        from charmvz_vis.plots.extrema import extrema_analysis

        fig = extrema_analysis(ds, attribute="avg_grain_size", k=2)
        assert isinstance(fig, plt.Figure)
        plt.close(fig)
