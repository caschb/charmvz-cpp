// Migration derivation (Rule 9): a migration is a change of PE between two
// consecutive executions of the same chare-array instance.
//
// The original implementation instead paired every PUP pack with an unpack,
// which is wrong twice over. The runtime emits pack/unpack around ordinary
// message serialisation, not just around load-balancer moves, so on the LeanMD
// reference trace it produced 4,292,985 "migrations" against 4,841,103
// executions -- roughly one per entry-method execution, where the true ceiling
// is on the order of 10^5. And the pairing itself was keyed on time alone, with
// no PE and no object identity, so it matched a pack on one PE to whichever
// unpack anywhere in the machine happened to come next.
//
// These cases pin the three properties that replaced it: only chare arrays
// migrate, only a change of PE counts, and the ordering is by time rather than
// by the order the log files happened to be read.

#include "log_parser.h"
#include "rc_parser.h"
#include "reconstruction.h"
#include "sts_parser.h"
#include "trace_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using charmvz::test::ParquetTable;
using charmvz::test::TempTrace;

constexpr auto kSts = "PROJECTIONS_ID \n"
                      "VERSION 11.0\n"
                      "PROCESSORS 3\n"
                      "TOTAL_CHARES 2\n"
                      "CHARE 0 \"AGroup\" -1\n"
                      "CHARE 1 \"Array1D\" 1\n"
                      "ENTRY CHARE 10 \"gmethod()\" 0 0\n"
                      "ENTRY CHARE 11 \"one(dummyMsg*)\" 1 0\n"
                      "TOTAL_EVENTS 0\n"
                      "TOTAL_STATS 0\n"
                      "END\n";

void run(const TempTrace &trace) {
  const auto sts = charmvz::parse_sts_file(trace.sts_path());
  charmvz::RcData rc;
  rc.global_start_time_us = 0;
  rc.global_end_time_us = 0;
  const auto result =
      charmvz::process_logs(trace.log_paths(), sts, rc, trace.out_dir(), -1);
  charmvz::reconstruct_message_and_migration(result, sts, rc, trace.out_dir());
}

// One execution of an array element: BEGIN_PROCESSING then END_PROCESSING on
// the same event serial. `event` only has to be unique within a PE's log.
auto execution(int ep, int event, const std::string &indices, int start,
               int end) -> std::string {
  return "2 0 " + std::to_string(ep) + " " + std::to_string(start) + " " +
         std::to_string(event) + " 0 64 900 " + indices + " 0\n" + "3 0 " +
         std::to_string(ep) + " " + std::to_string(end) + " " +
         std::to_string(event) + " 0 64 0\n";
}

} // namespace

TEST_CASE("A change of PE between executions is one migration",
          "[reconstruction][migration]") {
  TempTrace trace(kSts);
  trace.add_log(0, execution(11, 1, "7", 1000, 1200));
  trace.add_log(1, execution(11, 1, "7", 2000, 2200));
  run(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  REQUIRE(mig.rows() == 1);
  CHECK(mig.ints("collection_id")[0] == 1);
  CHECK(mig.ints("src_pe")[0] == 0);
  CHECK(mig.ints("dst_pe")[0] == 1);
  CHECK(mig.ints("last_exec_end_src_us")[0] == 1200);
  CHECK(mig.ints("first_exec_start_dst_us")[0] == 2000);
  // gap_us is an upper bound on the move, not its cost: CkLocMgr::emigrate()
  // emits no tracing (charm/src/ck-core/cklocation.C:2961), so this interval
  // also contains load-balancer and queueing time.
  CHECK(mig.ints("gap_us")[0] == 800);
  CHECK(mig.ints("migration_seq")[0] == 1);
}

TEST_CASE("Consecutive executions on one PE are not a migration",
          "[reconstruction][migration]") {
  TempTrace trace(kSts);
  trace.add_log(0, execution(11, 1, "7", 1000, 1200) +
                       execution(11, 2, "7", 1300, 1500));
  run(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  CHECK(mig.rows() == 0);
}

TEST_CASE("Non-array chares never migrate",
          "[reconstruction][migration][regression]") {
  // A group has one instance per PE by construction, so treating its executions
  // on different PEs as movement of a single object would fabricate a migration
  // for every PE it runs on. ndims == -1 is the STS's way of saying so.
  TempTrace trace(kSts);
  trace.add_log(0, execution(10, 1, "1 2 3 4", 1000, 1200));
  trace.add_log(1, execution(10, 1, "1 2 3 4", 2000, 2200));
  run(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  CHECK(mig.rows() == 0);
}

TEST_CASE("Executions are ordered by time, not by log file order",
          "[reconstruction][migration][regression]") {
  // PE 0's log is read first but its execution happens *second*. Ordering by
  // anything other than the timestamp would report the migration backwards,
  // and timestamps are comparable across PEs because they are already aligned
  // to the run's global start.
  TempTrace trace(kSts);
  trace.add_log(0, execution(11, 1, "7", 2000, 2200));
  trace.add_log(1, execution(11, 1, "7", 1000, 1200));
  run(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  REQUIRE(mig.rows() == 1);
  CHECK(mig.ints("src_pe")[0] == 1);
  CHECK(mig.ints("dst_pe")[0] == 0);
  CHECK(mig.ints("last_exec_end_src_us")[0] == 1200);
  CHECK(mig.ints("first_exec_start_dst_us")[0] == 2000);
}

TEST_CASE("migration_seq counts hops within an instance",
          "[reconstruction][migration]") {
  TempTrace trace(kSts);
  trace.add_log(0, execution(11, 1, "7", 1000, 1200));
  trace.add_log(1, execution(11, 1, "7", 2000, 2200));
  trace.add_log(2, execution(11, 1, "7", 3000, 3200));
  run(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  REQUIRE(mig.rows() == 2);
  const auto src = mig.ints("src_pe");
  const auto dst = mig.ints("dst_pe");
  const auto seq = mig.ints("migration_seq");
  // Row order within the file follows the instance grouping, so pair the
  // columns rather than assuming which hop landed first.
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i] == 0) {
      CHECK(dst[i] == 1);
      CHECK(seq[i] == 1);
    } else {
      CHECK(src[i] == 1);
      CHECK(dst[i] == 2);
      CHECK(seq[i] == 2);
    }
  }
}

TEST_CASE("Each instance is tracked separately",
          "[reconstruction][migration][regression]") {
  // Two elements of the same array, interleaved on the same PEs. Grouping by
  // anything coarser than the instance -- by collection, say -- would chain
  // their executions together and invent migrations between them.
  TempTrace trace(kSts);
  trace.add_log(0, execution(11, 1, "7", 1000, 1200) +
                       execution(11, 2, "8", 1300, 1500));
  trace.add_log(1, execution(11, 1, "7", 2000, 2200) +
                       execution(11, 2, "8", 2300, 2500));
  run(trace);

  ParquetTable mig(trace.out_dir() + "/migration_episode.parquet");
  REQUIRE(mig.rows() == 2);
  const auto seq = mig.ints("migration_seq");
  const auto instance = mig.ints("instance_id");
  // One hop each, so both are the first migration of their own instance.
  CHECK(seq[0] == 1);
  CHECK(seq[1] == 1);
  CHECK(instance[0] != instance[1]);
}
