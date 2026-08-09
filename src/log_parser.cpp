#include "log_parser.h"
#include "builders.h"
#include "parquet_writer.h"
#include "schema.h"
#include "utils/log_entry.h"
#include "zstr.hpp"
#include <arrow/builder.h>
#include <filesystem>
#include <limits>
#include <regex>
#include <spdlog/spdlog.h>
#include <sstream>

namespace charmvz {

namespace {

// How many chare-index values a BEGIN_PROCESSING record carries for this entry
// point. Per charm/src/ck-perf/trace-projections.C, an array chare writes
// exactly `ndims` values (as short int when ndims >= 4, as int otherwise --
// indistinguishable in the text format, where only the count matters), while a
// non-array chare (ndims == -1) writes four. Reading a fixed four would consume
// icputime as an index for 3D arrays and leave three values unread for 6D ones.
auto chare_index_arity(const StsData &sts_data, uint16_t ep_id) -> int32_t {
  auto ep_it = sts_data.ep_map.find(ep_id);
  if (ep_it == sts_data.ep_map.end())
    return NON_ARRAY_INDEX_COUNT;

  auto chare_it = sts_data.chare_map.find(ep_it->second.collection_id);
  if (chare_it == sts_data.chare_map.end())
    return NON_ARRAY_INDEX_COUNT;

  const int32_t ndims = chare_it->second.ndims;
  return ndims >= 1 ? ndims : NON_ARRAY_INDEX_COUNT;
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

  const int64_t global_start_us = rc_data.global_start_time_us;

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

    LogEntry last_begin_idle{};
    std::unordered_map<int32_t, LogEntry> open_processing_entries;

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

    while (std::getline(log_stream, line)) {
      if (line.empty())
        continue;
      std::istringstream iss(line);
      int token = 0;
      iss >> token;
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
        CreationRecord cr;
        cr.ep_id = e.eIdx;
        cr.msg_idx = e.mIdx;
        cr.msg_len = e.msglen;
        cr.send_time_us = e.itime;
        cr.enqueue_time_us = e.irecvtime;
        cr.is_broadcast = (type == LogType::CREATION_BCAST);
        cr.broadcast_fanout = (type == LogType::CREATION_BCAST) ? e.numpes : 1;
        cr.src_pe = current_pe_id;
        if (type == LogType::CREATION_MULTICAST)
          cr.dst_pes = e.pes;

        result.creation_map[std::make_tuple(current_pe_id, e.event)] = cr;
        break;
      }
      case LogType::BEGIN_PROCESSING: {
        iss >> e.mIdx >> e.eIdx >> e.itime >> e.event >> e.pe >> e.msglen >>
            e.irecvtime;
        const int32_t index_arity = chare_index_arity(sts_data, e.eIdx);
        for (int32_t i = 0; i < index_arity; ++i) {
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
        open_processing_entries[e.event] = e;

        BeginProcessingRecord bp;
        bp.dst_pe = current_pe_id;
        bp.recv_time_us = e.irecvtime;
        bp.exec_start_time_us = e.itime;
        result.begin_processing_map[std::make_tuple(e.pe, e.event)] = bp;

        int32_t cid = sts_data.entries.empty()
                          ? 0
                          : sts_data.entries.front().collection_id;
        auto ep_it = sts_data.ep_map.find(e.eIdx);
        if (ep_it != sts_data.ep_map.end())
          cid = ep_it->second.collection_id;

        auto chare_tup = std::make_tuple(cid, e.id[0], e.id[1], e.id[2],
                                         e.id[3], e.id[4], e.id[5]);
        if (result.chare_instances.find(chare_tup) ==
            result.chare_instances.end()) {
          ChareInstanceRecord inst;
          inst.instance_id = result.chare_instances.size() + 1;
          inst.collection_id = cid;
          inst.index_0 = e.id[0];
          inst.index_1 = e.id[1];
          inst.index_2 = e.id[2];
          inst.index_3 = e.id[3];
          inst.index_4 = e.id[4];
          inst.index_5 = e.id[5];
          result.chare_instances[chare_tup] = inst;
          chare_builder.Append(inst);
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

        auto begin_it = open_processing_entries.find(e.event);
        if (begin_it == open_processing_entries.end()) {
          spdlog::warn("Missing BEGIN_PROCESSING for event {} on PE {}",
                       e.event, current_pe_id);
          break;
        }

        const LogEntry &begin = begin_it->second;
        int32_t cid = 0;
        auto ep_it = sts_data.ep_map.find(begin.eIdx);
        if (ep_it != sts_data.ep_map.end())
          cid = ep_it->second.collection_id;

        auto chare_tup =
            std::make_tuple(cid, begin.id[0], begin.id[1], begin.id[2],
                            begin.id[3], begin.id[4], begin.id[5]);
        int64_t inst_id = -1;
        auto inst_it = result.chare_instances.find(chare_tup);
        if (inst_it != result.chare_instances.end())
          inst_id = inst_it->second.instance_id;

        exec_builder.Append(begin, e, current_pe_id,
                            rc_data.global_start_time_us, inst_id);

        // Retain this execution's location so Stage 3 can detect migrations as
        // changes of PE. Only chare arrays migrate, so skip everything else.
        if (inst_id >= 0 && is_chare_array(sts_data, cid)) {
          InstanceLocationRecord loc;
          loc.instance_id = inst_id;
          loc.collection_id = cid;
          loc.pe_id = current_pe_id;
          loc.start_time_us =
              static_cast<int64_t>(begin.itime) - rc_data.global_start_time_us;
          loc.end_time_us =
              static_cast<int64_t>(e.itime) - rc_data.global_start_time_us;
          result.instance_locations.push_back(loc);
        }

        open_processing_entries.erase(begin_it);
        break;
      }
      case LogType::BEGIN_IDLE: {
        iss >> e.itime >> e.pe;
        last_begin_idle = e;
        break;
      }
      case LogType::END_IDLE: {
        iss >> e.itime >> e.pe;
        idle_builder.Append(last_begin_idle, e, rc_data.global_start_time_us);
        break;
      }
      // BEGIN_PACK / END_PACK / BEGIN_UNPACK / END_UNPACK are deliberately not
      // collected. They are emitted by CkPackMessage() / CkUnpackMessage()
      // around ordinary message serialisation, not around chare migration, so
      // they cannot be used to reconstruct MigrationEpisode. See the comment on
      // schema::migration_episode().
      case LogType::BEGIN_COMPUTATION: {
        iss >> e.itime;
        ProcessingElementRecord per;
        per.pe_id = current_pe_id;
        per.total_pes = sts_data.total_pes;
        per.begin_time_us = e.itime;
        per.global_start_us = rc_data.global_start_time_us;
        result.pes.push_back(per);
        break;
      }
      case LogType::END_COMPUTATION: {
        iss >> e.itime;
        for (auto &per : result.pes) {
          if (per.pe_id == current_pe_id)
            per.end_time_us = e.itime;
        }
        break;
      }
      case LogType::USER_EVENT: {
        iss >> e.mIdx >> e.itime >> e.event >> e.pe;
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.user_event_id = e.mIdx;
        occurrence.has_user_event_id = true;
        occurrence.event = e.event;
        occurrence.has_event = true;
        occurrence.start_time_us =
            static_cast<int64_t>(e.itime) - global_start_us;
        attach_user_event_name(sts_data, occurrence);
        user_event_builder.Append(occurrence);
        break;
      }
      case LogType::USER_SUPPLIED: {
        iss >> e.userSuppliedData >> e.itime;
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.start_time_us =
            static_cast<int64_t>(e.itime) - global_start_us;
        occurrence.user_supplied_int = e.userSuppliedData;
        occurrence.has_user_supplied_int = true;
        user_event_builder.Append(occurrence);
        break;
      }
      case LogType::USER_SUPPLIED_NOTE: {
        iss >> e.itime;
        e.userSuppliedNote = read_pup_string(iss);
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.start_time_us =
            static_cast<int64_t>(e.itime) - global_start_us;
        occurrence.note = e.userSuppliedNote;
        occurrence.has_note = true;
        user_event_builder.Append(occurrence);
        break;
      }
      case LogType::USER_SUPPLIED_BRACKETED_NOTE: {
        iss >> e.itime >> e.iEndTime >> e.event;
        e.userSuppliedNote = read_pup_string(iss);
        UserEventOccurrence occurrence{};
        occurrence.pe_id = current_pe_id;
        occurrence.record_type = static_cast<int32_t>(type);
        occurrence.event = e.event;
        occurrence.has_event = true;
        occurrence.start_time_us =
            static_cast<int64_t>(e.itime) - global_start_us;
        occurrence.end_time_us =
            static_cast<int64_t>(e.iEndTime) - global_start_us;
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
        auto open_it = open_event_pairs.find(e.event);
        if (open_it == open_event_pairs.end()) {
          open_event_pairs[e.event] = e;
          break;
        }
        const LogEntry &begin = open_it->second;
        emit_bracket(static_cast<int32_t>(type), begin.mIdx, begin.event,
                     begin.nestedID,
                     static_cast<int64_t>(begin.itime) - global_start_us,
                     static_cast<int64_t>(e.itime) - global_start_us, true);
        open_event_pairs.erase(open_it);
        break;
      }
      case LogType::BEGIN_USER_EVENT_PAIR: {
        iss >> e.mIdx >> e.itime >> e.event >> e.pe >> e.nestedID;
        open_brackets[std::make_tuple(static_cast<int32_t>(e.mIdx), e.nestedID)]
            .push_back(e);
        break;
      }
      case LogType::END_USER_EVENT_PAIR: {
        iss >> e.mIdx >> e.itime >> e.event >> e.pe >> e.nestedID;
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
                       static_cast<int64_t>(e.itime) - global_start_us, 0,
                       false);
          break;
        }
        const LogEntry begin = open_it->second.back();
        open_it->second.pop_back();
        emit_bracket(static_cast<int32_t>(LogType::BEGIN_USER_EVENT_PAIR),
                     begin.mIdx, begin.event, begin.nestedID,
                     static_cast<int64_t>(begin.itime) - global_start_us,
                     static_cast<int64_t>(e.itime) - global_start_us, true);
        break;
      }
      case LogType::USER_STAT: {
        // `cputime` here is the application's own time value, written raw
        // rather than as integer microseconds like every other time field
        // (trace-projections.C:775-776), so it is read as a double. The
        // record's `pe` is genuine (CkMyPe()) but is read and discarded, since
        // every table in this schema keys on the log file's PE.
        iss >> e.itime >> e.statTime >> e.stat >> e.pe >> e.mIdx;
        UserStatSample sample{};
        sample.pe_id = current_pe_id;
        sample.stat_id = e.mIdx;
        sample.time_us = static_cast<int64_t>(e.itime) - global_start_us;
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
        MemorySample sample{};
        sample.pe_id = current_pe_id;
        sample.time_us = static_cast<int64_t>(e.itime) - global_start_us;
        sample.bytes = static_cast<int64_t>(e.memUsage);
        memory_sample_builder.Append(sample);
        break;
      }
      default:
        break;
      }
    }

    // Brackets still open at end of file: the run was cut short, or tracing
    // ended inside the bracket. Emit them with no end timestamp so the
    // occurrence is still visible.
    for (const auto &[event_serial, begin] : open_event_pairs) {
      spdlog::warn("Unmatched USER_EVENT_PAIR record for event serial {} on "
                   "PE {}",
                   event_serial, current_pe_id);
      emit_bracket(static_cast<int32_t>(LogType::USER_EVENT_PAIR), begin.mIdx,
                   begin.event, begin.nestedID,
                   static_cast<int64_t>(begin.itime) - global_start_us, 0,
                   false);
    }
    for (const auto &[key, stack] : open_brackets) {
      for (const auto &begin : stack) {
        spdlog::warn("Unclosed BEGIN_USER_EVENT_PAIR for user event {} "
                     "(nestedID {}) on PE {}",
                     std::get<0>(key), std::get<1>(key), current_pe_id);
        emit_bracket(static_cast<int32_t>(LogType::BEGIN_USER_EVENT_PAIR),
                     begin.mIdx, begin.event, begin.nestedID,
                     static_cast<int64_t>(begin.itime) - global_start_us, 0,
                     false);
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
