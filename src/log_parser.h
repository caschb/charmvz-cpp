#pragma once
#include "rc_parser.h"
#include "sts_parser.h"
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace charmvz {

struct TupleHash {
  // Variadic so the chare-instance key can carry a collection id plus all
  // CHARE_INDEX_SLOTS dimensions. Combines element hashes with an increasing
  // left shift, matching what the fixed 2- and 5-element versions did.
  template <class... Ts>
  std::size_t operator()(const std::tuple<Ts...> &p) const {
    std::size_t hash = 0;
    std::size_t shift = 0;
    std::apply(
        [&](const auto &...elems) {
          ((hash ^= std::hash<std::decay_t<decltype(elems)>>{}(elems)
                    << shift++),
           ...);
        },
        p);
    return hash;
  }
};

struct CreationRecord {
  int32_t ep_id;
  int32_t msg_idx;
  int32_t msg_len;
  int64_t send_time_us;
  int64_t enqueue_time_us;
  bool is_broadcast;
  int32_t broadcast_fanout;
  int32_t src_pe;
  std::vector<int32_t> dst_pes;
};

struct MigrationPack {
  int32_t src_pe;
  int64_t pack_start_us;
  int64_t pack_end_us;
};

struct MigrationUnpack {
  int32_t dst_pe;
  int64_t unpack_start_us;
  int64_t unpack_end_us;
};

struct ProcessingElementRecord {
  int32_t pe_id;
  int32_t total_pes;
  int64_t begin_time_us;
  int64_t end_time_us;
  int64_t global_start_us;
};

struct ChareInstanceRecord {
  int64_t instance_id;
  int32_t collection_id;
  int32_t index_0;
  int32_t index_1;
  int32_t index_2;
  int32_t index_3;
  int32_t index_4;
  int32_t index_5;
};

struct BeginProcessingRecord {
  int32_t dst_pe;
  int64_t recv_time_us;
  int64_t exec_start_time_us;
};

struct LogParserResult {
  std::unordered_map<std::tuple<int32_t, int32_t>, CreationRecord, TupleHash>
      creation_map;
  std::vector<MigrationPack> packs;
  std::vector<MigrationUnpack> unpacks;
  std::vector<ProcessingElementRecord> pes;
  // Keyed on (collection_id, index_0 .. index_5) -- the chare instance's
  // natural key.
  std::unordered_map<
      std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>,
      ChareInstanceRecord, TupleHash>
      chare_instances;
  std::unordered_map<std::tuple<int32_t, int32_t>, BeginProcessingRecord,
                     TupleHash>
      begin_processing_map;
};

auto process_logs(const std::vector<std::string> &log_file_paths,
                  const StsData &sts_data, const RcData &rc_data,
                  const std::string &output_dir) -> LogParserResult;

} // namespace charmvz
