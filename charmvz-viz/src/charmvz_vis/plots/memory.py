"""Memory Usage — heap consumption over time, per PE.

Analysis 10 of the Projections catalog. Reads the ``memory_sample`` table,
which is empty unless the traced application calls ``traceMemoryUsage()``: the
runtime samples nothing on its own.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

import matplotlib.pyplot as plt
import polars as pl

from ..colors import _BASE_PALETTE

if TYPE_CHECKING:
    from matplotlib.figure import Figure

    from ..dataset import TraceDataset

_UNITS = {
    "b": (1, "B"),
    "kib": (1024, "KiB"),
    "mib": (1024**2, "MiB"),
    "gib": (1024**3, "GiB"),
}


def memory_usage(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
    *,
    unit: str = "mib",
    aggregate: bool = False,
    figsize: tuple[float, float] = (12, 5),
) -> Figure:
    """Line chart of heap bytes over time, one series per PE.

    Parameters
    ----------
    ds : TraceDataset
    pes : PE filter
    time_range : filter on ``time_us``
    unit : one of ``b``, ``kib``, ``mib``, ``gib``
    aggregate : draw the min, mean and max across PEs instead of one line each.
        A per-PE line per processor is unreadable much beyond 16 PEs, and the
        envelope is what a memory question usually wants anyway
    figsize : figure size

    Returns
    -------
    matplotlib Figure.
    """
    key = unit.lower()
    if key not in _UNITS:
        raise ValueError(f"unit must be one of {sorted(_UNITS)}")
    divisor, unit_label = _UNITS[key]

    lf = ds.memory_sample
    if lf is None:
        raise ValueError(
            "This trace has no memory_sample.parquet. It was converted by a "
            "pipeline older than the MemorySample entity; re-run charmvz over "
            "the original logs."
        )

    if pes is not None:
        lf = lf.filter(pl.col("pe_id").is_in(list(pes)))
    if time_range is not None:
        lf = lf.filter(
            (pl.col("time_us") >= time_range[0]) & (pl.col("time_us") <= time_range[1])
        )
    df = lf.sort("pe_id", "time_us").collect()

    fig, ax = plt.subplots(figsize=figsize)

    if df.is_empty():
        ax.text(
            0.5,
            0.5,
            "No memory samples in this trace",
            ha="center",
            va="center",
            transform=ax.transAxes,
            fontsize=11,
            color="#6b6b6b",
        )
        ax.set_xlabel("Time (s)")
        ax.set_ylabel(f"Heap ({unit_label})")
        ax.set_title("Memory Usage")
        fig.tight_layout()
        return fig

    if aggregate:
        envelope = (
            df.group_by("time_us")
            .agg(
                pl.col("bytes").min().alias("min"),
                pl.col("bytes").mean().alias("mean"),
                pl.col("bytes").max().alias("max"),
            )
            .sort("time_us")
        )
        t = envelope["time_us"].to_numpy() / 1e6
        ax.fill_between(
            t,
            envelope["min"].to_numpy() / divisor,
            envelope["max"].to_numpy() / divisor,
            color=_BASE_PALETTE[0],
            alpha=0.25,
            label="min-max across PEs",
        )
        ax.plot(
            t,
            envelope["mean"].to_numpy() / divisor,
            color=_BASE_PALETTE[0],
            linewidth=1.6,
            label="mean",
        )
    else:
        for i, (pe_id_key, rows) in enumerate(
            sorted(df.group_by("pe_id"), key=lambda item: item[0][0])
        ):
            ax.plot(
                rows["time_us"].to_numpy() / 1e6,
                rows["bytes"].to_numpy() / divisor,
                label=f"PE {pe_id_key[0]}",
                color=_BASE_PALETTE[i % len(_BASE_PALETTE)],
                linewidth=1.2,
            )

    ax.set_xlabel("Time (s)")
    ax.set_ylabel(f"Heap ({unit_label})")
    ax.set_title("Memory Usage")

    handles, _ = ax.get_legend_handles_labels()
    if len(handles) <= 20:
        ax.legend(fontsize=8, ncol=2, framealpha=0.9)

    fig.tight_layout()
    return fig
