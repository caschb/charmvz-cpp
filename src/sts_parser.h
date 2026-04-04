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

struct StsData {
  std::string version;
  int32_t total_phases = 0;
  int32_t total_pes = 0;
  int32_t total_papi_events = 0;
  std::vector<std::string> papi_event_names;
  std::vector<ChareCollectionRecord> chares;
  std::vector<EntryMethodRecord> entries;
  std::vector<MessageTypeRecord> messages;

  std::unordered_map<int32_t, ChareCollectionRecord> chare_map;
  std::unordered_map<int32_t, EntryMethodRecord> ep_map;
  std::unordered_map<int32_t, MessageTypeRecord> message_map;
};

auto parse_sts_file(const std::string_view sts_file_path) -> StsData;

} // namespace charmvz
