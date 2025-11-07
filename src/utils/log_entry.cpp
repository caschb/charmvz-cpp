#include "log_entry.h"
#include <stdexcept>

auto to_string(const LogType &type) -> const char * {
  switch (type) {
  case LogType::UNKNOWN:
    return "UNKNOWN";
  case LogType::CREATION:
    return "CREATION";
  case LogType::BEGIN_PROCESSING:
    return "BEGIN_PROCESSING";
  case LogType::END_PROCESSING:
    return "END_PROCESSING";
  case LogType::BEGIN_COMPUTATION:
    return "BEGIN_COMPUTATION";
  case LogType::END_COMPUTATION:
    return "END_COMPUTATION";
  case LogType::USER_EVENT:
    return "USER_EVENT";
  case LogType::BEGIN_IDLE:
    return "BEGIN_IDLE";
  case LogType::END_IDLE:
    return "END_IDLE";
  case LogType::BEGIN_PACK:
    return "BEGIN_PACK";
  case LogType::END_PACK:
    return "END_PACK";
  case LogType::BEGIN_UNPACK:
    return "BEGIN_UNPACK";
  case LogType::END_UNPACK:
    return "END_UNPACK";
  case LogType::CREATION_BCAST:
    return "CREATION_BCAST";
  case LogType::END_PHASE:
    return "END_PHASE";
  case LogType::USER_EVENT_PAIR:
    return "USER_EVENT_PAIR";
  default:
    throw std::invalid_argument("Unknown LogType");
  }
}