#ifndef READER_H
#define READER_H
#include "src/utils/log_entry.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct Chare {
  std::string name;
  int64_t dimensions;
  int64_t idx;
};

struct Entry {
  std::string name;
  int64_t chare_id;
  int64_t msg_id;
  int64_t idx;
};

struct Message {
  int64_t size;
  int64_t idx;
};

struct StsData {
  std::vector<Chare> chares;
  std::vector<Entry> entries;
  std::vector<Message> messages;
};

StsData read_sts_file(const std::string_view sts_file_path);

std::vector<LogEntry>
read_log_files(const std::vector<std::string> &log_file_paths);

#endif
