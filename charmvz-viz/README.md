
# Table of Contents

1.  [CharmVZ-vis](#org6d29d28)
    1.  [Quick Start](#org897be22)
    2.  [Installation](#orgff711ea)
    3.  [Available Visualizations](#org460c201)
    4.  [Analysis Utilities](#org392545c)
    5.  [Running Tests](#orgab27cb3)



<a id="org6d29d28"></a>

# CharmVZ-vis

Visualization and analysis library for [CharmVZ](https://github.com/caschb/charmvz-cpp) Parquet trace data.


<a id="org897be22"></a>

## Quick Start

    import charmvz_vis as cv
    
    # Load a trace dataset produced by the charmvz C++ pipeline
    ds = cv.TraceDataset("./output/")
    print(ds)  # TraceDataset('output', 64 PEs, 12.345s)
    
    # Time Profile — stacked area of EP time usage over the run
    fig = cv.time_profile(ds, bin_width_us=500_000)
    fig.savefig("time_profile.png", dpi=150)
    
    # Usage Profile — work distribution per PE
    fig = cv.usage_profile(ds)
    
    # Entry Method Profile — pie/donut chart
    fig = cv.ep_profile(ds)
    
    # Execution time histogram
    fig = cv.execution_time_histogram(ds, n_bins=50)
    
    # Communication per PE
    fig = cv.comm_per_pe(ds, metric="sent_msgs")
    
    # Find bottleneck PEs
    fig = cv.extrema_analysis(ds, attribute="most_idle_time", k=5)


<a id="orgff711ea"></a>

## Installation

    # From the charmvz-viz directory:
    python3 -m venv .venv
    source .venv/bin/activate
    pip install -e ".[dev]"


<a id="org460c201"></a>

## Available Visualizations

<table border="2" cellspacing="0" cellpadding="6" rules="groups" frame="hsides">


<colgroup>
<col  class="org-left" />

<col  class="org-left" />
</colgroup>
<thead>
<tr>
<th scope="col" class="org-left">Function</th>
<th scope="col" class="org-left">Description</th>
</tr>
</thead>
<tbody>
<tr>
<td class="org-left"><code>time_profile()</code></td>
<td class="org-left">Stacked area chart of EP time vs. time bins</td>
</tr>

<tr>
<td class="org-left"><code>usage_profile()</code></td>
<td class="org-left">Stacked bar chart of EP time per PE</td>
</tr>

<tr>
<td class="org-left"><code>ep_profile()</code></td>
<td class="org-left">Pie/donut chart of global EP time fractions</td>
</tr>

<tr>
<td class="org-left"><code>execution_time_histogram()</code></td>
<td class="org-left">Histogram of execution durations</td>
</tr>

<tr>
<td class="org-left"><code>message_size_histogram()</code></td>
<td class="org-left">Histogram of message sizes</td>
</tr>

<tr>
<td class="org-left"><code>comm_per_pe()</code></td>
<td class="org-left">Bar chart of communication metrics per PE</td>
</tr>

<tr>
<td class="org-left"><code>extrema_analysis()</code></td>
<td class="org-left">Outlier identification and ranking</td>
</tr>
</tbody>
</table>


<a id="org392545c"></a>

## Analysis Utilities

    # Per-PE utilization metrics
    util_df = cv.per_pe_utilization(ds)
    
    # Load imbalance score (coefficient of variation)
    score = cv.load_imbalance_score(ds)


<a id="orgab27cb3"></a>

## Running Tests

    pytest

