"""Tests for the package's query execution policy."""

from __future__ import annotations

import os

from charmvz_vis.engine import _PREFETCH_VAR, bounded_scan


def test_overlapping_blocks_keep_the_limit_until_the_last_exits(monkeypatch) -> None:
    # Two threads' collects overlap like this: the first to finish must not
    # remove the limit while the other's query may still be starting.
    monkeypatch.delenv(_PREFETCH_VAR, raising=False)
    first, second = bounded_scan(), bounded_scan()
    first.__enter__()
    second.__enter__()
    first.__exit__(None, None, None)
    assert os.environ.get(_PREFETCH_VAR) == "2"
    second.__exit__(None, None, None)
    assert _PREFETCH_VAR not in os.environ
