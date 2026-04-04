"""Consistent color assignment for entry methods across all visualizations.

Every visualization that shows entry methods uses this module to ensure the same
EP always gets the same color.  Idle time is always a fixed gray.
"""

from __future__ import annotations

import colorsys
from typing import Sequence

import matplotlib.colors as mcolors

# Fixed palette for the first 20 EPs — curated for perceptual distinctness.
# Based on Tableau 20, but with slightly adjusted saturation for dark backgrounds.
_BASE_PALETTE: list[str] = [
    "#4e79a7",  # steel blue
    "#f28e2b",  # orange
    "#e15759",  # coral red
    "#76b7b2",  # teal
    "#59a14f",  # green
    "#edc948",  # gold
    "#b07aa1",  # mauve
    "#ff9da7",  # pink
    "#9c755f",  # brown
    "#bab0ac",  # warm gray
    "#af7aa1",  # plum
    "#86bcb6",  # seafoam
    "#f1ce63",  # mustard
    "#d37295",  # rose
    "#8cd17d",  # lime
    "#499894",  # dark teal
    "#b6992d",  # olive
    "#d4a6c8",  # lavender
    "#a0cbe8",  # sky blue
    "#ffbe7d",  # peach
]

IDLE_COLOR: str = "#3a3a3a"
OVERHEAD_COLOR: str = "#6b6b6b"
OTHER_COLOR: str = "#555555"


def _generate_hsl_color(index: int, total: int) -> str:
    """Generate a procedural color for EP indices beyond the base palette."""
    hue = (index * 0.618033988749895) % 1.0  # golden ratio spacing
    saturation = 0.55 + 0.15 * ((index % 3) / 2.0)
    lightness = 0.50 + 0.10 * ((index % 5) / 4.0)
    r, g, b = colorsys.hls_to_rgb(hue, lightness, saturation)
    return f"#{int(r*255):02x}{int(g*255):02x}{int(b*255):02x}"


class EPColorMap:
    """Maps entry method IDs to consistent colors.

    Parameters
    ----------
    ep_ids : sequence of ints
        All unique EP IDs that need colors.  The order determines palette
        assignment (first 20 get the curated palette, rest get procedural HSL).
    """

    def __init__(self, ep_ids: Sequence[int]) -> None:
        self._map: dict[int, str] = {}
        sorted_ids = sorted(ep_ids)
        for i, ep_id in enumerate(sorted_ids):
            if i < len(_BASE_PALETTE):
                self._map[ep_id] = _BASE_PALETTE[i]
            else:
                self._map[ep_id] = _generate_hsl_color(i, len(sorted_ids))

    def __getitem__(self, ep_id: int) -> str:
        return self._map.get(ep_id, OTHER_COLOR)

    def get(self, ep_id: int, default: str | None = None) -> str:
        return self._map.get(ep_id, default or OTHER_COLOR)

    def to_list(self, ep_ids: Sequence[int]) -> list[str]:
        """Return a list of colors in the same order as *ep_ids*."""
        return [self[eid] for eid in ep_ids]

    def to_matplotlib_cmap(self, ep_ids: Sequence[int]) -> mcolors.ListedColormap:
        """Return a Matplotlib ListedColormap for the given EP IDs."""
        return mcolors.ListedColormap(self.to_list(ep_ids))
