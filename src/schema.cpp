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

} // namespace charmvz::schema
