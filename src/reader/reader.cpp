#include "reader.h"
#include <string_view>
#include <fstream>
#include <sstream>
#include "spdlog/spdlog.h"
#include <vector>

void read_sts_file(const std::string_view sts_file_path) {

  spdlog::debug("Reading {}", sts_file_path);
  std::ifstream sts_file{sts_file_path.data(), std::ios::in};
  std::vector<Chare> chares;
  while (sts_file) {
    std::string line;
    std::getline(sts_file, line);
    std::istringstream line_stream{line};
    std::string token;
    line_stream >> token;
    if (token == "CHARE") {
      int64_t idx, ndims;
      std::string chare_name;
      line_stream >> idx >> chare_name >> ndims;

      Chare chare{chare_name.substr(1, chare_name.length() - 2), ndims, idx};
      chares.push_back(chare);
    } else if (token == "ENTRY") {
      int64_t idx, chare_id, msg_id;
      std::string chare_name;
      line_stream >> chare_name;
      line_stream >> idx >> chare_name >> chare_id >> msg_id;
      spdlog::trace("{} {} {} {}", idx, chare_name, chare_id, msg_id);
    }      
  }
  sts_file.close();
}
