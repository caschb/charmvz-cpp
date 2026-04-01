#include "sts_parser.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>

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
    if (line.empty()) continue;
    std::istringstream iss(line);
    std::string token;
    iss >> token;
    
    if (token == "PROCESSORS") {
      iss >> data.total_pes;
    } else if (token == "TOTAL_PAPI_EVENTS") {
      iss >> data.total_papi_events;
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
    }
  }
  return data;
}

} // namespace charmvz
