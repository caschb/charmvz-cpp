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

// One CREATION / CREATION_BCAST / CREATION_MULTICAST record, kept in log
// order. `send_to_enqueue_us` is the record's last field: creationDone()
// (charm/src/ck-perf/trace-projections.C) stores `curTime - log.time`, the
// source-side interval from the CREATION to the moment the send call returned
// after _CldEnqueue(). It is a duration on the source PE's clock, not a
// destination timestamp, and zero is a legitimate value.
struct CreationRecord {
  int32_t src_pe;
  int32_t event;
  int32_t ep_id;
  int32_t msg_idx;
  int32_t msg_len;
  int64_t send_time_us;
  int64_t send_to_enqueue_us;
  bool is_broadcast;
  int32_t broadcast_fanout;
  std::vector<int32_t> dst_pes;
};

// Where one chare-array instance was executing, and when. Stage 3 sorts these
// per instance and emits a MigrationEpisode wherever `pe_id` changes between
// consecutive executions. Collected only for collections with STS ndims >= 1:
// groups and nodegroups have one instance resident on every PE and never
// migrate, so including them would report every hop between their per-PE
// instances as a migration.
//
// An execution whose END_PROCESSING is missing is still a location: leaving it
// out would let the sequence step from the execution before it to the one
// after it as if the element had never been observed in between.
struct InstanceLocationRecord {
  int64_t instance_id;
  int32_t collection_id;
  int32_t pe_id;
  int64_t start_time_us;
  int64_t end_time_us;
  bool has_end_time;
};

// One row of processing_element.parquet: exactly one per valid PE log, whether
// or not the log carries its computation markers.
struct ProcessingElementRecord {
  int32_t pe_id;
  int32_t total_pes;
  int64_t begin_time_us;
  bool has_begin_time;
  int64_t end_time_us;
  bool has_end_time;
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

// The receiver side of a message: one BEGIN_PROCESSING, retained under the
// (src_pe, event) pair its `pe` and `event` fields carry. `recv_time_us` is the
// record's irecvtime; the runtime writes -1 when it has no value
// (`has_recv_time` false), and beginExecuteLocal() otherwise passes a constant
// 0.0, so the field is retained as written rather than interpreted.
// `instance_id` and `msg_len` are what tells a repeated processing of one
// delivery (a threaded entry method logs a BEGIN_PROCESSING per resume) from
// two different observations that happen to share the pair.
struct BeginProcessingRecord {
  int32_t dst_pe;
  int32_t ep_id;
  int64_t instance_id;
  int32_t msg_len;
  int64_t recv_time_us;
  bool has_recv_time;
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

// Counts of the records Stage 2 could not pair, so a run's completeness is
// visible without grepping the log output.
struct LogParseDiagnostics {
  int64_t malformed_records = 0;
  int64_t incomplete_executions = 0;
  int64_t orphan_end_processing = 0;
  int64_t mismatched_end_processing = 0;
  int64_t duplicate_open_executions = 0;
  int64_t incomplete_idle_intervals = 0;
  int64_t orphan_end_idle = 0;
  int64_t logs_without_begin_computation = 0;
  int64_t logs_without_end_computation = 0;
};

struct LogParserResult {
  // The STS index of the runtime's "dummy_thread_ep", or -1 when the STS
  // registers none. A BEGIN_PROCESSING of this entry method is a resume of a
  // threaded entry method (beginExecute(CmiObjId*)), logged under the serial
  // of the message that started the thread; it is the only case in which
  // several receivers under one pair are known to be one delivery.
  int32_t thread_ep_id = -1;
  // In log order; a (src_pe, event) pair that the runtime reuses appears as
  // many times as it was written, never collapsed.
  std::vector<CreationRecord> creations;
  std::vector<InstanceLocationRecord> instance_locations;
  std::vector<ProcessingElementRecord> pes;
  LogParseDiagnostics diagnostics;
  // Keyed on (collection_id, index_0 .. index_5) -- the chare instance's
  // natural key.
  std::unordered_map<
      std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>,
      ChareInstanceRecord, TupleHash>
      chare_instances;
  // Every BEGIN_PROCESSING keyed on its (pe, event) fields. A multimap because
  // the pair is not unique on the receiving side: a multicast is processed on
  // each of its destinations, and a threaded entry method logs one
  // BEGIN_PROCESSING per resume under the serial of the message that started
  // it.
  std::unordered_multimap<std::tuple<int32_t, int32_t>, BeginProcessingRecord,
                          TupleHash>
      begin_processing_map;
  // Empty unless a step-boundary user event was configured and found. Small by
  // construction -- one entry per (timestep, PE) -- so it is accumulated in
  // memory rather than streamed.
  std::vector<StepBoundaryRecord> step_boundaries;
};

// `step_event_id` selects the registered user event whose brackets delimit a
// timestep; pass NO_STEP_EVENT to skip step reconstruction entirely.
//
// Throws std::runtime_error when a BEGIN_PROCESSING names an entry method the
// STS does not register: its index arity is then unknown and the rest of the
// record cannot be read, so the conversion aborts rather than guessing.
constexpr int32_t NO_STEP_EVENT = -1;

auto process_logs(const std::vector<std::string> &log_file_paths,
                  const StsData &sts_data, const RcData &rc_data,
                  const std::string &output_dir,
                  int32_t step_event_id = NO_STEP_EVENT) -> LogParserResult;

} // namespace charmvz
