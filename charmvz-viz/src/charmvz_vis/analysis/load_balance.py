"""Load imbalance scoring."""

from __future__ import annotations

from typing import TYPE_CHECKING, Sequence

from .utilization import per_pe_utilization

if TYPE_CHECKING:
    from ..dataset import TraceDataset


def load_imbalance_score(
    ds: TraceDataset,
    pes: Sequence[int] | None = None,
    time_range: tuple[int, int] | None = None,
) -> float:
    """Compute load imbalance score: σ(utilization) / μ(utilization).

    A value of 0 means perfect balance; higher means more imbalanced.

    Returns
    -------
    float — the coefficient of variation of per-PE utilization.
    """
    util_df = per_pe_utilization(ds, pes=pes, time_range=time_range)
    mean_u = util_df["utilization"].mean()
    std_u = util_df["utilization"].std()
    if mean_u is None or mean_u == 0:
        return 0.0
    return float(std_u / mean_u)
