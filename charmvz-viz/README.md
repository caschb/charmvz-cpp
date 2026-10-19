# CharmVZ-vis

Visualization and analysis library for [CharmVZ](https://github.com/caschb/charmvz-cpp) Parquet trace data. Edit this README directly in Markdown.

## Quick Start

``` python
import charmvz_vis as cv

# Load a trace dataset produced by the charmvz C++ pipeline
ds = cv.TraceDataset("./output/")
print(ds)  # TraceDataset('output', 64 PEs, 12.345s)

# Time Profile — stacked area of EP time usage over the run
fig = cv.time_profile(ds, bin_width_us=500_000)
fig.savefig("time_profile.png", dpi=150)

# Usage Profile — work distribution per PE
fig = cv.usage_profile(ds)

# Entry Method Profile — ranked horizontal bars
fig = cv.ep_profile(ds)

# Execution time histogram
fig = cv.execution_time_histogram(ds, n_bins=50)

# Communication per PE
fig = cv.comm_per_pe(ds, metric="sent_msgs")

# Find bottleneck PEs
fig = cv.extrema_analysis(ds, attribute="most_idle_time", k=5)
```

## Installation

``` bash
# From the charmvz-viz directory:
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e . pytest ruff
```

## Available Visualizations

| Function | Description |
|----|----|
| `time_profile()` | Stacked area chart of EP time vs. time bins |
| `usage_profile()` | Stacked bar chart of EP time per PE |
| `ep_profile()` | Ranked horizontal bars of selected recorded execution shares |
| `execution_time_histogram()` | Histogram of execution durations |
| `message_size_histogram()` | Histogram of message sizes |
| `comm_per_pe()` | Bar chart of communication metrics per PE |
| `extrema_analysis()` | Outlier identification and ranking |
| `memory_usage()` | Application-triggered runtime memory samples |
| `user_stats_over_time()`, `user_stats_per_pe()` | Application-statistic views |
| `chare_duration_comparison()`, `chare_frequency_comparison()`, `chare_activity_heatmap()` | Chare-level comparisons |
| `timeline_overview()` | Per-PE activity overview |
| `percent_imbalance()`, `paper_percent_imbalance()` | Load-imbalance curves |
| `timeline_with_imbalance()`, `timeline_with_paper_imbalance()` | Combined timeline and imbalance views |
| `noise_timeline()` | Stacked bar chart of detected noise over time |

## Analysis Utilities

``` python
# Per-PE utilization metrics
util_df = cv.per_pe_utilization(ds)

# Load imbalance score (coefficient of variation)
score = cv.load_imbalance_score(ds)

# Noise Miner: clusters of recurring interruptions to execution
clusters = cv.noise_miner(ds, quantum_us=4000)
```

The Noise Miner reports one row per cluster of recurring noise, with its median
duration, the PEs affected, the occurrence count, the median interval between
occurrences on one PE, and a heuristic label for the likely source. Two signals
feed it: executions running longer than the median for their own entry method,
and wall-clock time covered by neither an execution nor an idle interval.
`noise_occurrences()` returns the individual events behind a cluster.

## Memory

Every function in `charmvz_vis.__all__` runs within a 2 GiB cgroup limit on a
20-PE ChaNGa trace with 88.5M executions and 55.9M messages (3.7 GB of
Parquet), with the default 32 threads. Apart from the cases listed under "Not
bounded", memory grows with the thread count rather than with the trace.

### How

- Every query runs on polars' streaming engine (`charmvz_vis.engine.collect`),
  whatever `POLARS_ENGINE_AFFINITY` or `pl.Config` say. The in-memory engine
  materialises the whole filtered scan before aggregating.
- Aggregations group executions by integer keys (`ep_id`, `pe_id`, bin
  index) and join entry-method and chare names onto the small result. A name
  string carried through a scan of every execution is enough to push a
  streaming aggregation past 2 GiB.
- Binned loads are summed in two sequential passes, one for the spans inside a
  single bin and one for the few that cross a bin boundary, rather than one
  union of two concurrent scans.
- Histograms count bins in polars, reproducing `np.histogram`'s bin edges and
  index arithmetic; the durations never reach numpy.
- The Noise Miner takes each entry method's median from per-duration counts
  and merges executions with idle intervals one PE at a time.
- The paper imbalance counts each execution once, in the bucket between the
  two sampled time points around its start, instead of cross-joining every
  execution with every time point.

The recorded `tools/compare_dumps.py` comparison found every table and every
plotted value equal to the pre-streaming implementation (whole numbers exactly,
other floats within a relative 1e-9) on the trace above and on an 80-PE,
4.8M-execution trace, apart from the order of PEs that tie in
`extrema_analysis(attribute="active_entry_methods")`, which also changes from
run to run without these changes. The paper imbalance with duplicate
`time_points_us` no longer multiplies the loads at a repeated point by its
multiplicity; its percentages are the same.

### Knobs

The streaming engine's working set is roughly `threads x prefetched row groups x projected row-group bytes`, plus the aggregate being built, plus what
the allocator keeps after a query ends.

| Variable | Effect |
|----|----|
| `POLARS_MAX_THREADS` | Thread pool size, fixed when polars is imported. The main lever on machines with many cores: a two-column aggregation over 88.5M rows peaked at 941 MiB with 32 threads, 475 with 8, 267 with 2 |
| `POLARS_ROW_GROUP_PREFETCH_SIZE` | Row groups read ahead per scan. `charmvz_vis` sets it to 2 for the duration of each query unless it is already set (941 to 633 MiB in the same aggregation); 1 is no better. This changes `os.environ` for the whole process while a query runs |
| `_RJEM_MALLOC_CONF` | jemalloc settings for polars; only read when polars is imported. `dirty_decay_ms:0,muzzy_decay_ms:0` returns freed pages at once instead of keeping them for the next query: `noise_miner` peaked at 760 MiB instead of 1909, about 30% slower. Not set by the package |

### Not bounded

- The Noise Miner's idle-gap sweep and the paper imbalance with explicit
  `time_points_us` hold one PE's executions at a time (4.8M rows here). So
  does the paper imbalance with the default points when start times are so
  large that rounding makes neighbouring points coincide.
- The Noise Miner's exact medians hold one row per distinct (entry method,
  duration) pair: 19,799 on the trace above, but a trace whose durations are
  nearly all distinct holds one per execution (a synthetic 20M-execution trace
  of distinct durations was OOM-killed at 2G).
- `bin_loads` splits boundary-crossing spans in a second pass whose size is
  about total busy time divided by the bin width. Keep `bin_width_us` at 1000
  or above on long runs.
- `noise_occurrences()` returns every occurrence of every reported cluster.
- `memory_usage()` and `user_stats_*()` load their whole (application-sampled)
  table.
- Per-PE plots and `per_interval_utilization()` grow with the number of PEs
  (and bins) by design.

### Checking it

`tools/constrained_viz.sh LIMIT TRACE_DIR [FUNCTION...]` runs each entry
point alone under a cgroup memory limit with swap off, one after another, and
prints its anon and page-cache peaks (from `tools/memprobe.py`) or the OOM
kill. Set `POLARS_TEMP_DIR` to a directory on disk first; the script refuses
a tmpfs. `tools/dump_tables.py` and `tools/compare_dumps.py` dump the data
behind every table and plot and compare two dumps, for checking that a change
leaves results alone; the comparison exits non-zero if an item differs or
exists on one side only.

``` bash
export POLARS_TEMP_DIR=/scratch/$USER/polars
tools/constrained_viz.sh 2G /path/to/trace
```

Historical measurements (before the Entry Method Profile bar-chart update; not remeasured):
result at 2G on the 88.5M-execution ChaNGa trace (32 threads, polars 1.39.3);
every case had `oom_kills=0`. These measurements describe the tested traces
and implementation; the resident-state limits above still apply.

| Case                            | anon peak (MiB) | cgroup peak (MiB) | wall (s) |
|---------------------------------|-----------------|-------------------|----------|
| `per_pe_utilization`            | 678             | 694               | 0.7      |
| `per_interval_utilization`      | 1010            | 1023              | 1.4      |
| `load_imbalance_score`          | 689             | 703               | 0.7      |
| `noise_miner`                   | 1777            | 1790              | 5.4      |
| `noise_occurrences`             | 1682            | 1694              | 4.7      |
| `time_profile`                  | 1410            | 1426              | 1.7      |
| `usage_profile`                 | 849             | 864               | 0.9      |
| `ep_profile`                    | 698             | 712               | 1.1      |
| `execution_time_histogram`      | 932             | 946               | 1.9      |
| `message_size_histogram`        | 665             | 676               | 1.8      |
| `comm_per_pe`                   | 534             | 547               | 0.7      |
| `extrema_analysis`              | 685             | 701               | 0.7      |
| `noise_timeline`                | 1782            | 1795              | 4.8      |
| `memory_usage`                  | 339             | 350               | 0.0      |
| `user_stats_over_time`          | 350             | 359               | 0.1      |
| `user_stats_per_pe`             | 336             | 349               | 0.1      |
| `chare_duration_comparison`     | 705             | 720               | 1.4      |
| `chare_frequency_comparison`    | 677             | 688               | 0.9      |
| `chare_activity_heatmap`        | 818             | 836               | 0.8      |
| `timeline_overview`             | 1555            | 1570              | 1.6      |
| `percent_imbalance`             | 1017            | 1031              | 1.3      |
| `paper_percent_imbalance`       | 1453            | 1465              | 2.1      |
| `timeline_with_imbalance`       | 1580            | 1597              | 2.9      |
| `timeline_with_paper_imbalance` | 1620            | 1635              | 3.4      |
| `time_profile`, PE 0, first 10% | 557             | 567               | 0.2      |

The baseline of about 340 MiB is the interpreter with polars, numpy and
matplotlib loaded.

## Running Tests

``` bash
pytest
```

### Entry Method Profile API change

`ep_profile` now draws horizontal bars and no longer accepts `donut`. Remove
that keyword from callers. The nineteen plotting exports are unchanged. Methods
rank by summed stored `wall_duration_us`, with ascending EP ID breaking ties.
PE and overlapping-window filters select rows without clipping their durations;
idle is excluded. Percentages divide by all selected execution duration, including
`Other`, which is placed last. The default figure is 12 by 7 inches; names wrap
beside the bars and the total recorded duration appears below the chart. Empty
selections and zero totals display an explicit message. Positive shares below
0.1% display `<0.1%`.
