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

} // namespace

auto process_logs(const std::vector<std::string> &log_file_paths,
                  const StsData &sts_data, const RcData &rc_data,
                  const std::string &output_dir) -> LogParserResult {
  LogParserResult result;

  auto exec_schema = charmvz::schema::execution(sts_data.papi_event_names);
  charmvz::ParquetWriter exec_writer(exec_schema,
                                     output_dir + "/execution.parquet");
  charmvz::ParquetWriter idle_writer(charmvz::schema::idle_interval(),
                                     output_dir + "/idle_interval.parquet");
  charmvz::ParquetWriter chare_writer(charmvz::schema::chare_instance(),
                                      output_dir + "/chare_instance.parquet");

  builders::ExecutionBuilder exec_builder(exec_writer, exec_schema,
                                          sts_data.total_papi_events);
  builders::IdleIntervalBuilder idle_builder(idle_writer);
  builders::ChareInstanceBuilder chare_builder(chare_writer);

  for (const auto &log_path : log_file_paths) {
    spdlog::info("Processing log: {}", log_path);

    std::string filename = std::filesystem::path(log_path).filename().string();
    std::smatch match;
    std::regex log_regex(R"(.*\.(\d+)\.log(\.gz)?$)");
    int32_t current_pe_id = -1;
    if (std::regex_match(filename, match, log_regex)) {
      current_pe_id = std::stoi(match[1]);
    }

    zstr::ifstream log_stream(log_path);
    std::string line;
    std::getline(log_stream, line);

    LogEntry last_begin_idle{};
    int64_t last_pack_start = 0;
    int64_t last_unpack_start = 0;
    std::unordered_map<int32_t, LogEntry> open_processing_entries;

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
      case LogType::BEGIN_PACK: {
        iss >> e.itime >> e.pe;
        last_pack_start = e.itime;
        break;
      }
      case LogType::END_PACK: {
        iss >> e.itime >> e.pe;
        MigrationPack p;
        p.src_pe = current_pe_id;
        p.pack_start_us = last_pack_start;
        p.pack_end_us = e.itime;
        result.packs.push_back(p);
        break;
      }
      case LogType::BEGIN_UNPACK: {
        iss >> e.itime >> e.pe;
        last_unpack_start = e.itime;
        break;
      }
      case LogType::END_UNPACK: {
        iss >> e.itime >> e.pe;
        MigrationUnpack u;
        u.dst_pe = current_pe_id;
        u.unpack_start_us = last_unpack_start;
        u.unpack_end_us = e.itime;
        result.unpacks.push_back(u);
        break;
      }
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
      default:
        break;
      }
    }
  }

  exec_builder.Flush();
  idle_builder.Flush();
  chare_builder.Flush();

  return result;
}

} // namespace charmvz
