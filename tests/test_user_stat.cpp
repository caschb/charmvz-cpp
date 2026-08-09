// Tests for USER_STAT (code 32) and MEMORY_USAGE_CURRENT (code 27).
//
// Both record types were skipped by the dispatch switch until the two entities
// were added, and both carry a field-order trap that produces a well-typed but
// wrong table when read in the order the rest of the format uses. The field
// orders asserted here were checked against charm/src/ck-perf/trace-projections.C
// (the `pup` switch at lines 770-782), not against the format note.

#include "log_parser.h"
#include "rc_parser.h"
#include "sts_parser.h"
#include "trace_fixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

using Catch::Approx;
using charmvz::test::ParquetTable;
using charmvz::test::TempTrace;

constexpr auto kStsWithStats = "PROJECTIONS_ID \n"
                               "VERSION 11.0\n"
                               "PROCESSORS 2\n"
                               "TOTAL_EVENTS 0\n"
                               "TOTAL_STATS 2\n"
                               "STAT 0 residual\n"
                               "STAT 1 particle count\n"
                               "END\n";

// Runs Stage 2 over a fixture. `global_start_us` is a parameter because
// timestamp alignment is one of the things being asserted.
auto run(const TempTrace &trace, int64_t global_start_us = 0)
    -> charmvz::LogParserResult {
  const auto sts = charmvz::parse_sts_file(trace.sts_path());
  charmvz::RcData rc;
  rc.global_start_time_us = global_start_us;
  rc.global_end_time_us = 0;
  return charmvz::process_logs(trace.log_paths(), sts, rc, trace.out_dir(),
                               charmvz::NO_STEP_EVENT);
}

} // namespace

TEST_CASE("USER_STAT records become user_stat rows", "[user_stat]") {
  TempTrace trace(kStsWithStats);
  // Field order: type itime cputime stat pe mIdx
  trace.add_log(0, "32 1000 -1 2.5 0 0\n"
                   "32 2000 0.125 7.5 0 1\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/user_stat.parquet");
  REQUIRE(table.rows() == 2);

  const auto pe_id = table.ints("pe_id");
  const auto stat_id = table.ints("stat_id");
  const auto name = table.strings("name");
  const auto time_us = table.ints("time_us");
  const auto value = table.doubles("stat_value");

  CHECK(*pe_id[0] == 0);
  CHECK(*stat_id[0] == 0);
  CHECK(*name[0] == "residual");
  CHECK(*time_us[0] == 1000);
  CHECK(*value[0] == Approx(2.5));

  CHECK(*stat_id[1] == 1);
  CHECK(*name[1] == "particle count");
  CHECK(*time_us[1] == 2000);
  CHECK(*value[1] == Approx(7.5));
}

TEST_CASE("USER_STAT statTime of -1 is stored as NULL", "[user_stat]") {
  TempTrace trace(kStsWithStats);
  // updateStat() supplies no time and the runtime writes -1 for it;
  // updateStatPair() supplies one. The first must not surface as a time of -1.
  trace.add_log(0, "32 1000 -1 2.5 0 0\n"
                   "32 2000 0.125 7.5 0 0\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/user_stat.parquet");
  const auto user_time = table.doubles("user_time_s");
  REQUIRE(user_time.size() == 2);
  CHECK_FALSE(user_time[0].has_value());
  REQUIRE(user_time[1].has_value());
  CHECK(*user_time[1] == Approx(0.125));
}

TEST_CASE("USER_STAT keeps the fractional part of the application's time",
          "[user_stat]") {
  TempTrace trace(kStsWithStats);
  // USER_STAT writes `cputime` raw, with none of the 1.0e6 conversion every
  // other time field gets. Reading it as an integer would truncate this to 0.
  trace.add_log(0, "32 1000 0.375 1.0 0 0\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/user_stat.parquet");
  const auto user_time = table.doubles("user_time_s");
  REQUIRE(user_time.size() == 1);
  REQUIRE(user_time[0].has_value());
  CHECK(*user_time[0] == Approx(0.375));
}

TEST_CASE("USER_STAT timestamps are aligned against the global start",
          "[user_stat]") {
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "32 5000 -1 1.0 0 0\n");
  run(trace, 1500);

  ParquetTable table(trace.out_dir() + "/user_stat.parquet");
  const auto time_us = table.ints("time_us");
  REQUIRE(time_us.size() == 1);
  CHECK(*time_us[0] == 3500);
}

TEST_CASE("USER_STAT with an unregistered stat id keeps a NULL name",
          "[user_stat]") {
  TempTrace trace(kStsWithStats);
  // Stat 7 is not in the STS registry. The sample is still evidence and is
  // kept; only its name is unknown.
  trace.add_log(0, "32 1000 -1 3.0 0 7\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/user_stat.parquet");
  REQUIRE(table.rows() == 1);
  const auto name = table.strings("name");
  const auto stat_id = table.ints("stat_id");
  CHECK(*stat_id[0] == 7);
  CHECK_FALSE(name[0].has_value());
}

TEST_CASE("USER_STAT is attributed to the PE that owns the log file",
          "[user_stat]") {
  TempTrace trace(kStsWithStats);
  // The record's own `pe` field is genuine here (it is CkMyPe()), but every
  // table in this schema keys on the file's PE. A log named test.1.log carries
  // PE 1's records whatever the field says.
  trace.add_log(1, "32 1000 -1 1.0 0 0\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/user_stat.parquet");
  const auto pe_id = table.ints("pe_id");
  REQUIRE(pe_id.size() == 1);
  CHECK(*pe_id[0] == 1);
}

TEST_CASE("user_stat.parquet is written even with no USER_STAT records",
          "[user_stat]") {
  // An empty table lets a consumer tell "the application declares no stats"
  // apart from "the pipeline never ran", the same reason simulation_step is
  // always written.
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "6 1000\n7 2000\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/user_stat.parquet");
  CHECK(table.rows() == 0);
}

TEST_CASE("MEMORY_USAGE_CURRENT records become memory_sample rows",
          "[memory_sample]") {
  TempTrace trace(kStsWithStats);
  // Field order: type memUsage itime. The byte count comes FIRST, reversing
  // every other record in the format.
  trace.add_log(0, "27 1048576 1000\n"
                   "27 2097152 2000\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/memory_sample.parquet");
  REQUIRE(table.rows() == 2);

  const auto pe_id = table.ints("pe_id");
  const auto time_us = table.ints("time_us");
  const auto bytes = table.ints("bytes");

  CHECK(*pe_id[0] == 0);
  CHECK(*time_us[0] == 1000);
  CHECK(*bytes[0] == 1048576);
  CHECK(*time_us[1] == 2000);
  CHECK(*bytes[1] == 2097152);
}

TEST_CASE("MEMORY_USAGE_CURRENT does not read its fields in the usual order",
          "[memory_sample]") {
  // The guard on the field-order trap. Reading `itime` first would put 1000 in
  // `bytes` and 1048576 in `time_us`: no error, no type violation, and a
  // memory chart that is a plot of the clock.
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "27 1048576 1000\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/memory_sample.parquet");
  const auto time_us = table.ints("time_us");
  const auto bytes = table.ints("bytes");
  REQUIRE(bytes.size() == 1);
  CHECK(*bytes[0] == 1048576);
  CHECK(*time_us[0] == 1000);
  CHECK(*bytes[0] != *time_us[0]);
}

TEST_CASE("MEMORY_USAGE_CURRENT is attributed to the log file's PE",
          "[memory_sample]") {
  // This record carries no PE field at all, so the file name is the only
  // source there is.
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "27 4096 1000\n");
  trace.add_log(1, "27 8192 1000\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/memory_sample.parquet");
  REQUIRE(table.rows() == 2);
  const auto pe_id = table.ints("pe_id");
  const auto bytes = table.ints("bytes");

  std::optional<int64_t> pe0_bytes;
  std::optional<int64_t> pe1_bytes;
  for (size_t i = 0; i < pe_id.size(); ++i) {
    if (*pe_id[i] == 0) {
      pe0_bytes = bytes[i];
    } else if (*pe_id[i] == 1) {
      pe1_bytes = bytes[i];
    }
  }
  REQUIRE(pe0_bytes.has_value());
  REQUIRE(pe1_bytes.has_value());
  CHECK(*pe0_bytes == 4096);
  CHECK(*pe1_bytes == 8192);
}

TEST_CASE("MEMORY_USAGE_CURRENT timestamps are aligned", "[memory_sample]") {
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "27 4096 5000\n");
  run(trace, 1500);

  ParquetTable table(trace.out_dir() + "/memory_sample.parquet");
  const auto time_us = table.ints("time_us");
  REQUIRE(time_us.size() == 1);
  CHECK(*time_us[0] == 3500);
}

TEST_CASE("A byte count above 2^31 survives the round trip",
          "[memory_sample]") {
  // The runtime writes an `unsigned long`; a 4 GiB heap is ordinary on the
  // machines these traces come from and must not wrap through an int32.
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "27 5368709120 1000\n");
  run(trace);

  ParquetTable table(trace.out_dir() + "/memory_sample.parquet");
  const auto bytes = table.ints("bytes");
  REQUIRE(bytes.size() == 1);
  CHECK(*bytes[0] == 5368709120LL);
}

TEST_CASE("A log whose name yields no PE is skipped, not attributed to -1",
          "[user_stat][memory_sample]") {
  // Found on a real trace: the run's own trace_<job>.tar.gz sat in the trace
  // directory, main.cpp accepted it for its .gz extension, zstr decompressed
  // it, and four stretches of tar header parsed as code-27 and code-32
  // records. They reached the output on pe_id -1, a PE that does not exist,
  // and no downstream join rejects such a row.
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "32 1000 -1 2.5 0 0\n"
                   "27 4096 1500\n");
  trace.add_unnamed_log("archive.tar", "32 2000 -1 9.9 0 0\n"
                                       "27 8192 2500\n");
  run(trace);

  ParquetTable stats(trace.out_dir() + "/user_stat.parquet");
  ParquetTable memory(trace.out_dir() + "/memory_sample.parquet");
  REQUIRE(stats.rows() == 1);
  REQUIRE(memory.rows() == 1);
  for (const auto &pe : stats.ints("pe_id")) {
    CHECK(*pe >= 0);
  }
  for (const auto &pe : memory.ints("pe_id")) {
    CHECK(*pe >= 0);
  }
}

TEST_CASE("The two record types do not consume each other's records",
          "[user_stat][memory_sample]") {
  // Both were previously dropped by the same `default:` arm, so a dispatch
  // mistake that routed one into the other would have gone unnoticed.
  TempTrace trace(kStsWithStats);
  trace.add_log(0, "32 1000 -1 2.5 0 0\n"
                   "27 4096 1500\n"
                   "32 2000 -1 3.5 0 1\n");
  run(trace);

  ParquetTable stats(trace.out_dir() + "/user_stat.parquet");
  ParquetTable memory(trace.out_dir() + "/memory_sample.parquet");
  CHECK(stats.rows() == 2);
  CHECK(memory.rows() == 1);
}
