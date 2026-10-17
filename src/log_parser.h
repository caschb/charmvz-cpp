#pragma once
#include "rc_parser.h"
#include "spill.h"
#include "sts_parser.h"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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

// One CREATION / CREATION_BCAST / CREATION_MULTICAST record. `seq` is its
// ordinal in Stage 2's read order, which keeps records sharing a (src_pe,
// event) pair in log order once they are sorted. `send_to_enqueue_us` is the
// record's last field: creationDone() (charm/src/ck-perf/trace-projections.C)
// stores `curTime - log.time`, the source-side interval from the CREATION to
// the moment the send call returned after _CldEnqueue(). It is a duration on
// the source PE's clock, not a destination timestamp, and zero is a legitimate
// value.
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
  int64_t seq;
  std::vector<int32_t> dst_pes;
};

inline auto record_heap_bytes(const CreationRecord &record) -> std::size_t {
  return record.dst_pes.capacity() * sizeof(int32_t);
}

template <> struct RunCodec<CreationRecord> {
  static constexpr uint32_t type_id = 1;
  static void encode(RunWriter &out, const CreationRecord &r) {
    out.put(r.src_pe);
    out.put(r.event);
    out.put(r.ep_id);
    out.put(r.msg_idx);
    out.put(r.msg_len);
    out.put(r.send_time_us);
    out.put(r.send_to_enqueue_us);
    out.put(static_cast<uint8_t>(r.is_broadcast));
    out.put(r.broadcast_fanout);
    out.put(r.seq);
    out.put(static_cast<uint32_t>(r.dst_pes.size()));
    out.put_bytes(r.dst_pes.data(), r.dst_pes.size() * sizeof(int32_t));
  }
  static void decode(RunReader &in, CreationRecord &r) {
    r.src_pe = in.get<int32_t>();
    r.event = in.get<int32_t>();
    r.ep_id = in.get<int32_t>();
    r.msg_idx = in.get<int32_t>();
    r.msg_len = in.get<int32_t>();
    r.send_time_us = in.get<int64_t>();
    r.send_to_enqueue_us = in.get<int64_t>();
    r.is_broadcast = in.get<uint8_t>() != 0;
    r.broadcast_fanout = in.get<int32_t>();
    r.seq = in.get<int64_t>();
    r.dst_pes.resize(in.get<uint32_t>());
    in.get_bytes(r.dst_pes.data(), r.dst_pes.size() * sizeof(int32_t));
  }
};

struct CreationOrder {
  auto operator()(const CreationRecord &a, const CreationRecord &b) const
      -> bool {
    return std::tie(a.src_pe, a.event, a.seq) <
           std::tie(b.src_pe, b.event, b.seq);
  }
};

// Where one chare-array instance was executing, and when. Stage 3 reads these
// per instance in time order and emits a MigrationEpisode wherever `pe_id`
// changes between consecutive executions. Collected only for collections with
// STS ndims >= 1: groups and nodegroups have one instance resident on every PE
// and never migrate, so including them would report every hop between their
// per-PE instances as a migration.
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

template <> struct RunCodec<InstanceLocationRecord> {
  static constexpr uint32_t type_id = 3;
  static void encode(RunWriter &out, const InstanceLocationRecord &r) {
    out.put(r.instance_id);
    out.put(r.collection_id);
    out.put(r.pe_id);
    out.put(r.start_time_us);
    out.put(r.end_time_us);
    out.put(static_cast<uint8_t>(r.has_end_time));
  }
  static void decode(RunReader &in, InstanceLocationRecord &r) {
    r.instance_id = in.get<int64_t>();
    r.collection_id = in.get<int32_t>();
    r.pe_id = in.get<int32_t>();
    r.start_time_us = in.get<int64_t>();
    r.end_time_us = in.get<int64_t>();
    r.has_end_time = in.get<uint8_t>() != 0;
  }
};

// Executions of one instance in time order. The trailing fields only break
// ties, so that two executions starting at the same microsecond on different
// PEs come out in one fixed order (lower PE first) rather than whichever the
// sort happened to leave first.
struct LocationOrder {
  auto operator()(const InstanceLocationRecord &a,
                  const InstanceLocationRecord &b) const -> bool {
    return std::tie(a.instance_id, a.start_time_us, a.pe_id, a.has_end_time,
                    a.end_time_us) < std::tie(b.instance_id, b.start_time_us,
                                              b.pe_id, b.has_end_time,
                                              b.end_time_us);
  }
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

// The receiver side of a message: one BEGIN_PROCESSING, with the (src_pe,
// event) pair its `pe` and `event` fields carry. `recv_time_us` is the
// record's irecvtime; the runtime writes -1 when it has no value
// (`has_recv_time` false), and beginExecuteLocal() otherwise passes a constant
// 0.0, so the field is retained as written rather than interpreted.
// `instance_id` and `msg_len` are what tells a repeated processing of one
// delivery (a threaded entry method logs a BEGIN_PROCESSING per resume) from
// two different observations that happen to share the pair.
struct BeginProcessingRecord {
  int32_t src_pe;
  int32_t event;
  int32_t dst_pe;
  int32_t ep_id;
  int32_t msg_len;
  bool has_recv_time;
  int64_t instance_id;
  int64_t recv_time_us;
  int64_t exec_start_time_us;
};

template <> struct RunCodec<BeginProcessingRecord> {
  static constexpr uint32_t type_id = 2;
  static void encode(RunWriter &out, const BeginProcessingRecord &r) {
    out.put(r.src_pe);
    out.put(r.event);
    out.put(r.dst_pe);
    out.put(r.ep_id);
    out.put(r.msg_len);
    out.put(static_cast<uint8_t>(r.has_recv_time));
    out.put(r.instance_id);
    out.put(r.recv_time_us);
    out.put(r.exec_start_time_us);
  }
  static void decode(RunReader &in, BeginProcessingRecord &r) {
    r.src_pe = in.get<int32_t>();
    r.event = in.get<int32_t>();
    r.dst_pe = in.get<int32_t>();
    r.ep_id = in.get<int32_t>();
    r.msg_len = in.get<int32_t>();
    r.has_recv_time = in.get<uint8_t>() != 0;
    r.instance_id = in.get<int64_t>();
    r.recv_time_us = in.get<int64_t>();
    r.exec_start_time_us = in.get<int64_t>();
  }
};

// The pair a receiver is linked to its creation by.
struct MessageKey {
  template <class Record>
  auto operator()(const Record &r) const -> std::pair<int32_t, int32_t> {
    return {r.src_pe, r.event};
  }
};

// Groups receivers under the pair a creation is looked up by. Within a group
// the order is total over every field, so which of two equally early
// candidates Stage 3 keeps does not depend on how the records were collected.
struct ReceiverOrder {
  auto operator()(const BeginProcessingRecord &a,
                  const BeginProcessingRecord &b) const -> bool {
    return std::tie(a.src_pe, a.event, a.dst_pe, a.exec_start_time_us, a.ep_id,
                    a.instance_id, a.msg_len, a.has_recv_time, a.recv_time_us) <
           std::tie(b.src_pe, b.event, b.dst_pe, b.exec_start_time_us, b.ep_id,
                    b.instance_id, b.msg_len, b.has_recv_time, b.recv_time_us);
  }
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

// One PE's occupancy of one timestep: the union of every bracket of the
// step-boundary event that PE logged for the step. `has_end` is false when the
// log ends inside the step, which happens if tracing stops mid-run.
struct StepExtent {
  int64_t start_us;
  int64_t end_us;
  bool has_end;
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
  // A (src_pe, event) pair that the runtime reuses appears as many times as
  // it was written, never collapsed.
  ExternalSorter<CreationRecord, CreationOrder> creations;
  ExternalSorter<InstanceLocationRecord, LocationOrder> instance_locations;
  std::vector<ProcessingElementRecord> pes;
  LogParseDiagnostics diagnostics;
  // Keyed on (collection_id, index_0 .. index_5) -- the chare instance's
  // natural key.
  std::unordered_map<
      std::tuple<int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t>,
      ChareInstanceRecord, TupleHash>
      chare_instances;
  // Every BEGIN_PROCESSING. The (pe, event) pair is not unique on the
  // receiving side: a multicast is processed on each of its destinations, and
  // a threaded entry method logs one BEGIN_PROCESSING per resume under the
  // serial of the message that started it.
  ExternalSorter<BeginProcessingRecord, ReceiverOrder> receivers;
  // Keyed on (step_id, pe_id). Empty unless a step-boundary user event was
  // configured and found. Repeated brackets are folded as they are read, so
  // this holds one entry per (timestep, PE) however often the application
  // reuses the event.
  std::map<std::pair<int32_t, int32_t>, StepExtent> step_boundaries;
  // Where the three sorters spill, and what spilling did. Always present;
  // without a budget it has no directory and nothing is written.
  std::unique_ptr<SpillContext> spill;
};

// How Stage 2 and 3 may use memory and disk.
struct PipelineOptions {
  // Bytes the conversion should stay within; 0 means no budget, in which case
  // nothing is ever written to disk.
  uint64_t memory_budget = 0;
  // Parent of the per-run spill directory. Empty: the system temp directory.
  std::filesystem::path spill_root;
  bool keep_spill = false;

  // Test hooks. Spilling every N pushes exercises run files and merges on
  // fixtures far too small to reach any real budget.
  uint64_t spill_every_n_records = 0;
  std::size_t merge_fan_in = 64;
  // 0 derives each from the budget.
  std::size_t io_buffer_bytes = 0;
  uint64_t max_group_bytes = 0;
  uint64_t fail_spill_writes_after_bytes = 0;
};

// `step_event_id` selects the registered user event whose brackets delimit a
// timestep; pass NO_STEP_EVENT to skip step reconstruction entirely.
//
// Throws std::runtime_error when a BEGIN_PROCESSING names an entry method the
// STS does not register: its index arity is then unknown and the rest of the
// record cannot be read, so the conversion aborts rather than guessing. Also
// throws when spilling is needed and the spill directory cannot be created or
// written.
constexpr int32_t NO_STEP_EVENT = -1;

auto process_logs(const std::vector<std::string> &log_file_paths,
                  const StsData &sts_data, const RcData &rc_data,
                  const std::string &output_dir,
                  int32_t step_event_id = NO_STEP_EVENT,
                  const PipelineOptions &options = {}) -> LogParserResult;

} // namespace charmvz
