#include "reader.h"
#include "spdlog/spdlog.h"
#include "src/utils/log_entry.h"
#include "src/utils/log_reader.h"
#include "src/utils/timeline.h"
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

// Example CHARE line: CHARE 11 "CkReductionMgr" -1
Chare parse_chare_line(const std::string_view line) {
  std::istringstream line_stream{std::string{line}};
  std::string token;
  line_stream >> token; // Skip "CHARE"
  int64_t idx, ndims;
  std::string chare_name;
  line_stream >> idx >> chare_name >> ndims;
  return Chare{chare_name.substr(1, chare_name.length() - 2), ndims, idx};
}

// Example ENTRY line: ENTRY CHARE 80
// "inmem_checkpoint(CkArrayCheckPTReqMessage* impl_msg)" 28 0
Entry parse_entry_line(const std::string_view line) {
  std::istringstream line_stream{std::string{line}};
  std::string token;
  line_stream >> token; // Skip "ENTRY"
  line_stream >> token; // Skip "CHARE"
  int64_t idx, chare_id, msg_id;
  std::string chare_name;
  line_stream >> idx;
  std::getline(line_stream, chare_name, '"');
  std::getline(line_stream, chare_name, '"');
  line_stream >> chare_id >> msg_id;
  return Entry{chare_name, chare_id, msg_id, idx};
}

// Example MESSAGE line: MESSAGE 0 56
Message parse_message_line(const std::string_view line) {
  std::istringstream line_stream{std::string{line}};
  std::string token;
  line_stream >> token; // Skip "MESSAGE"
  int64_t idx, size;
  line_stream >> idx >> size;
  return Message{size, idx};
}

StsData read_sts_file(const std::string_view sts_file_path) {
  spdlog::debug("Reading {}", sts_file_path);
  std::ifstream sts_file{sts_file_path.data(), std::ios::in};
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
      Chare chare = parse_chare_line(line);
      chares.push_back(chare);
    } else if (token == "ENTRY") {
      Entry entry = parse_entry_line(line);
      entries.push_back(entry);
    } else if (token == "MESSAGE") {
      Message message = parse_message_line(line);
      messages.push_back(message);
    }
  }
  sts_file.close();
  spdlog::debug("Read {} chares, {} entries, {} messages", chares.size(),
                entries.size(), messages.size());
  return StsData{chares, entries, messages};
}

std::vector<LogEntry>
read_log_files(const std::vector<std::string> &log_file_paths) {
  spdlog::debug("Reading {} log files", log_file_paths.size());
  std::vector<LogEntry> log_entries;
  for (const auto &log_file_path : log_file_paths) {
    spdlog::debug("Reading log file: {}", log_file_path);
    auto timeline = create_timeline(log_file_path);
  }
  return log_entries;
}
