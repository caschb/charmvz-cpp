#pragma once
#include "log_parser.h"
#include <string>

namespace charmvz {

void reconstruct_message_and_migration(const LogParserResult &log_data,
                                       const StsData &sts_data,
                                       const RcData &rc_data,
                                       const std::string &output_dir);

// Writes simulation_step.parquet from the step boundaries collected in Stage 2.
// Always writes the file, even when no boundaries were found, so a consumer can
// distinguish "no step instrumentation" from "the pipeline was not run".
void reconstruct_simulation_steps(const LogParserResult &log_data,
                                  const std::string &output_dir);

} // namespace charmvz
