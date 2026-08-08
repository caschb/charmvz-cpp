#include "reconstruction.h"
#include "parquet_writer.h"
#include "schema.h"
#include <algorithm>
#include <arrow/builder.h>
#include <map>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace charmvz {

void reconstruct_message_and_migration(const LogParserResult &log_data,
                                       const StsData &sts_data,
                                       const RcData &rc_data,
                                       const std::string &output_dir) {
  spdlog::info("Starting Stage 3 reconstruction message and migrations");

  // ProcessingElements
  ParquetWriter pe_writer(charmvz::schema::processing_element(),
                          output_dir + "/processing_element.parquet");
  arrow::Int32Builder pe_pe_id, pe_total_pes;
  arrow::Int64Builder pe_begin, pe_end, pe_global, pe_dur, pe_align;

  for (const auto &pe : log_data.pes) {
    PARQUET_THROW_NOT_OK(pe_pe_id.Append(pe.pe_id));
    PARQUET_THROW_NOT_OK(pe_total_pes.Append(pe.total_pes));
    PARQUET_THROW_NOT_OK(
        pe_begin.Append(pe.begin_time_us - rc_data.global_start_time_us));
    if (pe.end_time_us > 0) {
      PARQUET_THROW_NOT_OK(
          pe_end.Append(pe.end_time_us - rc_data.global_start_time_us));
      PARQUET_THROW_NOT_OK(pe_dur.Append(pe.end_time_us - pe.begin_time_us));
    } else {
      PARQUET_THROW_NOT_OK(pe_end.AppendNull());
      PARQUET_THROW_NOT_OK(pe_dur.AppendNull());
    }
    PARQUET_THROW_NOT_OK(pe_global.Append(pe.global_start_us));
    PARQUET_THROW_NOT_OK(
        pe_align.Append(pe.begin_time_us - pe.global_start_us));
  }
  if (pe_pe_id.length() > 0) {
    std::shared_ptr<arrow::Array> a_pe, a_total, a_b, a_e, a_g, a_d, a_a;
    PARQUET_THROW_NOT_OK(pe_pe_id.Finish(&a_pe));
    PARQUET_THROW_NOT_OK(pe_total_pes.Finish(&a_total));
    PARQUET_THROW_NOT_OK(pe_begin.Finish(&a_b));
    PARQUET_THROW_NOT_OK(pe_end.Finish(&a_e));
    PARQUET_THROW_NOT_OK(pe_global.Finish(&a_g));
    PARQUET_THROW_NOT_OK(pe_dur.Finish(&a_d));
    PARQUET_THROW_NOT_OK(pe_align.Finish(&a_a));
    auto batch = arrow::RecordBatch::Make(
        charmvz::schema::processing_element(), a_pe->length(),
        {a_pe, a_total, a_b, a_e, a_g, a_d, a_a});
    pe_writer.WriteBatch(batch);
  }

  // Messages
  ParquetWriter msg_writer(charmvz::schema::message(),
                           output_dir + "/message.parquet");
  arrow::Int64Builder m_id, m_send, m_enq, m_recv, m_exec, m_s2e, m_e2e,
      m_end2end;
  arrow::Int32Builder m_src, m_evt, m_ep, m_idx, m_len, m_fan, m_dst;
  arrow::BooleanBuilder m_bcast;

  int64_t msg_count = 0;
  auto flush_msg = [&]() {
    if (m_id.length() == 0)
      return;
    std::vector<std::shared_ptr<arrow::Array>> arrs(16);
    PARQUET_THROW_NOT_OK(m_id.Finish(&arrs[0]));
    PARQUET_THROW_NOT_OK(m_src.Finish(&arrs[1]));
    PARQUET_THROW_NOT_OK(m_evt.Finish(&arrs[2]));
    PARQUET_THROW_NOT_OK(m_ep.Finish(&arrs[3]));
    PARQUET_THROW_NOT_OK(m_idx.Finish(&arrs[4]));
    PARQUET_THROW_NOT_OK(m_len.Finish(&arrs[5]));
    PARQUET_THROW_NOT_OK(m_send.Finish(&arrs[6]));
    PARQUET_THROW_NOT_OK(m_enq.Finish(&arrs[7]));
    PARQUET_THROW_NOT_OK(m_bcast.Finish(&arrs[8]));
    PARQUET_THROW_NOT_OK(m_fan.Finish(&arrs[9]));
    PARQUET_THROW_NOT_OK(m_dst.Finish(&arrs[10]));
    PARQUET_THROW_NOT_OK(m_recv.Finish(&arrs[11]));
    PARQUET_THROW_NOT_OK(m_exec.Finish(&arrs[12]));
    PARQUET_THROW_NOT_OK(m_s2e.Finish(&arrs[13]));
    PARQUET_THROW_NOT_OK(m_e2e.Finish(&arrs[14]));
    PARQUET_THROW_NOT_OK(m_end2end.Finish(&arrs[15]));
    auto batch = arrow::RecordBatch::Make(charmvz::schema::message(),
                                          arrs[0]->length(), arrs);
    msg_writer.WriteBatch(batch);
  };

  for (const auto &kv : log_data.creation_map) {
    msg_count++;
    auto src_pe = std::get<0>(kv.first);
    auto event = std::get<1>(kv.first);
    const auto &cr = kv.second;

    PARQUET_THROW_NOT_OK(m_id.Append(msg_count));
    PARQUET_THROW_NOT_OK(m_src.Append(src_pe));
    PARQUET_THROW_NOT_OK(m_evt.Append(event));
    PARQUET_THROW_NOT_OK(m_ep.Append(cr.ep_id));
    PARQUET_THROW_NOT_OK(m_idx.Append(cr.msg_idx));
    PARQUET_THROW_NOT_OK(m_len.Append(cr.msg_len));
    PARQUET_THROW_NOT_OK(
        m_send.Append(cr.send_time_us - rc_data.global_start_time_us));
    if (cr.enqueue_time_us > 0) {
      PARQUET_THROW_NOT_OK(
          m_enq.Append(cr.enqueue_time_us - rc_data.global_start_time_us));
    } else {
      PARQUET_THROW_NOT_OK(m_enq.AppendNull());
    }
    PARQUET_THROW_NOT_OK(m_bcast.Append(cr.is_broadcast));
    PARQUET_THROW_NOT_OK(m_fan.Append(cr.broadcast_fanout));

    auto bp_it = log_data.begin_processing_map.find(kv.first);
    if (bp_it != log_data.begin_processing_map.end()) {
      PARQUET_THROW_NOT_OK(m_dst.Append(bp_it->second.dst_pe));
      PARQUET_THROW_NOT_OK(m_recv.Append(bp_it->second.recv_time_us -
                                         rc_data.global_start_time_us));
      PARQUET_THROW_NOT_OK(m_exec.Append(bp_it->second.exec_start_time_us -
                                         rc_data.global_start_time_us));
      PARQUET_THROW_NOT_OK(m_s2e.AppendNull());
      PARQUET_THROW_NOT_OK(m_e2e.AppendNull());
      PARQUET_THROW_NOT_OK(m_end2end.AppendNull());
    } else {
      PARQUET_THROW_NOT_OK(m_dst.AppendNull());
      PARQUET_THROW_NOT_OK(m_recv.AppendNull());
      PARQUET_THROW_NOT_OK(m_exec.AppendNull());
      PARQUET_THROW_NOT_OK(m_s2e.AppendNull());
      PARQUET_THROW_NOT_OK(m_e2e.AppendNull());
      PARQUET_THROW_NOT_OK(m_end2end.AppendNull());
    }

    if (m_id.length() >= 100000)
      flush_msg();
  }
  flush_msg();

  // MigrationEpisode (Rule 9): a migration is a change of PE between two
  // consecutive executions of the same chare-array instance. Pack/unpack events
  // are not involved -- see the comment on schema::migration_episode().
  ParquetWriter mig_writer(charmvz::schema::migration_episode(),
                           output_dir + "/migration_episode.parquet");
  spdlog::info("Writing MigrationEpisode.parquet");

  arrow::Int64Builder mig_id, mig_inst, src_end, dst_start, gap;
  arrow::Int32Builder mig_coll, mig_src, mig_dst, mig_seq;

  // Group executions by instance, then order each instance's executions in
  // time. Timestamps are already aligned to the global start, so they are
  // comparable across PEs.
  std::unordered_map<int64_t, std::vector<const InstanceLocationRecord *>>
      by_instance;
  for (const auto &loc : log_data.instance_locations)
    by_instance[loc.instance_id].push_back(&loc);

  int64_t migration_id = 0;

  for (auto &[instance_id, locations] : by_instance) {
    std::sort(
        locations.begin(), locations.end(),
        [](const InstanceLocationRecord *a, const InstanceLocationRecord *b) {
          return a->start_time_us < b->start_time_us;
        });

    int32_t sequence = 0;
    for (size_t i = 1; i < locations.size(); ++i) {
      const auto *previous = locations[i - 1];
      const auto *current = locations[i];
      if (previous->pe_id == current->pe_id)
        continue;

      ++migration_id;
      ++sequence;
      PARQUET_THROW_NOT_OK(mig_id.Append(migration_id));
      PARQUET_THROW_NOT_OK(mig_inst.Append(instance_id));
      PARQUET_THROW_NOT_OK(mig_coll.Append(current->collection_id));
      PARQUET_THROW_NOT_OK(mig_src.Append(previous->pe_id));
      PARQUET_THROW_NOT_OK(mig_dst.Append(current->pe_id));
      PARQUET_THROW_NOT_OK(src_end.Append(previous->end_time_us));
      PARQUET_THROW_NOT_OK(dst_start.Append(current->start_time_us));
      PARQUET_THROW_NOT_OK(
          gap.Append(current->start_time_us - previous->end_time_us));
      PARQUET_THROW_NOT_OK(mig_seq.Append(sequence));
    }
  }

  if (mig_id.length() > 0) {
    std::vector<std::shared_ptr<arrow::Array>> arrays(9);
    PARQUET_THROW_NOT_OK(mig_id.Finish(&arrays[0]));
    PARQUET_THROW_NOT_OK(mig_inst.Finish(&arrays[1]));
    PARQUET_THROW_NOT_OK(mig_coll.Finish(&arrays[2]));
    PARQUET_THROW_NOT_OK(mig_src.Finish(&arrays[3]));
    PARQUET_THROW_NOT_OK(mig_dst.Finish(&arrays[4]));
    PARQUET_THROW_NOT_OK(src_end.Finish(&arrays[5]));
    PARQUET_THROW_NOT_OK(dst_start.Finish(&arrays[6]));
    PARQUET_THROW_NOT_OK(gap.Finish(&arrays[7]));
    PARQUET_THROW_NOT_OK(mig_seq.Finish(&arrays[8]));
    auto batch = arrow::RecordBatch::Make(charmvz::schema::migration_episode(),
                                          arrays[0]->length(), arrays);
    mig_writer.WriteBatch(batch);
  }
}

void reconstruct_simulation_steps(const LogParserResult &log_data,
                                  const std::string &output_dir) {
  ParquetWriter step_writer(charmvz::schema::simulation_step(),
                            output_dir + "/simulation_step.parquet");

  if (log_data.step_boundaries.empty()) {
    spdlog::info("No step-boundary user events found; "
                 "simulation_step.parquet will be empty");
    return;
  }

  // A PE may bracket the same step more than once if the application reuses
  // the boundary event within a step. Fold those into one interval per
  // (step, PE) rather than emitting duplicate primary keys.
  struct StepExtent {
    int64_t start_us;
    int64_t end_us;
    bool has_end;
  };
  std::map<std::pair<int32_t, int32_t>, StepExtent> per_pe;

  for (const auto &boundary : log_data.step_boundaries) {
    auto key = std::make_pair(boundary.step_id, boundary.pe_id);
    auto [it, inserted] = per_pe.try_emplace(
        key, StepExtent{boundary.start_time_us, boundary.end_time_us,
                        boundary.has_end_time});
    if (inserted) {
      continue;
    }
    it->second.start_us = std::min(it->second.start_us, boundary.start_time_us);
    if (boundary.has_end_time) {
      it->second.end_us =
          it->second.has_end ? std::max(it->second.end_us, boundary.end_time_us)
                             : boundary.end_time_us;
      it->second.has_end = true;
    }
  }

  // The global extent of a step is the union of its per-PE intervals. It is
  // denormalized into every row so a consumer gets the whole-run view without
  // a second aggregation, exactly as ProcessingElement carries global_start_us.
  struct GlobalExtent {
    int64_t start_us;
    int64_t end_us;
    bool has_end;
    int32_t pe_count;
  };
  std::map<int32_t, GlobalExtent> per_step;

  for (const auto &[key, extent] : per_pe) {
    auto [it, inserted] = per_step.try_emplace(
        key.first,
        GlobalExtent{extent.start_us, extent.end_us, extent.has_end, 1});
    if (inserted) {
      continue;
    }
    it->second.start_us = std::min(it->second.start_us, extent.start_us);
    if (extent.has_end) {
      it->second.end_us = it->second.has_end
                              ? std::max(it->second.end_us, extent.end_us)
                              : extent.end_us;
      it->second.has_end = true;
    }
    ++it->second.pe_count;
  }

  arrow::Int32Builder step_id, pe_id, pe_count;
  arrow::Int64Builder start_us, end_us, duration_us, global_start_us,
      global_end_us;

  for (const auto &[key, extent] : per_pe) {
    const auto &global = per_step.at(key.first);
    PARQUET_THROW_NOT_OK(step_id.Append(key.first));
    PARQUET_THROW_NOT_OK(pe_id.Append(key.second));
    PARQUET_THROW_NOT_OK(start_us.Append(extent.start_us));
    if (extent.has_end) {
      PARQUET_THROW_NOT_OK(end_us.Append(extent.end_us));
      PARQUET_THROW_NOT_OK(duration_us.Append(extent.end_us - extent.start_us));
    } else {
      PARQUET_THROW_NOT_OK(end_us.AppendNull());
      PARQUET_THROW_NOT_OK(duration_us.AppendNull());
    }
    PARQUET_THROW_NOT_OK(global_start_us.Append(global.start_us));
    if (global.has_end) {
      PARQUET_THROW_NOT_OK(global_end_us.Append(global.end_us));
    } else {
      PARQUET_THROW_NOT_OK(global_end_us.AppendNull());
    }
    PARQUET_THROW_NOT_OK(pe_count.Append(global.pe_count));
  }

  std::vector<std::shared_ptr<arrow::Array>> arrays(8);
  PARQUET_THROW_NOT_OK(step_id.Finish(&arrays[0]));
  PARQUET_THROW_NOT_OK(pe_id.Finish(&arrays[1]));
  PARQUET_THROW_NOT_OK(start_us.Finish(&arrays[2]));
  PARQUET_THROW_NOT_OK(end_us.Finish(&arrays[3]));
  PARQUET_THROW_NOT_OK(duration_us.Finish(&arrays[4]));
  PARQUET_THROW_NOT_OK(global_start_us.Finish(&arrays[5]));
  PARQUET_THROW_NOT_OK(global_end_us.Finish(&arrays[6]));
  PARQUET_THROW_NOT_OK(pe_count.Finish(&arrays[7]));

  auto batch = arrow::RecordBatch::Make(charmvz::schema::simulation_step(),
                                        arrays[0]->length(), arrays);
  step_writer.WriteBatch(batch);
  spdlog::info("Wrote {} simulation_step rows across {} timesteps",
               per_pe.size(), per_step.size());
}

} // namespace charmvz
