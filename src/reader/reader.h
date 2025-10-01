#ifndef READER_H
#define READER_H
#include <cstdint>
#include <string_view>
#include <string>

void read_sts_file(std::string_view sts_file_path);

struct Chare {
  std::string name;
  int64_t dimensions;
  int64_t idx;
};


#endif
