#ifndef READER_H
#define READER_H
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

enum class LogType {
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
  int64_t msg_id;
  int64_t event_id;
  int64_t timestamp;
  int64_t event;
  int64_t processing_element;
  int64_t message_len;
  int64_t recvtime;
  int64_t user_event_id;    // For USER_EVENT and USER_EVENT_PAIR
  int64_t user_event_value; // For USER_EVENT_PAIR
};

StsData read_sts_file(std::string_view sts_file_path);

std::vector<LogEntry>
read_log_files(const std::vector<std::string> &log_file_paths);

#endif
