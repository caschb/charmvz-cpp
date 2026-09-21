#include "rc_parser.h"
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>

namespace charmvz {

auto parse_rc_file(const std::string_view rc_file_path) -> RcData {
  RcData data;
  if (rc_file_path.empty()) {
    spdlog::warn("No .projrc file: no clock reference; cross-PE derived "
                 "columns will be NULL and migrations are not reconstructed");
    return data;
  }
  std::ifstream f{std::string(rc_file_path)};
  if (!f.is_open()) {
    spdlog::warn("Cannot open .projrc file {}: treating the clock reference "
                 "as unavailable",
                 rc_file_path);
    return data;
  }

  bool has_start = false;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty())
      continue;
    std::istringstream iss(line);
    std::string token;
    iss >> token;

    if (token == "RC_GLOBAL_START_TIME") {
      int64_t value = 0;
      if (iss >> value) {
        data.global_start_time_us = value;
        has_start = true;
      }
    } else if (token == "RC_GLOBAL_END_TIME") {
      iss >> data.global_end_time_us;
    }
  }
  if (!has_start) {
    spdlog::warn("{} carries no RC_GLOBAL_START_TIME: treating the clock "
                 "reference as unavailable",
                 rc_file_path);
    return data;
  }
  data.available = true;
  return data;
}

} // namespace charmvz
