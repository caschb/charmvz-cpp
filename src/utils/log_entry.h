#pragma once

#include "spdlog/spdlog.h"
#include <cstdint>
#include <string>
#include <vector>

constexpr int32_t IDLE_ENTRY = -1;
constexpr size_t NUMPAPIEVENTS = 6;

// Chare-index slots stored per chare instance. Matches the `int[6]` used by the
// Projections Java reader (misc/LogEntry.java); Charm++ can in principle write
// up to 8 dimensions, and anything beyond this many is consumed but not stored.
constexpr size_t CHARE_INDEX_SLOTS = 6;

// A non-array chare (STS ndims == -1) writes exactly four index values, per
// charm/src/ck-perf/trace-projections.C.
constexpr int32_t NON_ARRAY_INDEX_COUNT = 4;

enum class LogType {
  UNKNOWN = -1,
  CREATION = 1,
  BEGIN_PROCESSING = 2,
  END_PROCESSING = 3,
  ENQUEUE = 4,
  DEQUEUE = 5,
  BEGIN_COMPUTATION = 6,
  END_COMPUTATION = 7,
  BEGIN_INTERRUPT = 8,
  END_INTERRUPT = 9,
  MESSAGE_RECV = 10,
  BEGIN_TRACE = 11,
  END_TRACE = 12,
  USER_EVENT = 13,
  BEGIN_IDLE = 14,
  END_IDLE = 15,
  BEGIN_PACK = 16,
  END_PACK = 17,
  BEGIN_UNPACK = 18,
  END_UNPACK = 19,
  CREATION_BCAST = 20,
  CREATION_MULTICAST = 21,
  MEMORY_MALLOC = 24,
  MEMORY_FREE = 25,
  USER_SUPPLIED = 26,
  MEMORY_USAGE_CURRENT = 27,
  USER_SUPPLIED_NOTE = 28,
  USER_SUPPLIED_BRACKETED_NOTE = 29,
  END_PHASE = 30,
  SURROGATE_BLOCK = 31,
  USER_STAT = 32,
  BEGIN_USER_EVENT_PAIR = 98,
  END_USER_EVENT_PAIR = 99,
  USER_EVENT_PAIR = 100,
};

struct LogEntry {
  LogType type;
  uint16_t mIdx;
  uint16_t eIdx;
  uint64_t itime;
  int32_t event;
  int32_t pe;
  int32_t msglen;
  uint64_t irecvtime;
  uint64_t icputime;
  int32_t id[CHARE_INDEX_SLOTS];
  uint64_t papiValues[NUMPAPIEVENTS];
  int32_t numpes;
  std::vector<int32_t> pes;
  // End timestamp of a self-contained bracketed record
  // (USER_SUPPLIED_BRACKETED_NOTE).
  uint64_t iEndTime;
  // Application-supplied nesting id carried by the bracketed user-event forms
  // (codes 98, 99, 100). By the convention this pipeline consumes it also
  // carries the timestep index -- see schema::simulation_step().
  int32_t nestedID;
  // The integer datum of a USER_SUPPLIED record.
  int32_t userSuppliedData;
  // The text of a USER_SUPPLIED_NOTE / USER_SUPPLIED_BRACKETED_NOTE record.
  std::string userSuppliedNote;
  // The statistic value of a USER_STAT record.
  double stat;
  // The application-supplied time of a USER_STAT record, written raw rather
  // than as integer microseconds (trace-projections.C:776). -1 means the
  // application called updateStat() and supplied none.
  double statTime;
  // The byte count of a MEMORY_USAGE_CURRENT record.
  uint64_t memUsage;
};

auto to_string(const LogType &type) -> const char *;

template <> struct spdlog::fmt_lib::formatter<LogEntry> {
  constexpr auto parse(spdlog::fmt_lib::format_parse_context &ctx) {
    return ctx.begin();
  }

  auto format(const LogEntry &entry,
              spdlog::fmt_lib::format_context &ctx) const {
    return spdlog::fmt_lib::format_to(ctx.out(),
                                      "LogEntry{{type: {}, mIdx: {}, itime: "
                                      "{}, eIdx: {}, event: {}, pe: {}}}",
                                      to_string(entry.type), entry.mIdx,
                                      entry.itime, entry.eIdx, entry.event,
                                      entry.pe);
  }
};