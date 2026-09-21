#include "reconstruction.h"
#include "builders.h"
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

namespace {

// Rule 1. One row per valid PE log. The computation bounds are the marker
// timestamps as the log holds them, in the same frame as every other stored
// timestamp. `global_start_us` records the origin shift the runtime applied
// before writing (0 when it applied none) and `aligned_begin_us` is the begin
// marker in that already-shifted frame; both are NULL without a clock
// reference.
void write_processing_elements(const LogParserResult &log_data,
                               const RcData &rc_data,
                               const std::string &output_dir) {
  ParquetWriter pe_writer(charmvz::schema::processing_element(),
                          output_dir + "/processing_element.parquet");
  arrow::Int32Builder pe_pe_id, pe_total_pes;
  arrow::Int64Builder pe_begin, pe_end, pe_global, pe_dur, pe_align;

  const bool clock = rc_data.clock_reference_usable();
  for (const auto &pe : log_data.pes) {
    PARQUET_THROW_NOT_OK(pe_pe_id.Append(pe.pe_id));
    PARQUET_THROW_NOT_OK(pe_total_pes.Append(pe.total_pes));
    if (pe.has_begin_time) {
      PARQUET_THROW_NOT_OK(pe_begin.Append(pe.begin_time_us));
    } else {
      PARQUET_THROW_NOT_OK(pe_begin.AppendNull());
    }
    if (pe.has_end_time) {
      PARQUET_THROW_NOT_OK(pe_end.Append(pe.end_time_us));
    } else {
      PARQUET_THROW_NOT_OK(pe_end.AppendNull());
    }
    if (pe.has_begin_time && pe.has_end_time) {
      PARQUET_THROW_NOT_OK(pe_dur.Append(pe.end_time_us - pe.begin_time_us));
    } else {
      PARQUET_THROW_NOT_OK(pe_dur.AppendNull());
    }
    if (clock) {
      PARQUET_THROW_NOT_OK(pe_global.Append(rc_data.global_start_time_us));
    } else {
      PARQUET_THROW_NOT_OK(pe_global.AppendNull());
    }
    if (clock && pe.has_begin_time) {
      PARQUET_THROW_NOT_OK(pe_align.Append(pe.begin_time_us));
    } else {
      PARQUET_THROW_NOT_OK(pe_align.AppendNull());
    }
  }
  if (pe_pe_id.length() == 0)
    return;
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

// Accumulates message.parquet rows and flushes them at ROW_GROUP_SIZE.
class MessageRows {
public:
  explicit MessageRows(ParquetWriter &writer) : writer_(writer) {}

  // A row with no receiver; every receiver-derived column is NULL.
  void append_unmatched(const CreationRecord &cr, const int32_t *explicit_dst) {
    append_common(cr);
    if (explicit_dst != nullptr) {
      PARQUET_THROW_NOT_OK(m_dst.Append(*explicit_dst));
    } else {
      PARQUET_THROW_NOT_OK(m_dst.AppendNull());
    }
    PARQUET_THROW_NOT_OK(m_recv.AppendNull());
    PARQUET_THROW_NOT_OK(m_exec.AppendNull());
    PARQUET_THROW_NOT_OK(m_e2e.AppendNull());
    PARQUET_THROW_NOT_OK(m_end2end.AppendNull());
    finish_row();
  }

  // A row whose receiver is `bp`. Cross-PE differences are computed only when
  // `cross_pe_ok`: both clocks share a reference, or they are the same clock.
  void append_matched(const CreationRecord &cr, const BeginProcessingRecord &bp,
                      bool cross_pe_ok) {
    append_common(cr);
    PARQUET_THROW_NOT_OK(m_dst.Append(bp.dst_pe));
    if (bp.has_recv_time) {
      PARQUET_THROW_NOT_OK(m_recv.Append(bp.recv_time_us));
    } else {
      PARQUET_THROW_NOT_OK(m_recv.AppendNull());
    }
    PARQUET_THROW_NOT_OK(m_exec.Append(bp.exec_start_time_us));
    if (cross_pe_ok) {
      PARQUET_THROW_NOT_OK(
          m_e2e.Append(bp.exec_start_time_us - enqueue_time(cr)));
      PARQUET_THROW_NOT_OK(
          m_end2end.Append(bp.exec_start_time_us - cr.send_time_us));
    } else {
      PARQUET_THROW_NOT_OK(m_e2e.AppendNull());
      PARQUET_THROW_NOT_OK(m_end2end.AppendNull());
    }
    finish_row();
  }

  void flush() {
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
    writer_.WriteBatch(batch);
  }

  [[nodiscard]] auto rows() const -> int64_t { return msg_count_; }

private:
  // The moment the send call returned on the source PE, on that PE's clock:
  // the CREATION timestamp plus the interval creationDone() recorded.
  static auto enqueue_time(const CreationRecord &cr) -> int64_t {
    return cr.send_time_us + cr.send_to_enqueue_us;
  }

  void append_common(const CreationRecord &cr) {
    ++msg_count_;
    PARQUET_THROW_NOT_OK(m_id.Append(msg_count_));
    PARQUET_THROW_NOT_OK(m_src.Append(cr.src_pe));
    PARQUET_THROW_NOT_OK(m_evt.Append(cr.event));
    PARQUET_THROW_NOT_OK(m_ep.Append(cr.ep_id));
    PARQUET_THROW_NOT_OK(m_idx.Append(cr.msg_idx));
    PARQUET_THROW_NOT_OK(m_len.Append(cr.msg_len));
    PARQUET_THROW_NOT_OK(m_send.Append(cr.send_time_us));
    PARQUET_THROW_NOT_OK(m_enq.Append(enqueue_time(cr)));
    PARQUET_THROW_NOT_OK(m_bcast.Append(cr.is_broadcast));
    if (cr.is_broadcast) {
      PARQUET_THROW_NOT_OK(m_fan.Append(cr.broadcast_fanout));
    } else {
      PARQUET_THROW_NOT_OK(m_fan.AppendNull());
    }
    PARQUET_THROW_NOT_OK(m_s2e.Append(cr.send_to_enqueue_us));
  }

  void finish_row() {
    if (m_id.length() >= builders::ROW_GROUP_SIZE)
      flush();
  }

  ParquetWriter &writer_;
  int64_t msg_count_ = 0;
  arrow::Int64Builder m_id, m_send, m_enq, m_recv, m_exec, m_s2e, m_e2e,
      m_end2end;
  arrow::Int32Builder m_src, m_evt, m_ep, m_idx, m_len, m_fan, m_dst;
  arrow::BooleanBuilder m_bcast;
};

// Rules 6 and 7. One row per unicast, one per explicit multicast destination,
// one per broadcast.
//
// A receiver is a BEGIN_PROCESSING carrying the creation's (src_pe, event)
// whose entry method is the one the message was created for: both records
// take eIdx from the same envelope, so a differing entry method means the
// receiver's (pe, event) fields came from another envelope. This is not a
// theoretical case. Messages delivered outside the traced send path keep
// whatever those envelope fields last held, and on the ChaNGa reference trace
// 23,859 of 4.23 M pairs matched on (src_pe, event) alone joined a creation to
// an execution of a different entry method, some of them logged before the
// creation existed.
//
// On the sending PE itself the two records share one clock, so a candidate
// that started before the creation cannot have processed it and is passed
// over. Across PEs no such test is possible without a synchronised clock, and
// none is attempted.
//
// Where a destination still logged several candidates, they are one delivery
// only in the one case the runtime documents: resumes of a threaded entry
// method, logged by beginExecute(CmiObjId*) under the thread's entry point
// ("dummy_thread_ep") and the serial of the message that started it, on the
// same chare instance. The earliest of those is the record that processed the
// message. Any other repetition (on the ChaNGa reference trace, 800 thousand
// groups of CkCache fills with identical entry method, instance and length)
// is a set of observations the pair cannot separate; the destination is
// written with NULL receiver fields and diagnosed rather than resolved by
// picking one.
void write_messages(const LogParserResult &log_data, const RcData &rc_data,
                    const std::string &output_dir) {
  ParquetWriter msg_writer(charmvz::schema::message(),
                           output_dir + "/message.parquet");
  MessageRows rows(msg_writer);
  const bool clock = rc_data.clock_reference_usable();

  int64_t unmatched_unicasts = 0;
  int64_t multi_pe_unicasts = 0;
  int64_t unmatched_multicast_destinations = 0;
  int64_t receivers_outside_multicast = 0;

  int64_t repeated_deliveries = 0;
  int64_t ambiguous_destinations = 0;

  struct Candidate {
    const BeginProcessingRecord *earliest;
    bool ambiguous;
  };
  std::map<int32_t, Candidate> earliest_by_dst;
  for (const auto &cr : log_data.creations) {
    if (cr.is_broadcast) {
      // No destination list exists, so nothing is matched (Rule 7).
      rows.append_unmatched(cr, nullptr);
      continue;
    }

    earliest_by_dst.clear();
    auto [first, last] = log_data.begin_processing_map.equal_range(
        std::make_tuple(cr.src_pe, cr.event));
    for (auto it = first; it != last; ++it) {
      const auto &bp = it->second;
      if (bp.ep_id != cr.ep_id)
        continue;
      if (bp.dst_pe == cr.src_pe && bp.exec_start_time_us < cr.send_time_us)
        continue;
      auto [slot, inserted] =
          earliest_by_dst.try_emplace(bp.dst_pe, Candidate{&bp, false});
      if (inserted)
        continue;
      const auto &kept = *slot->second.earliest;
      const bool verified_resume = log_data.thread_ep_id >= 0 &&
                                   bp.ep_id == log_data.thread_ep_id &&
                                   kept.instance_id == bp.instance_id;
      if (verified_resume) {
        ++repeated_deliveries;
      } else {
        slot->second.ambiguous = true;
      }
      if (bp.exec_start_time_us < kept.exec_start_time_us)
        slot->second.earliest = &bp;
    }

    auto emit_to = [&](int32_t dst, const Candidate *candidate) {
      if (candidate == nullptr) {
        rows.append_unmatched(cr, &dst);
        return;
      }
      if (candidate->ambiguous) {
        ++ambiguous_destinations;
        spdlog::debug("Creation (src_pe {}, event {}) has several receivers "
                      "on PE {} that are not thread resumes; destination "
                      "written without receiver fields",
                      cr.src_pe, cr.event, dst);
        rows.append_unmatched(cr, &dst);
        return;
      }
      rows.append_matched(cr, *candidate->earliest, clock || dst == cr.src_pe);
    };

    if (!cr.dst_pes.empty()) {
      for (const int32_t dst : cr.dst_pes) {
        auto found = earliest_by_dst.find(dst);
        if (found == earliest_by_dst.end()) {
          ++unmatched_multicast_destinations;
          emit_to(dst, nullptr);
        } else {
          emit_to(dst, &found->second);
        }
      }
      for (const auto &[dst, candidate] : earliest_by_dst) {
        if (std::find(cr.dst_pes.begin(), cr.dst_pes.end(), dst) ==
            cr.dst_pes.end()) {
          ++receivers_outside_multicast;
          spdlog::warn("BEGIN_PROCESSING on PE {} carries (src_pe {}, event "
                       "{}) of a multicast whose destination list omits it; "
                       "not linked",
                       dst, cr.src_pe, cr.event);
        }
      }
      continue;
    }

    if (earliest_by_dst.empty()) {
      ++unmatched_unicasts;
      rows.append_unmatched(cr, nullptr);
      continue;
    }
    if (earliest_by_dst.size() > 1) {
      // One CREATION with receivers of its own entry method on several PEs.
      // The trace cannot say which of them the message reached, so every
      // receiving PE gets a row and none is silently preferred. (On the
      // ChaNGa reference trace this never happens once the entry method is
      // required; without it, 7,478 creations matched stale envelopes on
      // other PEs.)
      ++multi_pe_unicasts;
      spdlog::debug("Creation (src_pe {}, event {}) was processed on {} PEs; "
                    "one row per receiving PE",
                    cr.src_pe, cr.event, earliest_by_dst.size());
    }
    for (const auto &[dst, candidate] : earliest_by_dst)
      emit_to(dst, &candidate);
  }
  rows.flush();

  spdlog::info("Wrote {} message rows from {} creations", rows.rows(),
               log_data.creations.size());
  if (unmatched_unicasts > 0)
    spdlog::info("{} unicast creations have no receiver", unmatched_unicasts);
  if (unmatched_multicast_destinations > 0)
    spdlog::info("{} multicast destinations have no receiver",
                 unmatched_multicast_destinations);
  if (multi_pe_unicasts > 0)
    spdlog::warn("{} single-message creations have receivers on more than "
                 "one PE; one row per receiving PE",
                 multi_pe_unicasts);
  if (repeated_deliveries > 0)
    spdlog::info("{} thread-resume BEGIN_PROCESSING records were folded into "
                 "the execution that started their thread",
                 repeated_deliveries);
  if (ambiguous_destinations > 0)
    spdlog::warn("{} destinations had several receivers under one (src_pe, "
                 "event) that are not thread resumes; written without "
                 "receiver fields",
                 ambiguous_destinations);
  if (receivers_outside_multicast > 0)
    spdlog::warn("{} receivers fall outside their multicast's destination list",
                 receivers_outside_multicast);
  if (!clock)
    spdlog::warn("No clock reference: enqueue_to_exec_us and end_to_end_us "
                 "are NULL except for messages a PE sent to itself");
}

// Rule 9. A migration is a change of PE between two consecutive executions of
// the same chare-array instance. Pack/unpack events are not involved -- see
// the comment on schema::migration_episode().
void write_migrations(const LogParserResult &log_data, const RcData &rc_data,
                      const std::string &output_dir) {
  ParquetWriter mig_writer(charmvz::schema::migration_episode(),
                           output_dir + "/migration_episode.parquet");
  if (!rc_data.clock_reference_usable()) {
    // Ordering executions across PEs compares their clocks, which nothing
    // relates without the run's reference.
    spdlog::warn("No clock reference: migration_episode.parquet is empty");
    return;
  }

  arrow::Int64Builder mig_id, mig_inst, src_end, dst_start, gap;
  arrow::Int32Builder mig_coll, mig_src, mig_dst, mig_seq;
  auto flush = [&]() {
    if (mig_id.length() == 0)
      return;
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
  };

  // Group executions by instance, then order each instance's executions in
  // time. The runtime wrote every PE's timestamps in the run's common frame,
  // so they compare across PEs up to the unrecorded per-PE clock offset.
  std::unordered_map<int64_t, std::vector<const InstanceLocationRecord *>>
      by_instance;
  for (const auto &loc : log_data.instance_locations)
    by_instance[loc.instance_id].push_back(&loc);

  int64_t migration_id = 0;
  int64_t unbounded_hops = 0;

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

      // The hop is counted even when it cannot be written, so a later hop's
      // ordinal still says how many preceded it.
      ++sequence;
      if (!previous->has_end_time) {
        // The source-side end is a non-null column and the execution never
        // ended in the log; the transition is observed but has no bounds.
        ++unbounded_hops;
        continue;
      }

      ++migration_id;
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
      if (mig_id.length() >= builders::ROW_GROUP_SIZE)
        flush();
    }
  }
  flush();
  spdlog::info("Wrote {} migration episodes", migration_id);
  if (unbounded_hops > 0)
    spdlog::warn("{} PE transitions follow an execution with no "
                 "END_PROCESSING and were not written",
                 unbounded_hops);
}

} // namespace

void reconstruct_message_and_migration(const LogParserResult &log_data,
                                       const RcData &rc_data,
                                       const std::string &output_dir) {
  spdlog::info("Starting Stage 3 reconstruction message and migrations");
  write_processing_elements(log_data, rc_data, output_dir);
  write_messages(log_data, rc_data, output_dir);
  write_migrations(log_data, rc_data, output_dir);
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
