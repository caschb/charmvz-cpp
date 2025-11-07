/**
 * @file log_reader.h
 * @brief Reader class for processing Charm++ log files
 */

#ifndef LOG_READER_H
#define LOG_READER_H
#include "log_entry.h"
#include "zstr.hpp"

/**
 * @class LogReader
 * @brief Provides sequential access to log entries from compressed Charm++ log
 * files
 */
class LogReader {
private:
  std::string log_file_path; ///< Path to the log file being read
  zstr::ifstream log_stream; ///< Compressed file input stream
  LogEntry last_begin_event; ///< Last BEGIN event encountered

public:
  /**
   * @brief Construct a LogReader for the specified log file
   * @param log_file_path Path to the compressed log file
   */
  LogReader(const std::string_view log_file_path)
      : log_file_path(log_file_path) {
    this->log_stream.open(log_file_path.data());
    std::string first_line;
    std::getline(this->log_stream, first_line);
  }

  /**
   * @brief Destructor
   */
  ~LogReader();

  /**
   * @brief Read and return the next log entry
   * @return The next LogEntry from the file
   */
  auto nextEntry() -> LogEntry;

  /**
   * @brief Check if there are more entries to read
   * @return true if more entries are available, false otherwise
   */
  auto hasNextEntry() -> bool;

  /**
   * @brief Get the last BEGIN event that was read
   * @return Pointer to the last BEGIN event, or nullptr if none
   */
  auto getLastBeginEvent() const -> const LogEntry *;
};
#endif