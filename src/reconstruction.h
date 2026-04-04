#pragma once
#include "log_parser.h"
#include <string>

namespace charmvz {

void reconstruct_message_and_migration(const LogParserResult &log_data,
                                       const StsData &sts_data,
                                       const RcData &rc_data,
                                       const std::string &output_dir);

} // namespace charmvz
