#pragma once
#include "sts_parser.h"
#include "rc_parser.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <tuple>

namespace charmvz {

struct TupleHash {
    template <class T1, class T2>
    std::size_t operator () (const std::tuple<T1,T2>& p) const {
        auto h1 = std::hash<T1>{}(std::get<0>(p));
        auto h2 = std::hash<T2>{}(std::get<1>(p));
        return h1 ^ (h2 << 1);
    }
    template <class T1, class T2, class T3, class T4, class T5>
    std::size_t operator () (const std::tuple<T1,T2,T3,T4,T5>& p) const {
        auto h1 = std::hash<T1>{}(std::get<0>(p));
        auto h2 = std::hash<T2>{}(std::get<1>(p));
        auto h3 = std::hash<T3>{}(std::get<2>(p));
        auto h4 = std::hash<T4>{}(std::get<3>(p));
        auto h5 = std::hash<T5>{}(std::get<4>(p));
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
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
};

struct LogParserResult {
    std::unordered_map<std::tuple<int32_t, int32_t>, CreationRecord, TupleHash> creation_map;
    std::vector<MigrationPack> packs;
    std::vector<MigrationUnpack> unpacks;
    std::vector<ProcessingElementRecord> pes;
    std::unordered_map<std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t>, ChareInstanceRecord, TupleHash> chare_instances;
};

auto process_logs(
    const std::vector<std::string>& log_file_paths,
    const StsData& sts_data,
    const RcData& rc_data,
    const std::string& output_dir
) -> LogParserResult;

} // namespace charmvz
