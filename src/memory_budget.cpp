#include "memory_budget.h"
#include "builders.h"
#include "schema.h"

#include <algorithm>
#include <arrow/type.h>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace charmvz {

namespace {

// Bytes one row of `schema` occupies in Arrow builders. Strings are charged a
// guessed average length; they only occur in user_event (names and notes).
auto row_bytes(const arrow::Schema &schema) -> uint64_t {
  constexpr uint64_t kAssumedStringBytes = 32;
  uint64_t bits = 0;
  for (const auto &field : schema.fields()) {
    bits += 1; // validity
    if (const auto *fixed =
            dynamic_cast<const arrow::FixedWidthType *>(field->type().get())) {
      bits += static_cast<uint64_t>(fixed->bit_width());
    } else {
      bits += (kAssumedStringBytes + sizeof(int32_t)) * 8;
    }
  }
  return (bits + 7) / 8;
}

} // namespace

auto parse_size(const std::string &text) -> uint64_t {
  size_t pos = 0;
  while (pos < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[pos])))
    ++pos;
  if (pos == 0)
    throw std::invalid_argument("not a size: \"" + text + "\"");

  uint64_t value = 0;
  for (size_t i = 0; i < pos; ++i) {
    const auto digit = static_cast<uint64_t>(text[i] - '0');
    if (value > (UINT64_MAX - digit) / 10)
      throw std::invalid_argument("size out of range: \"" + text + "\"");
    value = (value * 10) + digit;
  }

  std::string suffix;
  for (size_t i = pos; i < text.size(); ++i)
    suffix +=
        static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
  if (suffix.ends_with("IB"))
    suffix.resize(suffix.size() - 2);
  else if (suffix.size() > 1 && suffix.ends_with("B"))
    suffix.resize(suffix.size() - 1);

  int shift = 0;
  if (suffix.empty() || suffix == "B")
    shift = 0;
  else if (suffix == "K")
    shift = 10;
  else if (suffix == "M")
    shift = 20;
  else if (suffix == "G")
    shift = 30;
  else if (suffix == "T")
    shift = 40;
  else
    throw std::invalid_argument("unknown size suffix in \"" + text + "\"");

  if (shift > 0 && value > (UINT64_MAX >> shift))
    throw std::invalid_argument("size out of range: \"" + text + "\"");
  return value << shift;
}

auto format_size(uint64_t bytes) -> std::string {
  static constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  auto value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(units)) {
    value /= 1024.0;
    ++unit;
  }
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.1f %s", value, units[unit]);
  return buffer;
}

auto builder_floor_bytes(const std::vector<std::string> &papi_event_names)
    -> uint64_t {
  // Stage 2 fills six builders at once; Stage 3 starts after they are
  // flushed and fills the message and migration builders.
  const uint64_t stage2 =
      row_bytes(*schema::execution(papi_event_names)) +
      row_bytes(*schema::idle_interval()) +
      row_bytes(*schema::chare_instance()) + row_bytes(*schema::user_event()) +
      row_bytes(*schema::user_stat()) + row_bytes(*schema::memory_sample());
  const uint64_t stage3 =
      row_bytes(*schema::message()) + row_bytes(*schema::migration_episode());
  constexpr uint64_t kCopies = 3; // builder capacity doubling + encoder
  return std::max(stage2, stage3) * builders::ROW_GROUP_SIZE * kCopies;
}

} // namespace charmvz
