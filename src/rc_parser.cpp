#include "rc_parser.h"
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>

namespace charmvz {

auto parse_rc_file(const std::string_view rc_file_path) -> RcData {
  RcData data;
  std::ifstream f{std::string(rc_file_path)};
  if (!f.is_open()) {
    spdlog::error("Cannot open .projrc file: {}", rc_file_path);
    return data;
  }

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty())
      continue;
    std::istringstream iss(line);
    std::string token;
    iss >> token;

    if (token == "RC_GLOBAL_START_TIME") {
      iss >> data.global_start_time_us;
    } else if (token == "RC_GLOBAL_END_TIME") {
      iss >> data.global_end_time_us;
    }
  }
  return data;
}

} // namespace charmvz
