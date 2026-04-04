#include "sts_parser.h"
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

namespace charmvz {

auto parse_sts_file(const std::string_view sts_file_path) -> StsData {
  StsData data;
  std::ifstream f{std::string(sts_file_path)};
  if (!f.is_open()) {
    spdlog::error("Cannot open STS file: {}", sts_file_path);
    return data;
  }

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty())
      continue;
    std::istringstream iss(line);
    std::string token;
    iss >> token;

    if (token == "VERSION") {
      iss >> std::ws;
      if (iss.peek() == '"') {
        iss.get();
        std::getline(iss, data.version, '"');
      } else {
        iss >> data.version;
      }
      if (data.version != "11.0") {
        spdlog::error("Unsupported STS VERSION '{}'. Only 11.0 is supported.",
                      data.version);
        throw std::runtime_error("Unsupported STS VERSION");
      }
    } else if (token == "TOTAL_PHASES") {
      iss >> data.total_phases;
    } else if (token == "PROCESSORS") {
      iss >> data.total_pes;
    } else if (token == "TOTAL_PAPI_EVENTS") {
      iss >> data.total_papi_events;
      data.papi_event_names.resize(data.total_papi_events);
    } else if (token == "PAPI_EVENT") {
      int32_t papi_index = -1;
      std::string papi_name;
      iss >> papi_index >> papi_name;
      if (papi_index >= 0) {
        if (static_cast<size_t>(papi_index) >= data.papi_event_names.size()) {
          data.papi_event_names.resize(static_cast<size_t>(papi_index) + 1);
        }
        data.papi_event_names[static_cast<size_t>(papi_index)] = papi_name;
      }
    } else if (token == "CHARE") {
      ChareCollectionRecord chare;
      iss >> chare.collection_id;
      iss >> std::ws;
      std::string name;
      std::getline(iss, name, '"');
      std::getline(iss, name, '"');
      chare.name = name;
      iss >> chare.ndims;
      data.chares.push_back(chare);
      data.chare_map[chare.collection_id] = chare;
    } else if (token == "ENTRY") {
      std::string next_tok;
      iss >> next_tok;
      if (next_tok == "CHARE") {
        EntryMethodRecord ep;
        iss >> ep.ep_id;
        iss >> std::ws;
        std::string name;
        std::getline(iss, name, '"');
        std::getline(iss, name, '"');
        ep.name = name;
        iss >> ep.collection_id >> ep.msg_idx;
        data.entries.push_back(ep);
        data.ep_map[ep.ep_id] = ep;
      }
    } else if (token == "MESSAGE") {
      MessageTypeRecord message;
      iss >> message.msg_idx >> message.size;
      data.messages.push_back(message);
      data.message_map[message.msg_idx] = message;
    }
  }

  if (data.version.empty()) {
    spdlog::error("STS file is missing VERSION");
    throw std::runtime_error("Missing STS VERSION");
  }

  return data;
}

} // namespace charmvz
