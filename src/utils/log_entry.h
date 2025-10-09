#ifndef LOG_ENTRY_H
#define LOG_ENTRY_H
#include "spdlog/spdlog.h"
#include <cstdint>
#include <string>

constexpr int64_t IDLE_ENTRY = -1;

enum class LogType {
  UNKNOWN = -1,
  CREATION = 1,
  BEGIN_PROCESSING = 2,
  END_PROCESSING = 3,
  BEGIN_COMPUTATION = 6,
  END_COMPUTATION = 7,
  USER_EVENT = 13,
  BEGIN_IDLE = 14,
  END_IDLE = 15,
  BEGIN_PACK = 16,
  END_PACK = 17,
  BEGIN_UNPACK = 18,
  END_UNPACK = 19,
  CREATION_BCAST = 20,
  END_PHASE = 30,
  USER_EVENT_PAIR = 100,
};

struct LogEntry {
  LogType type;
  int64_t mtype;
  int64_t timestamp;
  int64_t entry;
  int64_t event;
  int64_t pe;
  bool open;
};

std::string to_string(const LogType &type);

template <> struct spdlog::fmt_lib::formatter<LogEntry> {
  constexpr auto parse(spdlog::fmt_lib::format_parse_context &ctx) {
    return ctx.begin();
  }

  auto format(const LogEntry &entry,
              spdlog::fmt_lib::format_context &ctx) const {
    return spdlog::fmt_lib::format_to(
        ctx.out(),
        "LogEntry{{type: {}, mtype: {}, timestamp: {}, "
        "entry: {}, event: {}, pe: {}}}",
        to_string(entry.type), entry.mtype, entry.timestamp, entry.entry,
        entry.event, entry.pe);
  }
};
#endif