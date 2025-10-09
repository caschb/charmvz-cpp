#ifndef LOG_READER_H
#define LOG_READER_H
#include "log_entry.h"
#include "zstr.hpp"

class LogReader {
private:
  std::string log_file_path;
  zstr::ifstream log_stream;
  LogEntry last_begin_event;

public:
  LogReader(const std::string_view log_file_path)
      : log_file_path(log_file_path) {
    this->log_stream.open(log_file_path.data());
    std::string first_line;
    std::getline(this->log_stream, first_line);
  }
  ~LogReader();
  LogEntry nextEntry();
  bool hasNextEntry();
  const LogEntry *getLastBeginEvent() const;
};
#endif