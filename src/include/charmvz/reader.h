/**
 * @file reader.h
 * @brief Functions and structures for reading Charm++ trace files
 */

#ifndef CHARMVZ_READER_H
#define CHARMVZ_READER_H

#include "charmvz/timeline.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace charmvz {
/**
 * @struct Chare
 * @brief Represents a Charm++ chare definition
 */
struct Chare {
  std::string name;   ///< Name of the chare
  int64_t dimensions; ///< Number of dimensions for array chares
  int64_t idx;        ///< Index in the chare table
};

/**
 * @struct Entry
 * @brief Represents a Charm++ entry method definition
 */
struct Entry {
  std::string name; ///< Name of the entry method
  int64_t chare_id; ///< ID of the chare this entry belongs to
  int64_t msg_id;   ///< ID of the message type for this entry
  int64_t idx;      ///< Index in the entry table
};

/**
 * @struct Message
 * @brief Represents a Charm++ message type definition
 */
struct Message {
  int64_t size; ///< Size of the message in bytes
  int64_t idx;  ///< Index in the message table
};

/**
 * @struct StsData
 * @brief Container for all static trace summary data
 */
struct StsData {
  std::vector<Chare> chares;     ///< List of chare definitions
  std::vector<Entry> entries;    ///< List of entry method definitions
  std::vector<Message> messages; ///< List of message type definitions
};

/**
 * @brief Reads and parses an STS (Static Trace Summary) file.
 *
 * @param sts_file_path Path to the STS file to be read
 * @return StsData Structure containing chares, entries, and messages from the
 * file
 */
auto read_sts_file(std::string_view sts_file_path) -> StsData;

/**
 * @brief Reads and parses multiple log files into Timeline objects.
 *
 * @param log_file_paths Vector of paths to log files to be processed
 * @return std::vector<Timeline> Vector of Timeline objects parsed from the log
 * files
 */
auto read_log_files(const std::vector<std::string> &log_file_paths)
    -> std::vector<Timeline>;

} // namespace charmvz


#endif
