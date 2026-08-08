#include "log_parser.h"
#include "rc_parser.h"
#include "reconstruction.h"
#include "sts_parser.h"
#include "trace_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using charmvz::test::ParquetTable;
using charmvz::test::TempTrace;

// The rows of user_event.parquet, keyed so a test can look one up without
// depending on the order in which PE log files happened to be listed.
struct UserEventRow {
  int64_t pe_id;
  int64_t record_type;
  std::optional<int64_t> user_event_id;
  std::optional<std::string> name;
  std::optional<int64_t> event;
  std::optional<int64_t> nested_id;
  int64_t start_time_us;
  std::optional<int64_t> end_time_us;
  std::optional<int64_t> duration_us;
  std::optional<int64_t> user_supplied_int;
  std::optional<std::string> note;
};

auto read_user_events(const std::string &out_dir) -> std::vector<UserEventRow> {
  ParquetTable table(out_dir + "/user_event.parquet");
  const auto pe_id = table.ints("pe_id");
  const auto record_type = table.ints("record_type");
  const auto user_event_id = table.ints("user_event_id");
  const auto name = table.strings("name");
  const auto event = table.ints("event");
  const auto nested_id = table.ints("nested_id");
  const auto start = table.ints("start_time_us");
  const auto end = table.ints("end_time_us");
  const auto duration = table.ints("duration_us");
  const auto supplied = table.ints("user_supplied_int");
  const auto note = table.strings("note");

  std::vector<UserEventRow> rows;
  for (size_t i = 0; i < pe_id.size(); ++i) {
    rows.push_back(UserEventRow{*pe_id[i], *record_type[i], user_event_id[i],
                                name[i], event[i], nested_id[i], *start[i],
                                end[i], duration[i], supplied[i], note[i]});
  }
  return rows;
}

// Runs Stage 2 and the step reconstruction over a fixture.
auto run_pipeline(const TempTrace &trace, const std::string &step_event_name)
    -> charmvz::LogParserResult {
  const auto sts = charmvz::parse_sts_file(trace.sts_path());
  charmvz::RcData rc;
  rc.global_start_time_us = 0;
  rc.global_end_time_us = 0;
  const int32_t step_event_id =
      charmvz::find_user_event_id(sts, step_event_name);
  auto result = charmvz::process_logs(trace.log_paths(), sts, rc,
                                      trace.out_dir(), step_event_id);
  charmvz::reconstruct_simulation_steps(result, trace.out_dir());
  return result;
}

constexpr auto kStsWithEvents = "PROJECTIONS_ID \n"
                                "VERSION 11.0\n"
                                "PROCESSORS 2\n"
                                "TOTAL_EVENTS 3\n"
                                "TOTAL_STATS 0\n"
                                "EVENT 4 SimulationStep\n"
                                "EVENT 6 PrefetchDone\n"
                                "EVENT 9 Inner work\n"
                                "END\n";

} // namespace

TEST_CASE("USER_EVENT records become point occurrences",
          "[log_parser][user_event]") {
  TempTrace trace(kStsWithEvents);
  // type mIdx itime event pe
  trace.add_log(0, "13 6 1500 77 0\n"
                   "13 6 1900 78 0\n");
  run_pipeline(trace, "SimulationStep");

  const auto rows = read_user_events(trace.out_dir());
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].pe_id == 0);
  CHECK(rows[0].record_type == 13);
  CHECK(rows[0].user_event_id == 6);
  CHECK(rows[0].name == "PrefetchDone");
  CHECK(rows[0].event == 77);
  CHECK(rows[0].start_time_us == 1500);
  // A point event has no extent: end and duration must be NULL rather than
  // equal to the start, so that a duration aggregate is not skewed by zeros.
  CHECK_FALSE(rows[0].end_time_us.has_value());
  CHECK_FALSE(rows[0].duration_us.has_value());
  CHECK(rows[1].start_time_us == 1900);
}

TEST_CASE("USER_EVENT_PAIR records pair on their shared event serial",
          "[log_parser][user_event]") {
  TempTrace trace(kStsWithEvents);
  // userBracketEvent() emits two records sharing one serial: begin then end.
  // type mIdx itime event pe nestedID
  trace.add_log(0, "100 9 1000 50 0 0\n"
                   "100 9 1250 50 0 0\n");
  run_pipeline(trace, "SimulationStep");

  const auto rows = read_user_events(trace.out_dir());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].record_type == 100);
  CHECK(rows[0].user_event_id == 9);
  CHECK(rows[0].name == "Inner work");
  CHECK(rows[0].start_time_us == 1000);
  CHECK(rows[0].end_time_us == 1250);
  CHECK(rows[0].duration_us == 250);
}

TEST_CASE("Bracketed user events are attributed to the log file's PE",
          "[log_parser][user_event][regression]") {
  // The LogEntry constructor for bracketed events never assigns `pe`
  // (charm/src/ck-perf/trace-projections.h:179-185), so every such record
  // carries a literal 0 no matter which PE wrote it. Trusting that field would
  // pile every bracketed user event in the run onto PE 0.
  TempTrace trace(kStsWithEvents);
  trace.add_log(1, "100 9 1000 50 0 0\n"
                   "100 9 1250 50 0 0\n"
                   "13 6 1300 51 1\n");
  run_pipeline(trace, "SimulationStep");

  const auto rows = read_user_events(trace.out_dir());
  REQUIRE(rows.size() == 2);
  for (const auto &row : rows) {
    CHECK(row.pe_id == 1);
  }
}

TEST_CASE("BEGIN/END_USER_EVENT_PAIR pair on user event id and nestedID",
          "[log_parser][user_event]") {
  TempTrace trace(kStsWithEvents);

  SECTION("a simple bracket closes into one row") {
    // These consume a fresh event serial each (98 -> 60, 99 -> 61), so they
    // cannot be paired on the serial the way USER_EVENT_PAIR is.
    trace.add_log(0, "98 9 2000 60 0 7\n"
                     "99 9 2400 61 0 7\n");
    run_pipeline(trace, "SimulationStep");

    const auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].record_type == 98);
    CHECK(rows[0].nested_id == 7);
    CHECK(rows[0].start_time_us == 2000);
    CHECK(rows[0].end_time_us == 2400);
    CHECK(rows[0].duration_us == 400);
  }

  SECTION("overlapping nestedIDs do not cross-pair") {
    // Two brackets of the same user event overlap without nesting: 1 opens,
    // 2 opens, 1 closes, 2 closes. This is the case nestedID exists to
    // disambiguate, and the only one that distinguishes pairing on
    // (id, nestedID) from pairing on the id alone -- with properly nested
    // brackets a LIFO stack gives the right answer either way.
    trace.add_log(0, "98 9 100 60 0 1\n"
                     "98 9 200 61 0 2\n"
                     "99 9 300 62 0 1\n"
                     "99 9 400 63 0 2\n");
    run_pipeline(trace, "SimulationStep");

    auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 2);
    std::map<int64_t, UserEventRow> by_nested;
    for (const auto &row : rows) {
      by_nested.emplace(*row.nested_id, row);
    }
    CHECK(by_nested.at(1).start_time_us == 100);
    CHECK(by_nested.at(1).end_time_us == 300);
    CHECK(by_nested.at(2).start_time_us == 200);
    CHECK(by_nested.at(2).end_time_us == 400);
  }

  SECTION("identical keys nest as a stack") {
    trace.add_log(0, "98 9 100 60 0 0\n"
                     "98 9 200 61 0 0\n"
                     "99 9 300 62 0 0\n"
                     "99 9 400 63 0 0\n");
    run_pipeline(trace, "SimulationStep");

    auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 2);
    // The inner bracket closes first.
    CHECK(rows[0].start_time_us == 200);
    CHECK(rows[0].end_time_us == 300);
    CHECK(rows[1].start_time_us == 100);
    CHECK(rows[1].end_time_us == 400);
  }
}

TEST_CASE("Unbalanced brackets are kept, not dropped",
          "[log_parser][user_event]") {
  TempTrace trace(kStsWithEvents);

  SECTION("a BEGIN with no END keeps the occurrence with a NULL end") {
    trace.add_log(0, "98 9 100 60 0 3\n");
    run_pipeline(trace, "SimulationStep");

    const auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].record_type == 98);
    CHECK(rows[0].start_time_us == 100);
    CHECK_FALSE(rows[0].end_time_us.has_value());
  }

  SECTION("an END with no BEGIN is recorded as an END") {
    // Happens when traceBegin() turns tracing on inside a bracket.
    trace.add_log(0, "99 9 500 60 0 3\n");
    run_pipeline(trace, "SimulationStep");

    const auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].record_type == 99);
    CHECK(rows[0].start_time_us == 500);
    CHECK_FALSE(rows[0].end_time_us.has_value());
  }

  SECTION("an unmatched USER_EVENT_PAIR half survives") {
    trace.add_log(0, "100 9 100 50 0 0\n");
    run_pipeline(trace, "SimulationStep");

    const auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].start_time_us == 100);
    CHECK_FALSE(rows[0].end_time_us.has_value());
  }
}

TEST_CASE("USER_SUPPLIED carries its integer datum",
          "[log_parser][user_event]") {
  TempTrace trace(kStsWithEvents);
  // type userSuppliedData itime -- note the datum precedes the timestamp.
  trace.add_log(0, "26 -42 1234\n");
  run_pipeline(trace, "SimulationStep");

  const auto rows = read_user_events(trace.out_dir());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].record_type == 26);
  CHECK(rows[0].start_time_us == 1234);
  CHECK(rows[0].user_supplied_int == -42);
  // This form has no registered id and no Projections serial.
  CHECK_FALSE(rows[0].user_event_id.has_value());
  CHECK_FALSE(rows[0].event.has_value());
}

TEST_CASE("User-supplied notes decode the PUP string encoding",
          "[log_parser][user_event]") {
  TempTrace trace(kStsWithEvents);

  SECTION("USER_SUPPLIED_NOTE") {
    // A std::string is a length followed by exactly that many characters with
    // no separator, because toProjectionsFile writes Tchar as "%c".
    trace.add_log(0, "28 900 5hello\n");
    run_pipeline(trace, "SimulationStep");

    const auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].record_type == 28);
    CHECK(rows[0].start_time_us == 900);
    CHECK(rows[0].note == "hello");
  }

  SECTION("a note containing spaces is read by length, not by token") {
    trace.add_log(0, "28 900 11rung 3 done\n");
    run_pipeline(trace, "SimulationStep");

    const auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].note == "rung 3 done");
  }

  SECTION("USER_SUPPLIED_BRACKETED_NOTE carries its own extent") {
    // type itime iEndTime event note
    trace.add_log(0, "29 1000 1600 88 4step\n");
    run_pipeline(trace, "SimulationStep");

    const auto rows = read_user_events(trace.out_dir());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].record_type == 29);
    CHECK(rows[0].event == 88);
    CHECK(rows[0].start_time_us == 1000);
    CHECK(rows[0].end_time_us == 1600);
    CHECK(rows[0].duration_us == 600);
    CHECK(rows[0].note == "step");
  }
}

TEST_CASE("simulation_step is reconstructed from step-boundary brackets",
          "[log_parser][simulation_step]") {
  TempTrace trace(kStsWithEvents);
  // Two PEs bracket two steps each, with the step index in nestedID. PE 1
  // trails PE 0, which is the whole reason the table is grained per PE.
  trace.add_log(0, "98 4 1000 10 0 0\n"
                   "99 4 2000 11 0 0\n"
                   "98 4 2000 12 0 1\n"
                   "99 4 3000 13 0 1\n");
  trace.add_log(1, "98 4 1100 10 0 0\n"
                   "99 4 2100 11 0 0\n"
                   "98 4 2100 12 0 1\n"
                   "99 4 3200 13 0 1\n");
  run_pipeline(trace, "SimulationStep");

  ParquetTable steps(trace.out_dir() + "/simulation_step.parquet");
  REQUIRE(steps.rows() == 4);

  const auto step_id = steps.ints("step_id");
  const auto pe_id = steps.ints("pe_id");
  const auto start = steps.ints("start_time_us");
  const auto end = steps.ints("end_time_us");
  const auto duration = steps.ints("duration_us");
  const auto global_start = steps.ints("global_start_time_us");
  const auto global_end = steps.ints("global_end_time_us");
  const auto pe_count = steps.ints("pe_count");

  std::map<std::pair<int64_t, int64_t>, size_t> index;
  for (size_t i = 0; i < step_id.size(); ++i) {
    index.emplace(std::make_pair(*step_id[i], *pe_id[i]), i);
  }
  REQUIRE(index.size() == 4);

  const auto step1_pe1 = index.at({1, 1});
  CHECK(start[step1_pe1] == 2100);
  CHECK(end[step1_pe1] == 3200);
  CHECK(duration[step1_pe1] == 1100);
  // The global extent is the union across PEs, denormalized into every row.
  CHECK(global_start[step1_pe1] == 2000);
  CHECK(global_end[step1_pe1] == 3200);
  CHECK(pe_count[step1_pe1] == 2);

  const auto step0_pe0 = index.at({0, 0});
  CHECK(start[step0_pe0] == 1000);
  CHECK(global_start[step0_pe0] == 1000);
  CHECK(global_end[step0_pe0] == 2100);
}

TEST_CASE("Repeated boundary brackets fold into one interval per step and PE",
          "[log_parser][simulation_step]") {
  // An application that brackets several sub-phases with the same boundary
  // event within one step must not produce duplicate (step_id, pe_id) rows.
  TempTrace trace(kStsWithEvents);
  trace.add_log(0, "98 4 1000 10 0 5\n"
                   "99 4 1200 11 0 5\n"
                   "98 4 1500 12 0 5\n"
                   "99 4 1800 13 0 5\n");
  run_pipeline(trace, "SimulationStep");

  ParquetTable steps(trace.out_dir() + "/simulation_step.parquet");
  REQUIRE(steps.rows() == 1);
  CHECK(steps.ints("step_id")[0] == 5);
  CHECK(steps.ints("start_time_us")[0] == 1000);
  CHECK(steps.ints("end_time_us")[0] == 1800);
}

TEST_CASE("Without a matching step event no timesteps are reconstructed",
          "[log_parser][simulation_step]") {
  // The user events are still materialized; only the step derivation is off.
  TempTrace trace(kStsWithEvents);
  trace.add_log(0, "98 9 1000 10 0 0\n"
                   "99 9 2000 11 0 0\n");
  const auto result = run_pipeline(trace, "NoSuchEvent");

  CHECK(result.step_boundaries.empty());
  ParquetTable steps(trace.out_dir() + "/simulation_step.parquet");
  CHECK(steps.rows() == 0);
  ParquetTable events(trace.out_dir() + "/user_event.parquet");
  CHECK(events.rows() == 1);
}

TEST_CASE("Unregistered user event ids still produce rows",
          "[log_parser][user_event]") {
  // traceUserEvent() does not require the id to have been registered.
  TempTrace trace(kStsWithEvents);
  trace.add_log(0, "13 77 1500 20 0\n");
  run_pipeline(trace, "SimulationStep");

  const auto rows = read_user_events(trace.out_dir());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].user_event_id == 77);
  CHECK_FALSE(rows[0].name.has_value());
}

TEST_CASE("User-event parsing leaves the execution stream intact",
          "[log_parser][regression]") {
  // The user-event records sit between the processing records they must not
  // disturb: a mis-sized read would desynchronise the stream for the rest of
  // the file. Only whole lines are consumed, so this checks the seam.
  TempTrace trace(kStsWithEvents);
  trace.add_log(0, "6 0\n"
                   "13 6 100 1 0\n"
                   "14 200 0\n"
                   "15 300 0\n"
                   "28 350 3abc\n"
                   "14 400 0\n"
                   "15 500 0\n"
                   "7 600\n");
  const auto result = run_pipeline(trace, "SimulationStep");

  REQUIRE(result.pes.size() == 1);
  CHECK(result.pes[0].end_time_us == 600);
  ParquetTable idle(trace.out_dir() + "/idle_interval.parquet");
  CHECK(idle.rows() == 2);
  CHECK(idle.ints("duration_us")[1] == 100);
}
