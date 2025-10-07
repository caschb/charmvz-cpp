#include "reader.h"
#include "spdlog/spdlog.h"
#include "zstr.hpp"
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
    spdlog::debug("Reading {}", log_file_path);
    zstr::ifstream log_file{log_file_path.c_str()};
    if (!log_file) {
      spdlog::error("Failed to open log file: {}", log_file_path);
      continue;
    }
    std::string line;
    while (std::getline(log_file, line)) {
      // Process each line of the log file
      std::istringstream line_stream{line};
      int token;
      line_stream >> token;
      if (token == static_cast<int>(LogType::CREATION)) {
        LogEntry entry;
        entry.type = LogType::CREATION;
        line_stream >> entry.msg_id >> entry.event_id >> entry.timestamp >>
            entry.event >> entry.processing_element >> entry.message_len >>
            entry.recvtime;
        spdlog::trace(
            "Log Entry - Type: {}, Msg ID: {}, Event ID: {}, Timestamp: {}, "
            "Event: {}, PE: {}, Msg Len: {}, Recv Time: {}",
            static_cast<int>(entry.type), entry.msg_id, entry.event_id,
            entry.timestamp, entry.event, entry.processing_element,
            entry.message_len, entry.recvtime);
        log_entries.push_back(entry);
      } else if (token == static_cast<int>(LogType::BEGIN_PROCESSING)) {
        // Handle BEGIN_PROCESSING log
      } else if (token == static_cast<int>(LogType::END_PROCESSING)) {
        // Handle END_PROCESSING log
      } else if (token == static_cast<int>(LogType::BEGIN_COMPUTATION)) {
        // Handle BEGIN_COMPUTATION log
      } else if (token == static_cast<int>(LogType::END_COMPUTATION)) {
        // Handle END_COMPUTATION log
      } else if (token == static_cast<int>(LogType::USER_EVENT)) {
        // Handle USER_EVENT log
      } else if (token == static_cast<int>(LogType::BEGIN_IDLE)) {
        // Handle BEGIN_IDLE log
      } else if (token == static_cast<int>(LogType::END_IDLE)) {
        // Handle END_IDLE log
      } else if (token == static_cast<int>(LogType::BEGIN_PACK)) {
        // Handle BEGIN_PACK log
      } else if (token == static_cast<int>(LogType::END_PACK)) {
        // Handle END_PACK log
      } else if (token == static_cast<int>(LogType::BEGIN_UNPACK)) {
        // Handle BEGIN_UNPACK log
      } else if (token == static_cast<int>(LogType::END_UNPACK)) {
        // Handle END_UNPACK log
      } else if (token == static_cast<int>(LogType::CREATION_BCAST)) {
        // Handle CREATION_BCAST log
      } else if (token == static_cast<int>(LogType::END_PHASE)) {
        // Handle END_PHASE log
      } else if (token == static_cast<int>(LogType::USER_EVENT_PAIR)) {
        // Handle USER_EVENT_PAIR log
      }
    }
    log_file.close();
  }
  return log_entries;
}
