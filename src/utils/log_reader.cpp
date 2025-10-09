#include "log_reader.h"
#include "log_entry.h"
#include "spdlog/spdlog.h"
#include "src/utils/log_entry.h"
#include "zstr.hpp"
#include <sstream>
#include <string>

LogReader::~LogReader() { this->log_stream.close(); }

LogEntry LogReader::nextEntry() {
  LogEntry entry;
  std::istringstream log_line_stream;
  std::string log_line;
  std::getline(this->log_stream, log_line);
  log_line_stream.str(log_line);

  int token = -1;
  log_line_stream >> token;

  switch (static_cast<LogType>(token)) {
  case LogType::CREATION:
    entry.type = LogType::CREATION;
    log_line_stream >> entry.mtype >> entry.entry >> entry.timestamp >>
        entry.event >> entry.pe;
    return entry;
  case LogType::BEGIN_PROCESSING:
    entry.type = LogType::BEGIN_PROCESSING;
    spdlog::debug("{}", log_line);
    break;
  case LogType::END_PROCESSING:
    entry.type = LogType::END_PROCESSING;
    break;
  case LogType::BEGIN_COMPUTATION:
    entry.type = LogType::BEGIN_COMPUTATION;
    break;
  case LogType::END_COMPUTATION:
    entry.type = LogType::END_COMPUTATION;
    break;
  case LogType::USER_EVENT:
    entry.type = LogType::USER_EVENT;
    break;
  case LogType::BEGIN_IDLE:
    entry.type = LogType::BEGIN_IDLE;
    log_line_stream >> entry.timestamp >> entry.pe;
    this->last_begin_event = entry;
    this->last_begin_event.open = true;
    break;
  case LogType::END_IDLE:
    entry.type = LogType::END_IDLE;
    log_line_stream >> entry.timestamp >> entry.pe;
    this->last_begin_event = entry;
    this->last_begin_event.open = false;
    break;
  case LogType::BEGIN_PACK:
    entry.type = LogType::BEGIN_PACK;
    break;
  case LogType::END_PACK:
    entry.type = LogType::END_PACK;
    break;
  case LogType::BEGIN_UNPACK:
    entry.type = LogType::BEGIN_UNPACK;
    break;
  case LogType::END_UNPACK:
    entry.type = LogType::END_UNPACK;
    break;
  case LogType::CREATION_BCAST:
    entry.type = LogType::CREATION_BCAST;
    break;
  case LogType::END_PHASE:
    entry.type = LogType::END_PHASE;
    break;
  case LogType::USER_EVENT_PAIR:
    entry.type = LogType::USER_EVENT_PAIR;
    break;
  default:
    spdlog::warn("Unknown log type: {}, {}", token, log_line);
    entry.type = LogType::UNKNOWN;
    break;
  }

  return entry;
}

bool LogReader::hasNextEntry() {
  char peek_char = this->log_stream.peek();
  return !this->log_stream.eof() && peek_char != '\n';
}

const LogEntry *LogReader::getLastBeginEvent() const {
  return &this->last_begin_event;
}
