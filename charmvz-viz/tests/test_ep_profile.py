"""Recorded-duration semantics and readable ranked profile rendering."""
from types import SimpleNamespace

import matplotlib.pyplot as plt
import numpy as np
import polars as pl
import pytest
from matplotlib.colors import to_rgba
from matplotlib.patches import Wedge

from charmvz_vis.colors import OTHER_COLOR, EPColorMap
from charmvz_vis.plots.ep_profile import ep_profile


@pytest.fixture
def profile_ds():
    # Two tied methods, a tiny share, a zero duration, and a different PE.
    ds = SimpleNamespace(
        time_range_us=(0, 100), ep_color_map=EPColorMap(range(6)),
        entry_method=pl.LazyFrame({'ep_id': list(range(6)),
            'name': ['dominant', 'B', 'A', 'tiny', 'zero', 'outside'],
            'collection_id': [0] * 6}),
        execution=pl.LazyFrame({'ep_id': [2, 0, 1, 3, 4, 5],
            'pe_id': [0, 0, 0, 0, 0, 1], 'start_time_us': [0] * 6,
            'end_time_us': [100] * 6,
            'wall_duration_us': [100, 799, 100, 1, 0, 1000]}),
        idle_interval=pl.LazyFrame({'pe_id': [0], 'wall_duration_us': [999999]}))
    yield ds
    plt.close('all')


def labels(ax):
    return [t.get_text() for t in ax.get_yticklabels()]


def widths(ax):
    return [p.get_width() for p in ax.patches]


def test_values_ties_other_denominator_colors(profile_ds):
    fig = ep_profile(profile_ds, pes=[0], top_n=2, threshold_pct=0)
    ax = fig.axes[0]
    assert labels(ax) == ['dominant', 'B', 'Other']
    np.testing.assert_allclose(widths(ax), [79.9, 10, 10.1])
    assert sum(widths(ax)) == pytest.approx(100)
    assert [p.get_facecolor() for p in ax.patches] == [
        to_rgba(profile_ds.ep_color_map[0]), to_rgba(profile_ds.ep_color_map[1]),
        to_rgba(OTHER_COLOR)]
    assert ax.yaxis_inverted() and ax.get_xlim() == (0, 100)
    assert ax.get_xscale() == 'linear' and ax.get_legend() is None
    assert not any(isinstance(p, Wedge) for p in ax.patches)
    assert [t.get_text() for t in ax.texts] == ['79.9%', '10.0%', '10.1%']
    assert fig.texts[0].get_text() == 'Total recorded execution duration: 0.00 s'


@pytest.mark.parametrize(('threshold', 'expected'), [(10, ['dominant', 'B', 'A', 'Other']),
                                                     (10.01, ['dominant', 'Other'])])
def test_threshold_inclusive_and_other_conservation(profile_ds, threshold, expected):
    ax = ep_profile(profile_ds, pes=[0], top_n=6, threshold_pct=threshold).axes[0]
    assert labels(ax) == expected
    assert sum(widths(ax)) == pytest.approx(100)
    assert widths(ax)[-1] == pytest.approx(0.1 if threshold == 10 else 20.1)


def test_window_selects_full_stored_duration_and_pe(profile_ds):
    # Every selected row crosses both window edges; never clip to the 10-us window.
    profile_ds.execution = profile_ds.execution.with_columns(
        (pl.col('wall_duration_us') * 10000).alias('wall_duration_us'))
    fig = ep_profile(profile_ds, pes=[1], time_range=(40, 50), threshold_pct=0)
    assert labels(fig.axes[0]) == ['outside']
    assert widths(fig.axes[0]) == [100]
    assert fig.texts[0].get_text() == 'Total recorded execution duration: 10.00 s'


@pytest.mark.parametrize('selection', ['empty_pe', 'empty_window', 'zero', 'null'])
def test_empty_and_zero_totals(profile_ds, selection):
    kw = {}
    if selection == 'empty_pe':
        kw['pes'] = []
    elif selection == 'empty_window':
        kw['time_range'] = (101, 110)
    else:
        profile_ds.execution = profile_ds.execution.with_columns(
            pl.lit(0 if selection == 'zero' else None, dtype=pl.Int64).alias('wall_duration_us'))
    fig = ep_profile(profile_ds, **kw)
    ax = fig.axes[0]
    assert not ax.patches
    expected = ('No selected executions' if selection.startswith('empty')
                else 'Zero total recorded execution duration')
    assert ax.texts[0].get_text() == expected
    assert fig.texts[0].get_text() == 'Total recorded execution duration: 0.00 s'


def test_long_signatures_skew_and_external_labels(profile_ds):
    signature = ('Namespace::Worker<std::array<double,256>>::compute('
                 'const VeryLongTypeName& argument)')
    profile_ds.entry_method = profile_ds.entry_method.with_columns(
        pl.when(pl.col('ep_id') == 0).then(pl.lit(signature))
        .otherwise(pl.col('name')).alias('name'))
    profile_ds.execution = profile_ds.execution.with_columns(
        pl.when(pl.col('ep_id') == 0).then(999999)
        .otherwise(pl.col('wall_duration_us')).alias('wall_duration_us'))
    fig = ep_profile(profile_ds, pes=[0], top_n=6, threshold_pct=0)
    ax = fig.axes[0]
    assert ''.join(labels(ax)[0].split()) == ''.join(signature.split())
    assert '\n' in labels(ax)[0]
    assert widths(ax)[0] > 99.9
    assert ax.texts[1].get_text() == '<0.1%'
    assert ax.texts[3].get_text() == '<0.1%'
    assert ax.texts[4].get_text() == '0.0%'
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    boxes = [t.get_window_extent(renderer) for t in ax.get_yticklabels()]
    assert all(not a.overlaps(b) for a, b in zip(boxes, boxes[1:]))
    for bar, text in zip(ax.patches, ax.texts):
        end = ax.transData.transform((bar.get_width(), 0))[0]
        assert text.get_window_extent(renderer).x0 > end
    for text in [*ax.get_yticklabels(), *ax.texts, *fig.texts]:
        assert fig.bbox.contains(*text.get_window_extent(renderer).get_points()[0])
        assert fig.bbox.contains(*text.get_window_extent(renderer).get_points()[1])


def test_other_can_be_largest_and_stays_last(profile_ds):
    ax = ep_profile(profile_ds, top_n=1, threshold_pct=100).axes[0]
    assert labels(ax) == ['Other']
    assert widths(ax) == [100]


def test_removed_donut_keyword(profile_ds):
    with pytest.raises(TypeError, match='donut'):
        ep_profile(profile_ds, donut=True)
