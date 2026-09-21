#pragma once
#include "sts_parser.h"
#include <string>

namespace charmvz {

// Stage 1 output: chare_collection.parquet, entry_method.parquet and
// message_type.parquet from the STS registry. Each file is written even when
// its table is empty, so a consumer can tell "nothing registered" from "the
// pipeline was not run".
void write_metadata_tables(const StsData &sts_data,
                           const std::string &output_dir);

} // namespace charmvz
