#pragma once
#include <string_view>
#include <cstdint>

namespace charmvz {

struct RcData {
  int64_t global_start_time_us = 0;
  int64_t global_end_time_us = 0;
};

auto parse_rc_file(const std::string_view rc_file_path) -> RcData;

} // namespace charmvz
