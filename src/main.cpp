#include "CLI/CLI.hpp"
#include "reader/reader.h"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  spdlog::cfg::load_env_levels();
  CLI::App app{"Parser for Charm++ files"};

  // std::filesystem::path logs_path;
  std::filesystem::path logs_path;
  app.add_option("-s,--sts", logs_path, "STS File Path")
      ->required()
      ->check(CLI::ExistingDirectory);
  CLI11_PARSE(app, argc, argv);

  std::string sts_file_path;
  std::vector<std::string> traces_paths;  
  for (auto const &entry : std::filesystem::directory_iterator{logs_path}) {
    std::string extension{entry.path().extension()};
    if (extension == ".sts") {
      sts_file_path.assign(entry.path());
    } else if (extension == ".gz") {
      traces_paths.push_back(entry.path().c_str());
    }
  }

  spdlog::debug("Total logs: {}", traces_paths.size());  

  read_sts_file(sts_file_path);

  return 0;
}
