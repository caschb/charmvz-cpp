"""charmvz_vis — Visualization and analysis library for CharmVZ Parquet traces.

Quick start::

    import charmvz_vis as cv

    ds = cv.TraceDataset("./output/")
    print(ds)  # TraceDataset('output', 64 PEs, 12.345s)

    fig = cv.time_profile(ds, bin_width_us=500_000)
    fig.savefig("time_profile.png")
"""

# Analysis functions
from .analysis import load_imbalance_score, per_interval_utilization, per_pe_utilization
from .dataset import TraceDataset

# Plot functions — Phase 1
from .plots import (
    chare_activity_heatmap,
    chare_duration_comparison,
    chare_frequency_comparison,
    comm_per_pe,
    ep_profile,
    execution_time_histogram,
    extrema_analysis,
    message_size_histogram,
    percent_imbalance,
    time_profile,
    timeline_overview,
    timeline_with_imbalance,
    usage_profile,
)

__version__ = "0.1.0"

__all__ = [
    # Core
    "TraceDataset",
    # Analysis
    "per_pe_utilization",
    "per_interval_utilization",
    "load_imbalance_score",
    # Plots
    "time_profile",
    "usage_profile",
    "ep_profile",
    "execution_time_histogram",
    "message_size_histogram",
    "comm_per_pe",
    "extrema_analysis",
    "chare_duration_comparison",
    "chare_frequency_comparison",
    "chare_activity_heatmap",
    "timeline_overview",
    "percent_imbalance",
    "timeline_with_imbalance",
]
