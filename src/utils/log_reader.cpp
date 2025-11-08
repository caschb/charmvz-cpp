#include "log_reader.h"
#include "log_entry.h"
#include "spdlog/spdlog.h"
#include "src/utils/log_entry.h"
#include "zstr.hpp"
#include <sstream>
#include <string>

LogReader::~LogReader() { this->log_stream.close(); }

auto LogReader::nextEntry() -> LogEntry {
  LogEntry log_entry{};
  std::istringstream log_line_stream;
  std::string log_line;
  std::getline(this->log_stream, log_line);
  log_line_stream.str(log_line);

  int token = -1;
  log_line_stream >> token;

  switch (static_cast<LogType>(token)) {
  case LogType::CREATION:
    log_entry.type = LogType::CREATION;
    log_line_stream >> log_entry.mtype >> log_entry.entry_point >>
        log_entry.timestamp >> log_entry.event >> log_entry.pe >>
        log_entry.mtype >> log_entry.send_time;
    return log_entry;
  case LogType::BEGIN_PROCESSING:
    log_entry.type = LogType::BEGIN_PROCESSING;
    log_line_stream >> log_entry.mtype >> log_entry.entry_point >>
        log_entry.timestamp >> log_entry.event >> log_entry.pe >>
        log_entry.mtype >> log_entry.recv_time;
    break;
  case LogType::END_PROCESSING:
    log_entry.type = LogType::END_PROCESSING;
    log_line_stream >> log_entry.mtype >> log_entry.entry_point >>
        log_entry.timestamp >> log_entry.event >> log_entry.pe >>
        log_entry.mtype;
    break;
  case LogType::BEGIN_COMPUTATION:
    log_entry.type = LogType::BEGIN_COMPUTATION;
    break;
  case LogType::END_COMPUTATION:
    log_entry.type = LogType::END_COMPUTATION;
    log_line_stream >> log_entry.timestamp;
    break;
  case LogType::USER_EVENT:
    log_entry.type = LogType::USER_EVENT;
    spdlog::debug("USER_EVENT:{}", log_line);
    break;
  case LogType::BEGIN_IDLE:
    log_entry.type = LogType::BEGIN_IDLE;
    log_line_stream >> log_entry.timestamp >> log_entry.pe;
    this->last_begin_event = log_entry;
    this->last_begin_event.open = true;
    break;
  case LogType::END_IDLE:
    log_entry.type = LogType::END_IDLE;
    log_line_stream >> log_entry.timestamp >> log_entry.pe;
    this->last_begin_event = log_entry;
    this->last_begin_event.open = false;
    break;
  case LogType::BEGIN_PACK:
    log_entry.type = LogType::BEGIN_PACK;
    log_line_stream >> log_entry.timestamp >> log_entry.pe;
    break;
  case LogType::END_PACK:
    log_entry.type = LogType::END_PACK;
    log_line_stream >> log_entry.timestamp >> log_entry.pe;
    break;
  case LogType::BEGIN_UNPACK:
    log_entry.type = LogType::BEGIN_UNPACK;
    log_line_stream >> log_entry.timestamp >> log_entry.pe;
    break;
  case LogType::END_UNPACK:
    log_entry.type = LogType::END_UNPACK;
    log_line_stream >> log_entry.timestamp >> log_entry.pe;
    break;
  case LogType::CREATION_BCAST:
    log_entry.type = LogType::CREATION_BCAST;
    log_line_stream >> log_entry.mtype >> log_entry.entry_point >>
        log_entry.timestamp >> log_entry.event >> log_entry.pe >>
        log_entry.mtype >> log_entry.send_time >> log_entry.num_pes;
    break;
  case LogType::END_PHASE:
    log_entry.type = LogType::END_PHASE;
    break;
  case LogType::USER_EVENT_PAIR:
    log_entry.type = LogType::USER_EVENT_PAIR;
    break;
  default:
    spdlog::warn("Unknown log type: {}, {}", token, log_line);
    log_entry.type = LogType::UNKNOWN;
    break;
  }

  return log_entry;
}

auto LogReader::hasNextEntry() -> bool {
  const int peek_char = this->log_stream.peek();
  return !this->log_stream.eof() && peek_char != '\n';
}

auto LogReader::getLastBeginEvent() const -> const LogEntry * {
  if (last_begin_event.open) {
    return &this->last_begin_event;
  }
  return nullptr;
}
