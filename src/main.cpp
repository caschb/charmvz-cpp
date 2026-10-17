#include "CLI/CLI.hpp"
#include "log_parser.h"
#include "memory_budget.h"
#include "metadata_tables.h"
#include "parquet_writer.h"
#include "rc_parser.h"
#include "reconstruction.h"
#include "schema.h"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"
#include "spill.h"
#include "sts_parser.h"
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
  std::string memory_budget_text = "0";
  charmvz::PipelineOptions options;
  bool clean_stale_spill = false;

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
    app.add_option("--memory-budget", memory_budget_text,
                   "Memory the conversion should stay within, e.g. 512M or "
                   "4G; state beyond it is spilled to disk. 0 = unlimited, "
                   "nothing is written to disk")
        ->capture_default_str();
    app.add_option("--spill-dir", options.spill_root,
                   "Directory under which spill runs are written; defaults "
                   "to the system temp directory (TMPDIR). Must be on disk: "
                   "a RAM-backed tmpfs defeats the budget")
        ->check(CLI::ExistingDirectory);
    app.add_flag("--keep-spill", options.keep_spill,
                 "Do not delete the spill directory when the run ends");
    app.add_flag("--clean-stale-spill", clean_stale_spill,
                 "Delete spill directories left in the spill root by "
                 "conversions that are no longer running");
    CLI11_PARSE(app, argc, argv);
    options.memory_budget = charmvz::parse_size(memory_budget_text);
  } catch (const std::exception &e) {
    spdlog::error("Error: {}", e.what());
    return 1;
  }

  if (clean_stale_spill) {
    const auto root = options.spill_root.empty()
                          ? std::filesystem::temp_directory_path()
                          : options.spill_root;
    charmvz::remove_stale_spill_directories(root);
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
  if (sts_file_path.empty()) {
    spdlog::error("No .sts file in {}: the log records cannot be interpreted",
                  logs_path.string());
    return 1;
  }
  if (rc_file_path.empty()) {
    spdlog::warn("No .projrc file in {}", logs_path.string());
  }

  try {
    // Stage 1
    auto sts_data = charmvz::parse_sts_file(sts_file_path);
    auto rc_data = charmvz::parse_rc_file(rc_file_path);

    if (options.memory_budget > 0) {
      const uint64_t minimum =
          charmvz::builder_floor_bytes(sts_data.papi_event_names) +
          charmvz::kBudgetHeadroomBytes;
      if (options.memory_budget < minimum) {
        spdlog::error("--memory-budget {} is below the {} this trace needs "
                      "for its output buffers alone",
                      charmvz::format_size(options.memory_budget),
                      charmvz::format_size(minimum));
        return 1;
      }
    }

    charmvz::write_metadata_tables(sts_data, out_path.string());

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
    auto log_result =
        charmvz::process_logs(traces_paths, sts_data, rc_data,
                              out_path.string(), step_event_id, options);

    // Stage 3 & 4
    charmvz::reconstruct_message_and_migration(log_result, rc_data,
                                               out_path.string());
    charmvz::reconstruct_simulation_steps(log_result, out_path.string());
    charmvz::log_spill_summary(log_result.spill->diagnostics);

    spdlog::info("Pipeline successfully finished.");
  } catch (const std::exception &e) {
    spdlog::error("Conversion aborted: {}", e.what());
    return 1;
  }
  return 0;
}
