#include "CLI/CLI.hpp"
#include "log_parser.h"
#include "parquet_writer.h"
#include "rc_parser.h"
#include "reconstruction.h"
#include "schema.h"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"
#include "sts_parser.h"
#include <arrow/builder.h>
#include <exception>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

auto main(int argc, char **argv) -> int {
  spdlog::cfg::load_env_levels();
  std::filesystem::path logs_path;
  std::filesystem::path out_path;
  // The name the application passed to traceRegisterUserEvent() for the
  // bracketed event that delimits one timestep. Configurable because the name
  // is the application's choice, not the runtime's.
  std::string step_event_name = "SimulationStep";

  try {
    CLI::App app{"Parser for Charm++ files to Apache Arrow"};
    app.add_option("-l,--logs", logs_path, "Logs Directory Path")
        ->required()
        ->check(CLI::ExistingDirectory);
    app.add_option("-o,--output", out_path, "Output Directory Path")
        ->required();
    app.add_option("-s,--step-event", step_event_name,
                   "Name of the registered bracketed user event that delimits "
                   "a timestep; its nestedID carries the step index")
        ->capture_default_str();
    CLI11_PARSE(app, argc, argv);
  } catch (const std::exception &e) {
    spdlog::error("Error: {}", e.what());
    return 1;
  }

  if (!std::filesystem::exists(out_path)) {
    std::filesystem::create_directories(out_path);
  }

  std::string sts_file_path;
  std::string rc_file_path;
  std::vector<std::string> traces_paths;

  for (auto const &entry : std::filesystem::directory_iterator{logs_path}) {
    const std::string extension{entry.path().extension()};
    if (extension == ".sts") {
      sts_file_path.assign(entry.path());
    } else if (extension == ".projrc") {
      rc_file_path.assign(entry.path());
    } else if (extension == ".gz" || extension == ".log") {
      // Match the per-PE log naming, not merely the extension. A trace
      // directory can legitimately hold other gzipped files -- an archive of
      // the trace itself is the common one -- and zstr will decompress any of
      // them happily. The parser then reads whatever bytes come out as
      // records, and the few that resemble one land in the output attributed
      // to no PE at all.
      static const std::regex log_name{R"(.*\.\d+\.log(\.gz)?$)"};
      const std::string filename{entry.path().filename()};
      if (std::regex_match(filename, log_name)) {
        traces_paths.emplace_back(entry.path().c_str());
      } else {
        spdlog::warn("Ignoring {}: not a per-PE log file name", filename);
      }
    }
  }

  spdlog::info("Total logs: {}", traces_paths.size());

  // Stage 1
  auto sts_data = charmvz::parse_sts_file(sts_file_path);
  auto rc_data = charmvz::parse_rc_file(rc_file_path);

  if (!sts_data.chares.empty()) {
    charmvz::ParquetWriter chare_writer(charmvz::schema::chare_collection(),
                                        out_path.string() +
                                            "/chare_collection.parquet");
    arrow::Int32Builder c_id, ndims;
    arrow::StringBuilder c_name;
    for (const auto &c : sts_data.chares) {
      PARQUET_THROW_NOT_OK(c_id.Append(c.collection_id));
      PARQUET_THROW_NOT_OK(c_name.Append(c.name));
      PARQUET_THROW_NOT_OK(ndims.Append(c.ndims));
    }
    std::shared_ptr<arrow::Array> a_cid, a_name, a_ndims;
    PARQUET_THROW_NOT_OK(c_id.Finish(&a_cid));
    PARQUET_THROW_NOT_OK(c_name.Finish(&a_name));
    PARQUET_THROW_NOT_OK(ndims.Finish(&a_ndims));
    auto batch =
        arrow::RecordBatch::Make(charmvz::schema::chare_collection(),
                                 a_cid->length(), {a_cid, a_name, a_ndims});
    chare_writer.WriteBatch(batch);
  }

  if (!sts_data.entries.empty()) {
    charmvz::ParquetWriter ep_writer(charmvz::schema::entry_method(),
                                     out_path.string() +
                                         "/entry_method.parquet");
    arrow::Int32Builder ep_id, c_id_ep, msg_idx;
    arrow::StringBuilder ep_name;
    for (const auto &ep : sts_data.entries) {
      PARQUET_THROW_NOT_OK(ep_id.Append(ep.ep_id));
      PARQUET_THROW_NOT_OK(ep_name.Append(ep.name));
      PARQUET_THROW_NOT_OK(c_id_ep.Append(ep.collection_id));
      PARQUET_THROW_NOT_OK(msg_idx.Append(ep.msg_idx));
    }
    std::shared_ptr<arrow::Array> a_ep_id, a_ep_name, a_cid_ep, a_msg_idx;
    PARQUET_THROW_NOT_OK(ep_id.Finish(&a_ep_id));
    PARQUET_THROW_NOT_OK(ep_name.Finish(&a_ep_name));
    PARQUET_THROW_NOT_OK(c_id_ep.Finish(&a_cid_ep));
    PARQUET_THROW_NOT_OK(msg_idx.Finish(&a_msg_idx));
    auto batch = arrow::RecordBatch::Make(
        charmvz::schema::entry_method(), a_ep_id->length(),
        {a_ep_id, a_ep_name, a_cid_ep, a_msg_idx});
    ep_writer.WriteBatch(batch);
  }

  if (!sts_data.messages.empty()) {
    charmvz::ParquetWriter msg_type_writer(charmvz::schema::message_type(),
                                           out_path.string() +
                                               "/message_type.parquet");
    arrow::Int32Builder mt_idx;
    arrow::Int64Builder mt_size;
    for (const auto &m : sts_data.messages) {
      PARQUET_THROW_NOT_OK(mt_idx.Append(m.msg_idx));
      PARQUET_THROW_NOT_OK(mt_size.Append(static_cast<int64_t>(m.size)));
    }
    std::shared_ptr<arrow::Array> a_mt_idx, a_mt_size;
    PARQUET_THROW_NOT_OK(mt_idx.Finish(&a_mt_idx));
    PARQUET_THROW_NOT_OK(mt_size.Finish(&a_mt_size));
    auto batch =
        arrow::RecordBatch::Make(charmvz::schema::message_type(),
                                 a_mt_idx->length(), {a_mt_idx, a_mt_size});
    msg_type_writer.WriteBatch(batch);
  }

  const int32_t step_event_id =
      charmvz::find_user_event_id(sts_data, step_event_name);
  if (step_event_id == charmvz::NO_STEP_EVENT) {
    spdlog::info("No user event named \"{}\" is registered in the STS file; "
                 "no timesteps will be reconstructed",
                 step_event_name);
  } else {
    spdlog::info("Reconstructing timesteps from user event {} (\"{}\")",
                 step_event_id, step_event_name);
  }

  // Stage 2
  auto log_result = charmvz::process_logs(traces_paths, sts_data, rc_data,
                                          out_path.string(), step_event_id);

  // Stage 3 & 4
  charmvz::reconstruct_message_and_migration(log_result, sts_data, rc_data,
                                             out_path.string());
  charmvz::reconstruct_simulation_steps(log_result, out_path.string());

  spdlog::info("Pipeline successfully finished.");
  return 0;
}
