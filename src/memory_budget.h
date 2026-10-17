#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace charmvz {

// Parses a byte count such as "512M", "4G" or "1048576". Suffixes K, M, G and
// T are binary (KiB, MiB, ...) and may be followed by "B" or "iB"; case is
// ignored. "0" means no budget. Throws std::invalid_argument on anything else,
// including a count that overflows 64 bits.
auto parse_size(const std::string &text) -> uint64_t;

// "512.0 MiB" style, for diagnostics.
auto format_size(uint64_t bytes) -> std::string;

// An estimate of the memory the pipeline needs whatever the budget: the Arrow
// builders of every streamed table at one full row group, doubled for the
// builders' capacity growth, plus the Parquet encoder's copy of the row group
// being written. It depends on the trace only through the number of PAPI
// counter columns. Not a bound: the budget enforcement measures the Arrow pool
// instead of trusting this.
auto builder_floor_bytes(const std::vector<std::string> &papi_event_names)
    -> uint64_t;

// The smallest budget worth running with: the floor plus room for the spill
// buffers. Anything below it would spill after every few records.
inline constexpr uint64_t kBudgetHeadroomBytes = uint64_t{64} << 20;

// Resident cost of one entry in LogParserResult::chare_instances (node, key,
// record and bucket on libstdc++/x86-64), and of one open BEGIN_PROCESSING
// awaiting its END (a LogEntry with its string and vector). Used to account
// for state that is never spilled.
inline constexpr uint64_t kChareInstanceBytes = 110;
inline constexpr uint64_t kOpenExecutionBytes = 250;

} // namespace charmvz
