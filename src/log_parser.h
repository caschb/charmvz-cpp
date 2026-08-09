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

// Where one chare-array instance was executing, and when. Stage 3 sorts these
// per instance and emits a MigrationEpisode wherever `pe_id` changes between
// consecutive executions. Collected only for collections with STS ndims >= 1:
// groups and nodegroups have one instance resident on every PE and never
// migrate, so including them would report every hop between their per-PE
// instances as a migration.
struct InstanceLocationRecord {
  int64_t instance_id;
  int32_t collection_id;
  int32_t pe_id;
  int64_t start_time_us;
  int64_t end_time_us;
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

// One user-event occurrence, ready to be written to user_event.parquet. Every
// optional field carries an explicit `has_*` flag rather than a sentinel,
// because 0 and -1 are both legitimate values for `nested_id`, `event` and
// `user_supplied_int`.
struct UserEventOccurrence {
  int32_t pe_id;
  int32_t record_type;
  int32_t user_event_id;
  bool has_user_event_id;
  std::string name;
  bool has_name;
  int32_t event;
  bool has_event;
  int32_t nested_id;
  bool has_nested_id;
  int64_t start_time_us;
  int64_t end_time_us;
  bool has_end_time;
  int32_t user_supplied_int;
  bool has_user_supplied_int;
  std::string note;
  bool has_note;
};

// One USER_STAT sample before writing. `user_time_s` is absent when the
// application called updateStat() rather than updateStatPair(), which the
// runtime records as -1; the flag keeps that apart from a genuine -1 the
// application chose to pass.
struct UserStatSample {
  int32_t pe_id;
  int32_t stat_id;
  std::string name;
  bool has_name;
  int64_t time_us;
  double stat_value;
  double user_time_s;
  bool has_user_time;
};

// One MEMORY_USAGE_CURRENT sample before writing.
struct MemorySample {
  int32_t pe_id;
  int64_t time_us;
  int64_t bytes;
};

// One PE's occupancy of one timestep, derived from a matched pair of
// step-boundary user events. `end_time_us` is absent when the log ends inside
// the step, which happens if tracing stops mid-run.
struct StepBoundaryRecord {
  int32_t step_id;
  int32_t pe_id;
  int64_t start_time_us;
  int64_t end_time_us;
  bool has_end_time;
};

struct LogParserResult {
  std::unordered_map<std::tuple<int32_t, int32_t>, CreationRecord, TupleHash>
      creation_map;
  std::vector<InstanceLocationRecord> instance_locations;
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
  // Empty unless a step-boundary user event was configured and found. Small by
  // construction -- one entry per (timestep, PE) -- so it is accumulated in
  // memory rather than streamed.
  std::vector<StepBoundaryRecord> step_boundaries;
};

// `step_event_id` selects the registered user event whose brackets delimit a
// timestep; pass NO_STEP_EVENT to skip step reconstruction entirely.
constexpr int32_t NO_STEP_EVENT = -1;

auto process_logs(const std::vector<std::string> &log_file_paths,
                  const StsData &sts_data, const RcData &rc_data,
                  const std::string &output_dir,
                  int32_t step_event_id = NO_STEP_EVENT) -> LogParserResult;

} // namespace charmvz
