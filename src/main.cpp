#include "CLI/CLI.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"
#include "sts_parser.h"
#include "rc_parser.h"
#include "log_parser.h"
#include "reconstruction.h"
#include <exception>
#include <filesystem>
#include <string>
#include <vector>
#include <arrow/builder.h>
#include "parquet_writer.h"
#include "schema.h"

auto main(int argc, char **argv) -> int {
  spdlog::cfg::load_env_levels();
  std::filesystem::path logs_path;
  std::filesystem::path out_path;

  try {
    CLI::App app{"Parser for Charm++ files to Apache Arrow"};
    app.add_option("-l,--logs", logs_path, "Logs Directory Path")
        ->required()
        ->check(CLI::ExistingDirectory);
    app.add_option("-o,--output", out_path, "Output Directory Path")
        ->required();
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
      traces_paths.emplace_back(entry.path().c_str());
    }
  }

  spdlog::info("Total logs: {}", traces_paths.size());

  // Stage 1
  auto sts_data = charmvz::parse_sts_file(sts_file_path);
  auto rc_data = charmvz::parse_rc_file(rc_file_path);

  if (!sts_data.chares.empty()) {
    charmvz::ParquetWriter chare_writer(charmvz::schema::chare_collection(), out_path.string() + "/chare_collection.parquet");
    arrow::Int32Builder c_id, ndims;
    arrow::StringBuilder c_name;
    for (const auto& c : sts_data.chares) {
       PARQUET_THROW_NOT_OK(c_id.Append(c.collection_id));
       PARQUET_THROW_NOT_OK(c_name.Append(c.name));
       PARQUET_THROW_NOT_OK(ndims.Append(c.ndims));
    }
    std::shared_ptr<arrow::Array> a_cid, a_name, a_ndims;
    PARQUET_THROW_NOT_OK(c_id.Finish(&a_cid));
    PARQUET_THROW_NOT_OK(c_name.Finish(&a_name));
    PARQUET_THROW_NOT_OK(ndims.Finish(&a_ndims));
    auto batch = arrow::RecordBatch::Make(charmvz::schema::chare_collection(), a_cid->length(), {a_cid, a_name, a_ndims});
    chare_writer.WriteBatch(batch);
  }

  if (!sts_data.entries.empty()) {
    charmvz::ParquetWriter ep_writer(charmvz::schema::entry_method(), out_path.string() + "/entry_method.parquet");
    arrow::Int32Builder ep_id, c_id_ep, msg_idx;
    arrow::StringBuilder ep_name;
    for (const auto& ep : sts_data.entries) {
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
    auto batch = arrow::RecordBatch::Make(charmvz::schema::entry_method(), a_ep_id->length(), {a_ep_id, a_ep_name, a_cid_ep, a_msg_idx});
    ep_writer.WriteBatch(batch);
  }

  // Stage 2
  auto log_result = charmvz::process_logs(traces_paths, sts_data, rc_data, out_path.string());
  
  // Stage 3 & 4
  charmvz::reconstruct_message_and_migration(log_result, sts_data, rc_data, out_path.string());

  spdlog::info("Pipeline successfully finished.");
  return 0;
}
