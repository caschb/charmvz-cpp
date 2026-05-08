"""Plots subpackage — Phase 1 visualization functions."""

from .comm_per_pe import comm_per_pe
from .ep_profile import ep_profile
from .extrema import extrema_analysis
from .histograms import execution_time_histogram, message_size_histogram
from .paper import (
    chare_activity_heatmap,
    chare_duration_comparison,
    chare_frequency_comparison,
    paper_percent_imbalance,
    percent_imbalance,
    timeline_overview,
    timeline_with_imbalance,
    timeline_with_paper_imbalance,
)
from .time_profile import time_profile
from .usage_profile import usage_profile

__all__ = [
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
    "paper_percent_imbalance",
    "timeline_with_imbalance",
    "timeline_with_paper_imbalance",
]
