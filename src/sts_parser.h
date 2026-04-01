#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

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

struct StsData {
  int32_t total_pes = 0;
  int32_t total_papi_events = 0;
  std::vector<ChareCollectionRecord> chares;
  std::vector<EntryMethodRecord> entries;
  
  std::unordered_map<int32_t, ChareCollectionRecord> chare_map;
  std::unordered_map<int32_t, EntryMethodRecord> ep_map;
};

auto parse_sts_file(const std::string_view sts_file_path) -> StsData;

} // namespace charmvz
