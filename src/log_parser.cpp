#include "log_parser.h"
#include "builders.h"
#include "parquet_writer.h"
#include "schema.h"
#include "utils/log_entry.h"
#include "zstr.hpp"
#include <arrow/builder.h>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace charmvz {

namespace {

// The collection an entry method belongs to and how many chare-index values
// its BEGIN_PROCESSING records carry. Per charm/src/ck-perf/trace-projections.C
// (LogEntry::pup, case BEGIN_PROCESSING), an array chare writes exactly `ndims`
// values (as short int when ndims >= 4, as int otherwise -- indistinguishable
// in the text format, where only the count matters), while a non-array chare
// (ndims == -1) writes four. Reading a fixed four would consume icputime as an
// index for 3D arrays and leave three values unread for 6D ones.
//
// An entry method the STS does not register therefore cannot be parsed at all:
// there is no arity to read the record with, and guessing one desynchronises
// every field after the index block. parse_sts_file() already guarantees that a
// registered entry method's collection exists.
struct ResolvedEntry {
  int32_t collection_id;
  int32_t index_arity;
};

auto resolve_entry(const StsData &sts_data, uint16_t ep_id, int32_t pe_id)
    -> ResolvedEntry {
  auto ep_it = sts_data.ep_map.find(ep_id);
  if (ep_it == sts_data.ep_map.end()) {
    spdlog::error("BEGIN_PROCESSING on PE {} names entry method {} which the "
                  "STS does not register; the record cannot be parsed",
                  pe_id, ep_id);
    throw std::runtime_error("Unregistered entry method in log");
  }
  const int32_t ndims =
      sts_data.chare_map.at(ep_it->second.collection_id).ndims;
  return {ep_it->second.collection_id,
          ndims >= 1 ? ndims : NON_ARRAY_INDEX_COUNT};
}

// Only chare arrays (STS ndims >= 1) can migrate between PEs. Groups and
// nodegroups have one instance per PE, so treating their executions as the
// movement of a single instance would fabricate migrations.
auto is_chare_array(const StsData &sts_data, int32_t collection_id) -> bool {
  auto chare_it = sts_data.chare_map.find(collection_id);
  return chare_it != sts_data.chare_map.end() && chare_it->second.ndims >= 1;
}

// Reads a std::string as the Projections text pup-er writes it: a decimal
// length, then exactly that many characters with no separator between them.
// `toProjectionsFile::bytes` emits Tchar as "%c"
// (charm/src/ck-perf/trace-projections.C:1527), so the characters begin
// immediately after the length's digits and must be read *without* skipping
// whitespace -- a note may legitimately start with a space.
auto read_pup_string(std::istringstream &iss) -> std::string {
  size_t length = 0;
  iss >> length;
  if (!iss) {
    return {};
  }
  std::string value(length, '\0');
  iss.read(value.data(), static_cast<std::streamsize>(length));
  value.resize(static_cast<size_t>(iss.gcount()));
  return value;
}

// Fills in the registered name of a user event, when the STS EVENT table
// declares one. Applications may emit ids they never registered.
void attach_user_event_name(const StsData &sts_data,
                            UserEventOccurrence &occurrence) {
  auto name_it = sts_data.user_event_map.find(occurrence.user_event_id);
  if (name_it != sts_data.user_event_map.end()) {
    occurrence.name = name_it->second.name;
    occurrence.has_name = true;
  }
}

} // namespace

auto process_logs(const std::vector<std::string> &log_file_paths,
                  const StsData &sts_data, const RcData &rc_data,
                  const std::string &output_dir, int32_t step_event_id)
    -> LogParserResult {
  LogParserResult result;
  for (const auto &ep : sts_data.entries) {
    if (ep.name == "dummy_thread_ep") {
      result.thread_ep_id = ep.ep_id;
      break;
    }
  }

  auto exec_schema = charmvz::schema::execution(sts_data.papi_event_names);
  charmvz::ParquetWriter exec_writer(exec_schema,
                                     output_dir + "/execution.parquet");
  charmvz::ParquetWriter idle_writer(charmvz::schema::idle_interval(),
                                     output_dir + "/idle_interval.parquet");
  charmvz::ParquetWriter chare_writer(charmvz::schema::chare_instance(),
                                      output_dir + "/chare_instance.parquet");
  charmvz::ParquetWriter user_event_writer(charmvz::schema::user_event(),
                                           output_dir + "/user_event.parquet");
  charmvz::ParquetWriter user_stat_writer(charmvz::schema::user_stat(),
                                          output_dir + "/user_stat.parquet");
  charmvz::ParquetWriter memory_sample_writer(
      charmvz::schema::memory_sample(), output_dir + "/memory_sample.parquet");

  builders::ExecutionBuilder exec_builder(exec_writer, exec_schema,
                                          sts_data.total_papi_events);
  builders::IdleIntervalBuilder idle_builder(idle_writer);
  builders::ChareInstanceBuilder chare_builder(chare_writer);
  builders::UserEventBuilder user_event_builder(user_event_writer);
  builders::UserStatBuilder user_stat_builder(user_stat_writer);
  builders::MemorySampleBuilder memory_sample_builder(memory_sample_writer);

  // Timestamps are stored as the logs hold them: when the runtime computed a
  // start offset it already subtracted it from every record before writing
  // (see RcData), so nothing is subtracted here.
  (void)rc_data;

  for (const auto &log_path : log_file_paths) {
    spdlog::info("Processing log: {}", log_path);

    std::string filename = std::filesystem::path(log_path).filename().string();
    std::smatch match;
    std::regex log_regex(R"(.*\.(\d+)\.log(\.gz)?$)");
    int32_t current_pe_id = -1;
    if (std::regex_match(filename, match, log_regex)) {
      current_pe_id = std::stoi(match[1]);
    } else {
      // Every row this pipeline writes is keyed on the PE that owns the log
      // file, so a file whose name yields no PE cannot be attributed at all.
      // Parsing it anyway would write rows on a PE that does not exist, which
      // no downstream join rejects.
      spdlog::error("Skipping {}: cannot determine the PE from its name",
                    filename);
      continue;
    }

    zstr::ifstream log_stream(log_path);
    std::string line;
    std::getline(log_stream, line);

    // One ProcessingElement row per valid log, whether or not the markers
    // that bound its computation were written.
    ProcessingElementRecord pe_record{};
    pe_record.pe_id = current_pe_id;
    pe_record.total_pes = sts_data.total_pes;

    // Idle state is explicit: an END_IDLE with nothing open is diagnosed and
    // dropped, never paired with a previous interval's start.
    std::optional<LogEntry> open_idle;

    // Keyed on the (source PE, serial) pair both records carry: END_PROCESSING
    // repeats execPe and execEvent (trace-projections.C, endExecuteLocal), so
    // two sources whose serials coincide never close each other's execution.
    // Ordered so that whatever is still open at end of file is emitted
    // deterministically.
    std::map<std::pair<int32_t, int32_t>, LogEntry> open_processing_entries;

    // Closes one execution. `end` is null when no END_PROCESSING was seen.
    auto emit_execution = [&](const LogEntry &begin, const LogEntry *end) {
      const auto entry = resolve_entry(sts_data, begin.eIdx, current_pe_id);
      auto chare_tup =
          std::make_tuple(entry.collection_id, begin.id[0], begin.id[1],
                          begin.id[2], begin.id[3], begin.id[4], begin.id[5]);
      int64_t inst_id = -1;
      auto inst_it = result.chare_instances.find(chare_tup);
      if (inst_it != result.chare_instances.end())
        inst_id = inst_it->second.instance_id;

      exec_builder.Append(begin, end, current_pe_id, inst_id);

      // Retain this execution's location so Stage 3 can detect migrations as
      // changes of PE. Only chare arrays migrate, so skip everything else.
      if (inst_id >= 0 && is_chare_array(sts_data, entry.collection_id)) {
        InstanceLocationRecord loc;
        loc.instance_id = inst_id;
        loc.collection_id = entry.collection_id;
        loc.pe_id = current_pe_id;
        loc.start_time_us = static_cast<int64_t>(begin.itime);
        loc.has_end_time = end != nullptr;
        loc.end_time_us = end != nullptr ? static_cast<int64_t>(end->itime) : 0;
        result.instance_locations.push_back(loc);
      }
    };

    // USER_EVENT_PAIR writes its begin and its end as two records sharing one
    // `event` serial (trace-projections.C:1093-1096), so they pair on that.
    std::unordered_map<int32_t, LogEntry> open_event_pairs;
    // BEGIN_/END_USER_EVENT_PAIR consume a fresh serial each
    // (trace-projections.C:1102,1109), so they cannot pair on `event`. They
    // pair on (user event id, nestedID), which is precisely what nestedID
    // exists for; the vector is a stack so identically-keyed brackets can nest.
    std::unordered_map<std::tuple<int32_t, int32_t>, std::vector<LogEntry>,
                       TupleHash>
        open_brackets;

    // Emits one row for a bracketed user event, and records a timestep
    // boundary when the bracket is the configured step-boundary event.
    auto emit_bracket = [&](int32_t record_type, int32_t user_event_id,
                            int32_t event, int32_t nested_id, int64_t start_us,
                            int64_t end_us, bool has_end) {
      UserEventOccurrence occurrence{};
      occurrence.pe_id = current_pe_id;
      occurrence.record_type = record_type;
      occurrence.user_event_id = user_event_id;
      occurrence.has_user_event_id = true;
      occurrence.event = event;
      occurrence.has_event = true;
      occurrence.nested_id = nested_id;
      occurrence.has_nested_id = true;
      occurrence.start_time_us = start_us;
      occurrence.end_time_us = end_us;
      occurrence.has_end_time = has_end;
      attach_user_event_name(sts_data, occurrence);
      user_event_builder.Append(occurrence);

      if (step_event_id != NO_STEP_EVENT && user_event_id == step_event_id) {
        StepBoundaryRecord step{};
        step.step_id = nested_id;
        step.pe_id = current_pe_id;
        step.start_time_us = start_us;
        step.end_time_us = end_us;
        step.has_end_time = has_end;
        result.step_boundaries.push_back(step);
      }
    };

    // A record whose required fields could not all be read is a truncated
    // line, typically the last one of a log cut off mid-write. Its defaulted
    // zeros must not reach any table or pairing state.
    auto malformed = [&](std::istringstream &iss, LogType type) {
      if (iss)
        return false;
      spdlog::warn("Malformed {} record on PE {} ({}); skipped",
                   to_string(type), current_pe_id, line);
      ++result.diagnostics.malformed_records;
      return true;
    };

    while (std::getline(log_stream, line)) {
      if (line.empty())
        continue;
      std::istringstream iss(line);
      int token = 0;
      iss >> token;
      if (!iss) {
        ++result.diagnostics.malformed_records;
        spdlog::warn("Unreadable record on PE {} ({}); skipped", current_pe_id,
                     line);
        continue;
      }
      LogType type = static_cast<LogType>(token);

      LogEntry e{};
      e.type = type;

      switch (type) {
      case LogType::CREATION:
      case LogType::CREATION_BCAST:
      case LogType::CREATION_MULTICAST: {
        iss >> e.mIdx >> e.eIdx >> e.itime >> e.event >> e.pe >> e.msglen >>
            e.irecvtime;
        if (type == LogType::CREATION_MULTICAST) {
          iss >> e.numpes;
          e.pes.resize(e.numpes);
          for (int i = 0; i < e.numpes; i++)
            iss >> e.pes[i];
        } else if (type == LogType::CREATION_BCAST) {
          iss >> e.numpes;
        }
        if (malformed(iss, type))
          break;
        CreationRecord cr;
        cr.src_pe = current_pe_id;
        cr.event = e.event;
        cr.ep_id = e.eIdx;
        cr.msg_idx = e.mIdx;
        cr.msg_len = e.msglen;
        cr.send_time_us = static_cast<int64_t>(e.itime);
        cr.send_to_enqueue_us = static_cast<int64_t>(e.irecvtime);
        cr.is_broadcast = (type == LogType::CREATION_BCAST);
        cr.broadcast_fanout = (type == LogType::CREATION_BCAST) ? e.numpes : 0;
        if (type == LogType::CREATION_MULTICAST)
          cr.dst_pes = std::move(e.pes);
        result.creations.push_back(std::move(cr));
        break;
      }
      case LogType::BEGIN_PROCESSING: {
        iss >> e.mIdx >> e.eIdx >> e.itime >> e.event >> e.pe >> e.msglen >>
            e.irecvtime;
        if (malformed(iss, type))
          break;
        const auto entry = resolve_entry(sts_data, e.eIdx, current_pe_id);
        for (int32_t i = 0; i < entry.index_arity; ++i) {
          int32_t index_value = 0;
          iss >> index_value;
          if (i < static_cast<int32_t>(CHARE_INDEX_SLOTS)) {
            e.id[i] = index_value;
          }
        }
        iss >> e.icputime;
        for (int32_t i = 0; i < sts_data.total_papi_events; ++i) {
          uint64_t papi_value = 0;
          iss >> papi_value;
          if (i < static_cast<int32_t>(NUMPAPIEVENTS)) {
            e.papiValues[i] = papi_value;
          }
        }
        if (malformed(iss, type))
          break;

        auto chare_tup = std::make_tuple(entry.collection_id, e.id[0], e.id[1],
                                         e.id[2], e.id[3], e.id[4], e.id[5]);
        if (result.chare_instances.find(chare_tup) ==
            result.chare_instances.end()) {
          ChareInstanceRecord inst;
          inst.instance_id =
              static_cast<int64_t>(result.chare_instances.size()) + 1;
          inst.collection_id = entry.collection_id;
          inst.index_0 = e.id[0];
          inst.index_1 = e.id[1];
          inst.index_2 = e.id[2];
          inst.index_3 = e.id[3];
          inst.index_4 = e.id[4];
          inst.index_5 = e.id[5];
          result.chare_instances[chare_tup] = inst;
          chare_builder.Append(inst);
        }

        BeginProcessingRecord bp;
        bp.dst_pe = current_pe_id;
        bp.ep_id = e.eIdx;
        bp.instance_id = result.chare_instances.at(chare_tup).instance_id;
        bp.msg_len = e.msglen;
        bp.has_recv_time = e.irecvtime != std::numeric_limits<uint64_t>::max();
        bp.recv_time_us =
            bp.has_recv_time ? static_cast<int64_t>(e.irecvtime) : 0;
        bp.exec_start_time_us = static_cast<int64_t>(e.itime);
        result.begin_processing_map.emplace(std::make_tuple(e.pe, e.event), bp);

        auto [open_it, inserted] = open_processing_entries.try_emplace(
            std::make_pair(e.pe, e.event), e);
        if (!inserted) {
          // Two BEGIN_PROCESSING records share a serial with no END between
          // them. (pe_id, event) is the table's key, so the pair cannot be
          // told apart downstream; both observations are still written, the
          // earlier one as incomplete, rather than one silently replacing
          // the other.
          spdlog::warn("Repeated BEGIN_PROCESSING for (src_pe {}, event {}) "
                       "on PE {} before its END_PROCESSING; the earlier "
                       "execution is written with NULL end fields",
                       e.pe, e.event, current_pe_id);
          ++result.diagnostics.duplicate_open_executions;
          emit_execution(open_it->second, nullptr);
          open_it->second = e;
        }
        break;
      }
      case LogType::END_PROCESSING: {
        iss >> e.mIdx >> e.eIdx >> e.itime >> e.event >> e.pe >> e.msglen >>
            e.icputime;
        for (int32_t i = 0; i < sts_data.total_papi_events; ++i) {
          uint64_t papi_value = 0;
          iss >> papi_value;
          if (i < static_cast<int32_t>(NUMPAPIEVENTS)) {
            e.papiValues[i] = papi_value;
          }
        }
        // A truncated END leaves its BEGIN open rather than closing it with
        // defaulted zeros.
        if (malformed(iss, type))
          break;

        auto begin_it =
            open_processing_entries.find(std::make_pair(e.pe, e.event));
        if (begin_it == open_processing_entries.end()) {
          // Nothing to pair with and no start fields to write: the row's
          // start time and index tuple only exist on the BEGIN record.
          spdlog::warn("END_PROCESSING for (src_pe {}, event {}) on PE {} at "
                       "{} us has no open BEGIN_PROCESSING; dropped",
                       e.pe, e.event, current_pe_id, e.itime);
          ++result.diagnostics.orphan_end_processing;
          break;
        }
        if (begin_it->second.eIdx != e.eIdx) {
          // The runtime repeats the entry method on the END record
          // (endExecuteLocal writes execEp), so a different one means this
          // END does not belong to that BEGIN. The BEGIN stays open.
          spdlog::warn("END_PROCESSING for (src_pe {}, event {}) on PE {} "
                       "names entry method {} but the open BEGIN_PROCESSING "
                       "names {}; dropped",
                       e.pe, e.event, current_pe_id, e.eIdx,
                       begin_it->second.eIdx);
          ++result.diagnostics.mismatched_end_processing;
          break;
        }
        emit_execution(begin_it->second, &e);
        open_processing_entries.erase(begin_it);
        break;
      }
      case LogType::BEGIN_IDLE: {
        iss >> e.itime >> e.pe;
        if (malformed(iss, type))
          break;
        if (open_idle) {
          spdlog::warn("BEGIN_IDLE at {} us on PE {} while the idle interval "
                       "from {} us is still open; the earlier one is written "
                       "with a NULL end",
                       e.itime, current_pe_id, open_idle->itime);
          ++result.diagnostics.incomplete_idle_intervals;
          idle_builder.Append(current_pe_id, *open_idle, nullptr);
        }
        open_idle = e;
        break;
      }
      case LogType::END_IDLE: {
        iss >> e.itime >> e.pe;
        if (malformed(iss, type))
          break;
        if (!open_idle) {
          spdlog::warn("END_IDLE at {} us on PE {} has no open BEGIN_IDLE; "
                       "dropped",
                       e.itime, current_pe_id);
          ++result.diagnostics.orphan_end_idle;
          break;
        }
        idle_builder.Append(current_pe_id, *open_idle, &e);
        open_idle.reset();
        break;
      }
      // BEGIN_PACK / END_PACK / BEGIN_UNPACK / END_UNPACK are deliberately not
      // collected. They are emitted by CkPackMessage() / CkUnpackMessage()
      // around ordinary message serialisation, not around chare migration, so
      // they cannot be used to reconstruct MigrationEpisode. See the comment on
      // schema::migration_episode().
      case LogType::BEGIN_COMPUTATION: {
        iss >> e.itime;
        if (malformed(iss, type))
          break;
        if (pe_record.has_begin_time) {
          spdlog::warn("Repeated BEGIN_COMPUTATION on PE {}; keeping the first",
                       current_pe_id);
          break;
        }
        pe_record.begin_time_us = static_cast<int64_t>(e.itime);
        pe_record.has_begin_time = true;
        break;
      }
      case LogType::END_COMPUTATION: {
        iss >> e.itime;
        if (malformed(iss, type))
          break;
        if (pe_record.has_end_time) {
          spdlog::warn("Repeated END_COMPUTATION on PE {}; keeping the first",
                       current_pe_id);
          break;
        }
        pe_record.end_time_us = static_cast<int64_t>(e.itime);
        pe_record.has_end_time = true;
        break;
      }
      case LogType::USER_EVENT: {
        iss >> e.mIdx >> e.itime >> e.event >> e.pe;
        if (malformed(iss, type))
          break;
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.user_event_id = e.mIdx;
        occurrence.has_user_event_id = true;
        occurrence.event = e.event;
        occurrence.has_event = true;
        occurrence.start_time_us = static_cast<int64_t>(e.itime);
        attach_user_event_name(sts_data, occurrence);
        user_event_builder.Append(occurrence);
        break;
      }
      case LogType::USER_SUPPLIED: {
        iss >> e.userSuppliedData >> e.itime;
        if (malformed(iss, type))
          break;
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.start_time_us = static_cast<int64_t>(e.itime);
        occurrence.user_supplied_int = e.userSuppliedData;
        occurrence.has_user_supplied_int = true;
        user_event_builder.Append(occurrence);
        break;
      }
      case LogType::USER_SUPPLIED_NOTE: {
        iss >> e.itime;
        if (malformed(iss, type))
          break;
        e.userSuppliedNote = read_pup_string(iss);
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.start_time_us = static_cast<int64_t>(e.itime);
        occurrence.note = e.userSuppliedNote;
        occurrence.has_note = true;
        user_event_builder.Append(occurrence);
        break;
      }
      case LogType::USER_SUPPLIED_BRACKETED_NOTE: {
        iss >> e.itime >> e.iEndTime >> e.event;
        if (malformed(iss, type))
          break;
        e.userSuppliedNote = read_pup_string(iss);
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.event = e.event;
        occurrence.has_event = true;
        occurrence.start_time_us = static_cast<int64_t>(e.itime);
        occurrence.end_time_us = static_cast<int64_t>(e.iEndTime);
        occurrence.has_end_time = true;
        occurrence.note = e.userSuppliedNote;
        occurrence.has_note = true;
        user_event_builder.Append(occurrence);
        break;
      }
      case LogType::USER_EVENT_PAIR: {
        // The record's own `pe` field is meaningless for the bracketed forms
        // -- their LogEntry constructor never assigns it, so it is 0 on every
        // PE. Attribution uses the PE the log file belongs to.
        iss >> e.mIdx >> e.itime >> e.event >> e.pe >> e.nestedID;
        if (malformed(iss, type))
          break;
        auto open_it = open_event_pairs.find(e.event);
        if (open_it == open_event_pairs.end()) {
          open_event_pairs[e.event] = e;
          break;
        }
        const LogEntry &begin = open_it->second;
        emit_bracket(static_cast<int32_t>(type), begin.mIdx, begin.event,
                     begin.nestedID, static_cast<int64_t>(begin.itime),
                     static_cast<int64_t>(e.itime), true);
        open_event_pairs.erase(open_it);
        break;
      }
      case LogType::BEGIN_USER_EVENT_PAIR: {
        iss >> e.mIdx >> e.itime >> e.event >> e.pe >> e.nestedID;
        if (malformed(iss, type))
          break;
        open_brackets[std::make_tuple(static_cast<int32_t>(e.mIdx), e.nestedID)]
            .push_back(e);
        break;
      }
      case LogType::END_USER_EVENT_PAIR: {
        iss >> e.mIdx >> e.itime >> e.event >> e.pe >> e.nestedID;
        if (malformed(iss, type))
          break;
        auto key = std::make_tuple(static_cast<int32_t>(e.mIdx), e.nestedID);
        auto open_it = open_brackets.find(key);
        if (open_it == open_brackets.end() || open_it->second.empty()) {
          // An END with no BEGIN: tracing was switched on mid-bracket, or the
          // application is unbalanced. Keep it as a zero-width occurrence
          // rather than silently dropping the evidence.
          spdlog::warn("END_USER_EVENT_PAIR with no open bracket for user "
                       "event {} (nestedID {}) on PE {}",
                       e.mIdx, e.nestedID, current_pe_id);
          emit_bracket(static_cast<int32_t>(type), e.mIdx, e.event, e.nestedID,
                       static_cast<int64_t>(e.itime), 0, false);
          break;
        }
        const LogEntry begin = open_it->second.back();
        open_it->second.pop_back();
        emit_bracket(static_cast<int32_t>(LogType::BEGIN_USER_EVENT_PAIR),
                     begin.mIdx, begin.event, begin.nestedID,
                     static_cast<int64_t>(begin.itime),
                     static_cast<int64_t>(e.itime), true);
        break;
      }
      case LogType::USER_STAT: {
        // `cputime` here is the application's own time value, written raw
        // rather than as integer microseconds like every other time field
        // (trace-projections.C:775-776), so it is read as a double. The
        // record's `pe` is genuine (CkMyPe()) but is read and discarded, since
        // every table in this schema keys on the log file's PE.
        iss >> e.itime >> e.statTime >> e.stat >> e.pe >> e.mIdx;
        if (malformed(iss, type))
          break;
        UserStatSample sample{};
        sample.pe_id = current_pe_id;
        sample.stat_id = e.mIdx;
        sample.time_us = static_cast<int64_t>(e.itime);
        sample.stat_value = e.stat;
        // updateStat() records -1 for "the application supplied no time"
        // (trace-projections.C:1144-1148).
        sample.has_user_time = e.statTime != -1.0;
        sample.user_time_s = e.statTime;
        auto stat_it = sts_data.user_stat_map.find(sample.stat_id);
        if (stat_it != sts_data.user_stat_map.end()) {
          sample.name = stat_it->second.name;
          sample.has_name = true;
        }
        user_stat_builder.Append(sample);
        break;
      }
      case LogType::MEMORY_USAGE_CURRENT: {
        // The byte count comes *before* the timestamp, reversing the order
        // every other record uses (trace-projections.C:770-772), and the
        // record carries no PE field at all.
        iss >> e.memUsage >> e.itime;
        if (malformed(iss, type))
          break;
        MemorySample sample{};
        sample.pe_id = current_pe_id;
        sample.time_us = static_cast<int64_t>(e.itime);
        sample.bytes = static_cast<int64_t>(e.memUsage);
        memory_sample_builder.Append(sample);
        break;
      }
      default:
        break;
      }
    }

    // Whatever is still open at end of file was cut short by the end of the
    // log, not by the application. Each is written with NULL end fields so the
    // observation survives and its incompleteness is visible.
    for (const auto &[key, begin] : open_processing_entries) {
      spdlog::warn("BEGIN_PROCESSING for (src_pe {}, event {}) on PE {} has no "
                   "END_PROCESSING before end of log; written with NULL end "
                   "fields",
                   key.first, key.second, current_pe_id);
      ++result.diagnostics.incomplete_executions;
      emit_execution(begin, nullptr);
    }
    if (open_idle) {
      spdlog::warn("BEGIN_IDLE at {} us on PE {} has no END_IDLE before end "
                   "of log; written with a NULL end",
                   open_idle->itime, current_pe_id);
      ++result.diagnostics.incomplete_idle_intervals;
      idle_builder.Append(current_pe_id, *open_idle, nullptr);
    }
    if (!pe_record.has_begin_time) {
      spdlog::warn("PE {} log has no BEGIN_COMPUTATION", current_pe_id);
      ++result.diagnostics.logs_without_begin_computation;
    }
    if (!pe_record.has_end_time) {
      spdlog::warn("PE {} log has no END_COMPUTATION", current_pe_id);
      ++result.diagnostics.logs_without_end_computation;
    }
    result.pes.push_back(pe_record);

    for (const auto &[event_serial, begin] : open_event_pairs) {
      spdlog::warn("Unmatched USER_EVENT_PAIR record for event serial {} on "
                   "PE {}",
                   event_serial, current_pe_id);
      emit_bracket(static_cast<int32_t>(LogType::USER_EVENT_PAIR), begin.mIdx,
                   begin.event, begin.nestedID,
                   static_cast<int64_t>(begin.itime), 0, false);
    }
    for (const auto &[key, stack] : open_brackets) {
      for (const auto &begin : stack) {
        spdlog::warn("Unclosed BEGIN_USER_EVENT_PAIR for user event {} "
                     "(nestedID {}) on PE {}",
                     std::get<0>(key), std::get<1>(key), current_pe_id);
        emit_bracket(static_cast<int32_t>(LogType::BEGIN_USER_EVENT_PAIR),
                     begin.mIdx, begin.event, begin.nestedID,
                     static_cast<int64_t>(begin.itime), 0, false);
      }
    }
  }

  exec_builder.Flush();
  idle_builder.Flush();
  chare_builder.Flush();
  user_event_builder.Flush();
  user_stat_builder.Flush();
  memory_sample_builder.Flush();

  return result;
}

} // namespace charmvz
