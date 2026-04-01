#include "log_entry.h"
#include <stdexcept>

auto to_string(const LogType &type) -> const char * {
  switch (type) {
  case LogType::UNKNOWN: return "UNKNOWN";
  case LogType::CREATION: return "CREATION";
  case LogType::BEGIN_PROCESSING: return "BEGIN_PROCESSING";
  case LogType::END_PROCESSING: return "END_PROCESSING";
  case LogType::ENQUEUE: return "ENQUEUE";
  case LogType::DEQUEUE: return "DEQUEUE";
  case LogType::BEGIN_COMPUTATION: return "BEGIN_COMPUTATION";
  case LogType::END_COMPUTATION: return "END_COMPUTATION";
  case LogType::BEGIN_INTERRUPT: return "BEGIN_INTERRUPT";
  case LogType::END_INTERRUPT: return "END_INTERRUPT";
  case LogType::MESSAGE_RECV: return "MESSAGE_RECV";
  case LogType::BEGIN_TRACE: return "BEGIN_TRACE";
  case LogType::END_TRACE: return "END_TRACE";
  case LogType::USER_EVENT: return "USER_EVENT";
  case LogType::BEGIN_IDLE: return "BEGIN_IDLE";
  case LogType::END_IDLE: return "END_IDLE";
  case LogType::BEGIN_PACK: return "BEGIN_PACK";
  case LogType::END_PACK: return "END_PACK";
  case LogType::BEGIN_UNPACK: return "BEGIN_UNPACK";
  case LogType::END_UNPACK: return "END_UNPACK";
  case LogType::CREATION_BCAST: return "CREATION_BCAST";
  case LogType::CREATION_MULTICAST: return "CREATION_MULTICAST";
  case LogType::MEMORY_MALLOC: return "MEMORY_MALLOC";
  case LogType::MEMORY_FREE: return "MEMORY_FREE";
  case LogType::USER_SUPPLIED: return "USER_SUPPLIED";
  case LogType::MEMORY_USAGE_CURRENT: return "MEMORY_USAGE_CURRENT";
  case LogType::USER_SUPPLIED_NOTE: return "USER_SUPPLIED_NOTE";
  case LogType::USER_SUPPLIED_BRACKETED_NOTE: return "USER_SUPPLIED_BRACKETED_NOTE";
  case LogType::END_PHASE: return "END_PHASE";
  case LogType::SURROGATE_BLOCK: return "SURROGATE_BLOCK";
  case LogType::USER_STAT: return "USER_STAT";
  case LogType::BEGIN_USER_EVENT_PAIR: return "BEGIN_USER_EVENT_PAIR";
  case LogType::END_USER_EVENT_PAIR: return "END_USER_EVENT_PAIR";
  case LogType::USER_EVENT_PAIR: return "USER_EVENT_PAIR";
  default: return "UNKNOWN";
  }
}