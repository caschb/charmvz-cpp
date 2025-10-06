#ifndef READER_H
#define READER_H
#include <cstdint>
#include <string_view>
#include <string>
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

StsData read_sts_file(std::string_view sts_file_path);

#endif
