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

  // Stage 2
  auto log_result = charmvz::process_logs(traces_paths, sts_data, rc_data, out_path.string());
  
  // Stage 3 & 4
  charmvz::reconstruct_message_and_migration(log_result, sts_data, rc_data, out_path.string());

  spdlog::info("Pipeline successfully finished.");
  return 0;
}
