#pragma once
#include <cstdint>
#include <string_view>

namespace charmvz {

// The run's clock reference, from `<pgm>.projrc`.
//
// `available` is false when the file is absent, unreadable, or carries no
// RC_GLOBAL_START_TIME; it is true for a valid zero offset, which is what every
// trace produced with a non-absolute Converse timer records. The distinction
// matters because a zero offset is a legitimate origin, whereas a missing file
// leaves no evidence that the per-PE timestamps share one, so the cross-PE
// derivations gated on `clock_reference_usable()` must stay NULL.
//
// What the value is, per charm/src/ck-perf/trace-projections.C
// (TraceProjectionsBOC::startTimeAnalysis/startTimeDone, LogPool::writeRC):
// the minimum over PEs of the first buffered record's time, computed only when
// CmiTimerAbsolute() is set, and subtracted by the runtime from every buffered
// record on every PE (LogPool::setNewStartTime) *before the logs are written*.
// The log files therefore already carry the shift; the pipeline stores
// timestamps as the logs hold them and never subtracts the value again. It is
// kept as `global_start_us` metadata only. It shifts a common origin; it does
// not measure or correct per-PE clock offsets.
struct RcData {
  bool available = false;
  int64_t global_start_time_us = 0;
  int64_t global_end_time_us = 0;

  [[nodiscard]] auto clock_reference_usable() const -> bool {
    return available;
  }
};

// An empty path means "no .projrc was found"; the result is unavailable.
auto parse_rc_file(const std::string_view rc_file_path) -> RcData;

} // namespace charmvz
