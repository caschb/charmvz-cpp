# CharmVZ

CharmVZ converts Charm++ Projections traces into thirteen Parquet tables for
programmatic analysis with Python, R, Julia, DuckDB and other Parquet readers.

The repository also contains [charmvz-viz](charmvz-viz/), a Python package that reads those tables and reproduces the Projections visualizations.

## Status

The converter supports Projections 11.0 and rejects other STS versions.
Aggregate counts and durations have been cross-validated against the headless
Java Projections reader on ChaNGa and LeanMD reference traces. These checks
validate the compared aggregates; message relationships and other reconstruction
rules also have fixture-based tests. See [validation](#validation).

The converter can spill reconstruction records to disk under a memory target.
Some state remains resident; see [memory behavior](#memory-budget-and-spilling).

Edit this README and [the visualization README](charmvz-viz/README.md)
directly in Markdown.

## Overview

CharmVZ bridges Charm++ performance tracing and modern data analysis. [Projections](https://github.com/charmplusplus/projections) provides an interactive GUI; CharmVZ targets programmatic analysis, scripted comparison across runs, and custom visualization, by turning the trace into a normalized relational schema.

### Key features

- **Streaming**: logs are parsed line by line and flushed in row groups, so a multi-gigabyte trace never lands in memory at once
- **Transparent decompression**: plain `.log` and gzipped `.log.gz` are read the same way, and a single trace directory may mix both
- Thirteen relational tables retain observed object identity, executions,
  messages, placement transitions and application annotations.
- Derived durations and PAPI deltas are stored with the records. Receive-time
  placeholders limit the interpretation of queue-wait arithmetic.
- **Compressed output**: ZSTD by default, measured 2.54x smaller than Parquet's uncompressed default for about 7% more wall time
- **Application semantics**: user events are materialized, and application-declared timesteps become a first-class dimension

## Installation

### Prerequisites

- C++20 compatible compiler (GCC 10+ or Clang 12+)
- [Meson](https://mesonbuild.com/) build system and [Ninja](https://ninja-build.org/) backend

### Dependencies

All but one are **system dependencies** that must be installed beforehand; Meson locates them with `dependency()` and does not fetch them.

- **Apache Arrow** and **Parquet** (C++): the output format
- **spdlog** (\>= 1.15.3): logging
- **CLI11** (\>= 2.5.0): command-line parsing
- **zlib**: gzip support for compressed logs
- **Catch2** (\>= 3.10.0): tests only, and optional. The test targets are guarded by `if catch2_dep.found()`, so a build without it still succeeds and simply skips them

`zstr` is the one vendored dependency: a git submodule at `subprojects/zstr`, consumed as a CMake subproject.

On macOS with Homebrew:

``` bash
brew install meson ninja apache-arrow spdlog cli11 catch2
```

### Build instructions

``` bash
# Clone, including the zstr submodule
git clone --recurse-submodules https://github.com/caschb/charmvz-cpp.git
cd charmvz-cpp
# (if already cloned without it: git submodule update --init --recursive)

# Release build
meson setup builddir-rel --buildtype=release
meson compile -C builddir-rel

# Tests
meson test -C builddir-rel
```

A `justfile` automates the sanitizer build used for development:

``` bash
just compile-debug   # configures builddir-deb with ASan + UBSan, then compiles
just cleanup-debug
```

Log verbosity is set through the `SPDLOG_LEVEL` environment variable, not a flag:

``` bash
SPDLOG_LEVEL=debug ./builddir-rel/charmvz -l traces/ -o out/
```

### Building on the Kabré cluster

The recorded working configuration below targets the compute-node image.
Check the installed toolchain before reusing it on a different image.

|                     | Login (`login-*.cnca`) | Compute (`kura-*.cnca`) |
|---------------------|------------------------|-------------------------|
| OS                  | AlmaLinux 9.7          | AlmaLinux 8.10          |
| glibc               | 2.34                   | 2.28                    |
| System `g++`        | 11.5                   | 8.5                     |
| `gcc/13.4.0` module | works                  | will not start          |
| GNU `time`          | present                | absent                  |

Binaries have to run on the compute nodes, so glibc 2.28 is the target. The
`gcc` modules are built for the login image and abort with missing
`GLIBC_2.32` symbols on a compute node, and the compute nodes' own `g++` 8.5
predates the C++20 this project needs. Neither system compiler is usable.

The recorded setup uses a user-space conda environment providing Arrow, Parquet, spdlog,
CLI11, meson and ninja. Its libraries need at most `GLIBC_2.17`, so they load
on the compute nodes without complaint.

One gap remains. The conda toolchain is `gcc` 16, whose `libstdc++` inlines a
read of `__libc_single_threaded` into anything using `shared_ptr`. That object
arrived in glibc 2.32 and does not exist in 2.28, so every such translation
unit fails to link. Note that conda's own `sysroot_linux-64` does not save you
here: the package is labelled 2.34 but ships a glibc 2.28 `libc.so.6`.

Supplying the symbol resolves it, and costs nothing the platform would not
already pay, since a `libstdc++` built against 2.28 uses the atomic refcount
path regardless:

``` cpp
// glibc228_compat.cpp
extern "C" char __libc_single_threaded = 0;
```

``` bash
ENV=/path/to/conda/env
export PATH="$ENV/bin:$PATH" PKG_CONFIG_PATH="$ENV/lib/pkgconfig"
export LD_LIBRARY_PATH="$ENV/lib:$LD_LIBRARY_PATH"
export CC="$ENV/bin/x86_64-conda-linux-gnu-cc" CXX="$ENV/bin/x86_64-conda-linux-gnu-c++"
$CXX -O2 -fPIC -c glibc228_compat.cpp -o glibc228_compat.o
export LDFLAGS="$PWD/glibc228_compat.o"
meson setup builddir-kabre --buildtype=release && meson compile -C builddir-kabre
```

Build and test on a compute node, never the login node: the login nodes are
limited, and a build that succeeds there may not even start where it has to
run. The test suite passes under this configuration against Arrow 25.0.0.

## Usage

### Command line

``` bash
./builddir-rel/charmvz -l <trace_dir> -o <output_dir> [-s <step_event_name>]
                       [--memory-budget <size> --spill-dir <dir>]
```

| Option | Required | Description |
|----|----|----|
| `-l`, `--logs` | yes | Directory holding the `.sts`, `.projrc` and per-PE log files |
| `-o`, `--output` | yes | Directory for the Parquet output; created if absent |
| `-s`, `--step-event` | no | Name of the registered user event that delimits a timestep (default `SimulationStep`) |
| `--memory-budget` | no | Memory target, e.g. `512M` or `4G`; `0` (default) disables spilling. See [memory behavior](#memory-budget-and-spilling) |
| `--spill-dir` | no | Where spilled data goes (default: the system temp directory, `$TMPDIR` or `/tmp`). Must be on disk |
| `--keep-spill` | no | Keep the spill directory after the run, for debugging |
| `--clean-stale-spill` | no | Delete `charmvz-spill-*` directories left in the spill directory by conversions that are no longer running |

Pass the trace directory. CharmVZ discovers its `.sts`, optional `.projrc`
and per-PE logs. Missing STS metadata aborts conversion; missing clock-reference
metadata suppresses migrations and cross-PE timing derivations.

### Input files

Standard Charm++ Projections output, produced by building with `-tracemode projections` and running with `+traceroot`:

- `<pgm>.sts` – symbol table: chare, entry-method, message and user-event names; PE count; PAPI counter names
- `<pgm>.projrc` – runtime origin-shift metadata, used to gate cross-PE derivations
- `<pgm>.<pe>.log` or `<pgm>.<pe>.log.gz` – one time-ordered event log per PE

``` bash
./charmrun +p4 ./myapp +traceroot traces/
```

A Charm++ built with `-DZLIB=1` writes the logs gzipped. Both forms are read transparently, and mixing them in one directory is fine.

### Output tables

| Table | Primary key | Contents |
|----|----|----|
| `processing_element.parquet` | `pe_id` | One row per PE log file |
| `chare_collection.parquet` | `collection_id` | Chare types, with their `ndims` |
| `entry_method.parquet` | `ep_id` | Entry methods and their owning collection |
| `message_type.parquet` | `msg_idx` | Declared message sizes from the STS |
| `chare_instance.parquet` | `instance_id` | Chare elements; natural key `(collection_id, index_0..5)` |
| `execution.parquet` | none (bag of observations) | Processing intervals, including incomplete intervals, with PAPI counters |
| `message.parquet` | `message_id` | Sends linked to their receiving execution |
| `idle_interval.parquet` | `(pe_id, start_time_us)` | Idle intervals, including incomplete intervals |
| `migration_episode.parquet` | `migration_id` | PE transitions of chare-array elements |
| `user_event.parquet` | – | Application-emitted trace events |
| `simulation_step.parquet` | `(step_id, pe_id)` | Application timesteps |
| `user_stat.parquet` | none (bag of samples) | Named application statistics and their supplied times |
| `memory_sample.parquet` | none (bag of samples) | Application-triggered runtime memory samples |

All thirteen tables are written, including expected-empty application tables.
Execution serials are not unique row keys: different senders and repeated
observations can reuse them. The [shared schema](../notes/agent_instructions/charm-schema.md)
defines each entity; [src/schema.cpp](src/schema.cpp) defines the Arrow columns.

Log timestamps are already in microseconds and are stored as written. The
runtime applies `RC_GLOBAL_START_TIME` before writing the logs; the converter
does not subtract it again. This origin shift does not correct offsets between
PE clocks. In the inspected runtime, the processing receive field is a zero
placeholder, so stored receive and queue-wait values do not measure arrival or
queueing. The creation record's last field is a source-side send duration.
See the [verified reconstruction contract](../notes/reconstruction-contract.md).

### Timesteps

The Charm++ runtime has no notion of a timestep, so `simulation_step.parquet` is populated only when the application declares its own boundaries. To make that happen, register a user event and bracket each step with it, passing the step index as the `nestedID` argument:

``` cpp
// once, at startup
int stepUE = traceRegisterUserEvent("SimulationStep");

// around each timestep
beginUserBracketEvent(stepUE, step);
// ... the timestep ...
endUserBracketEvent(stepUE, step);
```

Pass a different name with `-s` if you register one. If no matching event is found, CharmVZ logs a note and writes an empty `simulation_step.parquet` – an uninstrumented trace is a normal input, not an error.

The table has one row per `(step_id, pe_id)`. Attribute an execution to the step whose interval **on its own PE** contains it: PEs cross a boundary microseconds apart, so the denormalized `global_start_time_us` / `global_end_time_us` columns overlap between adjacent steps and are for display rather than attribution.

### Memory budget and spilling

Message linkage and migration detection sort sends, receiving executions and
array-element locations. Without a budget these records remain in memory.
With `--memory-budget`, external sorters spill sorted runs to disk and merge
them during reconstruction. The budget is a target, not a hard RSS limit:

``` bash
./builddir-rel/charmvz -l traces/ -o out/ --memory-budget 1G --spill-dir /scratch/$USER
```

- The output is identical, row for row, to an unlimited run. Only the time and the disk traffic change.
- The spill directory must be on disk. **A RAM-backed `/tmp` (tmpfs, common on desktop Linux) defeats the budget**: spilled data written there still occupies memory. Check with `df -T /tmp`; if the type is `tmpfs`, pass `--spill-dir` pointing at a real file system.
- Expect to need free disk space on the order of the decompressed log size. A warning is logged when the spill file system looks too small.
- Instance identities, open processing and user-event pairing state, step
  aggregates and output buffers remain resident. Startup refuses a budget below
  the estimated output buffers plus 64 MiB. If resident state prevents meeting
  the target, conversion warns and continues over budget.
- The spill directory (`charmvz-spill-<pid>-<n>`) is removed when the run ends, including when it aborts. A killed run leaves it behind; `--clean-stale-spill` removes those.
- The end-of-run log reports runs and bytes spilled, merge passes and peak memory.

To check a budget under an enforced limit, `tools/constrained_run.sh` runs a conversion in a cgroup (`systemd-run --user`, swap disabled) and compares its output with an unlimited run:

``` bash
tools/constrained_run.sh 1G 512M traces/ out-512M/ /scratch/$USER out-unlimited/
```

## Analysis examples

### Python with polars

``` python
import polars as pl

execution = pl.scan_parquet("out/execution.parquet")
entry_method = pl.scan_parquet("out/entry_method.parquet")

# Time spent per entry method, named rather than numbered
by_ep = (
    execution.group_by("ep_id")
    .agg(
        pl.col("wall_duration_us").sum().alias("total_us"),
        pl.col("wall_duration_us").mean().alias("mean_us"),
        pl.len().alias("calls"),
    )
    .join(entry_method.select("ep_id", "name"), on="ep_id")
    .sort("total_us", descending=True)
)
print(by_ep.collect().head(10))
```

### Python with pandas

``` python
import pandas as pd

df = pd.read_parquet("out/execution.parquet")

# Load imbalance: busy time per PE
per_pe = df.groupby("pe_id")["wall_duration_us"].sum()
print("max/mean =", per_pe.max() / per_pe.mean())

df["wall_duration_us"].hist(bins=50)
```

### R with arrow and dplyr

``` r
library(arrow)
library(dplyr)

execution <- read_parquet("out/execution.parquet")

execution %>%
  group_by(pe_id) %>%
  summarise(
    total_us  = sum(wall_duration_us),
    events    = n(),
    mean_us   = mean(wall_duration_us)
  )
```

### PAPI counters

When the trace was collected with PAPI active, `execution.parquet` carries six fixed counter columns per direction (`papi_begin_0..5`, `papi_end_0..5`, `papi_delta_0..5`). The columns stay numerically indexed; the counter **names** live in the file's Arrow schema metadata, so consumers never have to introspect the schema before querying:

``` python
import pyarrow.parquet as pq

meta = pq.read_schema("out/execution.parquet").metadata
print(meta[b"papi_event_0"])   # e.g. b'PAPI_L2_TCM'
```

## charmvz-viz

The [charmvz-viz](charmvz-viz/) subdirectory is a separate Python package (`charmvz_vis`) that reads the output directory and provides analysis and plotting APIs modelled on the Projections views – time profile, usage profile, entry-point profile, histograms, per-PE communication, and extrema analysis.

``` python
import charmvz_vis as cv

ds = cv.TraceDataset("out/")
cv.time_profile(ds)
cv.usage_profile(ds)
```

See [charmvz-viz/README.md](charmvz-viz/README.md) for setup and the full API.

## Architecture

A four-stage pipeline, orchestrated in `src/main.cpp`. Logs are never read into memory in full.

| Stage | Work |
|----|----|
| 1 | Parse `.sts` and `.projrc`; write the metadata tables; resolve the step-boundary event |
| 2 | Stream each PE log; emit execution, idle, identity, user-event, statistic and memory-sample rows; sort sends, receivers and array locations |
| 3+4 | Merge-join messages, reconstruct placement transitions and processing elements, and fold per-PE application-step boundaries |

High-volume Arrow builders flush at 100,000 rows. Registry tables,
processing elements and simulation steps use whole-table batches. Reconstruction
uses three external sorters ([src/spill.h](src/spill.h)) and a common merge path
for budgeted and unlimited conversion. Receiver groups too large for the budget
can also be spilled. See [memory behavior](#memory-budget-and-spilling) for the
remaining resident state.

Some details that are easy to get wrong and are handled deliberately:

- A chare array writes exactly `ndims` index values, not a fixed four. Reading four consumes an unrelated field as an index for 3-D arrays and under-reads 6-D ones.
- Migrations are derived from PE transitions between consecutive executions of an element. Pack/unpack events are message serialization, not migration, and `CkLocMgr::emigrate()` emits no tracing at all – so a migration has no directly measurable cost in a trace.
- Message linkage matches on `(src_pe, event)`, never on the event serial alone, which is only unique per PE.
- Candidate receivers must also share the creation's entry method. Repeated
  receivers are folded only for verified thread resumes on the same instance;
  ambiguous destinations retain NULL receiver-derived fields.
- Bracketed user events carry a `pe` field that is always 0, so they are attributed to the PE that owns the log file.

## Testing

``` bash
meson test -C builddir-rel
```

Catch2 suites live in `tests/`, one file per unit (`test_sts_parser`, `test_parquet_writer`, `test_log_parser`). To add one, create `tests/test_<unit>.cpp` and append the unit name to the `foreach` list in `meson.build`.

New tests are expected to be **mutation-checked**: revert the behaviour a test guards and confirm the test fails. A test that still passes against the broken version is not testing what it claims to.

## Validation

The shared [schema](../notes/agent_instructions/charm-schema.md) and
[reconstruction contract](../notes/reconstruction-contract.md) state the
semantics checked by the parser and reconstruction tests. The trace-format
reference is [notes/charm-trace-format.md](../notes/charm-trace-format.md).

For a new dataset, convert the trace, run `../ccgrid2027/phase2/ProjValidate.java`
on the same input, then compare its output with
`../ccgrid2027/phase2/compare_projreference.py`. The
[analysis procedures](../ccgrid2027/phase2/README.md) document this harness.
Preserve the input, converter revision, commands and comparison output.
Check identity uniqueness where the schema defines it, PE and step coverage,
and domain-row coverage before interpreting a join. Matched user-event brackets
produce one row per pair, while the reference counts endpoint records.

## Contributing

``` bash
just compile-debug              # ASan + UBSan build
meson test -C builddir-deb
clang-format -i src/*.cpp src/*.h src/utils/* tests/*.cpp
```

## License

See [LICENSE](LICENSE) for details.

## Acknowledgments

- **Projections Team**: for the original algorithms and trace format
- **Charm++ Team**: for the runtime tracing infrastructure

## Related Projects

- [Projections](https://github.com/charmplusplus/projections): interactive GUI-based visualization tool
- [Charm++](https://github.com/UIUC-PPL/charm): parallel programming framework

## Support

- **Issues**: report bugs and feature requests via [GitHub Issues](https://github.com/caschb/charmvz-cpp/issues)
