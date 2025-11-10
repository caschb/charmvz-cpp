#include "charmvz/reader.h"
#include "charmvz/timeline.h"
#include "spdlog/spdlog.h"
#include <cstdint>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
// Example CHARE line: CHARE 11 "CkReductionMgr" -1
auto parse_chare_line(const std::string_view line) {
  std::istringstream line_stream{std::string{line}};
  std::string token;
  line_stream >> token; // Skip "CHARE"
  int64_t idx{};
  int64_t ndims{};
  std::string chare_name;
  line_stream >> idx >> chare_name >> ndims;
  return Chare{.name = chare_name.substr(1, chare_name.length() - 2),
               .dimensions = ndims,
               .idx = idx};
}

// Example ENTRY line: ENTRY CHARE 80
// "inmem_checkpoint(CkArrayCheckPTReqMessage* impl_msg)" 28 0
auto parse_entry_line(const std::string_view line) {
  std::istringstream line_stream{std::string{line}};
  std::string token;
  line_stream >> token; // Skip "ENTRY"
  line_stream >> token; // Skip "CHARE"
  int64_t idx{};
  int64_t chare_id{};
  int64_t msg_id{};
  std::string chare_name{};
  line_stream >> idx;
  std::getline(line_stream, chare_name, '"');
  std::getline(line_stream, chare_name, '"');
  line_stream >> chare_id >> msg_id;
  return Entry{
      .name = chare_name, .chare_id = chare_id, .msg_id = msg_id, .idx = idx};
}

// Example MESSAGE line: MESSAGE 0 56
auto parse_message_line(const std::string_view line) {
  std::istringstream line_stream{std::string{line}};
  std::string token{};
  line_stream >> token; // Skip "MESSAGE"
  int64_t idx{};
  int64_t size{};
  line_stream >> idx >> size;
  return Message{.size = size, .idx = idx};
}

} // namespace

auto read_sts_file(const std::string_view sts_file_path) -> StsData {
  spdlog::debug("Reading {}", sts_file_path);
  std::ifstream sts_file{std::string(sts_file_path), std::ios::in};
  std::vector<Chare> chares;
  std::vector<Entry> entries;
  std::vector<Message> messages;
  while (sts_file) {
    std::string line;
    std::getline(sts_file, line);
    std::istringstream line_stream{line};
    std::string token;
    line_stream >> token;
    if (token == "CHARE") {
      const Chare chare = parse_chare_line(line);
      chares.emplace_back(chare);
    } else if (token == "ENTRY") {
      const Entry entry = parse_entry_line(line);
      entries.emplace_back(entry);
    } else if (token == "MESSAGE") {
      const Message message = parse_message_line(line);
      messages.emplace_back(message);
    }
  }
  sts_file.close();
  spdlog::debug("Read {} chares, {} entries, {} messages", chares.size(),
                entries.size(), messages.size());
  return StsData{.chares = chares, .entries = entries, .messages = messages};
}

auto read_log_files(const std::vector<std::string> &log_file_paths)
    -> std::vector<Timeline> {
  spdlog::debug("Reading {} log files", log_file_paths.size());
  std::vector<Timeline> timelines;
  for (const auto &log_file_path : log_file_paths) {
    spdlog::debug("Reading log file: {}", log_file_path);
    auto timeline = create_timeline(log_file_path);
    timelines.push_back(timeline);
  }
  return timelines;
}
