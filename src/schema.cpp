#include "schema.h"

namespace charmvz::schema {

auto processing_element() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema(
      {arrow::field("pe_id", arrow::int32(), false),
       arrow::field("total_pes", arrow::int32(), false),
       arrow::field("begin_time_us", arrow::int64(), true),
       arrow::field("end_time_us", arrow::int64(), true),
       arrow::field("global_start_us", arrow::int64(), true),
       arrow::field("computation_duration_us", arrow::int64(), true),
       arrow::field("aligned_begin_us", arrow::int64(), true)});
}

auto chare_collection() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema({arrow::field("collection_id", arrow::int32(), false),
                        arrow::field("name", arrow::utf8(), false),
                        arrow::field("ndims", arrow::int32(), false)});
}

auto entry_method() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema({arrow::field("ep_id", arrow::int32(), false),
                        arrow::field("name", arrow::utf8(), false),
                        arrow::field("collection_id", arrow::int32(), false),
                        arrow::field("msg_idx", arrow::int32(), false)});
}

auto chare_instance() -> std::shared_ptr<arrow::Schema> {
  // Six index columns, matching the Projections Java reader's `int[6]`. A chare
  // array populates its first `ndims` columns and leaves the rest zero.
  return arrow::schema({arrow::field("instance_id", arrow::int64(), false),
                        arrow::field("collection_id", arrow::int32(), false),
                        arrow::field("index_0", arrow::int32(), false),
                        arrow::field("index_1", arrow::int32(), false),
                        arrow::field("index_2", arrow::int32(), false),
                        arrow::field("index_3", arrow::int32(), false),
                        arrow::field("index_4", arrow::int32(), false),
                        arrow::field("index_5", arrow::int32(), false)});
}

auto execution(const std::vector<std::string> &papi_event_names)
    -> std::shared_ptr<arrow::Schema> {
  auto schema =
      arrow::schema({arrow::field("pe_id", arrow::int32(), false),
                     arrow::field("event", arrow::int32(), false),
                     arrow::field("instance_id", arrow::int64(), true),
                     arrow::field("ep_id", arrow::int32(), false),
                     arrow::field("src_pe", arrow::int32(), false),
                     arrow::field("msg_idx", arrow::int32(), false),
                     arrow::field("msg_len", arrow::int32(), false),
                     arrow::field("start_time_us", arrow::int64(), false),
                     arrow::field("recv_time_us", arrow::int64(), true),
                     arrow::field("start_cpu_us", arrow::int64(), false),
                     arrow::field("end_time_us", arrow::int64(), true),
                     arrow::field("end_cpu_us", arrow::int64(), true),
                     arrow::field("papi_begin_0", arrow::int64(), true),
                     arrow::field("papi_begin_1", arrow::int64(), true),
                     arrow::field("papi_begin_2", arrow::int64(), true),
                     arrow::field("papi_begin_3", arrow::int64(), true),
                     arrow::field("papi_begin_4", arrow::int64(), true),
                     arrow::field("papi_begin_5", arrow::int64(), true),
                     arrow::field("papi_end_0", arrow::int64(), true),
                     arrow::field("papi_end_1", arrow::int64(), true),
                     arrow::field("papi_end_2", arrow::int64(), true),
                     arrow::field("papi_end_3", arrow::int64(), true),
                     arrow::field("papi_end_4", arrow::int64(), true),
                     arrow::field("papi_end_5", arrow::int64(), true),
                     arrow::field("wall_duration_us", arrow::int64(), true),
                     arrow::field("cpu_duration_us", arrow::int64(), true),
                     arrow::field("queue_wait_us", arrow::int64(), true),
                     arrow::field("papi_delta_0", arrow::int64(), true),
                     arrow::field("papi_delta_1", arrow::int64(), true),
                     arrow::field("papi_delta_2", arrow::int64(), true),
                     arrow::field("papi_delta_3", arrow::int64(), true),
                     arrow::field("papi_delta_4", arrow::int64(), true),
                     arrow::field("papi_delta_5", arrow::int64(), true)});

  if (papi_event_names.empty()) {
    return schema;
  }

  std::vector<std::string> keys;
  std::vector<std::string> values;
  keys.reserve(papi_event_names.size());
  values.reserve(papi_event_names.size());
  for (size_t i = 0; i < papi_event_names.size(); ++i) {
    if (papi_event_names[i].empty()) {
      continue;
    }
    keys.push_back("papi_event_" + std::to_string(i));
    values.push_back(papi_event_names[i]);
  }

  if (keys.empty()) {
    return schema;
  }

  return schema->WithMetadata(
      std::make_shared<arrow::KeyValueMetadata>(keys, values));
}

auto message() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema(
      {arrow::field("message_id", arrow::int64(), false),
       arrow::field("src_pe", arrow::int32(), false),
       arrow::field("event", arrow::int32(), false),
       arrow::field("ep_id", arrow::int32(), false),
       arrow::field("msg_idx", arrow::int32(), false),
       arrow::field("msg_len", arrow::int32(), false),
       arrow::field("send_time_us", arrow::int64(), false),
       arrow::field("enqueue_time_us", arrow::int64(), true),
       arrow::field("is_broadcast", arrow::boolean(), false),
       arrow::field("broadcast_fanout", arrow::int32(), true),
       arrow::field("dst_pe", arrow::int32(), true),
       arrow::field("recv_time_us", arrow::int64(), true),
       arrow::field("exec_start_time_us", arrow::int64(), true),
       arrow::field("send_to_enqueue_us", arrow::int64(), true),
       arrow::field("enqueue_to_exec_us", arrow::int64(), true),
       arrow::field("end_to_end_us", arrow::int64(), true)});
}

auto idle_interval() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema({arrow::field("pe_id", arrow::int32(), false),
                        arrow::field("start_time_us", arrow::int64(), false),
                        arrow::field("end_time_us", arrow::int64(), true),
                        arrow::field("duration_us", arrow::int64(), true)});
}

// A migration is derived from a change of PE between consecutive executions of
// the same chare-array instance -- NOT from BEGIN_PACK/END_PACK, which the
// runtime emits around ordinary message serialisation in CkPackMessage() and
// never around migration (CkLocMgr::emigrate() emits no pack tracing at all).
// Consequently there is no observable pack/unpack cost for a migration; the
// timing available is the bracket between the last execution on the source PE
// and the first on the destination, whose width is an upper bound on the true
// migration cost because it also contains queueing and load-balancer time.
auto migration_episode() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema(
      {arrow::field("migration_id", arrow::int64(), false),
       arrow::field("instance_id", arrow::int64(), false),
       arrow::field("collection_id", arrow::int32(), false),
       arrow::field("src_pe", arrow::int32(), false),
       arrow::field("dst_pe", arrow::int32(), false),
       arrow::field("last_exec_end_src_us", arrow::int64(), false),
       arrow::field("first_exec_start_dst_us", arrow::int64(), false),
       arrow::field("gap_us", arrow::int64(), false),
       arrow::field("migration_seq", arrow::int32(), false)});
}

// One row per user-event occurrence, covering both the instantaneous forms
// (USER_EVENT 13, USER_SUPPLIED 26, USER_SUPPLIED_NOTE 28) and the bracketed
// ones (USER_SUPPLIED_BRACKETED_NOTE 29, BEGIN/END_USER_EVENT_PAIR 98/99,
// USER_EVENT_PAIR 100). `record_type` keeps the originating Charm++ type code
// so a consumer can tell the forms apart without reverse-engineering which
// columns happen to be NULL.
//
// `pe_id` comes from the log file name, never from the record's own `pe` field:
// the bracketed forms are constructed by a LogEntry constructor that never sets
// `pe` (trace-projections.h:179-185), so they carry a literal 0 on every PE.
auto user_event() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema({arrow::field("pe_id", arrow::int32(), false),
                        arrow::field("record_type", arrow::int32(), false),
                        arrow::field("user_event_id", arrow::int32(), true),
                        arrow::field("name", arrow::utf8(), true),
                        arrow::field("event", arrow::int32(), true),
                        arrow::field("nested_id", arrow::int32(), true),
                        arrow::field("start_time_us", arrow::int64(), false),
                        arrow::field("end_time_us", arrow::int64(), true),
                        arrow::field("duration_us", arrow::int64(), true),
                        arrow::field("user_supplied_int", arrow::int32(), true),
                        arrow::field("note", arrow::utf8(), true)});
}

// A timestep of the traced application, as delimited on one PE by a bracketed
// user event whose registered STS name matches the configured step-boundary
// event. The step index is the bracket's `nestedID` -- the only application
// supplied integer a bracketed user event carries.
//
// The grain is (step_id, pe_id) rather than step alone because that is what
// attribution needs: an Execution belongs to the step whose interval *on its
// own PE* contains it. PEs do not enter and leave a step together, so a single
// global interval per step would overlap its neighbours and make the
// attribution ambiguous. The global extent is denormalized into every row
// instead, the same way ProcessingElement carries `global_start_us`.
auto simulation_step() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema(
      {arrow::field("step_id", arrow::int32(), false),
       arrow::field("pe_id", arrow::int32(), false),
       arrow::field("start_time_us", arrow::int64(), false),
       arrow::field("end_time_us", arrow::int64(), true),
       arrow::field("duration_us", arrow::int64(), true),
       arrow::field("global_start_time_us", arrow::int64(), false),
       arrow::field("global_end_time_us", arrow::int64(), true),
       arrow::field("pe_count", arrow::int32(), false)});
}

// Lookup for the STS `MESSAGE <msgIdx> <size>` table, which gives the declared
// size of each message type referenced by Execution.msg_idx and
// Message.msg_idx.
auto message_type() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema({arrow::field("msg_idx", arrow::int32(), false),
                        arrow::field("size_bytes", arrow::int64(), false)});
}

// One application-declared statistic sample, from a USER_STAT record (code 32)
// emitted by updateStat() / updateStatPair().
//
// `stat_id` indexes the STS `STAT` registry populated by
// traceRegisterUserStat(); the registered name is denormalized into every row,
// as UserEvent does, because a stat has no attribute beyond its name and
// Parquet dictionary-encodes the repetition away.
//
// `user_time_s` is the one time field in this schema that is *not* aligned
// microseconds. USER_STAT writes the `cputime` member raw
// (trace-projections.C:775-776), with no `1.0e6*` conversion of the kind every
// other record applies, so the value is a float in whatever unit the
// application passed to updateStatPair(). updateStat() supplies -1, which is
// stored as NULL rather than as a negative time.
//
// Unlike the bracketed user-event forms, the record's own `pe` field is
// genuine: it is CkMyPe() at emission (trace-projections.C:1141). `pe_id` still
// comes from the log file name, so that every table in this schema keys on PE
// the same way.
auto user_stat() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema({arrow::field("pe_id", arrow::int32(), false),
                        arrow::field("stat_id", arrow::int32(), false),
                        arrow::field("name", arrow::utf8(), true),
                        arrow::field("time_us", arrow::int64(), false),
                        arrow::field("stat_value", arrow::float64(), false),
                        arrow::field("user_time_s", arrow::float64(), true)});
}

// One heap-usage sample, from a MEMORY_USAGE_CURRENT record (code 27) emitted
// by traceMemoryUsage().
//
// The record carries no PE field at all, so `pe_id` can only come from the log
// file name. It also writes `memUsage` *before* `itime`
// (trace-projections.C:770-772), reversing the field order every other record
// uses; reading the two in the usual order yields a byte count parsed as a
// timestamp, which produces a well-typed table of nonsense.
//
// `bytes` is int64 rather than uint64: the runtime writes an `unsigned long`,
// but Parquet has no unsigned 64-bit type that round-trips through Arrow
// without surprising consumers, and a heap measured in exabytes is not a case
// worth carrying.
auto memory_sample() -> std::shared_ptr<arrow::Schema> {
  return arrow::schema({arrow::field("pe_id", arrow::int32(), false),
                        arrow::field("time_us", arrow::int64(), false),
                        arrow::field("bytes", arrow::int64(), false)});
}

} // namespace charmvz::schema
