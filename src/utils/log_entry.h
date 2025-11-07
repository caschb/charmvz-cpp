/**
 * @file log_entry.h
 * @brief Definitions for Charm++ log entry types and structures
 */

#ifndef LOG_ENTRY_H
#define LOG_ENTRY_H
#include "spdlog/spdlog.h"
#include <cstdint>
#include <string>

/// Special entry point ID indicating idle time
constexpr int64_t IDLE_ENTRY = -1;

/**
 * @enum LogType
 * @brief Enumeration of Charm++ log entry types
 */
enum class LogType {
  UNKNOWN = -1,          ///< Unknown or unrecognized log type
  CREATION = 1,          ///< Object creation event
  BEGIN_PROCESSING = 2,  ///< Start of message processing
  END_PROCESSING = 3,    ///< End of message processing
  BEGIN_COMPUTATION = 6, ///< Start of computation
  END_COMPUTATION = 7,   ///< End of computation
  USER_EVENT = 13,       ///< User-defined event
  BEGIN_IDLE = 14,       ///< Start of idle time
  END_IDLE = 15,         ///< End of idle time
  BEGIN_PACK = 16,       ///< Start of message packing
  END_PACK = 17,         ///< End of message packing
  BEGIN_UNPACK = 18,     ///< Start of message unpacking
  END_UNPACK = 19,       ///< End of message unpacking
  CREATION_BCAST = 20,   ///< Broadcast creation event
  END_PHASE = 30,        ///< End of a computation phase
  USER_EVENT_PAIR = 100, ///< Paired user-defined event
};

/**
 * @struct LogEntry
 * @brief Represents a single entry from a Charm++ log file
 */
struct LogEntry {
  LogType type;      ///< Type of log entry
  int64_t mtype;     ///< Message type ID
  int64_t timestamp; ///< Timestamp of the event
  int64_t entry;     ///< Entry method ID
  int64_t event;     ///< Event ID
  int64_t pe;        ///< Processing element (PE) ID
  bool open;         ///< Whether this is an opening event
};

/**
 * @brief Convert a LogType to its string representation
 * @param type The LogType to convert
 * @return String representation of the log type
 */
std::string to_string(const LogType &type);

/**
 * @brief Formatter specialization for LogEntry to enable formatted output
 */
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