#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace charmvz {

struct ChareCollectionRecord {
  int32_t collection_id;
  std::string name;
  int32_t ndims;
};

struct EntryMethodRecord {
  int32_t ep_id;
  std::string name;
  int32_t collection_id;
  int32_t msg_idx;
};

struct MessageTypeRecord {
  int32_t msg_idx;
  uint32_t size;
};

// One `EVENT <id> <name>` record: a user event registered by the application
// via traceRegisterUserEvent(). `event_id` is the integer the application
// chose, and is what a USER_EVENT log record carries in its `mIdx` field.
struct UserEventRecord {
  int32_t event_id;
  std::string name;
};

// One `STAT <id> <name>` record: a user stat counter registered via
// traceRegisterUserStat(), referenced by USER_STAT log records.
struct UserStatRecord {
  int32_t stat_id;
  std::string name;
};

struct StsData {
  std::string version;
  int32_t total_phases = 0;
  int32_t total_pes = 0;
  int32_t total_papi_events = 0;
  std::vector<std::string> papi_event_names;
  int32_t total_user_events = 0;
  int32_t total_user_stats = 0;
  std::vector<ChareCollectionRecord> chares;
  std::vector<EntryMethodRecord> entries;
  std::vector<MessageTypeRecord> messages;
  std::vector<UserEventRecord> user_events;
  std::vector<UserStatRecord> user_stats;

  std::unordered_map<int32_t, ChareCollectionRecord> chare_map;
  std::unordered_map<int32_t, EntryMethodRecord> ep_map;
  std::unordered_map<int32_t, MessageTypeRecord> message_map;
  std::unordered_map<int32_t, UserEventRecord> user_event_map;
  std::unordered_map<int32_t, UserStatRecord> user_stat_map;
};

auto parse_sts_file(const std::string_view sts_file_path) -> StsData;

// Resolves a registered user event by the name the application passed to
// traceRegisterUserEvent(). Returns -1 when no EVENT record carries that name.
// Names are compared exactly, including interior spaces: the STS EVENT line is
// `EVENT <id> <name>` with the name running to end of line, so multi-word names
// such as "Particle interaction" are ordinary.
auto find_user_event_id(const StsData &sts_data, std::string_view name)
    -> int32_t;

} // namespace charmvz
