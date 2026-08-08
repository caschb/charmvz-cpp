// Chare-index decoding: a BEGIN_PROCESSING record carries exactly `ndims`
// index values for a chare array, and four only for a non-array chare
// (charm/src/ck-perf/trace-projections.C:708-721). Reading a fixed four
// consumed `icputime` as `index_3` on a 1-D or 3-D array, which made every
// execution of an element look like a distinct instance -- a 22x inflation of
// the chare population on the LeanMD reference trace, and the defect that would
// have made the CCGrid paper's domain join meaningless.
//
// Each case asserts on `start_cpu_us` as well as the indices: `icputime` is the
// token immediately after the index block, so its value is the direct evidence
// that the parser consumed the right number of them and left the stream in
// sync.

#include "log_parser.h"
#include "rc_parser.h"
#include "sts_parser.h"
#include "trace_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using charmvz::test::ParquetTable;
using charmvz::test::TempTrace;

// One collection of each arity that behaves differently, and one entry method
// into each. ndims -1 is how the STS writes a group or a singleton chare.
constexpr auto kSts = "PROJECTIONS_ID \n"
                      "VERSION 11.0\n"
                      "PROCESSORS 2\n"
                      "TOTAL_CHARES 4\n"
                      "CHARE 0 \"AGroup\" -1\n"
                      "CHARE 1 \"Array1D\" 1\n"
                      "CHARE 2 \"Array3D\" 3\n"
                      "CHARE 3 \"Array6D\" 6\n"
                      "ENTRY CHARE 10 \"gmethod()\" 0 0\n"
                      "ENTRY CHARE 11 \"one(dummyMsg*)\" 1 0\n"
                      "ENTRY CHARE 12 \"three(dummyMsg*)\" 2 0\n"
                      "ENTRY CHARE 13 \"six(dummyMsg*)\" 3 0\n"
                      "TOTAL_EVENTS 0\n"
                      "TOTAL_STATS 0\n"
                      "END\n";

auto run(const TempTrace &trace) -> charmvz::LogParserResult {
  const auto sts = charmvz::parse_sts_file(trace.sts_path());
  charmvz::RcData rc;
  rc.global_start_time_us = 0;
  rc.global_end_time_us = 0;
  return charmvz::process_logs(trace.log_paths(), sts, rc, trace.out_dir(), -1);
}

// BEGIN_PROCESSING: type mIdx eIdx itime event pe msglen irecvtime <idx...>
//                   icputime
// END_PROCESSING:   type mIdx eIdx itime event pe msglen icputime
auto begin_processing(int ep, int event, const std::string &indices,
                      int cputime) -> std::string {
  return "2 0 " + std::to_string(ep) + " 1000 " + std::to_string(event) +
         " 0 64 900 " + indices + " " + std::to_string(cputime) + "\n";
}

auto end_processing(int ep, int event) -> std::string {
  return "3 0 " + std::to_string(ep) + " 1200 " + std::to_string(event) +
         " 0 64 1500\n";
}

} // namespace

TEST_CASE("A 1-D array chare carries exactly one index value",
          "[log_parser][chare_index][regression]") {
  TempTrace trace(kSts);
  trace.add_log(0, begin_processing(11, 1, "7", 555) + end_processing(11, 1));
  run(trace);

  ParquetTable chares(trace.out_dir() + "/chare_instance.parquet");
  REQUIRE(chares.rows() == 1);
  CHECK(chares.ints("collection_id")[0] == 1);
  CHECK(chares.ints("index_0")[0] == 7);
  // The unused slots must stay zero. Reading a fixed four would put icputime
  // (555) in index_1 and leave the rest of the record misaligned.
  CHECK(chares.ints("index_1")[0] == 0);
  CHECK(chares.ints("index_2")[0] == 0);
  CHECK(chares.ints("index_3")[0] == 0);
  CHECK(chares.ints("index_4")[0] == 0);
  CHECK(chares.ints("index_5")[0] == 0);

  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 1);
  CHECK(execs.ints("start_cpu_us")[0] == 555);
}

TEST_CASE("Repeated executions of one array element are one instance",
          "[log_parser][chare_index][regression]") {
  // The inflation in its simplest form. Two executions of element 7 differing
  // only in icputime: if icputime reaches the natural key, they become two
  // instances and every per-chare aggregate fragments.
  TempTrace trace(kSts);
  trace.add_log(0, begin_processing(11, 1, "7", 555) + end_processing(11, 1) +
                       begin_processing(11, 2, "7", 999) +
                       end_processing(11, 2));
  run(trace);

  ParquetTable chares(trace.out_dir() + "/chare_instance.parquet");
  CHECK(chares.rows() == 1);

  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 2);
  // Both executions must resolve to that single instance.
  CHECK(execs.ints("instance_id")[0] == execs.ints("instance_id")[1]);
}

TEST_CASE("Index arity follows the collection's ndims",
          "[log_parser][chare_index]") {
  SECTION("three dimensions") {
    TempTrace trace(kSts);
    trace.add_log(0,
                  begin_processing(12, 1, "1 2 3", 777) + end_processing(12, 1));
    run(trace);

    ParquetTable chares(trace.out_dir() + "/chare_instance.parquet");
    REQUIRE(chares.rows() == 1);
    CHECK(chares.ints("index_0")[0] == 1);
    CHECK(chares.ints("index_1")[0] == 2);
    CHECK(chares.ints("index_2")[0] == 3);
    CHECK(chares.ints("index_3")[0] == 0);
    ParquetTable execs(trace.out_dir() + "/execution.parquet");
    CHECK(execs.ints("start_cpu_us")[0] == 777);
  }

  SECTION("six dimensions fill every slot") {
    // Six is the width Projections itself reserves (`int[6]`), and LeanMD's
    // Compute chares are 6-D, so this is the case that used to leave two
    // dimensions unread rather than over-reading.
    TempTrace trace(kSts);
    trace.add_log(0, begin_processing(13, 1, "1 2 3 4 5 6", 888) +
                         end_processing(13, 1));
    run(trace);

    ParquetTable chares(trace.out_dir() + "/chare_instance.parquet");
    REQUIRE(chares.rows() == 1);
    CHECK(chares.ints("index_0")[0] == 1);
    CHECK(chares.ints("index_1")[0] == 2);
    CHECK(chares.ints("index_2")[0] == 3);
    CHECK(chares.ints("index_3")[0] == 4);
    CHECK(chares.ints("index_4")[0] == 5);
    CHECK(chares.ints("index_5")[0] == 6);
    ParquetTable execs(trace.out_dir() + "/execution.parquet");
    CHECK(execs.ints("start_cpu_us")[0] == 888);
  }

  SECTION("a non-array chare carries four") {
    // ndims == -1: the runtime writes four values regardless, so four is right
    // here and reading one would desynchronise the record.
    TempTrace trace(kSts);
    trace.add_log(0, begin_processing(10, 1, "1 2 3 4", 999) +
                         end_processing(10, 1));
    run(trace);

    ParquetTable chares(trace.out_dir() + "/chare_instance.parquet");
    REQUIRE(chares.rows() == 1);
    CHECK(chares.ints("index_0")[0] == 1);
    CHECK(chares.ints("index_3")[0] == 4);
    CHECK(chares.ints("index_4")[0] == 0);
    ParquetTable execs(trace.out_dir() + "/execution.parquet");
    CHECK(execs.ints("start_cpu_us")[0] == 999);
  }
}

TEST_CASE("An entry method absent from the STS falls back to four",
          "[log_parser][chare_index]") {
  // A trace can reference an ep the STS does not describe. Guessing an array
  // arity there would misparse; four matches the non-array layout and keeps the
  // stream aligned.
  TempTrace trace(kSts);
  trace.add_log(0, begin_processing(99, 1, "1 2 3 4", 111) +
                       end_processing(99, 1));
  run(trace);

  ParquetTable execs(trace.out_dir() + "/execution.parquet");
  REQUIRE(execs.rows() == 1);
  CHECK(execs.ints("start_cpu_us")[0] == 111);
}
