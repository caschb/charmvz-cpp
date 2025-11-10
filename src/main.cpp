#include "CLI/CLI.hpp"
#include "charmvz/reader.h"
#include "charmvz/writer.h"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

auto main(int argc, char **argv) -> int {
  spdlog::cfg::load_env_levels();
  std::filesystem::path logs_path;
  try {
    CLI::App app{"Parser for Charm++ files"};
    app.add_option("-l,--logs", logs_path, "Logs Directory Path")
        ->required()
        ->check(CLI::ExistingDirectory);
    CLI11_PARSE(app, argc, argv);
  } catch (const std::exception &e) {
    spdlog::error("Error: {}", e.what());
    return 1;
  }

  std::string sts_file_path;
  std::vector<std::string> traces_paths;
  for (auto const &entry : std::filesystem::directory_iterator{logs_path}) {
    const std::string extension{entry.path().extension()};
    if (extension == ".sts") {
      sts_file_path.assign(entry.path());
    } else if (extension == ".gz") {
      traces_paths.emplace_back(entry.path().c_str());
    }
  }

  spdlog::debug("Total logs: {}", traces_paths.size());

  auto sts_data = charmvz::read_sts_file(sts_file_path);
  auto timelines = charmvz::read_log_files(traces_paths);
  for (auto &timeline : timelines) {
    charmvz::write_timeline(timeline);
  }
  return 0;
}
