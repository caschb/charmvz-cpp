"""Communication Per PE — bar chart of messages/bytes sent or received per PE.

Answers: "Which processors are the heaviest communicators?"
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Literal, Sequence

import matplotlib.pyplot as plt
import numpy as np
import polars as pl

from ..filters import apply_filters

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset

# Supported metric names
Metric = Literal[
    "sent_msgs", "sent_bytes",
    "recv_msgs", "recv_bytes",
]

_METRIC_LABELS = {
    "sent_msgs": "Messages Sent",
    "sent_bytes": "Bytes Sent",
    "recv_msgs": "Messages Received",
    "recv_bytes": "Bytes Received",
}


def comm_per_pe(
    ds: TraceDataset,
    metric: Metric = "sent_msgs",
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    figsize: tuple[float, float] | None = None,
) -> Figure:
    """Create a Communication Per PE bar chart.

    Parameters
    ----------
    ds : TraceDataset
    metric : one of 'sent_msgs', 'sent_bytes', 'recv_msgs', 'recv_bytes'
    pes : PE IDs to include, or None for all
    time_range : (start_us, end_us) or None for full trace
    figsize : figure size; auto-scaled if None

    Returns
    -------
    matplotlib Figure
    """
    tr = time_range or ds.time_range_us

    if metric in ("sent_msgs", "sent_bytes"):
        # Source-side: filter messages by src_pe and send_time
        msg_lf = apply_filters(
            ds.message,
            pes=pes,
            time_range=tr,
            pe_col="src_pe",
            start_col="send_time_us",
            end_col=None,
        )
        pe_col = "src_pe"
    else:
        # Destination-side: filter by dst_pe and exec_start_time
        msg_lf = apply_filters(
            ds.message,
            pes=pes,
            time_range=tr,
            pe_col="dst_pe",
            start_col="exec_start_time_us",
            end_col=None,
        )
        # Exclude broadcasts (dst_pe is null)
        msg_lf = msg_lf.filter(pl.col("dst_pe").is_not_null())
        pe_col = "dst_pe"

    if metric in ("sent_msgs", "recv_msgs"):
        agg_df = (
            msg_lf.group_by(pe_col)
            .agg(pl.len().alias("value"))
            .collect()
        )
        y_label = "Message count"
    else:
        agg_df = (
            msg_lf.group_by(pe_col)
            .agg(pl.col("msg_len").sum().alias("value"))
            .collect()
        )
        y_label = "Bytes"

    if len(agg_df) == 0:
        fig, ax = plt.subplots(figsize=(10, 5))
        ax.text(0.5, 0.5, "No data", ha="center", va="center", fontsize=14)
        return fig

    agg_df = agg_df.rename({pe_col: "pe_id"}).sort("pe_id")
    pe_ids = agg_df["pe_id"].to_numpy()
    values = agg_df["value"].to_numpy()

    n_pes = len(pe_ids)
    if figsize is None:
        figsize = (max(8, n_pes * 0.25), 5)

    fig, ax = plt.subplots(figsize=figsize)

    bar_color = "#4e79a7" if "sent" in metric else "#e15759"
    ax.bar(np.arange(n_pes), values, color=bar_color, alpha=0.85, edgecolor="white", linewidth=0.3)

    if n_pes <= 64:
        ax.set_xticks(np.arange(n_pes))
        ax.set_xticklabels(pe_ids, fontsize=max(4, 8 - n_pes // 20), rotation=90)
    else:
        step = max(1, n_pes // 32)
        ax.set_xticks(np.arange(0, n_pes, step))
        ax.set_xticklabels(pe_ids[::step], fontsize=6)

    ax.set_xlabel("PE")
    ax.set_ylabel(y_label)
    ax.set_title(_METRIC_LABELS.get(metric, metric))
    fig.tight_layout()
    return fig
